#include "breezy.h"
#include "network_internal.h"
#include <string.h>

#ifdef _WIN32
#define U_FD(o)     (*(SOCKET*)((char*)(o) + 24))   /* UdpSocket reuses the socket-handle layout. */
#define U_CLOSED(o) (*(int64_t*)((char*)(o) + 32))

/* Datagram layout: 24 data(byte[]) | 32 host(string) | 40 port. num_obj_fields = 2. */
#define DG_DATA(o) (*(void**)((char*)(o) + 24))
#define DG_HOST(o) (*(void**)((char*)(o) + 32))
#define DG_PORT(o) (*(int64_t*)((char*)(o) + 40))

/* Typeinfo: [finalizer][num_obj_fields][off0...], matching the array/map/entry
   convention. No finalizer (the two managed children are released generically);
   offsets 24, 32 are data + host. */
static int64_t g_dgram_typeinfo[4] = { 0 /* Finalizer. */, 2, 24, 32 };
static int64_t g_dgram_vtable[2];

static void *dgram_vtable(void)
{
	g_dgram_vtable[0] = (int64_t)&g_dgram_typeinfo[0];
	return &g_dgram_vtable[1];
}

static void *dgram_new(void *data, void *host, int64_t port)
{
	void *o = bzy_alloc(48);
	*(void**)o = dgram_vtable();
	DG_DATA(o) = data;   /* Owned (+1) transferred in. */
	DG_HOST(o) = host;   /* Owned (+1) transferred in. */
	DG_PORT(o) = port;
	return o;
}

/* A timeout arms this: after ms it cancels the pending overlapped op, which then
   completes with ERROR_OPERATION_ABORTED. cc lives on the caller's (breeze) stack. */
typedef struct
{
	SOCKET fd;
	OVERLAPPED *ov;
} CancelCtx;

static VOID CALLBACK cancel_cb(PVOID p, BOOLEAN timed_out)
{
	(void)timed_out;
	CancelCtx *c = (CancelCtx*)p;
	CancelIoEx((HANDLE)c->fd, c->ov);
}

void *bzy_udp_new(int64_t port)
{
	bzy_iocp_ensure();
	SOCKET fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	u_long nb = 1;
	ioctlsocket(fd, FIONBIO, &nb);   /* Non-blocking: enables the synchronous try-path. */

	struct sockaddr_in addr;
	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = INADDR_ANY;
	addr.sin_port = htons((unsigned short)port);
	bind(fd, (struct sockaddr*)&addr, sizeof(addr));

	bzy_iocp_associate((void*)fd);
	return bzy_sock_wrap(fd);   /* Same managed handle + closesocket finalizer. */
}

int64_t bzy_udp_port(void *u)
{
	struct sockaddr_in addr;
	int len = sizeof(addr);
	if (getsockname(U_FD(u), (struct sockaddr*)&addr, &len) != 0)
	{
		return -1;
	}

	return (int64_t)ntohs(addr.sin_port);
}

static int64_t udp_send_bytes(void *u, void *host, int64_t port, const char *buf, int64_t len)
{
	struct sockaddr_in dst;
	memset(&dst, 0, sizeof(dst));
	dst.sin_family = AF_INET;
	dst.sin_port = htons((unsigned short)port);
	dst.sin_addr.s_addr = inet_addr(bzy_str_data(host));   /* IPv4 dotted-quad; getaddrinfo for names. */
	if (dst.sin_addr.s_addr == INADDR_NONE)
	{
		if (bzy_resolve4(bzy_str_data(host), (int)port, &dst) != 0)
		{
			return 0;
		}
	}

	WSABUF wb;
	wb.buf = (char*)buf;
	wb.len = (ULONG)len;
	DWORD got = 0;

	char opbuf[BZY_IOCP_OP_SIZE];
	IocpOp *op = (IocpOp*)opbuf;
	bzy_iocp_op_reset(op);

	int rc = WSASendTo(U_FD(u), &wb, 1, &got, 0, (struct sockaddr*)&dst, sizeof(dst),
					   (OVERLAPPED*)bzy_iocp_op_overlapped(op), NULL);
	if (rc != 0 && WSAGetLastError() != WSA_IO_PENDING)
	{
		return 0;
	}

	bzy_iocp_park(op);
	if (bzy_iocp_op_err(op))
	{
		return 0;
	}

	return (int64_t)bzy_iocp_op_bytes(op);
}

int64_t bzy_udp_send_to(void *u, void *host, int64_t port, void *data)
{
	int64_t n = bzy_array_len(data);
	const char *buf = (const char*)data + 32;   /* Packed bytes, no copy. */
	return udp_send_bytes(u, host, port, buf, n);
}

int64_t bzy_udp_send_text_to(void *u, void *host, int64_t port, void *str)
{
	return udp_send_bytes(u, host, port, bzy_str_data(str), bzy_str_len(str));
}

/* WSARecvFrom into buf. timeout_ms<0 = infinite. Returns bytes, -1 err, -2 timeout. */
static int udp_recv(void *u, char *buf, int max, int64_t timeout_ms,
					struct sockaddr_in *from, int *fromlen)
{
	WSABUF wb;
	wb.buf = buf;
	wb.len = (ULONG)max;
	DWORD flags = 0, got = 0;
	*fromlen = sizeof(*from);
	memset(from, 0, sizeof(*from));

	char opbuf[BZY_IOCP_OP_SIZE];
	IocpOp *op = (IocpOp*)opbuf;
	bzy_iocp_op_reset(op);
	OVERLAPPED *ov = (OVERLAPPED*)bzy_iocp_op_overlapped(op);

	int rc = WSARecvFrom(U_FD(u), &wb, 1, &got, &flags, (struct sockaddr*)from, fromlen, ov, NULL);
	if (rc != 0 && WSAGetLastError() != WSA_IO_PENDING)
	{
		return -1;
	}

	HANDLE timer = NULL;
	CancelCtx cc = { U_FD(u), ov };
	if (timeout_ms >= 0)
	{
		CreateTimerQueueTimer(&timer, NULL, cancel_cb, &cc, (DWORD)timeout_ms, 0, WT_EXECUTEONLYONCE);
	}

	bzy_iocp_park(op);
	if (timer)
	{
		DeleteTimerQueueTimer(NULL, timer, INVALID_HANDLE_VALUE);
	}

	int e = bzy_iocp_op_err(op);
	if (e == ERROR_OPERATION_ABORTED)
	{
		return -2;
	}

	if (e)
	{
		return -1;
	}

	return (int)bzy_iocp_op_bytes(op);
}

static void *make_dgram(const char *buf, int n, struct sockaddr_in *from)
{
	void *arr = bzy_array_new_sized(n < 0 ? 0 : n, 1, 0);   /* Packed byte[]. */
	if (n > 0)
	{
		memcpy((char*)arr + 32, buf, (size_t)n);
	}

	char ip[INET_ADDRSTRLEN] = {0};
	inet_ntop(AF_INET, &from->sin_addr, ip, sizeof(ip));
	void *host = bzy_str_new(ip, (int64_t)strlen(ip));
	return dgram_new(arr, host, (int64_t)ntohs(from->sin_port));
}

void *bzy_udp_receive(void *u)
{
	char buf[65536];                  /* Max IPv4 UDP payload. */
	struct sockaddr_in from;
	int fromlen;
	int n = udp_recv(u, buf, sizeof(buf), -1, &from, &fromlen);
	return make_dgram(buf, n < 0 ? 0 : n, &from);
}

void *bzy_udp_receive_timeout(void *u, int64_t ms)
{
	char buf[65536];
	struct sockaddr_in from;
	int fromlen;
	int n = udp_recv(u, buf, sizeof(buf), ms, &from, &fromlen);
	if (n == -2)
	{
		return NULL;   /* Timed out. */
	}

	return make_dgram(buf, n < 0 ? 0 : n, &from);
}

void *bzy_udp_try_receive(void *u)
{
	char buf[65536];
	struct sockaddr_in from;
	int fromlen = sizeof(from);
	memset(&from, 0, sizeof(from));
	int n = recvfrom(U_FD(u), buf, sizeof(buf), 0, (struct sockaddr*)&from, &fromlen);
	if (n < 0)
	{
		return NULL;   /* WSAEWOULDBLOCK: no datagram queued. */
	}

	return make_dgram(buf, n, &from);
}

void bzy_udp_close(void *u)
{
	if (!U_CLOSED(u) && U_FD(u) != INVALID_SOCKET)
	{
		closesocket(U_FD(u));
		U_CLOSED(u) = 1;
	}
}

void *bzy_dgram_data(void *d)
{
	bzy_retain(DG_DATA(d));
	return DG_DATA(d);
}

void *bzy_dgram_text(void *d)
{
	void *arr = DG_DATA(d);
	int64_t n = bzy_array_len(arr);
	const char *buf = (const char*)arr + 32;   /* Packed bytes, no copy. */
	return bzy_str_new(buf, n);
}

void *bzy_dgram_host(void *d)
{
	bzy_retain(DG_HOST(d));
	return DG_HOST(d);
}

int64_t bzy_dgram_port(void *d)
{
	return DG_PORT(d);
}

#else
/* ===== POSIX: UDP on the epoll reactor (recvfrom/sendto, non-blocking). ===== */
#define U_FD(o)     (*(int64_t*)((char*)(o) + 24))
#define U_CLOSED(o) (*(int64_t*)((char*)(o) + 32))

#define DG_DATA(o) (*(void**)((char*)(o) + 24))
#define DG_HOST(o) (*(void**)((char*)(o) + 32))
#define DG_PORT(o) (*(int64_t*)((char*)(o) + 40))

static int64_t g_dgram_typeinfo[4] = { 0 /* Finalizer. */, 2, 24, 32 };
static int64_t g_dgram_vtable[2];

static void *dgram_vtable(void)
{
	g_dgram_vtable[0] = (int64_t)&g_dgram_typeinfo[0];
	return &g_dgram_vtable[1];
}

static void *dgram_new(void *data, void *host, int64_t port)
{
	void *o = bzy_alloc(48);
	*(void**)o = dgram_vtable();
	DG_DATA(o) = data;   /* Owned (+1) transferred in. */
	DG_HOST(o) = host;   /* Owned (+1) transferred in. */
	DG_PORT(o) = port;
	return o;
}

void *bzy_udp_new(int64_t port)
{
	bzy_reactor_ensure();
	int fd = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK, 0);
	if (fd < 0)
	{
		return NULL;
	}

	struct sockaddr_in addr;
	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = INADDR_ANY;
	addr.sin_port = htons((unsigned short)port);
	bind(fd, (struct sockaddr*)&addr, sizeof(addr));
	return bzy_sock_wrap(fd);
}

int64_t bzy_udp_port(void *u)
{
	struct sockaddr_in addr;
	socklen_t len = sizeof(addr);
	if (getsockname((int)U_FD(u), (struct sockaddr*)&addr, &len) != 0)
	{
		return -1;
	}

	return (int64_t)ntohs(addr.sin_port);
}

static int64_t udp_send_bytes(void *u, void *host, int64_t port, const char *buf, int64_t len)
{
	struct sockaddr_in dst;
	memset(&dst, 0, sizeof(dst));
	dst.sin_family = AF_INET;
	dst.sin_port = htons((unsigned short)port);
	dst.sin_addr.s_addr = inet_addr(bzy_str_data(host));   /* IPv4 dotted-quad; getaddrinfo for names. */
	if (dst.sin_addr.s_addr == INADDR_NONE)
	{
		if (bzy_resolve4(bzy_str_data(host), (int)port, &dst) != 0)
		{
			return 0;
		}
	}

	for (;;)
	{
		ssize_t n = sendto((int)U_FD(u), buf, (size_t)len, MSG_NOSIGNAL, (struct sockaddr*)&dst, sizeof(dst));
		if (n >= 0)
		{
			return (int64_t)n;
		}

		if (errno != EAGAIN && errno != EWOULDBLOCK)
		{
			return 0;
		}

		if (bzy_reactor_wait((int)U_FD(u), 1, -1) < 0)
		{
			return 0;
		}
	}
}

int64_t bzy_udp_send_to(void *u, void *host, int64_t port, void *data)
{
	int64_t n = bzy_array_len(data);
	const char *buf = (const char*)data + 32;   /* Packed bytes, no copy. */
	return udp_send_bytes(u, host, port, buf, n);
}

int64_t bzy_udp_send_text_to(void *u, void *host, int64_t port, void *str)
{
	return udp_send_bytes(u, host, port, bzy_str_data(str), bzy_str_len(str));
}

/* One recvfrom, parking on read-readiness. timeout_ms<0 = infinite. bytes, -1 err, -2 timeout. */
static int udp_recv(void *u, char *buf, int max, int64_t timeout_ms, struct sockaddr_in *from)
{
	for (;;)
	{
		socklen_t fromlen = sizeof(*from);
		memset(from, 0, sizeof(*from));
		ssize_t n = recvfrom((int)U_FD(u), buf, (size_t)max, 0, (struct sockaddr*)from, &fromlen);
		if (n >= 0)
		{
			return (int)n;
		}

		if (errno != EAGAIN && errno != EWOULDBLOCK)
		{
			return -1;
		}

		int r = bzy_reactor_wait((int)U_FD(u), 0, timeout_ms);
		if (r == 0)
		{
			return -2;
		}

		if (r < 0)
		{
			return -1;
		}
	}
}

static void *make_dgram(const char *buf, int n, struct sockaddr_in *from)
{
	void *arr = bzy_array_new_sized(n < 0 ? 0 : n, 1, 0);   /* Packed byte[]. */
	if (n > 0)
	{
		memcpy((char*)arr + 32, buf, (size_t)n);
	}

	char ip[INET_ADDRSTRLEN] = {0};
	inet_ntop(AF_INET, &from->sin_addr, ip, sizeof(ip));
	void *host = bzy_str_new(ip, (int64_t)strlen(ip));
	return dgram_new(arr, host, (int64_t)ntohs(from->sin_port));
}

void *bzy_udp_receive(void *u)
{
	char buf[65536];
	struct sockaddr_in from;
	int n = udp_recv(u, buf, sizeof(buf), -1, &from);
	return make_dgram(buf, n < 0 ? 0 : n, &from);
}

void *bzy_udp_receive_timeout(void *u, int64_t ms)
{
	char buf[65536];
	struct sockaddr_in from;
	int n = udp_recv(u, buf, sizeof(buf), ms, &from);
	if (n == -2)
	{
		return NULL;   /* Timed out. */
	}

	return make_dgram(buf, n < 0 ? 0 : n, &from);
}

void *bzy_udp_try_receive(void *u)
{
	char buf[65536];
	struct sockaddr_in from;
	socklen_t fromlen = sizeof(from);
	memset(&from, 0, sizeof(from));
	ssize_t n = recvfrom((int)U_FD(u), buf, sizeof(buf), 0, (struct sockaddr*)&from, &fromlen);
	if (n < 0)
	{
		return NULL;   /* EAGAIN: no datagram queued. */
	}

	return make_dgram(buf, (int)n, &from);
}

void bzy_udp_close(void *u)
{
	if (!U_CLOSED(u) && U_FD(u) >= 0)
	{
		close((int)U_FD(u));
		U_CLOSED(u) = 1;
	}
}

void *bzy_dgram_data(void *d)
{
	bzy_retain(DG_DATA(d));
	return DG_DATA(d);
}

void *bzy_dgram_text(void *d)
{
	void *arr = DG_DATA(d);
	int64_t n = bzy_array_len(arr);
	const char *buf = (const char*)arr + 32;   /* Packed bytes, no copy. */
	return bzy_str_new(buf, n);
}

void *bzy_dgram_host(void *d)
{
	bzy_retain(DG_HOST(d));
	return DG_HOST(d);
}

int64_t bzy_dgram_port(void *d)
{
	return DG_PORT(d);
}

#endif
