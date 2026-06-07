#include "breezy.h"
#include "network_internal.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>       /* snprintf. */

#ifdef _WIN32
#include <mswsock.h>     /* AcceptEx / ConnectEx / GetAcceptExSockaddrs. */

/* Socket/Listener share this layout (object_size = 40):
   0 vtable | 8 rc | 16 gcinfo | 24 SOCKET fd | 32 closed(int64). Leaf object
   (num_obj_fields = 0); the finalizer closesocket()s an unclosed fd. */
#define SK_FD(o)     (*(SOCKET*)((char*)(o) + 24))
#define SK_CLOSED(o) (*(int64_t*)((char*)(o) + 32))

static void bzy_socket_finalize(void *o)
{
	if (!SK_CLOSED(o) && SK_FD(o) != INVALID_SOCKET)
	{
		closesocket(SK_FD(o));
		SK_CLOSED(o) = 1;
	}
}

static int64_t g_sock_typeinfo[2] = { 0 /* Finalizer (set on first use). */, 0 };
static int64_t g_sock_vtable[2];

static void *sock_vtable(void)
{
	g_sock_typeinfo[0] = (int64_t)(void*)bzy_socket_finalize;
	g_sock_vtable[0] = (int64_t)&g_sock_typeinfo[0];
	return &g_sock_vtable[1];
}

/* Allocate a managed socket-handle object wrapping fd. Shared with udp.c. */
void *bzy_sock_wrap(SOCKET fd)
{
	void *o = bzy_alloc(40);
	*(void**)o = sock_vtable();
	SK_FD(o) = fd;
	SK_CLOSED(o) = 0;
	/* A socket/listener handle is routinely handed to a per-connection breeze on
	   another worker (accept -> spawn handle), so its refcount must be atomic. Mark
	   it SHARED at birth - the object is still thread-confined here, so the OR cannot
	   race, the same fix channels use. Without this the README's headline accept/
	   spawn server pattern races the refcount across cores and crashes under load. */
	bzy_share_crosscore(o);

	/* Disable Nagle on stream sockets. A request/response breeze does tiny writes,
	   and Nagle holds each small segment behind the previous one's ACK - on a
	   ping-pong that interacts with delayed-ACK for ~ms per round-trip. Go sets
	   TCP_NODELAY by default; match it so latency is not paid in buffering. UDP
	   shares this wrapper, so gate on the socket type. */
	{
		int stype = 0;
		int slen = (int)sizeof(stype);
		int one = 1;
		if (getsockopt(fd, SOL_SOCKET, SO_TYPE, (char*)&stype, &slen) == 0 && stype == SOCK_STREAM)
		{
			setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, (const char*)&one, sizeof(one));
		}
	}
	return o;
}

/* Resolve host:port (IPv4) into a sockaddr_in. Returns 0 on success. Shared with udp.c. */
int bzy_resolve4(const char *host, int port, struct sockaddr_in *out)
{
	memset(out, 0, sizeof(*out));
	out->sin_family = AF_INET;
	out->sin_port = htons((unsigned short)port);

	struct addrinfo hints, *res = NULL;
	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_INET;
	hints.ai_socktype = SOCK_STREAM;
	char portstr[16];
	snprintf(portstr, sizeof(portstr), "%d", port);
	if (getaddrinfo(host, portstr, &hints, &res) != 0 || !res)
	{
		return -1;
	}

	memcpy(out, res->ai_addr, sizeof(struct sockaddr_in));
	freeaddrinfo(res);
	return 0;
}

/* A timeout arms this: after ms it cancels the pending overlapped op, which then
   completes with ERROR_OPERATION_ABORTED. cc lives on the caller's (breeze) stack,
   stable while parked. */
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

void *bzy_listener_new(int64_t port)
{
	bzy_iocp_ensure();
	SOCKET fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	u_long nb = 1;
	ioctlsocket(fd, FIONBIO, &nb);   /* Non-blocking: enables the synchronous try-path; harmless to overlapped I/O. */
	int yes = 1;
	setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (const char*)&yes, sizeof(yes));

	struct sockaddr_in addr;
	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = INADDR_ANY;
	addr.sin_port = htons((unsigned short)port);
	bind(fd, (struct sockaddr*)&addr, sizeof(addr));
	listen(fd, SOMAXCONN);

	bzy_iocp_associate((void*)fd);
	return bzy_sock_wrap(fd);
}

int64_t bzy_listener_port(void *l)
{
	struct sockaddr_in addr;
	int len = sizeof(addr);
	if (getsockname(SK_FD(l), (struct sockaddr*)&addr, &len) != 0)
	{
		return -1;
	}

	return (int64_t)ntohs(addr.sin_port);
}

/* AcceptEx is an extension fn; load its pointer once per process. */
static LPFN_ACCEPTEX accept_ex(SOCKET s)
{
	static LPFN_ACCEPTEX fn = NULL;
	if (!fn)
	{
		GUID guid = WSAID_ACCEPTEX;
		DWORD got = 0;
		WSAIoctl(s, SIO_GET_EXTENSION_FUNCTION_POINTER, &guid, sizeof(guid),
				 &fn, sizeof(fn), &got, NULL, NULL);
	}

	return fn;
}

void *bzy_listener_accept(void *l)
{
	SOCKET acc = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	u_long nb = 1;
	ioctlsocket(acc, FIONBIO, &nb);
	bzy_iocp_associate((void*)acc);

	/* AcceptEx writes local+remote addresses into this buffer (16 + sizeof(sockaddr_in)
	   each, per MSDN). The bytes-received slot is 0 (we accept without an initial read). */
	char addrbuf[2 * (sizeof(struct sockaddr_in) + 16)];
	char opbuf[BZY_IOCP_OP_SIZE];
	IocpOp *op = (IocpOp*)opbuf;
	bzy_iocp_op_reset(op);

	DWORD got = 0;
	BOOL ok = accept_ex(SK_FD(l))(SK_FD(l), acc, addrbuf,
								  0, sizeof(struct sockaddr_in) + 16, sizeof(struct sockaddr_in) + 16,
								  &got, (OVERLAPPED*)bzy_iocp_op_overlapped(op));
	if (!ok && WSAGetLastError() != WSA_IO_PENDING)
	{
		closesocket(acc);
		return NULL;
	}

	bzy_iocp_park(op);          /* Completion packet wakes us (posted even on immediate success). */
	if (bzy_iocp_op_err(op))
	{
		closesocket(acc);
		return NULL;
	}

	/* Inherit the listener's properties so getpeername/shutdown work on the accepted socket. */
	SOCKET lf = SK_FD(l);
	setsockopt(acc, SOL_SOCKET, SO_UPDATE_ACCEPT_CONTEXT, (const char*)&lf, sizeof(lf));
	return bzy_sock_wrap(acc);
}

void *bzy_listener_try_accept(void *l)
{
	SOCKET acc = accept(SK_FD(l), NULL, NULL);   /* Non-blocking listener. */
	if (acc == INVALID_SOCKET)
	{
		return NULL;   /* WSAEWOULDBLOCK: no pending connection. */
	}

	bzy_iocp_associate((void*)acc);
	u_long nb = 1;
	ioctlsocket(acc, FIONBIO, &nb);
	return bzy_sock_wrap(acc);
}

void *bzy_listener_accept_timeout(void *l, int64_t ms)
{
	SOCKET acc = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	u_long nb = 1;
	ioctlsocket(acc, FIONBIO, &nb);
	bzy_iocp_associate((void*)acc);

	char addrbuf[2 * (sizeof(struct sockaddr_in) + 16)];
	char opbuf[BZY_IOCP_OP_SIZE];
	IocpOp *op = (IocpOp*)opbuf;
	bzy_iocp_op_reset(op);
	OVERLAPPED *ov = (OVERLAPPED*)bzy_iocp_op_overlapped(op);

	DWORD got = 0;
	BOOL ok = accept_ex(SK_FD(l))(SK_FD(l), acc, addrbuf,
								  0, sizeof(struct sockaddr_in) + 16, sizeof(struct sockaddr_in) + 16,
								  &got, ov);
	if (!ok && WSAGetLastError() != WSA_IO_PENDING)
	{
		closesocket(acc);
		return NULL;
	}

	HANDLE timer = NULL;
	CancelCtx cc = { SK_FD(l), ov };   /* AcceptEx is issued on the listener fd; cancel that. */
	if (ms >= 0)
	{
		CreateTimerQueueTimer(&timer, NULL, cancel_cb, &cc, (DWORD)ms, 0, WT_EXECUTEONLYONCE);
	}

	bzy_iocp_park(op);
	if (timer)
	{
		DeleteTimerQueueTimer(NULL, timer, INVALID_HANDLE_VALUE);
	}

	if (bzy_iocp_op_err(op))
	{
		closesocket(acc);
		return NULL;   /* Includes ERROR_OPERATION_ABORTED = timeout. */
	}

	SOCKET lf = SK_FD(l);
	setsockopt(acc, SOL_SOCKET, SO_UPDATE_ACCEPT_CONTEXT, (const char*)&lf, sizeof(lf));
	return bzy_sock_wrap(acc);
}

/* ConnectEx is an extension fn; load its pointer once. */
static LPFN_CONNECTEX connect_ex(SOCKET s)
{
	static LPFN_CONNECTEX fn = NULL;
	if (!fn)
	{
		GUID guid = WSAID_CONNECTEX;
		DWORD got = 0;
		WSAIoctl(s, SIO_GET_EXTENSION_FUNCTION_POINTER, &guid, sizeof(guid),
				 &fn, sizeof(fn), &got, NULL, NULL);
	}

	return fn;
}

void *bzy_socket_connect(void *host, int64_t port)
{
	bzy_iocp_ensure();
	SOCKET fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	u_long nb = 1;
	ioctlsocket(fd, FIONBIO, &nb);

	/* ConnectEx requires an explicitly bound socket. */
	struct sockaddr_in local;
	memset(&local, 0, sizeof(local));
	local.sin_family = AF_INET;
	local.sin_addr.s_addr = INADDR_ANY;
	bind(fd, (struct sockaddr*)&local, sizeof(local));
	bzy_iocp_associate((void*)fd);

	struct sockaddr_in dst;
	if (bzy_resolve4(bzy_str_data(host), (int)port, &dst) != 0)
	{
		closesocket(fd);
		return NULL;
	}

	char opbuf[BZY_IOCP_OP_SIZE];
	IocpOp *op = (IocpOp*)opbuf;
	bzy_iocp_op_reset(op);

	DWORD got = 0;
	BOOL ok = connect_ex(fd)(fd, (struct sockaddr*)&dst, sizeof(dst), NULL, 0, &got,
							 (OVERLAPPED*)bzy_iocp_op_overlapped(op));
	if (!ok && WSAGetLastError() != WSA_IO_PENDING)
	{
		closesocket(fd);
		return NULL;
	}

	bzy_iocp_park(op);
	if (bzy_iocp_op_err(op))
	{
		closesocket(fd);
		return NULL;
	}

	setsockopt(fd, SOL_SOCKET, SO_UPDATE_CONNECT_CONTEXT, NULL, 0);
	return bzy_sock_wrap(fd);
}

/* One WSARecv into buf. timeout_ms<0 = infinite. Returns bytes (0=EOF), -1 err, -2 timeout. */
static int sock_recv(void *s, char *buf, int max, int64_t timeout_ms)
{
	WSABUF wb;
	wb.buf = buf;
	wb.len = (ULONG)max;
	DWORD flags = 0, got = 0;

	char opbuf[BZY_IOCP_OP_SIZE];
	IocpOp *op = (IocpOp*)opbuf;
	bzy_iocp_op_reset(op);
	OVERLAPPED *ov = (OVERLAPPED*)bzy_iocp_op_overlapped(op);

	int rc = WSARecv(SK_FD(s), &wb, 1, &got, &flags, ov, NULL);
	if (rc != 0 && WSAGetLastError() != WSA_IO_PENDING)
	{
		return -1;
	}

	HANDLE timer = NULL;
	CancelCtx cc = { SK_FD(s), ov };
	if (timeout_ms >= 0)
	{
		CreateTimerQueueTimer(&timer, NULL, cancel_cb, &cc, (DWORD)timeout_ms, 0, WT_EXECUTEONLYONCE);
	}

	bzy_iocp_park(op);
	if (timer)
	{
		DeleteTimerQueueTimer(NULL, timer, INVALID_HANDLE_VALUE);   /* Waits for any running callback. */
	}

	int e = bzy_iocp_op_err(op);
	if (e == ERROR_OPERATION_ABORTED)
	{
		return -2;   /* Timed out (the timer cancelled the op). */
	}

	if (e)
	{
		return -1;
	}

	return (int)bzy_iocp_op_bytes(op);   /* 0 = graceful close. */
}

/* Synchronous non-blocking recv for the try-path. n>0 bytes, 0 = EOF, -1 = nothing ready/error. */
static int sock_try_recv(void *s, char *buf, int max)
{
	int n = recv(SK_FD(s), buf, max, 0);   /* Socket is non-blocking (FIONBIO). */
	if (n > 0)
	{
		return n;
	}

	if (n == 0)
	{
		return 0;   /* Peer closed. */
	}

	return -1;      /* WSAEWOULDBLOCK or error: nothing to deliver. */
}

static void *bytes_to_array(const char *buf, int n)
{
	void *arr = bzy_array_new(n, 0);     /* Value byte[]: one byte per 8-byte slot. */
	int64_t *slots = (int64_t*)((char*)arr + 32);
	for (int i = 0; i < n; i++)
	{
		slots[i] = (unsigned char)buf[i];
	}

	return arr;
}

void *bzy_socket_read(void *s, int64_t maxbytes)
{
	if (maxbytes < 1)
	{
		maxbytes = 1;
	}

	char *buf = malloc((size_t)maxbytes);
	int n = sock_recv(s, buf, (int)maxbytes, -1);   /* Base op: parks forever, never times out. */
	if (n < 0)
	{
		n = 0;   /* Surface errors as EOF (empty byte[]); a hard error model is a follow-up. */
	}

	void *arr = bytes_to_array(buf, n);
	free(buf);
	return arr;
}

void *bzy_socket_read_text(void *s, int64_t maxbytes)
{
	if (maxbytes < 1)
	{
		maxbytes = 1;
	}

	char *buf = malloc((size_t)maxbytes);
	int n = sock_recv(s, buf, (int)maxbytes, -1);
	if (n < 0)
	{
		n = 0;
	}

	void *str = bzy_str_new(buf, n);
	free(buf);
	return str;
}

void *bzy_socket_read_timeout(void *s, int64_t maxbytes, int64_t ms)
{
	if (maxbytes < 1)
	{
		maxbytes = 1;
	}

	char *buf = malloc((size_t)maxbytes);
	int n = sock_recv(s, buf, (int)maxbytes, ms);
	void *r = (n == -2) ? NULL : bytes_to_array(buf, n < 0 ? 0 : n);   /* NULL = timeout. */
	free(buf);
	return r;
}

void *bzy_socket_try_read(void *s, int64_t maxbytes)
{
	if (maxbytes < 1)
	{
		maxbytes = 1;
	}

	char *buf = malloc((size_t)maxbytes);
	int n = sock_try_recv(s, buf, (int)maxbytes);
	void *r = (n < 0) ? NULL : bytes_to_array(buf, n);   /* NULL = nothing ready; len 0 = EOF. */
	free(buf);
	return r;
}

void *bzy_socket_read_text_timeout(void *s, int64_t maxbytes, int64_t ms)
{
	if (maxbytes < 1)
	{
		maxbytes = 1;
	}

	char *buf = malloc((size_t)maxbytes);
	int n = sock_recv(s, buf, (int)maxbytes, ms);
	void *r = (n == -2) ? NULL : bzy_str_new(buf, n < 0 ? 0 : n);
	free(buf);
	return r;
}

void *bzy_socket_try_read_text(void *s, int64_t maxbytes)
{
	if (maxbytes < 1)
	{
		maxbytes = 1;
	}

	char *buf = malloc((size_t)maxbytes);
	int n = sock_try_recv(s, buf, (int)maxbytes);
	void *r = (n < 0) ? NULL : bzy_str_new(buf, n);
	free(buf);
	return r;
}

/* Write all of buf[0..len) with as many WSASend parks as needed. Returns bytes sent. */
static int64_t sock_send_all(void *s, const char *buf, int64_t len)
{
	int64_t sent = 0;
	while (sent < len)
	{
		WSABUF wb;
		wb.buf = (char*)(buf + sent);
		wb.len = (ULONG)(len - sent);
		DWORD got = 0;

		char opbuf[BZY_IOCP_OP_SIZE];
		IocpOp *op = (IocpOp*)opbuf;
		bzy_iocp_op_reset(op);

		int rc = WSASend(SK_FD(s), &wb, 1, &got, 0, (OVERLAPPED*)bzy_iocp_op_overlapped(op), NULL);
		if (rc != 0 && WSAGetLastError() != WSA_IO_PENDING)
		{
			break;
		}

		bzy_iocp_park(op);
		if (bzy_iocp_op_err(op) || bzy_iocp_op_bytes(op) == 0)
		{
			break;
		}

		sent += (int64_t)bzy_iocp_op_bytes(op);
	}

	return sent;
}

int64_t bzy_socket_write(void *s, void *data)
{
	int64_t n = bzy_array_len(data);
	int64_t *slots = (int64_t*)((char*)data + 32);
	char *buf = malloc((size_t)(n > 0 ? n : 1));
	for (int64_t i = 0; i < n; i++)
	{
		buf[i] = (char)(unsigned char)slots[i];
	}

	int64_t sent = sock_send_all(s, buf, n);
	free(buf);
	return sent;
}

int64_t bzy_socket_write_text(void *s, void *str)
{
	return sock_send_all(s, bzy_str_data(str), bzy_str_len(str));
}

void bzy_socket_close(void *s)
{
	if (!SK_CLOSED(s) && SK_FD(s) != INVALID_SOCKET)
	{
		closesocket(SK_FD(s));
		SK_CLOSED(s) = 1;
	}
}

void bzy_listener_close(void *l)
{
	bzy_socket_close(l);   /* Same layout + finalizer. */
}

#else
/* ===== POSIX: non-blocking BSD sockets on the epoll reactor. =====
   Each op loops: try the non-blocking syscall; on EAGAIN park for readiness via
   bzy_reactor_wait, then retry. The fd is stored as an int64 in the handle slot. */
#define SK_FD(o)     (*(int64_t*)((char*)(o) + 24))
#define SK_CLOSED(o) (*(int64_t*)((char*)(o) + 32))

static void bzy_socket_finalize(void *o)
{
	if (!SK_CLOSED(o) && SK_FD(o) >= 0)
	{
		close((int)SK_FD(o));
		SK_CLOSED(o) = 1;
	}
}

static int64_t g_sock_typeinfo[2] = { 0 /* Finalizer (set on first use). */, 0 };
static int64_t g_sock_vtable[2];

static void *sock_vtable(void)
{
	g_sock_typeinfo[0] = (int64_t)(void*)bzy_socket_finalize;
	g_sock_vtable[0] = (int64_t)&g_sock_typeinfo[0];
	return &g_sock_vtable[1];
}

void *bzy_sock_wrap(int fd)
{
	void *o = bzy_alloc(40);
	*(void**)o = sock_vtable();
	SK_FD(o) = fd;
	SK_CLOSED(o) = 0;
	/* A socket/listener handle is routinely handed to a per-connection breeze on
	   another worker (accept -> spawn handle), so its refcount must be atomic. Mark
	   it SHARED at birth - the object is still thread-confined here, so the OR cannot
	   race, the same fix channels use. Without this the README's headline accept/
	   spawn server pattern races the refcount across cores and crashes under load. */
	bzy_share_crosscore(o);

	/* Disable Nagle on stream sockets. A request/response breeze does tiny writes,
	   and Nagle holds each small segment behind the previous one's ACK - on a
	   ping-pong that interacts with delayed-ACK for ~ms per round-trip. Go sets
	   TCP_NODELAY by default; match it so latency is not paid in buffering. UDP
	   shares this wrapper, so gate on the socket type. */
	{
		int stype = 0;
		socklen_t slen = sizeof(stype);
		int one = 1;
		if (getsockopt(fd, SOL_SOCKET, SO_TYPE, &stype, &slen) == 0 && stype == SOCK_STREAM)
		{
			setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
		}
	}
	return o;
}

int bzy_resolve4(const char *host, int port, struct sockaddr_in *out)
{
	memset(out, 0, sizeof(*out));
	out->sin_family = AF_INET;
	out->sin_port = htons((unsigned short)port);

	struct addrinfo hints, *res = NULL;
	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_INET;
	hints.ai_socktype = SOCK_STREAM;
	char portstr[16];
	snprintf(portstr, sizeof(portstr), "%d", port);
	if (getaddrinfo(host, portstr, &hints, &res) != 0 || !res)
	{
		return -1;
	}

	memcpy(out, res->ai_addr, sizeof(struct sockaddr_in));
	freeaddrinfo(res);
	return 0;
}

void *bzy_listener_new(int64_t port)
{
	bzy_reactor_ensure();
	int fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
	if (fd < 0)
	{
		return NULL;
	}

	int yes = 1;
	setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

	struct sockaddr_in addr;
	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = INADDR_ANY;
	addr.sin_port = htons((unsigned short)port);
	if (bind(fd, (struct sockaddr*)&addr, sizeof(addr)) != 0 || listen(fd, SOMAXCONN) != 0)
	{
		close(fd);
		return NULL;
	}

	return bzy_sock_wrap(fd);
}

int64_t bzy_listener_port(void *l)
{
	struct sockaddr_in addr;
	socklen_t len = sizeof(addr);
	if (getsockname((int)SK_FD(l), (struct sockaddr*)&addr, &len) != 0)
	{
		return -1;
	}

	return (int64_t)ntohs(addr.sin_port);
}

void *bzy_listener_accept(void *l)
{
	for (;;)
	{
		int fd = accept4((int)SK_FD(l), NULL, NULL, SOCK_NONBLOCK);
		if (fd >= 0)
		{
			return bzy_sock_wrap(fd);
		}

		if (errno != EAGAIN && errno != EWOULDBLOCK)
		{
			return NULL;
		}

		if (bzy_reactor_wait((int)SK_FD(l), 0, -1) < 0)
		{
			return NULL;
		}
	}
}

void *bzy_listener_try_accept(void *l)
{
	int fd = accept4((int)SK_FD(l), NULL, NULL, SOCK_NONBLOCK);
	return (fd >= 0) ? bzy_sock_wrap(fd) : NULL;   /* NULL = nothing pending. */
}

void *bzy_listener_accept_timeout(void *l, int64_t ms)
{
	for (;;)
	{
		int fd = accept4((int)SK_FD(l), NULL, NULL, SOCK_NONBLOCK);
		if (fd >= 0)
		{
			return bzy_sock_wrap(fd);
		}

		if (errno != EAGAIN && errno != EWOULDBLOCK)
		{
			return NULL;
		}

		if (bzy_reactor_wait((int)SK_FD(l), 0, ms) <= 0)
		{
			return NULL;   /* 0 = timed out, -1 = error. */
		}
	}
}

void *bzy_socket_connect(void *host, int64_t port)
{
	bzy_reactor_ensure();
	int fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
	if (fd < 0)
	{
		return NULL;
	}

	struct sockaddr_in dst;
	if (bzy_resolve4(bzy_str_data(host), (int)port, &dst) != 0)
	{
		close(fd);
		return NULL;
	}

	if (connect(fd, (struct sockaddr*)&dst, sizeof(dst)) != 0)
	{
		if (errno != EINPROGRESS)
		{
			close(fd);
			return NULL;
		}

		if (bzy_reactor_wait(fd, 1, -1) < 0)   /* Wait until writable = connected/failed. */
		{
			close(fd);
			return NULL;
		}

		int err = 0;
		socklen_t el = sizeof(err);
		getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &el);
		if (err != 0)
		{
			close(fd);
			return NULL;
		}
	}

	return bzy_sock_wrap(fd);
}

/* One recv, parking on read-readiness. timeout_ms<0 = infinite. bytes (0=EOF), -1 err, -2 timeout. */
static int sock_recv(void *s, char *buf, int max, int64_t timeout_ms)
{
	for (;;)
	{
		ssize_t n = recv((int)SK_FD(s), buf, (size_t)max, 0);
		if (n > 0)
		{
			return (int)n;
		}

		if (n == 0)
		{
			return 0;   /* Peer closed. */
		}

		if (errno != EAGAIN && errno != EWOULDBLOCK)
		{
			return -1;
		}

		int r = bzy_reactor_wait((int)SK_FD(s), 0, timeout_ms);
		if (r == 0)
		{
			return -2;   /* Timed out. */
		}

		if (r < 0)
		{
			return -1;
		}
		/* Ready: retry the recv. */
	}
}

/* Synchronous non-blocking recv for the try-path. n>0 bytes, 0 = EOF, -1 = nothing ready/error. */
static int sock_try_recv(void *s, char *buf, int max)
{
	ssize_t n = recv((int)SK_FD(s), buf, (size_t)max, 0);
	if (n > 0)
	{
		return (int)n;
	}

	if (n == 0)
	{
		return 0;
	}

	return -1;   /* EAGAIN or error: nothing to deliver. */
}

static void *bytes_to_array(const char *buf, int n)
{
	void *arr = bzy_array_new(n, 0);     /* Value byte[]: one byte per 8-byte slot. */
	int64_t *slots = (int64_t*)((char*)arr + 32);
	for (int i = 0; i < n; i++)
	{
		slots[i] = (unsigned char)buf[i];
	}

	return arr;
}

void *bzy_socket_read(void *s, int64_t maxbytes)
{
	if (maxbytes < 1)
	{
		maxbytes = 1;
	}

	char *buf = malloc((size_t)maxbytes);
	int n = sock_recv(s, buf, (int)maxbytes, -1);
	if (n < 0)
	{
		n = 0;
	}

	void *arr = bytes_to_array(buf, n);
	free(buf);
	return arr;
}

void *bzy_socket_read_text(void *s, int64_t maxbytes)
{
	if (maxbytes < 1)
	{
		maxbytes = 1;
	}

	char *buf = malloc((size_t)maxbytes);
	int n = sock_recv(s, buf, (int)maxbytes, -1);
	if (n < 0)
	{
		n = 0;
	}

	void *str = bzy_str_new(buf, n);
	free(buf);
	return str;
}

void *bzy_socket_read_timeout(void *s, int64_t maxbytes, int64_t ms)
{
	if (maxbytes < 1)
	{
		maxbytes = 1;
	}

	char *buf = malloc((size_t)maxbytes);
	int n = sock_recv(s, buf, (int)maxbytes, ms);
	void *r = (n == -2) ? NULL : bytes_to_array(buf, n < 0 ? 0 : n);   /* NULL = timeout. */
	free(buf);
	return r;
}

void *bzy_socket_try_read(void *s, int64_t maxbytes)
{
	if (maxbytes < 1)
	{
		maxbytes = 1;
	}

	char *buf = malloc((size_t)maxbytes);
	int n = sock_try_recv(s, buf, (int)maxbytes);
	void *r = (n < 0) ? NULL : bytes_to_array(buf, n);   /* NULL = nothing ready; len 0 = EOF. */
	free(buf);
	return r;
}

void *bzy_socket_read_text_timeout(void *s, int64_t maxbytes, int64_t ms)
{
	if (maxbytes < 1)
	{
		maxbytes = 1;
	}

	char *buf = malloc((size_t)maxbytes);
	int n = sock_recv(s, buf, (int)maxbytes, ms);
	void *r = (n == -2) ? NULL : bzy_str_new(buf, n < 0 ? 0 : n);
	free(buf);
	return r;
}

void *bzy_socket_try_read_text(void *s, int64_t maxbytes)
{
	if (maxbytes < 1)
	{
		maxbytes = 1;
	}

	char *buf = malloc((size_t)maxbytes);
	int n = sock_try_recv(s, buf, (int)maxbytes);
	void *r = (n < 0) ? NULL : bzy_str_new(buf, n);
	free(buf);
	return r;
}

/* Write all of buf[0..len), parking on write-readiness as needed. Returns bytes sent. */
static int64_t sock_send_all(void *s, const char *buf, int64_t len)
{
	int64_t sent = 0;
	while (sent < len)
	{
		ssize_t n = send((int)SK_FD(s), buf + sent, (size_t)(len - sent), MSG_NOSIGNAL);
		if (n > 0)
		{
			sent += (int64_t)n;
			continue;
		}

		if (n == 0)
		{
			break;
		}

		if (errno != EAGAIN && errno != EWOULDBLOCK)
		{
			break;
		}

		if (bzy_reactor_wait((int)SK_FD(s), 1, -1) < 0)
		{
			break;
		}
	}

	return sent;
}

int64_t bzy_socket_write(void *s, void *data)
{
	int64_t n = bzy_array_len(data);
	int64_t *slots = (int64_t*)((char*)data + 32);
	char *buf = malloc((size_t)(n > 0 ? n : 1));
	for (int64_t i = 0; i < n; i++)
	{
		buf[i] = (char)(unsigned char)slots[i];
	}

	int64_t sent = sock_send_all(s, buf, n);
	free(buf);
	return sent;
}

int64_t bzy_socket_write_text(void *s, void *str)
{
	return sock_send_all(s, bzy_str_data(str), bzy_str_len(str));
}

void bzy_socket_close(void *s)
{
	if (!SK_CLOSED(s) && SK_FD(s) >= 0)
	{
		close((int)SK_FD(s));
		SK_CLOSED(s) = 1;
	}
}

void bzy_listener_close(void *l)
{
	bzy_socket_close(l);   /* Same layout + finalizer. */
}

#endif
