/* DTLS over UDP via dynamically-loaded OpenSSL with memory BIOs pumped through the
   reactor. Own TU: a program that calls no Network.dtls* links none of this, and
   OpenSSL is dlopen'd lazily so there is no link-time dependency.

   This mirrors tls.c almost verbatim -- the SSL state machine is identical; the
   transport is a connected UDP socket (send/recv to one peer) instead of a TCP
   Socket, and three things are genuinely new: (1) a fixed link MTU, because a
   memory BIO cannot answer a path-MTU query; (2) a handshake retransmission branch
   in the pump, because UDP does not guarantee delivery; (3) per-peer demultiplexing
   in the server, because one UDP socket carries every peer's datagrams.

   The ossl table + loader are duplicated from tls.c on purpose: own-TU no-bloat
   requires that a DTLS-only program not drag in the TLS TU. */
#include "breezy.h"
#include "network_internal.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#ifndef _WIN32
  #include <dlfcn.h>
  #include <sys/time.h>   /* struct timeval (DTLS retransmit deadline). */
  #include "pollstate.h"
#endif

/* OpenSSL handles are opaque to us. */
#define BZ_SSL_ERROR_WANT_READ  2
#define BZ_SSL_ERROR_WANT_WRITE 3
#define BZ_SSL_FILETYPE_PEM     1
#define BZ_SSL_VERIFY_PEER      1
#define BZ_X509_V_OK            0
#define BZ_SSL_CTRL_OPTIONS             32   /* SSL_ctrl: OR in option bits. */
#define BZ_SSL_CTRL_SET_TLSEXT_HOSTNAME 55
#define BZ_TLSEXT_NAMETYPE_host_name    0
#define BZ_DTLS_CTRL_GET_TIMEOUT        73   /* SSL_ctrl: fill a struct timeval; 1 if pending. */
#define BZ_DTLS_CTRL_SET_LINK_MTU       120  /* SSL_ctrl: pin the link MTU. */
#define BZ_SSL_OP_NO_QUERY_MTU          0x1000L  /* Do not ask the BIO for the path MTU. */
#define BZ_DTLS_LINK_MTU                1200 /* Conservative IPv6-safe datagram size. */

/* Dynamically-resolved OpenSSL entry points (DTLS subset of the tls.c table). */
static struct
{
	int loaded;            /* 0 = not tried, 1 = ok, -1 = failed. */
	void *(*CTX_new)(const void *method);
	const void *(*DTLS_client_method)(void);
	const void *(*DTLS_server_method)(void);
	void  (*CTX_free)(void *ctx);
	void  (*CTX_set_verify)(void *ctx, int mode, void *cb);
	int   (*CTX_load_verify_locations)(void *ctx, const char *caFile, const char *caPath);
	int   (*CTX_set_default_verify_paths)(void *ctx);
	int   (*CTX_use_certificate_chain_file)(void *ctx, const char *file);
	int   (*CTX_use_PrivateKey_file)(void *ctx, const char *file, int type);
	void *(*SSL_new)(void *ctx);
	void  (*SSL_free)(void *ssl);
	void  (*SSL_set_bio)(void *ssl, void *rbio, void *wbio);
	void *(*BIO_new)(const void *method);
	const void *(*BIO_s_mem)(void);
	int   (*BIO_read)(void *bio, void *buf, int len);
	int   (*BIO_write)(void *bio, const void *buf, int len);
	void  (*SSL_set_connect_state)(void *ssl);
	void  (*SSL_set_accept_state)(void *ssl);
	int   (*SSL_do_handshake)(void *ssl);
	int   (*SSL_read)(void *ssl, void *buf, int num);
	int   (*SSL_write)(void *ssl, const void *buf, int num);
	int   (*SSL_get_error)(const void *ssl, int ret);
	long  (*SSL_get_verify_result)(const void *ssl);
	int   (*SSL_set1_host)(void *ssl, const char *host);
	long  (*SSL_ctrl)(void *ssl, int cmd, long larg, void *parg);
	int   (*SSL_shutdown)(void *ssl);
	void *(*SSL_get_rbio)(const void *ssl);
	void *(*SSL_get_wbio)(const void *ssl);
	long  (*DTLSv1_handle_timeout)(void *ssl);   /* Real fn (not a macro): re-queue the lost flight. */
} ossl;

/* Minimal dl helpers (mirrors tls.c; kept local for own-TU). */
#ifdef _WIN32
static void *dl_open_first(const char **names)
{
	for (int i = 0; names[i]; i++)
	{
		HMODULE h = LoadLibraryA(names[i]);
		if (h) { return (void*)h; }
	}
	return NULL;
}
static void *dl_sym(void *h, const char *n) { return (void*)GetProcAddress((HMODULE)h, n); }
#else
static void *dl_open_first(const char **names)
{
	for (int i = 0; names[i]; i++)
	{
		void *h = dlopen(names[i], RTLD_NOW | RTLD_GLOBAL);
		if (h) { return h; }
	}
	return NULL;
}
static void *dl_sym(void *h, const char *n) { return dlsym(h, n); }
#endif

/* Resolve libssl (+ libcrypto transitively) once. Returns 1 on success, 0 on
   failure (sets the io-error). */
static int dtls_load(void)
{
	if (ossl.loaded == 1) { return 1; }
	if (ossl.loaded == -1) { bzy_io_fail("DTLS unavailable: OpenSSL not found."); return 0; }

#ifdef _WIN32
	const char *sslnames[]    = { "libssl-3-x64.dll", "libssl-3.dll", "libssl.dll", NULL };
	const char *cryptonames[] = { "libcrypto-3-x64.dll", "libcrypto-3.dll", "libcrypto.dll", NULL };
#else
	const char *sslnames[]    = { "libssl.so.3", "libssl.so", "libssl.so.1.1", NULL };
	const char *cryptonames[] = { "libcrypto.so.3", "libcrypto.so", "libcrypto.so.1.1", NULL };
#endif
	void *h = dl_open_first(sslnames);
	void *hc = dl_open_first(cryptonames);
	if (!h || !hc) { ossl.loaded = -1; bzy_io_fail("DTLS unavailable: OpenSSL not found."); return 0; }

	/* SSL_* / SSL_CTX_* / DTLS_*_method live in libssl; BIO_* live in libcrypto. */
	#define SYM(field, name) do { \
		*(void**)(&ossl.field) = dl_sym(h, name); \
		if (!ossl.field) { ossl.loaded = -1; bzy_io_fail("DTLS unavailable: OpenSSL symbol missing."); return 0; } \
	} while (0)
	#define SYMC(field, name) do { \
		*(void**)(&ossl.field) = dl_sym(hc, name); \
		if (!ossl.field) { ossl.loaded = -1; bzy_io_fail("DTLS unavailable: OpenSSL symbol missing."); return 0; } \
	} while (0)
	SYM(CTX_new, "SSL_CTX_new");
	SYM(DTLS_client_method, "DTLS_client_method");
	SYM(DTLS_server_method, "DTLS_server_method");
	SYM(CTX_free, "SSL_CTX_free");
	SYM(CTX_set_verify, "SSL_CTX_set_verify");
	SYM(CTX_load_verify_locations, "SSL_CTX_load_verify_locations");
	SYM(CTX_set_default_verify_paths, "SSL_CTX_set_default_verify_paths");
	SYM(CTX_use_certificate_chain_file, "SSL_CTX_use_certificate_chain_file");
	SYM(CTX_use_PrivateKey_file, "SSL_CTX_use_PrivateKey_file");
	SYM(SSL_new, "SSL_new");
	SYM(SSL_free, "SSL_free");
	SYM(SSL_set_bio, "SSL_set_bio");
	SYMC(BIO_new, "BIO_new");
	SYMC(BIO_s_mem, "BIO_s_mem");
	SYMC(BIO_read, "BIO_read");
	SYMC(BIO_write, "BIO_write");
	SYM(SSL_set_connect_state, "SSL_set_connect_state");
	SYM(SSL_set_accept_state, "SSL_set_accept_state");
	SYM(SSL_do_handshake, "SSL_do_handshake");
	SYM(SSL_read, "SSL_read");
	SYM(SSL_write, "SSL_write");
	SYM(SSL_get_error, "SSL_get_error");
	SYM(SSL_get_verify_result, "SSL_get_verify_result");
	SYM(SSL_set1_host, "SSL_set1_host");
	SYM(SSL_ctrl, "SSL_ctrl");
	SYM(SSL_shutdown, "SSL_shutdown");
	SYM(SSL_get_rbio, "SSL_get_rbio");
	SYM(SSL_get_wbio, "SSL_get_wbio");
	SYM(DTLSv1_handle_timeout, "DTLSv1_handle_timeout");
	#undef SYM
	#undef SYMC

	ossl.loaded = 1;
	return 1;
}

/* ---- DtlsSocket layout (object_size = 56), identical to TlsSocket:
   0 vtable | 8 rc | 16 gcinfo | 24 transport(Socket*) | 32 SSL* | 40 SSL_CTX*
   ctx_owned (NULL = shared, owned by the listener) | 48 closed(int64). ---- */
#define DTS_TRANSPORT(o) (*(void**)((char*)(o) + 24))
#define DTS_SSL(o)       (*(void**)((char*)(o) + 32))
#define DTS_CTXOWN(o)    (*(void**)((char*)(o) + 40))
#define DTS_CLOSED(o)    (*(int64_t*)((char*)(o) + 48))

static int64_t g_dtls_sock_typeinfo[2] = { 0, 0 };
static int64_t g_dtls_sock_vtable[2];

static void dtls_sock_finalize(void *o)
{
	if (DTS_CLOSED(o)) { return; }
	if (DTS_SSL(o)) { ossl.SSL_free(DTS_SSL(o)); DTS_SSL(o) = NULL; }
	if (DTS_CTXOWN(o)) { ossl.CTX_free(DTS_CTXOWN(o)); DTS_CTXOWN(o) = NULL; }
	if (DTS_TRANSPORT(o)) { bzy_release(DTS_TRANSPORT(o)); DTS_TRANSPORT(o) = NULL; }
	DTS_CLOSED(o) = 1;
}

static void *dtls_sock_vtable(void)
{
	g_dtls_sock_typeinfo[0] = (int64_t)(void*)dtls_sock_finalize;
	g_dtls_sock_vtable[0] = (int64_t)&g_dtls_sock_typeinfo[0];
	return &g_dtls_sock_vtable[1];
}

/* Owned byte[] of n bytes copied from buf (mirrors socket.c bytes_to_array). */
static void *dtls_bytes_to_array(const char *buf, int n)
{
	void *arr = bzy_array_new_sized(n < 0 ? 0 : n, 1, 0);   /* Packed byte[]. */
	if (n > 0)
	{
		memcpy((char*)arr + 32, buf, (size_t)n);
	}

	return arr;
}

/* ---- The memory-BIO pump. Ciphertext moves over the connected UDP transport. ---- */

/* Drain pending ciphertext from the SSL's wbio to the network. 0 ok, -1 error. */
static int dtls_flush(void *s)
{
	void *wbio = ossl.SSL_get_wbio(DTS_SSL(s));
	char buf[4096];
	for (;;)
	{
		int n = ossl.BIO_read(wbio, buf, (int)sizeof(buf));
		if (n <= 0) { return 0; }                         /* Nothing pending (mem BIO). */
		if (bzy_sock_send_all(DTS_TRANSPORT(s), buf, n) < 0) { return -1; }
	}
}

/* Read one ciphertext datagram from the network into the SSL's rbio. timeout_ms
   bounds the wait (the DTLS retransmit deadline during a handshake; -1 otherwise).
   Returns: 1 ok, 0 EOF, -1 error, -2 timeout. */
static int dtls_feed(void *s, int64_t timeout_ms)
{
	void *rbio = ossl.SSL_get_rbio(DTS_SSL(s));
	char buf[4096];
	int n = bzy_sock_recv(DTS_TRANSPORT(s), buf, (int)sizeof(buf), timeout_ms);
	if (n == -2) { return -2; }
	if (n < 0) { return -1; }
	if (n == 0) { return 0; }
	if (ossl.BIO_write(rbio, buf, n) != n) { return -1; }
	return 1;
}

/* The DTLS retransmit deadline in ms, or -1 if none is pending. */
static int64_t dtls_timeout_ms(void *ssl)
{
	struct timeval tv;
	memset(&tv, 0, sizeof(tv));
	if (ossl.SSL_ctrl(ssl, BZ_DTLS_CTRL_GET_TIMEOUT, 0, &tv) != 1) { return -1; }
	return (int64_t)tv.tv_sec * 1000 + (int64_t)tv.tv_usec / 1000;
}

/* Run an SSL op to completion, pumping ciphertext both ways and retrying after each
   round. op_kind: 0 = handshake, 1 = read, 2 = write. For read/write buf/len are the
   plaintext buffer. Returns: handshake -> 1 ok / -1 fail; read -> bytes (0 EOF, -1
   err); write -> bytes written (-1 err). Unlike the TLS pump, a read-park during the
   handshake is bounded by the DTLS retransmit deadline: on expiry the lost flight is
   re-queued and re-sent, NOT treated as EOF. */
static int dtls_run(void *s, int op_kind, void *buf, int len)
{
	void *ssl = DTS_SSL(s);
	for (;;)
	{
		int ret;
		if (op_kind == 0) { ret = ossl.SSL_do_handshake(ssl); }
		else if (op_kind == 1) { ret = ossl.SSL_read(ssl, buf, len); }
		else { ret = ossl.SSL_write(ssl, buf, len); }

		if (op_kind == 0 && ret == 1) { if (dtls_flush(s) < 0) { return -1; } return 1; }
		if (op_kind == 1 && ret > 0) { return ret; }
		if (op_kind == 2 && ret > 0) { if (dtls_flush(s) < 0) { return -1; } return ret; }

		int err = ossl.SSL_get_error(ssl, ret);
		if (err != BZ_SSL_ERROR_WANT_READ && err != BZ_SSL_ERROR_WANT_WRITE)
		{
			if (op_kind == 1 && ret == 0) { return 0; }   /* Clean EOF on read. */
			return -1;                                    /* Fatal. */
		}
		if (dtls_flush(s) < 0) { return -1; }             /* Always push pending bytes first. */
		if (err == BZ_SSL_ERROR_WANT_READ)
		{
			int64_t to = (op_kind == 0) ? dtls_timeout_ms(ssl) : -1;
			int f = dtls_feed(s, to);
			if (f == -2)
			{
				/* Retransmit deadline expired: re-queue the lost flight and re-send. */
				ossl.DTLSv1_handle_timeout(ssl);
				if (dtls_flush(s) < 0) { return -1; }
				continue;
			}
			if (f <= 0) { return -1; }                    /* Peer closed mid-op. */
		}
		/* Loop: retry the SSL op. */
	}
}

/* Allocate a DtlsSocket over an owned transport Socket + a fresh SSL. ctx_own is the
   SSL_CTX to free on close (client) or NULL (server shares the listener's). */
static void *dtls_sock_wrap(void *transport, void *ssl, void *ctx_own)
{
	void *o = bzy_alloc(56);
	*(void**)o = dtls_sock_vtable();
	DTS_TRANSPORT(o) = transport;
	DTS_SSL(o) = ssl;
	DTS_CTXOWN(o) = ctx_own;
	DTS_CLOSED(o) = 0;
	bzy_share_crosscore(o);
	return o;
}

/* Attach fresh memory BIOs to ssl. Returns 0 ok, -1 on alloc failure. */
static int dtls_attach_bios(void *ssl)
{
	void *rbio = ossl.BIO_new(ossl.BIO_s_mem());
	void *wbio = ossl.BIO_new(ossl.BIO_s_mem());
	if (!rbio || !wbio) { return -1; }
	ossl.SSL_set_bio(ssl, rbio, wbio);
	return 0;
}

/* A memory BIO cannot answer a path-MTU query, so suppress the query and pin a
   fixed conservative link MTU. Without this OpenSSL faults or emits oversized
   records. */
static void dtls_set_mtu(void *ssl)
{
	ossl.SSL_ctrl(ssl, BZ_SSL_CTRL_OPTIONS, BZ_SSL_OP_NO_QUERY_MTU, NULL);
	ossl.SSL_ctrl(ssl, BZ_DTLS_CTRL_SET_LINK_MTU, BZ_DTLS_LINK_MTU, NULL);
}

static void *dtls_connect_impl(void *host, int64_t port, void *caBundle, int insecure)
{
	if (!dtls_load()) { return NULL; }

	void *ctx = ossl.CTX_new(ossl.DTLS_client_method());
	if (!ctx) { bzy_io_fail("Network.dtlsConnect: SSL_CTX_new failed."); return NULL; }
	if (!insecure)
	{
		if (caBundle) { ossl.CTX_load_verify_locations(ctx, bzy_str_data(caBundle), NULL); }
		else { ossl.CTX_set_default_verify_paths(ctx); }
		ossl.CTX_set_verify(ctx, BZ_SSL_VERIFY_PEER, NULL);
	}

	void *transport = bzy_udp_connect(host, port);   /* Owned, connected UDP Socket. */
	if (!transport) { ossl.CTX_free(ctx); bzy_io_fail("Network.dtlsConnect: UDP connect failed."); return NULL; }

	void *ssl = ossl.SSL_new(ctx);
	if (!ssl || dtls_attach_bios(ssl) != 0)
	{
		if (ssl) { ossl.SSL_free(ssl); }
		bzy_release(transport); ossl.CTX_free(ctx);
		bzy_io_fail("Network.dtlsConnect: SSL setup failed."); return NULL;
	}
	dtls_set_mtu(ssl);
	if (!insecure)
	{
		ossl.SSL_set1_host(ssl, bzy_str_data(host));                                                 /* Verify hostname. */
		ossl.SSL_ctrl(ssl, BZ_SSL_CTRL_SET_TLSEXT_HOSTNAME, BZ_TLSEXT_NAMETYPE_host_name, (void*)bzy_str_data(host)); /* SNI. */
	}
	ossl.SSL_set_connect_state(ssl);

	void *s = dtls_sock_wrap(transport, ssl, ctx);
	if (dtls_run(s, 0, NULL, 0) != 1)
	{
		bzy_io_fail("Network.dtlsConnect: handshake failed.");   /* Finalizer cleans up; io_check throws. */
		return s;
	}
	if (!insecure && ossl.SSL_get_verify_result(ssl) != BZ_X509_V_OK)
	{
		bzy_io_fail("Network.dtlsConnect: certificate verification failed.");
	}
	return s;
}

void *bzy_dtls_connect_ca(void *host, int64_t port, void *caBundle)
{
	return dtls_connect_impl(host, port, caBundle, 0);
}

void *bzy_dtls_connect(void *host, int64_t port)
{
	return dtls_connect_impl(host, port, NULL, 0);
}

void *bzy_dtls_connect_insecure(void *host, int64_t port)
{
	return dtls_connect_impl(host, port, NULL, 1);
}

void *bzy_dtls_read(void *s, int64_t maxbytes)
{
	if (DTS_CLOSED(s)) { bzy_io_fail("DtlsSocket.read: socket is closed."); return NULL; }
	int max = (int)(maxbytes > 0 ? maxbytes : 1);
	/* Stage the plaintext on the stack for the common record; only a rare oversized
	   read touches the heap. */
	char stackbuf[16384];
	char *buf = (max <= (int)sizeof(stackbuf)) ? stackbuf : (char*)malloc((size_t)max);
	if (!buf) { bzy_io_fail("DtlsSocket.read: out of memory."); return NULL; }
	int n = dtls_run(s, 1, buf, max);
	if (n < 0)
	{
		if (buf != stackbuf) { free(buf); }
		bzy_io_fail("DtlsSocket.read: DTLS read error."); return NULL;
	}
	void *arr = dtls_bytes_to_array(buf, n);   /* n == 0 -> empty byte[] (clean shutdown). */
	if (buf != stackbuf) { free(buf); }
	return arr;
}

int64_t bzy_dtls_write(void *s, void *data)
{
	if (DTS_CLOSED(s)) { bzy_io_fail("DtlsSocket.write: socket is closed."); return 0; }
	int len = (int)bzy_array_len(data);
	const char *bytes = (const char*)data + 32;   /* Packed byte[] payload. */
	if (len == 0) { return 0; }
	int n = dtls_run(s, 2, (void*)bytes, len);
	if (n < 0) { bzy_io_fail("DtlsSocket.write: DTLS write error."); return 0; }
	return n;
}

void bzy_dtls_close(void *s)
{
	if (DTS_CLOSED(s)) { return; }
	if (DTS_SSL(s)) { ossl.SSL_shutdown(DTS_SSL(s)); dtls_flush(s); }
	dtls_sock_finalize(s);
}

#ifndef _WIN32
/* ===== POSIX server: connected-socket-per-peer demultiplexing. ===== */

/* ---- DtlsListener layout (object_size = 48 + PollDesc):
   0 vtable | 8 rc | 16 gcinfo | 24 discovery fd(int64) | 32 SSL_CTX* | 40 closed |
   48 PollDesc. The discovery fd is a wildcard-bound UDP socket used only to learn a
   new peer's address; each accepted peer then gets its own connected socket. ---- */
#define DTL_FD(o)      (*(int64_t*)((char*)(o) + 24))
#define DTL_CTX(o)     (*(void**)((char*)(o) + 32))
#define DTL_CLOSED(o)  (*(int64_t*)((char*)(o) + 40))
#define DTL_POLL(o)    ((PollDesc*)((char*)(o) + 48))
#define DTL_OBJSIZE    (48 + (int)sizeof(PollDesc))

static int64_t g_dtls_list_typeinfo[2] = { 0, 0 };
static int64_t g_dtls_list_vtable[2];

static void dtls_list_finalize(void *o)
{
	if (DTL_CLOSED(o)) { return; }
	if (DTL_CTX(o)) { ossl.CTX_free(DTL_CTX(o)); DTL_CTX(o) = NULL; }
	if (DTL_FD(o) >= 0) { close((int)DTL_FD(o)); DTL_FD(o) = -1; }
	DTL_CLOSED(o) = 1;
}

static void *dtls_list_vtable(void)
{
	g_dtls_list_typeinfo[0] = (int64_t)(void*)dtls_list_finalize;
	g_dtls_list_vtable[0] = (int64_t)&g_dtls_list_typeinfo[0];
	return &g_dtls_list_vtable[1];
}

void *bzy_dtls_listen(int64_t port, void *certPath, void *keyPath)
{
	if (!dtls_load()) { return NULL; }

	void *ctx = ossl.CTX_new(ossl.DTLS_server_method());
	if (!ctx) { bzy_io_fail("Network.dtlsListen: SSL_CTX_new failed."); return NULL; }
	if (ossl.CTX_use_certificate_chain_file(ctx, bzy_str_data(certPath)) != 1)
	{
		ossl.CTX_free(ctx); bzy_io_fail("Network.dtlsListen: cannot load certificate chain."); return NULL;
	}
	if (ossl.CTX_use_PrivateKey_file(ctx, bzy_str_data(keyPath), BZ_SSL_FILETYPE_PEM) != 1)
	{
		ossl.CTX_free(ctx); bzy_io_fail("Network.dtlsListen: cannot load private key."); return NULL;
	}

	bzy_reactor_ensure();
	int fd = socket(AF_INET6, SOCK_DGRAM | SOCK_NONBLOCK, 0);
	if (fd < 0) { ossl.CTX_free(ctx); bzy_io_fail("Network.dtlsListen: socket failed."); return NULL; }
	int v6only = 0;   /* Dual-stack: discover IPv6 and IPv4 (v4-mapped) peers. */
	setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &v6only, sizeof(v6only));
	int yes = 1;
	setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
#ifdef SO_REUSEPORT
	setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &yes, sizeof(yes));   /* Per-peer sockets re-bind this port. */
#endif

	struct sockaddr_in6 addr;
	memset(&addr, 0, sizeof(addr));
	addr.sin6_family = AF_INET6;
	addr.sin6_addr = in6addr_any;
	addr.sin6_port = htons((unsigned short)port);
	if (bind(fd, (struct sockaddr*)&addr, sizeof(addr)) != 0)
	{
		close(fd); ossl.CTX_free(ctx); bzy_io_fail("Network.dtlsListen: bind failed."); return NULL;
	}

	void *o = bzy_alloc(DTL_OBJSIZE);
	*(void**)o = dtls_list_vtable();
	DTL_FD(o) = fd;
	DTL_CTX(o) = ctx;
	DTL_CLOSED(o) = 0;
	bzy_poll_init(DTL_POLL(o));
	bzy_share_crosscore(o);
	return o;
}

void *bzy_dtls_accept(void *l)
{
	int fd = (int)DTL_FD(l);
	PollDesc *pd = DTL_POLL(l);

	/* 1. Discover the next peer: peek one datagram (the ClientHello) on the wildcard
	   fd, learning the peer sockaddr. */
	char hello[16384];
	struct sockaddr_storage peer;
	socklen_t plen;
	int n;
	for (;;)
	{
		bzy_poll_reset(pd, BZY_POLL_READ);
		plen = sizeof(peer);
		memset(&peer, 0, sizeof(peer));
		n = (int)recvfrom(fd, hello, sizeof(hello), 0, (struct sockaddr*)&peer, &plen);
		if (n >= 0) { break; }
		if (errno != EAGAIN && errno != EWOULDBLOCK)
		{
			bzy_io_fail("DtlsListener.accept: recvfrom failed."); return NULL;
		}
		if (bzy_poll_wait(pd, fd, BZY_POLL_READ, -1) < 0)
		{
			bzy_io_fail("DtlsListener.accept: wait failed."); return NULL;
		}
	}

	/* 2. Open a per-peer socket bound to the same local port and connected to the
	   peer; the more-specific 4-tuple wins over the wildcard, so this peer's later
	   datagrams route here. */
	struct sockaddr_storage local;
	socklen_t llen = sizeof(local);
	getsockname(fd, (struct sockaddr*)&local, &llen);

	int cfd = socket(local.ss_family, SOCK_DGRAM | SOCK_NONBLOCK, 0);
	if (cfd < 0) { bzy_io_fail("DtlsListener.accept: socket failed."); return NULL; }
	if (local.ss_family == AF_INET6)
	{
		int v6only = 0;
		setsockopt(cfd, IPPROTO_IPV6, IPV6_V6ONLY, &v6only, sizeof(v6only));
	}
	int yes = 1;
	setsockopt(cfd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
#ifdef SO_REUSEPORT
	setsockopt(cfd, SOL_SOCKET, SO_REUSEPORT, &yes, sizeof(yes));
#endif
	if (bind(cfd, (struct sockaddr*)&local, llen) != 0 || connect(cfd, (struct sockaddr*)&peer, plen) != 0)
	{
		close(cfd); bzy_io_fail("DtlsListener.accept: per-peer bind/connect failed."); return NULL;
	}

	void *transport = bzy_sock_wrap(cfd);   /* Owned, reactor-registered on first park. */
	void *ssl = ossl.SSL_new(DTL_CTX(l));
	if (!ssl || dtls_attach_bios(ssl) != 0)
	{
		if (ssl) { ossl.SSL_free(ssl); }
		bzy_release(transport);
		bzy_io_fail("DtlsListener.accept: SSL setup failed."); return NULL;
	}
	dtls_set_mtu(ssl);
	ossl.SSL_set_accept_state(ssl);

	/* 3. Feed the peeked ClientHello into the rbio, then run the server handshake on
	   the connected transport via the retransmitting pump. */
	if (n > 0) { ossl.BIO_write(ossl.SSL_get_rbio(ssl), hello, n); }

	void *s = dtls_sock_wrap(transport, ssl, NULL);   /* Server conn shares the listener ctx. */
	if (dtls_run(s, 0, NULL, 0) != 1)
	{
		bzy_io_fail("DtlsListener.accept: handshake failed.");   /* Finalizer cleans up. */
	}
	return s;
}

int64_t bzy_dtls_listener_port(void *l)
{
	struct sockaddr_in6 addr;
	socklen_t len = sizeof(addr);
	if (getsockname((int)DTL_FD(l), (struct sockaddr*)&addr, &len) != 0)
	{
		return -1;
	}

	return (int64_t)ntohs(addr.sin6_port);
}

void bzy_dtls_close_listener(void *l)
{
	dtls_list_finalize(l);
}

#else
/* ===== Windows: DTLS server is a Stage-2 item (SO_REUSEPORT 4-tuple routing is
   unreliable). The client path above works on Windows; these loudly fail so the
   absence is never a silent pass. ===== */
void *bzy_dtls_listen(int64_t port, void *certPath, void *keyPath)
{
	(void)port; (void)certPath; (void)keyPath;
	bzy_io_fail("Network.dtlsListen: the DTLS server is POSIX-only in this release (Windows is Stage 2).");
	return NULL;
}

void *bzy_dtls_accept(void *l)
{
	(void)l;
	bzy_io_fail("DtlsListener.accept: the DTLS server is POSIX-only in this release.");
	return NULL;
}

int64_t bzy_dtls_listener_port(void *l)
{
	(void)l;
	return -1;
}

void bzy_dtls_close_listener(void *l)
{
	(void)l;
}
#endif
