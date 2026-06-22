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

/* Authentication crypto (crypto.c, libcrypto bound lazily). */
extern int  bzy_crypto_load(void);
extern void bzy_crypto_sha256(const unsigned char *in, size_t len, unsigned char *out32);
extern void bzy_crypto_md5(const unsigned char *in, size_t len, unsigned char *out16);
extern void bzy_crypto_hmac_sha256(const unsigned char *key, int klen, const unsigned char *data, size_t dlen, unsigned char *out32);
extern int  bzy_crypto_pbkdf2_sha256(const char *pass, int plen, const unsigned char *salt, int slen, int iters, unsigned char *out, int outlen);
extern int  bzy_crypto_rand(unsigned char *out, int len);

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

/* ---- small encodings + a tagged-message sender (for authentication) ---------- */

static const char B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

/* Base64-encode `len` bytes into `out` (NUL-terminated). Returns the text length. */
static int b64_encode(const unsigned char *in, int len, char *out)
{
	int o = 0;
	int i = 0;
	while (i + 2 < len)
	{
		out[o++] = B64[in[i] >> 2];
		out[o++] = B64[((in[i] & 3) << 4) | (in[i + 1] >> 4)];
		out[o++] = B64[((in[i + 1] & 15) << 2) | (in[i + 2] >> 6)];
		out[o++] = B64[in[i + 2] & 63];
		i += 3;
	}

	if (len - i == 1)
	{
		out[o++] = B64[in[i] >> 2];
		out[o++] = B64[(in[i] & 3) << 4];
		out[o++] = '=';
		out[o++] = '=';
	}
	else if (len - i == 2)
	{
		out[o++] = B64[in[i] >> 2];
		out[o++] = B64[((in[i] & 3) << 4) | (in[i + 1] >> 4)];
		out[o++] = B64[(in[i + 1] & 15) << 2];
		out[o++] = '=';
	}

	out[o] = '\0';
	return o;
}

static int b64_val(char c)
{
	if (c >= 'A' && c <= 'Z') { return c - 'A'; }
	if (c >= 'a' && c <= 'z') { return c - 'a' + 26; }
	if (c >= '0' && c <= '9') { return c - '0' + 52; }
	if (c == '+') { return 62; }
	if (c == '/') { return 63; }
	return -1;   /* '=' or padding/whitespace. */
}

/* Base64-decode `inlen` chars into `out`; returns the byte count, -1 on a bad char. */
static int b64_decode(const char *in, int inlen, unsigned char *out)
{
	int o = 0;
	int bits = 0;
	int acc = 0;
	for (int i = 0; i < inlen; i++)
	{
		if (in[i] == '=') { break; }
		int v = b64_val(in[i]);
		if (v < 0) { return -1; }
		acc = (acc << 6) | v;
		bits += 6;
		if (bits >= 8)
		{
			bits -= 8;
			out[o++] = (unsigned char)((acc >> bits) & 0xFF);
		}
	}

	return o;
}

static void hex_encode(const unsigned char *in, int len, char *out)
{
	static const char H[] = "0123456789abcdef";
	for (int i = 0; i < len; i++)
	{
		out[2 * i]     = H[in[i] >> 4];
		out[2 * i + 1] = H[in[i] & 15];
	}

	out[2 * len] = '\0';
}

/* Send a tagged frontend message (tag + int32 length + body), one socket write. */
static int send_tagged(void *sock, char tag, const char *body, int64_t blen)
{
	Wbuf w = { 0 };
	w_u8(&w, (unsigned char)tag);
	int64_t at = w.len;
	w_i32(&w, 0);
	w_bytes(&w, body, blen);
	w_patch_len(&w, at);
	int64_t rc = bzy_sock_send_all(sock, w.p, w.len);
	free(w.p);
	return rc >= 0;
}

/* ---- authentication methods -------------------------------------------------- */

/* AuthenticationCleartextPassword: send the password as a NUL-terminated string.
   Safe only over TLS -- documented as such. */
static int auth_cleartext(void *sock, const char *password)
{
	return send_tagged(sock, 'p', password, (int64_t)strlen(password) + 1);
}

/* AuthenticationMD5Password: send "md5" + md5_hex(md5_hex(password+user) + salt). */
static int auth_md5(void *sock, const char *user, const char *password, const unsigned char salt[4])
{
	if (!bzy_crypto_load())
	{
		bzy_db_set_error("PostgreSQL MD5 authentication requires OpenSSL (libcrypto), which was not found.");
		return 0;
	}

	size_t pl = strlen(password), ul = strlen(user);
	unsigned char *cat = (unsigned char*)malloc(pl + ul);
	memcpy(cat, password, pl);
	memcpy(cat + pl, user, ul);
	unsigned char d1[16];
	bzy_crypto_md5(cat, pl + ul, d1);
	free(cat);

	char h1[33];
	hex_encode(d1, 16, h1);            /* 32 hex chars. */
	unsigned char buf2[36];
	memcpy(buf2, h1, 32);
	memcpy(buf2 + 32, salt, 4);
	unsigned char d2[16];
	bzy_crypto_md5(buf2, 36, d2);

	char body[40];
	memcpy(body, "md5", 3);
	hex_encode(d2, 16, body + 3);      /* "md5" + 32 hex + NUL. */
	return send_tagged(sock, 'p', body, (int64_t)strlen(body) + 1);
}

/* Find "<key>=" in a comma-delimited SCRAM message; copy the value (to the next
   comma or end) into out. Returns the value length, or -1 if the key is absent. */
static int scram_field(const char *msg, char key, char *out, int outcap)
{
	const char *p = msg;
	while (*p)
	{
		if (p[0] == key && p[1] == '=')
		{
			const char *v = p + 2;
			const char *e = strchr(v, ',');
			int n = e ? (int)(e - v) : (int)strlen(v);
			if (n >= outcap) { n = outcap - 1; }
			memcpy(out, v, n);
			out[n] = '\0';
			return n;
		}

		const char *nx = strchr(p, ',');
		if (!nx) { break; }
		p = nx + 1;
	}

	return -1;
}

/* The SCRAM-SHA-256 client exchange (RFC 5802). Sends client-first, reads
   server-first ('R' SASLContinue), sends client-final with the client proof, reads
   and verifies server-final ('R' SASLFinal). Returns 1 on success; on failure the
   error sink is set. The caller's loop then reads AuthenticationOk + ReadyForQuery. */
static int auth_scram(Reader *r, void *sock, const char *user, const char *password)
{
	(void)user;   /* The username travels in the startup message; SCRAM sends n=, . */
	if (!bzy_crypto_load())
	{
		bzy_db_set_error("PostgreSQL SCRAM authentication requires OpenSSL (libcrypto), which was not found.");
		return 0;
	}

	/* Client nonce: 18 random bytes -> 24 base64 chars (no '=' padding, no comma). */
	unsigned char nraw[18];
	if (!bzy_crypto_rand(nraw, 18))
	{
		bzy_db_set_error("Failed to generate a SCRAM client nonce.");
		return 0;
	}

	char cnonce[40];
	b64_encode(nraw, 18, cnonce);

	char first_bare[80];
	snprintf(first_bare, sizeof(first_bare), "n=,r=%s", cnonce);

	/* SASLInitialResponse: mechanism cstr, int32 data length, then "n,," + bare. */
	char client_first[96];
	snprintf(client_first, sizeof(client_first), "n,,%s", first_bare);
	{
		Wbuf w = { 0 };
		w_cstr(&w, "SCRAM-SHA-256");
		w_i32(&w, (int32_t)strlen(client_first));
		w_bytes(&w, client_first, (int64_t)strlen(client_first));
		int ok = send_tagged(sock, 'p', w.p, w.len);
		free(w.p);
		if (!ok) { bzy_db_set_error("Failed to send the SCRAM client-first message."); return 0; }
	}

	/* Read 'R' AuthenticationSASLContinue (sub 11): the server-first-message. */
	char tag;
	char *body;
	int64_t blen;
	if (!rd_msg(r, &tag, &body, &blen) || tag != 'R' || blen < 4 || be32(body) != 11)
	{
		bzy_db_set_error("Unexpected message during SCRAM (expected SASLContinue).");
		return 0;
	}

	char server_first[512];
	int sflen = (int)(blen - 4);
	if (sflen >= (int)sizeof(server_first)) { sflen = (int)sizeof(server_first) - 1; }
	memcpy(server_first, body + 4, sflen);
	server_first[sflen] = '\0';

	char combined[128], salt_b64[256], iters_s[16];
	if (scram_field(server_first, 'r', combined, sizeof(combined)) < 0
		|| scram_field(server_first, 's', salt_b64, sizeof(salt_b64)) < 0
		|| scram_field(server_first, 'i', iters_s, sizeof(iters_s)) < 0)
	{
		bzy_db_set_error("Malformed SCRAM server-first message.");
		return 0;
	}

	if (strncmp(combined, cnonce, strlen(cnonce)) != 0)   /* Server must echo our nonce. */
	{
		bzy_db_set_error("SCRAM nonce mismatch (possible man-in-the-middle).");
		return 0;
	}

	unsigned char salt[192];
	int saltlen = b64_decode(salt_b64, (int)strlen(salt_b64), salt);
	int iters = atoi(iters_s);
	if (saltlen <= 0 || iters <= 0)
	{
		bzy_db_set_error("Invalid SCRAM salt or iteration count.");
		return 0;
	}

	/* SaltedPassword -> ClientKey -> StoredKey -> ClientSignature -> proof. */
	unsigned char salted[32];
	if (!bzy_crypto_pbkdf2_sha256(password, (int)strlen(password), salt, saltlen, iters, salted, 32))
	{
		bzy_db_set_error("SCRAM PBKDF2 derivation failed.");
		return 0;
	}

	unsigned char client_key[32], stored_key[32], server_key[32];
	bzy_crypto_hmac_sha256(salted, 32, (const unsigned char*)"Client Key", 10, client_key);
	bzy_crypto_sha256(client_key, 32, stored_key);
	bzy_crypto_hmac_sha256(salted, 32, (const unsigned char*)"Server Key", 10, server_key);

	char final_noproof[160];
	snprintf(final_noproof, sizeof(final_noproof), "c=biws,r=%s", combined);

	char auth_msg[1024];
	snprintf(auth_msg, sizeof(auth_msg), "%s,%s,%s", first_bare, server_first, final_noproof);

	unsigned char client_sig[32], server_sig[32];
	bzy_crypto_hmac_sha256(stored_key, 32, (const unsigned char*)auth_msg, strlen(auth_msg), client_sig);
	bzy_crypto_hmac_sha256(server_key, 32, (const unsigned char*)auth_msg, strlen(auth_msg), server_sig);

	unsigned char proof[32];
	for (int i = 0; i < 32; i++) { proof[i] = client_key[i] ^ client_sig[i]; }
	char proof_b64[64];
	b64_encode(proof, 32, proof_b64);

	char client_final[256];
	snprintf(client_final, sizeof(client_final), "%s,p=%s", final_noproof, proof_b64);
	if (!send_tagged(sock, 'p', client_final, (int64_t)strlen(client_final)))
	{
		bzy_db_set_error("Failed to send the SCRAM client-final message.");
		return 0;
	}

	/* Read 'R' AuthenticationSASLFinal (sub 12): v=<base64 ServerSignature>. */
	if (!rd_msg(r, &tag, &body, &blen) || tag != 'R' || blen < 4 || be32(body) != 12)
	{
		bzy_db_set_error("Unexpected message during SCRAM (expected SASLFinal).");
		return 0;
	}

	char server_final[128];
	int ffl = (int)(blen - 4);
	if (ffl >= (int)sizeof(server_final)) { ffl = (int)sizeof(server_final) - 1; }
	memcpy(server_final, body + 4, ffl);
	server_final[ffl] = '\0';

	char vsig_b64[64];
	if (scram_field(server_final, 'v', vsig_b64, sizeof(vsig_b64)) < 0)
	{
		bzy_db_set_error("Malformed SCRAM server-final message.");
		return 0;
	}

	unsigned char vsig[64];
	int vlen = b64_decode(vsig_b64, (int)strlen(vsig_b64), vsig);
	if (vlen != 32 || memcmp(vsig, server_sig, 32) != 0)
	{
		bzy_db_set_error("SCRAM server signature verification failed (possible man-in-the-middle).");
		return 0;
	}

	return 1;
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
int bzy_pg_run_startup(void *sock, const char *user, const char *password, const char *db)
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
			else if (sub == 3)                       /* Cleartext password. */
			{
				if (!auth_cleartext(sock, password)) { bzy_db_set_error("Failed to send the cleartext password."); break; }
				continue;
			}
			else if (sub == 5)                       /* MD5: a 4-byte salt follows. */
			{
				if (blen < 8) { bzy_db_set_error("Malformed MD5 authentication request."); break; }
				unsigned char salt[4];
				memcpy(salt, body + 4, 4);
				if (!auth_md5(sock, user, password, salt)) { break; }
				continue;
			}
			else if (sub == 10)                      /* SASL (SCRAM-SHA-256). */
			{
				if (!auth_scram(&r, sock, user, password)) { break; }
				continue;
			}

			bzy_db_set_error("Unsupported PostgreSQL authentication method.");
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
	void *sock = bzy_socket_connect(host, port);
	if (!sock)
	{
		bzy_db_set_error("Could not connect to the PostgreSQL server.");
		return NULL;
	}

	if (!bzy_pg_run_startup(sock, bzy_str_data(user), bzy_str_data(pass), bzy_str_data(db)))
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
