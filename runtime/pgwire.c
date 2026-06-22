/* runtime/pgwire.c -- PostgreSQL v3 frontend/backend protocol over a Breezy Socket.
   Own TU: a program that never names Postgres links none of this. The driver is
   async by reusing the reactor-parked socket I/O (bzy_sock_recv/bzy_sock_send_all),
   so a query parks its breeze instead of blocking a worker thread; auth crypto
   (Task 5) reuses the libcrypto already bound for TLS.

   This file is Task 3 of the Phase F.1 plan: the message framing, the buffered
   reader, the frontend message builder, and the startup handshake to
   ReadyForQuery. Authentication (Task 5), query (Tasks 6-7), the Breezy entry
   points (Task 4), and TLS (Task 8) build on this layer. */
#include "breezy.h"
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

/* Reactor-parked raw byte I/O on a Socket (shared from socket.c). */
extern int     bzy_sock_recv(void *s, char *buf, int max, int64_t timeout_ms);
extern int64_t bzy_sock_send_all(void *s, const char *buf, int64_t len);

/* Socket lifecycle (socket.c): connect returns a managed Socket (+1) or NULL. */
extern void *bzy_socket_connect(void *host, int64_t port);
extern void  bzy_socket_close(void *s);

/* Shared result model (dbresult.c) -- the error sink used for a failed handshake. */
extern void bzy_db_set_error(const char *msg);

#define PG_PROTOCOL_V3 196608   /* 3.0 in the int32 (major<<16 | minor). */

/* ---- the buffered reader (one growable buffer + a scan cursor, as httpproto) -- */

typedef struct
{
	void   *sock;
	char   *buf;
	int64_t len;
	int64_t cap;
	int64_t pos;
	int     eof;
} Reader;

/* Append more bytes from the socket; sets eof at a clean close. 1 = bytes added. */
static int rd_fill(Reader *r)
{
	if (r->eof) { return 0; }
	if (r->len + 65536 > r->cap)
	{
		r->cap = r->cap ? r->cap * 2 : 65536;
		if (r->cap < r->len + 65536) { r->cap = r->len + 65536; }
		r->buf = (char*)realloc(r->buf, (size_t)r->cap);
	}

	int n = bzy_sock_recv(r->sock, r->buf + r->len, 65536, -1);
	if (n <= 0) { r->eof = 1; return 0; }
	r->len += n;
	return 1;
}

/* Ensure at least `need` bytes are buffered from pos; 1 ok, 0 at EOF. */
static int rd_need(Reader *r, int64_t need)
{
	while (r->len - r->pos < need)
	{
		if (!rd_fill(r)) { return 0; }
	}

	return 1;
}

/* Read a big-endian int32 from a byte pointer. */
static int32_t be32(const char *p)
{
	return (int32_t)( ((uint32_t)(unsigned char)p[0] << 24) | ((uint32_t)(unsigned char)p[1] << 16)
	                | ((uint32_t)(unsigned char)p[2] << 8)  |  (uint32_t)(unsigned char)p[3] );
}

/* Read one backend message: a 1-byte tag + an int32 length (length covers itself,
   excludes the tag). On success *tag is set, *body points at the message body
   (length - 4 bytes), and the cursor advances past it. Returns 0 at EOF.
   NOTE: *body points into the reader buffer and is valid only until the next
   rd_msg call (which may realloc) -- consume it before reading again. */
static int rd_msg(Reader *r, char *tag, char **body, int64_t *blen)
{
	if (!rd_need(r, 5)) { return 0; }
	*tag = r->buf[r->pos];
	int32_t L = be32(r->buf + r->pos + 1);
	if (L < 4) { return 0; }
	int64_t total = 1 + (int64_t)L;          /* tag + length-field + body. */
	if (!rd_need(r, total)) { return 0; }
	*body = r->buf + r->pos + 5;
	*blen = (int64_t)L - 4;
	r->pos += total;
	return 1;
}

/* ---- the frontend message builder (one growable buffer, one send) ------------ */

typedef struct
{
	char   *p;
	int64_t len;
	int64_t cap;
} Wbuf;

static void w_bytes(Wbuf *w, const void *b, int64_t n)
{
	if (w->len + n > w->cap)
	{
		w->cap = (w->len + n) * 2 + 64;
		w->p = (char*)realloc(w->p, (size_t)w->cap);
	}

	memcpy(w->p + w->len, b, (size_t)n);
	w->len += n;
}

static void w_u8(Wbuf *w, unsigned char v)
{
	w_bytes(w, &v, 1);
}

static void w_i32(Wbuf *w, int32_t v)
{
	char b[4] = { (char)(v >> 24), (char)(v >> 16), (char)(v >> 8), (char)v };
	w_bytes(w, b, 4);
}

static void w_cstr(Wbuf *w, const char *s)
{
	w_bytes(w, s, (int64_t)strlen(s) + 1);   /* Include the terminating NUL. */
}

/* Patch the int32 length placeholder at offset `at` to cover [at, len). */
static void w_patch_len(Wbuf *w, int64_t at)
{
	int32_t v = (int32_t)(w->len - at);
	w->p[at]     = (char)(v >> 24);
	w->p[at + 1] = (char)(v >> 16);
	w->p[at + 2] = (char)(v >> 8);
	w->p[at + 3] = (char)v;
}

/* ---- the startup handshake --------------------------------------------------- */

/* StartupMessage: int32 length, int32 protocol(196608), then "user\0<u>\0
   database\0<d>\0" and a final \0. No type tag (the one tagless frontend message). */
static int send_startup(void *sock, const char *user, const char *db)
{
	Wbuf w = { 0 };
	int64_t at = w.len;
	w_i32(&w, 0);                 /* Length placeholder. */
	w_i32(&w, PG_PROTOCOL_V3);
	w_cstr(&w, "user");
	w_cstr(&w, user);
	w_cstr(&w, "database");
	w_cstr(&w, db);
	w_u8(&w, 0);                  /* Terminating empty key. */
	w_patch_len(&w, at);
	int64_t rc = bzy_sock_send_all(sock, w.p, w.len);
	free(w.p);
	return rc >= 0;
}

/* Decode an ErrorResponse/NoticeResponse body into the error sink. The body is a
   series of <1-byte field code><NUL-terminated value>, terminated by a 0 byte;
   'C' is the SQLSTATE, 'M' the human-readable message. */
static void set_error_from_response(const char *body, int64_t blen)
{
	const char *sqlstate = "";
	const char *message  = "Server error.";
	int64_t i = 0;
	while (i < blen && body[i] != 0)
	{
		char code = body[i++];
		const char *val = body + i;
		while (i < blen && body[i] != 0) { i++; }   /* Scan to the value's NUL. */
		if (i < blen) { i++; }                        /* Step past the NUL. */
		if (code == 'C') { sqlstate = val; }
		else if (code == 'M') { message = val; }
	}

	char buf[512];
	snprintf(buf, sizeof(buf), "%s: %s", sqlstate, message);
	bzy_db_set_error(buf);
}

/* Run startup on an already-connected socket: send the StartupMessage, then pump
   backend messages until ReadyForQuery (success, returns 1) or an error (returns
   0 with the error sink set). Authentication beyond AuthenticationOk lands in
   Task 5 -- here any auth request other than "Ok" is reported as unsupported. */
int bzy_pg_run_startup(void *sock, const char *user, const char *db)
{
	if (!send_startup(sock, user, db))
	{
		bzy_db_set_error("Failed to send the PostgreSQL startup message.");
		return 0;
	}

	Reader r = { 0 };
	r.sock = sock;
	int ok = 0;
	char tag;
	char *body;
	int64_t blen;
	while (rd_msg(&r, &tag, &body, &blen))
	{
		if (tag == 'R')                              /* Authentication. */
		{
			int32_t sub = (blen >= 4) ? be32(body) : -1;
			if (sub == 0) { continue; }              /* AuthenticationOk. */
			bzy_db_set_error("PostgreSQL authentication required but not yet supported (Task 5).");
			break;
		}
		else if (tag == 'E')                         /* ErrorResponse. */
		{
			set_error_from_response(body, blen);
			break;
		}
		else if (tag == 'Z')                         /* ReadyForQuery. */
		{
			ok = 1;
			break;
		}
		/* ParameterStatus 'S', BackendKeyData 'K', NoticeResponse 'N': skipped. */
	}

	/* A clean EOF with no prior error (E / unsupported-auth set theirs above). */
	if (!ok && r.eof) { bzy_db_set_error("Connection closed during PostgreSQL startup."); }
	free(r.buf);
	return ok;
}

/* ---- the PgConnection node + the Breezy entry points -------------------------- */

/* PgConnection: sock@24, size 32. typeinfo {0,1,24} -- one managed slot so ARC and
   the cycle collector trace (and release) the held Socket. */
#define PGC_SOCK 24
#define PGC_SIZE 32

static int64_t g_pgc_ti[3] = { 0, 1, PGC_SOCK };
static int64_t g_pgc_vt[2];
static int     g_pgc_vt_built;

static void *pgc_vtable(void)
{
	if (!g_pgc_vt_built) { g_pgc_vt[0] = (int64_t)&g_pgc_ti[0]; g_pgc_vt_built = 1; }
	return &g_pgc_vt[1];
}

/* Connect, handshake, and wrap the socket in a PgConnection. On any failure the
   error sink is set (the codegen-emitted bzy_db_check raises it) and NULL returns.
   `pass` is unused until SCRAM/MD5 auth (Task 5); the AuthenticationOk path here
   ignores it. */
void *bzy_pg_connect(void *host, int64_t port, void *user, void *pass, void *db)
{
	(void)pass;
	void *sock = bzy_socket_connect(host, port);
	if (!sock)
	{
		bzy_db_set_error("Could not connect to the PostgreSQL server.");
		return NULL;
	}

	if (!bzy_pg_run_startup(sock, bzy_str_data(user), bzy_str_data(db)))
	{
		bzy_socket_close(sock);   /* Startup set the error. */
		bzy_release(sock);
		return NULL;
	}

	void *n = bzy_alloc(PGC_SIZE);
	*(void**)n = pgc_vtable();
	*(void**)((char*)n + PGC_SOCK) = sock;   /* Transfer the +1 from bzy_socket_connect. */
	return n;
}

/* Send Terminate and close the socket. The node still owns its +1 Socket reference,
   released when the PgConnection itself is released; the close is idempotent. */
void bzy_pg_close(void *conn)
{
	if (!conn) { return; }
	void *sock = *(void**)((char*)conn + PGC_SOCK);
	if (sock)
	{
		char term[5] = { 'X', 0, 0, 0, 4 };   /* Terminate: tag 'X' + int32 length 4. */
		bzy_sock_send_all(sock, term, 5);
		bzy_socket_close(sock);
	}
}
