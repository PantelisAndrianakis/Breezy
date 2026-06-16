/* TLS over a plain Socket via dynamically-loaded OpenSSL with memory BIOs pumped
   through the reactor. Own TU: a program that calls no Network.tls* links none of
   this, and OpenSSL is dlopen'd lazily so there is no link-time dependency.

   Design: a TlsSocket reuses an ordinary Socket as its ciphertext transport (the
   plain socket already does reactor-parked recv/send), so the pump never touches
   the platform connect/accept/recv/send code directly -- it drives SSL_* over two
   memory BIOs and moves ciphertext to/from the transport with bzy_sock_recv /
   bzy_sock_send_all. The pump runs on the breeze; every ciphertext move parks via
   the transport's reactor, so TLS scales like any other socket. */
#include "breezy.h"
#include "network_internal.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#ifndef _WIN32
  #include <dlfcn.h>
#endif

/* OpenSSL handles are opaque to us. */
#define BZ_SSL_ERROR_WANT_READ  2
#define BZ_SSL_ERROR_WANT_WRITE 3
#define BZ_SSL_FILETYPE_PEM     1
#define BZ_SSL_VERIFY_PEER      1
#define BZ_X509_V_OK            0
#define BZ_SSL_CTRL_SET_TLSEXT_HOSTNAME 55
#define BZ_TLSEXT_NAMETYPE_host_name    0

/* Dynamically-resolved OpenSSL entry points. */
static struct
{
	int loaded;            /* 0 = not tried, 1 = ok, -1 = failed. */
	void *(*CTX_new)(const void *method);
	const void *(*TLS_client_method)(void);
	const void *(*TLS_server_method)(void);
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
} ossl;

/* Minimal dl helpers (mirrors runtime/desktop.c; kept local). */
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
static int tls_load(void)
{
	if (ossl.loaded == 1) { return 1; }
	if (ossl.loaded == -1) { bzy_io_fail("TLS unavailable: OpenSSL not found."); return 0; }

#ifdef _WIN32
	const char *sslnames[]    = { "libssl-3-x64.dll", "libssl-3.dll", "libssl.dll", NULL };
	const char *cryptonames[] = { "libcrypto-3-x64.dll", "libcrypto-3.dll", "libcrypto.dll", NULL };
#else
	const char *sslnames[]    = { "libssl.so.3", "libssl.so", "libssl.so.1.1", NULL };
	const char *cryptonames[] = { "libcrypto.so.3", "libcrypto.so", "libcrypto.so.1.1", NULL };
#endif
	void *h = dl_open_first(sslnames);
	void *hc = dl_open_first(cryptonames);
	if (!h || !hc) { ossl.loaded = -1; bzy_io_fail("TLS unavailable: OpenSSL not found."); return 0; }

	/* SSL_* / SSL_CTX_* / TLS_*_method live in libssl; BIO_* live in libcrypto. */
	#define SYM(field, name) do { \
		*(void**)(&ossl.field) = dl_sym(h, name); \
		if (!ossl.field) { ossl.loaded = -1; bzy_io_fail("TLS unavailable: OpenSSL symbol missing."); return 0; } \
	} while (0)
	#define SYMC(field, name) do { \
		*(void**)(&ossl.field) = dl_sym(hc, name); \
		if (!ossl.field) { ossl.loaded = -1; bzy_io_fail("TLS unavailable: OpenSSL symbol missing."); return 0; } \
	} while (0)
	SYM(CTX_new, "SSL_CTX_new");
	SYM(TLS_client_method, "TLS_client_method");
	SYM(TLS_server_method, "TLS_server_method");
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
	#undef SYM
	#undef SYMC

	ossl.loaded = 1;
	return 1;
}

/* ---- TlsSocket layout (object_size = 56):
   0 vtable | 8 rc | 16 gcinfo | 24 transport(Socket*) | 32 SSL* | 40 SSL_CTX*
   ctx_owned (NULL = shared, owned by the listener) | 48 closed(int64). Leaf:
   the finalizer frees ssl + ctx_owned and releases the transport. ---- */
#define TLS_TRANSPORT(o) (*(void**)((char*)(o) + 24))
#define TLS_SSL(o)       (*(void**)((char*)(o) + 32))
#define TLS_CTXOWN(o)    (*(void**)((char*)(o) + 40))
#define TLS_CLOSED(o)    (*(int64_t*)((char*)(o) + 48))

/* ---- TlsListener layout (object_size = 48):
   0 vtable | 8 rc | 16 gcinfo | 24 listener(Listener*) | 32 SSL_CTX* ctx |
   40 closed(int64). ---- */
#define TLL_LISTENER(o)  (*(void**)((char*)(o) + 24))
#define TLL_CTX(o)       (*(void**)((char*)(o) + 32))
#define TLL_CLOSED(o)    (*(int64_t*)((char*)(o) + 40))

static int64_t g_tls_sock_typeinfo[2] = { 0, 0 };
static int64_t g_tls_sock_vtable[2];
static int64_t g_tls_list_typeinfo[2] = { 0, 0 };
static int64_t g_tls_list_vtable[2];

static void tls_sock_finalize(void *o)
{
	if (TLS_CLOSED(o)) { return; }
	if (TLS_SSL(o)) { ossl.SSL_free(TLS_SSL(o)); TLS_SSL(o) = NULL; }
	if (TLS_CTXOWN(o)) { ossl.CTX_free(TLS_CTXOWN(o)); TLS_CTXOWN(o) = NULL; }
	if (TLS_TRANSPORT(o)) { bzy_release(TLS_TRANSPORT(o)); TLS_TRANSPORT(o) = NULL; }
	TLS_CLOSED(o) = 1;
}

static void tls_list_finalize(void *o)
{
	if (TLL_CLOSED(o)) { return; }
	if (TLL_CTX(o)) { ossl.CTX_free(TLL_CTX(o)); TLL_CTX(o) = NULL; }
	if (TLL_LISTENER(o)) { bzy_release(TLL_LISTENER(o)); TLL_LISTENER(o) = NULL; }
	TLL_CLOSED(o) = 1;
}

static void *tls_sock_vtable(void)
{
	g_tls_sock_typeinfo[0] = (int64_t)(void*)tls_sock_finalize;
	g_tls_sock_vtable[0] = (int64_t)&g_tls_sock_typeinfo[0];
	return &g_tls_sock_vtable[1];
}
static void *tls_list_vtable(void)
{
	g_tls_list_typeinfo[0] = (int64_t)(void*)tls_list_finalize;
	g_tls_list_vtable[0] = (int64_t)&g_tls_list_typeinfo[0];
	return &g_tls_list_vtable[1];
}

/* Owned byte[] of n bytes copied from buf (mirrors socket.c bytes_to_array). */
static void *tls_bytes_to_array(const char *buf, int n)
{
	void *arr = bzy_array_new_sized(n < 0 ? 0 : n, 1, 0);   /* Packed byte[]. */
	if (n > 0)
	{
		memcpy((char*)arr + 32, buf, (size_t)n);
	}

	return arr;
}

/* ---- The memory-BIO pump. Ciphertext moves over the plain transport socket. ---- */

/* Drain pending ciphertext from the SSL's wbio to the network. 0 ok, -1 error. */
static int tls_flush(void *s)
{
	void *wbio = ossl.SSL_get_wbio(TLS_SSL(s));
	char buf[4096];
	for (;;)
	{
		int n = ossl.BIO_read(wbio, buf, (int)sizeof(buf));
		if (n <= 0) { return 0; }                         /* Nothing pending (mem BIO). */
		if (bzy_sock_send_all(TLS_TRANSPORT(s), buf, n) < 0) { return -1; }
	}
}

/* Read one chunk of ciphertext from the network into the SSL's rbio.
   1 ok, 0 EOF, -1 error. */
static int tls_feed(void *s)
{
	void *rbio = ossl.SSL_get_rbio(TLS_SSL(s));
	char buf[4096];
	int n = bzy_sock_recv(TLS_TRANSPORT(s), buf, (int)sizeof(buf), -1);
	if (n < 0) { return -1; }
	if (n == 0) { return 0; }
	if (ossl.BIO_write(rbio, buf, n) != n) { return -1; }
	return 1;
}

/* Run an SSL op to true completion, pumping ciphertext both ways and retrying
   after each round. op_kind: 0 = handshake, 1 = read, 2 = write. For read/write
   buf/len are the plaintext buffer. Returns: handshake -> 1 ok / -1 fail;
   read -> bytes (0 EOF, -1 err); write -> bytes written (-1 err). */
static int tls_run(void *s, int op_kind, void *buf, int len)
{
	for (;;)
	{
		int ret;
		if (op_kind == 0) { ret = ossl.SSL_do_handshake(TLS_SSL(s)); }
		else if (op_kind == 1) { ret = ossl.SSL_read(TLS_SSL(s), buf, len); }
		else { ret = ossl.SSL_write(TLS_SSL(s), buf, len); }

		if (op_kind == 0 && ret == 1) { if (tls_flush(s) < 0) { return -1; } return 1; }
		if (op_kind == 1 && ret > 0) { return ret; }
		if (op_kind == 2 && ret > 0) { if (tls_flush(s) < 0) { return -1; } return ret; }

		int err = ossl.SSL_get_error(TLS_SSL(s), ret);
		if (err != BZ_SSL_ERROR_WANT_READ && err != BZ_SSL_ERROR_WANT_WRITE)
		{
			if (op_kind == 1 && ret == 0) { return 0; }   /* Clean EOF on read. */
			return -1;                                    /* Fatal. */
		}
		if (tls_flush(s) < 0) { return -1; }              /* Always push handshake bytes first. */
		if (err == BZ_SSL_ERROR_WANT_READ)
		{
			int f = tls_feed(s);
			if (f <= 0) { return -1; }                    /* Peer closed mid-op. */
		}
		/* Loop: retry the SSL op. */
	}
}

/* Allocate a TlsSocket over an owned transport Socket + a fresh SSL. ctx_own is
   the SSL_CTX to free on close (client) or NULL (server shares the listener's). */
static void *tls_sock_wrap(void *transport, void *ssl, void *ctx_own)
{
	void *o = bzy_alloc(56);
	*(void**)o = tls_sock_vtable();
	TLS_TRANSPORT(o) = transport;
	TLS_SSL(o) = ssl;
	TLS_CTXOWN(o) = ctx_own;
	TLS_CLOSED(o) = 0;
	bzy_share_crosscore(o);
	return o;
}

/* Attach fresh memory BIOs to ssl. Returns 0 ok, -1 on alloc failure. */
static int tls_attach_bios(void *ssl)
{
	void *rbio = ossl.BIO_new(ossl.BIO_s_mem());
	void *wbio = ossl.BIO_new(ossl.BIO_s_mem());
	if (!rbio || !wbio) { return -1; }
	ossl.SSL_set_bio(ssl, rbio, wbio);
	return 0;
}

void *bzy_tls_connect_ca(void *host, int64_t port, void *caBundle)
{
	if (!tls_load()) { return NULL; }

	void *ctx = ossl.CTX_new(ossl.TLS_client_method());
	if (!ctx) { bzy_io_fail("Network.tlsConnect: SSL_CTX_new failed."); return NULL; }
	if (caBundle) { ossl.CTX_load_verify_locations(ctx, bzy_str_data(caBundle), NULL); }
	else { ossl.CTX_set_default_verify_paths(ctx); }
	ossl.CTX_set_verify(ctx, BZ_SSL_VERIFY_PEER, NULL);

	void *transport = bzy_socket_connect(host, port);   /* Owned, reactor-registered Socket. */
	if (!transport) { ossl.CTX_free(ctx); bzy_io_fail("Network.tlsConnect: TCP connect failed."); return NULL; }

	void *ssl = ossl.SSL_new(ctx);
	if (!ssl || tls_attach_bios(ssl) != 0)
	{
		if (ssl) { ossl.SSL_free(ssl); }
		bzy_release(transport); ossl.CTX_free(ctx);
		bzy_io_fail("Network.tlsConnect: SSL setup failed."); return NULL;
	}
	ossl.SSL_set1_host(ssl, bzy_str_data(host));                                                 /* Verify hostname. */
	ossl.SSL_ctrl(ssl, BZ_SSL_CTRL_SET_TLSEXT_HOSTNAME, BZ_TLSEXT_NAMETYPE_host_name, (void*)bzy_str_data(host)); /* SNI. */
	ossl.SSL_set_connect_state(ssl);

	void *s = tls_sock_wrap(transport, ssl, ctx);
	if (tls_run(s, 0, NULL, 0) != 1)
	{
		bzy_io_fail("Network.tlsConnect: handshake failed.");   /* Finalizer cleans up; io_check throws. */
		return s;
	}
	if (ossl.SSL_get_verify_result(ssl) != BZ_X509_V_OK)
	{
		bzy_io_fail("Network.tlsConnect: certificate verification failed.");
	}
	return s;
}

void *bzy_tls_connect(void *host, int64_t port)
{
	return bzy_tls_connect_ca(host, port, NULL);
}

void *bzy_tls_listen(int64_t port, void *certPath, void *keyPath)
{
	if (!tls_load()) { return NULL; }

	void *ctx = ossl.CTX_new(ossl.TLS_server_method());
	if (!ctx) { bzy_io_fail("Network.tlsListen: SSL_CTX_new failed."); return NULL; }
	if (ossl.CTX_use_certificate_chain_file(ctx, bzy_str_data(certPath)) != 1)
	{
		ossl.CTX_free(ctx); bzy_io_fail("Network.tlsListen: cannot load certificate chain."); return NULL;
	}
	if (ossl.CTX_use_PrivateKey_file(ctx, bzy_str_data(keyPath), BZ_SSL_FILETYPE_PEM) != 1)
	{
		ossl.CTX_free(ctx); bzy_io_fail("Network.tlsListen: cannot load private key."); return NULL;
	}

	void *listener = bzy_listener_new(port);   /* Owned Listener. */
	if (!listener) { ossl.CTX_free(ctx); bzy_io_fail("Network.tlsListen: bind/listen failed."); return NULL; }

	void *o = bzy_alloc(48);
	*(void**)o = tls_list_vtable();
	TLL_LISTENER(o) = listener;
	TLL_CTX(o) = ctx;
	TLL_CLOSED(o) = 0;
	bzy_share_crosscore(o);
	return o;
}

void *bzy_tls_accept(void *l)
{
	void *transport = bzy_listener_accept(TLL_LISTENER(l));   /* Owned Socket; parks. */
	if (!transport) { bzy_io_fail("TlsListener.accept: accept failed."); return NULL; }

	void *ssl = ossl.SSL_new(TLL_CTX(l));
	if (!ssl || tls_attach_bios(ssl) != 0)
	{
		if (ssl) { ossl.SSL_free(ssl); }
		bzy_release(transport);
		bzy_io_fail("TlsListener.accept: SSL setup failed."); return NULL;
	}
	ossl.SSL_set_accept_state(ssl);

	void *s = tls_sock_wrap(transport, ssl, NULL);   /* Server conn shares the listener ctx. */
	if (tls_run(s, 0, NULL, 0) != 1)
	{
		bzy_io_fail("TlsListener.accept: handshake failed.");   /* Finalizer cleans up. */
	}
	return s;
}

void *bzy_tls_read(void *s, int64_t maxbytes)
{
	if (TLS_CLOSED(s)) { bzy_io_fail("TlsSocket.read: socket is closed."); return NULL; }
	int max = (int)(maxbytes > 0 ? maxbytes : 1);
	/* Stage the plaintext on the stack for the common small read; only the rare
	   large read touches the heap. The result byte[] must be sized to the actual
	   byte count, so a staging copy is unavoidable - but the per-read malloc/free
	   is not. */
	char stackbuf[16384];
	char *buf = (max <= (int)sizeof(stackbuf)) ? stackbuf : (char*)malloc((size_t)max);
	if (!buf) { bzy_io_fail("TlsSocket.read: out of memory."); return NULL; }
	int n = tls_run(s, 1, buf, max);
	if (n < 0)
	{
		if (buf != stackbuf) { free(buf); }
		bzy_io_fail("TlsSocket.read: TLS read error."); return NULL;
	}
	void *arr = tls_bytes_to_array(buf, n);   /* n == 0 -> empty byte[] (clean EOF). */
	if (buf != stackbuf) { free(buf); }
	return arr;
}

int64_t bzy_tls_write(void *s, void *data)
{
	if (TLS_CLOSED(s)) { bzy_io_fail("TlsSocket.write: socket is closed."); return 0; }
	int len = (int)bzy_array_len(data);
	const char *bytes = (const char*)data + 32;   /* Packed byte[] payload. */
	if (len == 0) { return 0; }
	int n = tls_run(s, 2, (void*)bytes, len);
	if (n < 0) { bzy_io_fail("TlsSocket.write: TLS write error."); return 0; }
	return n;
}

void bzy_tls_close(void *s)
{
	if (TLS_CLOSED(s)) { return; }
	if (TLS_SSL(s)) { ossl.SSL_shutdown(TLS_SSL(s)); tls_flush(s); }
	tls_sock_finalize(s);
}

int64_t bzy_tls_listener_port(void *l)
{
	return bzy_listener_port(TLL_LISTENER(l));
}

void bzy_tls_close_listener(void *l)
{
	tls_list_finalize(l);
}
