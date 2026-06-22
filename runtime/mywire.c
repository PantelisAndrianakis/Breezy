/* runtime/mywire.c -- MySQL/MariaDB client/server protocol over a Breezy Socket.
   Own TU: a program that never names Mysql links none of this. Async by reusing
   the reactor-parked socket I/O, like the PostgreSQL driver (pgwire.c); the shared
   DbResult/Row result model (dbresult.c) and the auth crypto (crypto.c) are reused
   unchanged. One driver serves both MySQL and MariaDB -- same wire protocol.

   MySQL framing differs from Postgres: every packet is [3-byte little-endian payload
   length][1-byte sequence number][payload], integers are little-endian, and strings
   are often length-encoded. This file is Task 3 of the Phase F.2 plan: the framing,
   the length-encoded codec, the buffered reader, and the handshake to an OK/ERR
   decision. Authentication (Task 5), query (Tasks 6-7), and the Breezy entry points
   (Task 4) build on this layer. */
#include "breezy.h"
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

extern int     bzy_sock_recv(void *s, char *buf, int max, int64_t timeout_ms);
extern int64_t bzy_sock_send_all(void *s, const char *buf, int64_t len);
extern void *bzy_socket_connect(void *host, int64_t port);
extern void  bzy_socket_close(void *s);
extern void bzy_db_set_error(const char *msg);
extern void *bzy_db_result_new(void *colnames, void *rows, int64_t rowcount);
extern void *bzy_db_row_new(void *values, void *colnames);

/* Authentication crypto (crypto.c, libcrypto bound lazily). */
extern int  bzy_crypto_load(void);
extern void bzy_crypto_sha1(const unsigned char *in, size_t len, unsigned char *out20);
extern void bzy_crypto_sha256(const unsigned char *in, size_t len, unsigned char *out32);

/* TLS transport (tls.c): the raw-byte twins + the upgrade-an-existing-socket entry.
   Used only by connectTls; a plaintext connection never references these. */
extern int     bzy_tls_recv(void *s, char *buf, int max, int64_t timeout_ms);
extern int64_t bzy_tls_send_all(void *s, const char *buf, int64_t len);
extern void   *bzy_tls_upgrade_client(void *transport, const char *host, int insecure);
extern void    bzy_tls_close(void *s);

/* The transport a connection rides on: a plain Socket, or after connectTls a
   TlsSocket. x_recv/x_send branch on is_tls so all the protocol code below stays
   transport-agnostic -- it threads an Xport* where it used to thread a void *sock. */
typedef struct
{
	void *t;
	int   is_tls;
} Xport;

static int x_recv(Xport *x, char *buf, int max, int64_t timeout_ms)
{
	return x->is_tls ? bzy_tls_recv(x->t, buf, max, timeout_ms)
		   : bzy_sock_recv(x->t, buf, max, timeout_ms);
}

static int64_t x_send(Xport *x, const char *buf, int64_t len)
{
	return x->is_tls ? bzy_tls_send_all(x->t, buf, len)
		   : bzy_sock_send_all(x->t, buf, len);
}

/* Client capability flags we advertise. */
#define CLIENT_LONG_PASSWORD     0x00000001u
#define CLIENT_LONG_FLAG         0x00000004u
#define CLIENT_CONNECT_WITH_DB   0x00000008u
#define CLIENT_PROTOCOL_41       0x00000200u
#define CLIENT_SSL               0x00000800u
#define CLIENT_TRANSACTIONS      0x00002000u
#define CLIENT_SECURE_CONNECTION 0x00008000u
#define CLIENT_PLUGIN_AUTH       0x00080000u

/* ---- little-endian readers --------------------------------------------------- */

static uint32_t le16(const unsigned char *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8);
}

static uint32_t le24(const unsigned char *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16);
}

static uint32_t le32(const unsigned char *p)
{
	return le24(p) | ((uint32_t)p[3] << 24);
}

static uint64_t le64(const unsigned char *p)
{
	uint64_t v = 0;
	for (int i = 0; i < 8; i++)
	{
		v |= (uint64_t)p[i] << (8 * i);
	}

	return v;
}

/* Decode a length-encoded integer; *adv receives the bytes consumed. A leading
   0xfb (NULL) or 0xfe-as-EOF is the caller's job to detect before calling. */
static uint64_t lenenc_int(const unsigned char *p, int64_t *adv)
{
	unsigned char c = p[0];
	if (c < 0xfb)
	{
		*adv = 1;
		return c;
	}

	if (c == 0xfc)
	{
		*adv = 3;
		return le16(p + 1);
	}

	if (c == 0xfd)
	{
		*adv = 4;
		return le24(p + 1);
	}
	*adv = 9;   /* 0xfe: 8-byte. */
	uint64_t v = 0;
	for (int i = 0; i < 8; i++)
	{
		v |= (uint64_t)p[1 + i] << (8 * i);
	}

	return v;
}

/* Read a length-encoded string in [*p, end); advances *p, sets *slen, and sets
   *isnull on a 0xfb NULL marker. Returns the data pointer (NULL when NULL). */
static const char *lenenc_str(const unsigned char **p, const unsigned char *end, int64_t *slen, int *isnull)
{
	*isnull = 0;
	*slen = 0;
	if (*p >= end)
	{
		return NULL;
	}

	if (**p == 0xfb)
	{
		*isnull = 1;
		(*p)++;
		return NULL;
	}

	int64_t adv;
	uint64_t n = lenenc_int(*p, &adv);
	*p += adv;
	const char *s = (const char*)*p;
	if (*p + n > end)
	{
		n = (uint64_t)(end - *p);    /* Defensive clamp. */
	}
	*p += n;
	*slen = (int64_t)n;
	return s;
}

/* ---- the buffered packet reader ---------------------------------------------- */

typedef struct
{
	Xport  *x;
	char   *buf;
	int64_t len;
	int64_t cap;
	int64_t pos;
	int     eof;
} Reader;

static int rd_fill(Reader *r)
{
	if (r->eof)
	{
		return 0;
	}

	if (r->len + 65536 > r->cap)
	{
		r->cap = r->cap ? r->cap * 2 : 65536;
		if (r->cap < r->len + 65536)
		{
			r->cap = r->len + 65536;
		}

		r->buf = (char*)realloc(r->buf, (size_t)r->cap);
	}

	int n = x_recv(r->x, r->buf + r->len, 65536, -1);
	if (n <= 0)
	{
		r->eof = 1;
		return 0;
	}

	r->len += n;
	return 1;
}

static int rd_need(Reader *r, int64_t need)
{
	while (r->len - r->pos < need)
	{
		if (!rd_fill(r))
		{
			return 0;
		}
	}

	return 1;
}

/* Read one packet: a 4-byte header (3-byte LE length + 1-byte sequence) then the
   payload. *payload points into the reader buffer (valid until the next rd_packet);
   *plen is the payload length, *seq the sequence number. 0 at EOF. */
static int rd_packet(Reader *r, unsigned char **payload, int64_t *plen, int *seq)
{
	if (!rd_need(r, 4))
	{
		return 0;
	}

	const unsigned char *h = (const unsigned char*)(r->buf + r->pos);
	int64_t L = le24(h);
	*seq = h[3];
	if (!rd_need(r, 4 + L))
	{
		return 0;
	}
	*payload = (unsigned char*)(r->buf + r->pos + 4);
	*plen = L;
	r->pos += 4 + L;
	return 1;
}

/* ---- the frontend packet builder --------------------------------------------- */

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

static void w_le16(Wbuf *w, uint32_t v)
{
	unsigned char b[2] = { (unsigned char)v, (unsigned char)(v >> 8) };
	w_bytes(w, b, 2);
}

static void w_le32(Wbuf *w, uint32_t v)
{
	unsigned char b[4] = { (unsigned char)v, (unsigned char)(v >> 8), (unsigned char)(v >> 16), (unsigned char)(v >> 24) };
	w_bytes(w, b, 4);
}

/* Write a length-encoded integer (for a prepared-parameter value length). */
static void w_lenenc(Wbuf *w, uint64_t n)
{
	if (n < 251)
	{
		w_u8(w, (unsigned char)n);
	}
	else if (n < 65536)
	{
		w_u8(w, 0xfc);
		w_le16(w, (uint32_t)n);
	}
	else if (n < 16777216)
	{
		w_u8(w, 0xfd);
		w_u8(w, (unsigned char)n);
		w_u8(w, (unsigned char)(n >> 8));
		w_u8(w, (unsigned char)(n >> 16));
	}
	else
	{
		w_u8(w, 0xfe);
		w_le32(w, (uint32_t)n);
		w_le32(w, (uint32_t)(n >> 32));
	}
}

static void w_cstr(Wbuf *w, const char *s)
{
	w_bytes(w, s, (int64_t)strlen(s) + 1);
}

static void w_zero(Wbuf *w, int n)
{
	for (int i = 0; i < n; i++)
	{
		w_u8(w, 0);
	}
}

/* Send a payload as one packet: a 3-byte LE length + the sequence byte + payload. */
static int send_packet(Xport *x, int seq, const char *payload, int64_t plen)
{
	Wbuf h = { 0 };
	w_u8(&h, (unsigned char)plen);
	w_u8(&h, (unsigned char)(plen >> 8));
	w_u8(&h, (unsigned char)(plen >> 16));
	w_u8(&h, (unsigned char)seq);
	w_bytes(&h, payload, plen);
	int64_t rc = x_send(x, h.p, h.len);
	free(h.p);
	return rc >= 0;
}

/* ---- ERR packet -> the error sink -------------------------------------------- */

/* An ERR packet: 0xff, error code (LE16), '#', 5-byte SQLSTATE, message to the end. */
static void set_error_from_err(const unsigned char *payload, int64_t plen)
{
	char sqlstate[6] = "HY000";
	const char *msg = (const char*)payload + 3;
	int64_t msglen = plen - 3;
	if (plen >= 9 && payload[3] == '#')
	{
		memcpy(sqlstate, payload + 4, 5);
		msg = (const char*)payload + 9;
		msglen = plen - 9;
	}

	char buf[512];
	int n = snprintf(buf, sizeof(buf), "%s: ", sqlstate);
	if (msglen > (int64_t)sizeof(buf) - n - 1)
	{
		msglen = (int64_t)sizeof(buf) - n - 1;
	}

	if (msglen < 0)
	{
		msglen = 0;
	}

	memcpy(buf + n, msg, (size_t)msglen);
	buf[n + msglen] = '\0';
	bzy_db_set_error(buf);
}

/* ---- the handshake ----------------------------------------------------------- */

/* The pieces of the initial handshake the response needs. */
typedef struct
{
	unsigned char scramble[20];
	char          plugin[64];
} Handshake;

/* Parse the server's initial handshake (protocol 10). Returns 1 on success. */
static int parse_handshake(const unsigned char *p, int64_t plen, Handshake *hs)
{
	const unsigned char *end = p + plen;
	memset(hs, 0, sizeof(*hs));
	strcpy(hs->plugin, "mysql_native_password");   /* Default if none advertised. */

	if (plen < 1 || *p != 10)
	{
		return 0;    /* Protocol version 10. */
	}

	p++;
	while (p < end && *p)
	{
		p++;    /* server version cstr. */
	}

	p++;
	if (p + 4 > end)
	{
		return 0;
	}

	p += 4;                                        /* connection id. */
	if (p + 8 > end)
	{
		return 0;
	}

	memcpy(hs->scramble, p, 8);                    /* auth-plugin-data part 1. */
	p += 8;
	p += 1;                                        /* filler. */
	if (p + 2 > end)
	{
		return 1;    /* No extended part; scramble is short. */
	}

	p += 2;                                        /* capability flags lower. */

	if (p + 1 > end)
	{
		return 1;
	}

	p += 1;                                        /* character set. */
	if (p + 2 > end)
	{
		return 1;
	}

	p += 2;                                        /* status flags. */
	if (p + 2 > end)
	{
		return 1;
	}

	p += 2;                                        /* capability flags upper. */
	int auth_data_len = (p < end) ? *p : 0;
	p += 1;
	p += 10;                                       /* reserved. */

	int part2 = auth_data_len ? auth_data_len - 8 : 13;
	int take = part2 - 1;                          /* Drop the trailing NUL. */
	if (take > 12)
	{
		take = 12;
	}

	if (take < 0)
	{
		take = 0;
	}

	if (p + take <= end)
	{
		memcpy(hs->scramble + 8, p, take);
	}

	p += part2;

	if (p < end)                                   /* auth plugin name cstr. */
	{
		size_t i = 0;
		while (p < end && *p && i < sizeof(hs->plugin) - 1)
		{
			hs->plugin[i++] = (char)*p++;
		}

		hs->plugin[i] = '\0';
	}

	return 1;
}

/* ---- auth-response computation ----------------------------------------------- */

/* mysql_native_password: SHA1(pw) XOR SHA1(scramble + SHA1(SHA1(pw))), 20 bytes. */
static int auth_native(const char *password, const unsigned char *scramble, unsigned char *out)
{
	if (!password[0])
	{
		return 0;
	}

	unsigned char h1[20], h2[20], h3[20], cat[40];
	bzy_crypto_sha1((const unsigned char*)password, strlen(password), h1);
	bzy_crypto_sha1(h1, 20, h2);
	memcpy(cat, scramble, 20);
	memcpy(cat + 20, h2, 20);
	bzy_crypto_sha1(cat, 40, h3);
	for (int i = 0; i < 20; i++)
	{
		out[i] = (unsigned char)(h1[i] ^ h3[i]);
	}

	return 20;
}

/* caching_sha2_password fast path:
   SHA256(pw) XOR SHA256(SHA256(SHA256(pw)) + scramble), 32 bytes. */
static int auth_caching_sha2(const char *password, const unsigned char *scramble, unsigned char *out)
{
	if (!password[0])
	{
		return 0;
	}

	unsigned char d1[32], d2[32], d3[32], cat[52];
	bzy_crypto_sha256((const unsigned char*)password, strlen(password), d1);
	bzy_crypto_sha256(d1, 32, d2);
	memcpy(cat, d2, 32);
	memcpy(cat + 32, scramble, 20);
	bzy_crypto_sha256(cat, 52, d3);
	for (int i = 0; i < 32; i++)
	{
		out[i] = (unsigned char)(d1[i] ^ d3[i]);
	}

	return 32;
}

static int compute_auth(const char *plugin, const char *password, const unsigned char *scramble, unsigned char *out)
{
	if (strcmp(plugin, "mysql_native_password") == 0)
	{
		return auth_native(password, scramble, out);
	}

	if (strcmp(plugin, "caching_sha2_password") == 0)
	{
		return auth_caching_sha2(password, scramble, out);
	}

	return 0;   /* Unknown plugin -> empty; the server will AuthSwitch or reject. */
}

/* The capability flags the handshake response advertises. with_ssl adds CLIENT_SSL
   (set on both the SSLRequest packet and the response that follows it over TLS). */
static uint32_t client_caps(const char *db, int with_ssl)
{
	uint32_t caps = CLIENT_LONG_PASSWORD | CLIENT_LONG_FLAG | CLIENT_PROTOCOL_41
					| CLIENT_TRANSACTIONS | CLIENT_SECURE_CONNECTION | CLIENT_PLUGIN_AUTH;
	if (db && db[0])
	{
		caps |= CLIENT_CONNECT_WITH_DB;
	}

	if (with_ssl)
	{
		caps |= CLIENT_SSL;
	}

	return caps;
}

/* Send the SSLRequest packet: the first 32 bytes of a handshake response (capability
   flags with CLIENT_SSL, max-packet, charset, the 23-byte filler) and no username or
   auth. The server switches to TLS after this; the full handshake response follows
   over the encrypted channel. */
static int send_ssl_request(Xport *x, int seq, const char *db)
{
	Wbuf w = { 0 };
	w_le32(&w, client_caps(db, 1));
	w_le32(&w, 0x01000000);          /* Max packet size 16 MiB. */
	w_u8(&w, 45);                    /* utf8mb4_general_ci. */
	w_zero(&w, 23);                  /* Reserved. */
	int ok = send_packet(x, seq, w.p, w.len);
	free(w.p);
	return ok;
}

/* Build + send the Handshake Response. `auth` is the computed auth-response; `seq` is
   the next sequence number (handshake+1 plaintext, handshake+2 after an SSLRequest).
   with_ssl keeps CLIENT_SSL set so the response matches the SSLRequest's caps. */
static int send_handshake_response(Xport *x, int seq, const char *user, const char *db,
								   const char *plugin, const unsigned char *auth, int authlen, int with_ssl)
{
	Wbuf w = { 0 };
	w_le32(&w, client_caps(db, with_ssl));
	w_le32(&w, 0x01000000);          /* Max packet size 16 MiB. */
	w_u8(&w, 45);                    /* utf8mb4_general_ci. */
	w_zero(&w, 23);                  /* Reserved. */
	w_cstr(&w, user);
	w_u8(&w, (unsigned char)authlen);   /* CLIENT_SECURE_CONNECTION: 1-byte length + data. */
	if (authlen)
	{
		w_bytes(&w, auth, authlen);
	}

	if (db && db[0])
	{
		w_cstr(&w, db);
	}

	w_cstr(&w, plugin);

	int ok = send_packet(x, seq, w.p, w.len);
	free(w.p);
	return ok;
}

/* Run the handshake on a connected socket: read the server greeting, compute the
   auth-response for the server's plugin, send the response, and resolve the auth
   exchange (OK / ERR / AuthSwitchRequest / AuthMoreData). Returns 1 on success,
   0 with the error sink set. caching_sha2_password's full-auth path (0x01 0x04)
   needs TLS, which is the deferred connectTls follow-up. */
int bzy_my_run_handshake(Xport *x, const char *host, const char *user, const char *password, const char *db, int tls_mode)
{
	Reader r = { 0 };
	r.x = x;

	unsigned char *payload;
	int64_t plen;
	int seq;
	if (!rd_packet(&r, &payload, &plen, &seq))
	{
		bzy_db_set_error("No handshake from the MySQL server.");
		free(r.buf);
		return 0;
	}

	if (plen >= 1 && payload[0] == 0xff)
	{
		set_error_from_err(payload, plen);
		free(r.buf);
		return 0;
	}

	Handshake hs;
	if (!parse_handshake(payload, plen, &hs))
	{
		bzy_db_set_error("Malformed MySQL handshake.");
		free(r.buf);
		return 0;
	}

	if (password[0] && !bzy_crypto_load())
	{
		bzy_db_set_error("MySQL authentication requires OpenSSL (libcrypto), which was not found.");
		free(r.buf);
		return 0;
	}

	int rseq = seq + 1;   /* The next packet's sequence after the greeting. */
	if (tls_mode)
	{
		/* MySQL upgrades mid-handshake: send the SSLRequest, switch to TLS over the
		   same socket, then run the rest of the handshake -- and the whole session --
		   encrypted. The transport swap is written back through x for the caller. */
		if (!send_ssl_request(x, rseq, db))
		{
			bzy_db_set_error("Failed to send the MySQL SSLRequest.");
			free(r.buf);
			return 0;
		}

		rseq++;
		void *tls = bzy_tls_upgrade_client(x->t, host, tls_mode == 2);
		if (!tls)
		{
			bzy_db_set_error("MySQL TLS handshake or certificate verification failed.");
			x->t = NULL;   /* The upgrade already consumed + released the socket. */
			free(r.buf);
			return 0;
		}

		x->t = tls;
		x->is_tls = 1;
	}

	unsigned char auth[64];
	int authlen = compute_auth(hs.plugin, password, hs.scramble, auth);
	if (!send_handshake_response(x, rseq, user, db, hs.plugin, auth, authlen, tls_mode ? 1 : 0))
	{
		bzy_db_set_error("Failed to send the MySQL handshake response.");
		free(r.buf);
		return 0;
	}

	int ok = 0;
	for (;;)
	{
		if (!rd_packet(&r, &payload, &plen, &seq))
		{
			bzy_db_set_error("Connection closed during MySQL authentication.");
			break;
		}

		unsigned char marker = (plen >= 1) ? payload[0] : 0xff;
		if (marker == 0x00)
		{
			ok = 1;    /* OK. */
			break;
		}

		if (marker == 0xff)
		{
			set_error_from_err(payload, plen);
			break;
		}

		if (marker == 0xfe)   /* AuthSwitchRequest: 0xfe + plugin cstr + scramble. */
		{
			char newplugin[64];
			size_t i = 0;
			const unsigned char *q = payload + 1;
			const unsigned char *end = payload + plen;
			while (q < end && *q && i < sizeof(newplugin) - 1)
			{
				newplugin[i++] = (char)*q++;
			}

			newplugin[i] = '\0';
			if (q < end)
			{
				q++;    /* Step past the NUL. */
			}

			unsigned char newscr[20];
			memset(newscr, 0, 20);
			int64_t avail = end - q;
			if (avail > 20)
			{
				avail = 20;
			}

			if (avail > 0)
			{
				memcpy(newscr, q, (size_t)avail);
			}

			unsigned char a2[64];
			int a2len = compute_auth(newplugin, password, newscr, a2);
			if (!send_packet(x, seq + 1, (const char*)a2, a2len))
			{
				bzy_db_set_error("Failed to send the MySQL auth-switch response.");
				break;
			}
			continue;
		}

		if (marker == 0x01)   /* AuthMoreData (caching_sha2_password). */
		{
			unsigned char sub = (plen >= 2) ? payload[1] : 0;
			if (sub == 0x03)
			{
				continue;    /* fast_auth_success -> next packet is OK. */
			}

			if (sub == 0x04)                 /* full_auth: no cached entry on the server. */
			{
				if (!x->is_tls)
				{
					bzy_db_set_error("MySQL caching_sha2_password full authentication requires a TLS connection; use Mysql.connectTls (or a mysql_native_password account).");
					break;
				}
				/* Over TLS the cleartext password is safe; send it NUL-terminated. */
				if (!send_packet(x, seq + 1, password, (int64_t)strlen(password) + 1))
				{
					bzy_db_set_error("Failed to send the MySQL full-auth password.");
					break;
				}
				continue;   /* The next packet is OK (or ERR). */
			}

			bzy_db_set_error("Unsupported MySQL authentication continuation.");
			break;
		}

		bzy_db_set_error("Unexpected MySQL authentication response.");
		break;
	}

	free(r.buf);
	return ok;
}

/* ---- the MyConnection node + the Breezy entry points -------------------------- */

/* MyConnection: transport@24 (a Socket, or a TlsSocket after connectTls), is_tls@32,
   size 40, typeinfo {0,1,24} -- the single managed slot traces the transport whichever
   kind it is; is_tls is a plain int the GC ignores. */
#define MYC_SOCK 24
#define MYC_TLS  32
#define MYC_SIZE 40

static int64_t g_myc_ti[3] = { 0, 1, MYC_SOCK };
static int64_t g_myc_vt[2];
static int     g_myc_vt_built;

static void *myc_vtable(void)
{
	if (!g_myc_vt_built)
	{
		g_myc_vt[0] = (int64_t)&g_myc_ti[0];
		g_myc_vt_built = 1;
	}

	return &g_myc_vt[1];
}

/* Connect, handshake, and wrap the socket in a MyConnection. On any failure the
   error sink is set (the codegen-emitted bzy_db_check raises it) and NULL returns. */
void *bzy_my_connect(void *host, int64_t port, void *user, void *pass, void *db)
{
	void *sock = bzy_socket_connect(host, port);
	if (!sock)
	{
		bzy_db_set_error("Could not connect to the MySQL server.");
		return NULL;
	}

	Xport x = { sock, 0 };
	if (!bzy_my_run_handshake(&x, NULL, bzy_str_data(user), bzy_str_data(pass), bzy_str_data(db), 0))
	{
		bzy_socket_close(sock);
		bzy_release(sock);
		return NULL;
	}

	void *n = bzy_alloc(MYC_SIZE);
	*(void**)n = myc_vtable();
	*(void**)((char*)n + MYC_SOCK) = sock;   /* Transfer the +1 from bzy_socket_connect. */
	*(int*)((char*)n + MYC_TLS) = 0;
	return n;
}

/* Connect over TLS. MySQL negotiates the upgrade mid-handshake (read the greeting,
   send an SSLRequest with CLIENT_SSL, switch to TLS, then send the handshake response
   over the encrypted channel). insecure skips certificate + hostname verification
   (self-signed / dev). Over TLS, caching_sha2_password full authentication works too. */
static void *my_connect_tls(void *host, int64_t port, void *user, void *pass, void *db, int insecure)
{
	void *sock = bzy_socket_connect(host, port);
	if (!sock)
	{
		bzy_db_set_error("Could not connect to the MySQL server.");
		return NULL;
	}

	Xport x = { sock, 0 };
	if (!bzy_my_run_handshake(&x, bzy_str_data(host), bzy_str_data(user), bzy_str_data(pass),
							  bzy_str_data(db), insecure ? 2 : 1))
	{
		/* x.t is the current transport: the original socket if the upgrade had not
		   run, the TlsSocket if it had, or NULL if the upgrade itself failed (already
		   cleaned up). Tear down whatever remains. */
		if (x.t)
		{
			if (x.is_tls)
			{
				bzy_tls_close(x.t);
			}
			else
			{
				bzy_socket_close(x.t);
			}

			bzy_release(x.t);
		}

		return NULL;
	}

	void *n = bzy_alloc(MYC_SIZE);
	*(void**)n = myc_vtable();
	*(void**)((char*)n + MYC_SOCK) = x.t;   /* The TlsSocket; the +1 transferred. */
	*(int*)((char*)n + MYC_TLS) = 1;
	return n;
}

void *bzy_my_connect_tls(void *host, int64_t port, void *user, void *pass, void *db)
{
	return my_connect_tls(host, port, user, pass, db, 0);   /* Verify cert + hostname. */
}

void *bzy_my_connect_tls_insecure(void *host, int64_t port, void *user, void *pass, void *db)
{
	return my_connect_tls(host, port, user, pass, db, 1);   /* Skip verification. */
}

/* Send COM_QUIT and close the socket (idempotent; the node keeps its +1 Socket
   reference, released when the MyConnection is released). */
void bzy_my_close(void *conn)
{
	if (!conn)
	{
		return;
	}

	void *sock = *(void**)((char*)conn + MYC_SOCK);
	if (sock)
	{
		int is_tls = *(int*)((char*)conn + MYC_TLS);
		Xport x = { sock, is_tls };
		char quit[1] = { 0x01 };   /* COM_QUIT, packet seq 0. */
		send_packet(&x, 0, quit, 1);
		if (is_tls)
		{
			bzy_tls_close(sock);
		}
		else
		{
			bzy_socket_close(sock);
		}
	}
}

/* ---- the text-protocol query ------------------------------------------------- */

static void arr_set(void *arr, int64_t i, void *p)
{
	((void**)((char*)arr + 32))[i] = p;   /* Array slots live at +32. */
}

/* Decode a COM_QUERY response into a DbResult: a column count, that many
   ColumnDefinition41 packets (we keep the `name`), an EOF, then text rows until
   EOF, or an OK packet (non-row -> affected count) / ERR. Frees the reader buffer. */
static void *my_collect(Reader *r)
{
	unsigned char *payload;
	int64_t plen;
	int seq;
	if (!rd_packet(r, &payload, &plen, &seq))
	{
		bzy_db_set_error("Connection closed during the query.");
		free(r->buf);
		return NULL;
	}

	unsigned char m0 = (plen >= 1) ? payload[0] : 0xff;
	if (m0 == 0xff)
	{
		set_error_from_err(payload, plen);
		free(r->buf);
		return NULL;
	}

	if (m0 == 0x00)                                  /* OK packet: a non-row statement. */
	{
		int64_t adv;
		uint64_t affected = lenenc_int(payload + 1, &adv);
		free(r->buf);
		return bzy_db_result_new(NULL, NULL, (int64_t)affected);
	}

	if (m0 == 0xfb)
	{
		bzy_db_set_error("MySQL LOCAL INFILE is not supported.");
		free(r->buf);
		return NULL;
	}

	int64_t adv;
	uint64_t ncols = lenenc_int(payload, &adv);
	void *colnames = bzy_array_new((int64_t)ncols, 1);

	for (uint64_t c = 0; c < ncols; c++)             /* ColumnDefinition41 packets. */
	{
		if (!rd_packet(r, &payload, &plen, &seq))
		{
			bzy_db_set_error("Truncated MySQL column definitions.");
			bzy_release(colnames);
			free(r->buf);
			return NULL;
		}

		const unsigned char *p = payload;
		const unsigned char *end = payload + plen;
		int isnull;
		int64_t sl;
		lenenc_str(&p, end, &sl, &isnull);            /* catalog. */
		lenenc_str(&p, end, &sl, &isnull);            /* schema. */
		lenenc_str(&p, end, &sl, &isnull);            /* table. */
		lenenc_str(&p, end, &sl, &isnull);            /* org_table. */
		const char *name = lenenc_str(&p, end, &sl, &isnull);   /* name. */
		arr_set(colnames, (int64_t)c, bzy_str_new(name ? name : "", sl));
	}

	/* An EOF packet follows the column block (CLIENT_DEPRECATE_EOF is not set). */
	if (!rd_packet(r, &payload, &plen, &seq))
	{
		bzy_db_set_error("Truncated MySQL result (no column EOF).");
		bzy_release(colnames);
		free(r->buf);
		return NULL;
	}

	void  **rowbuf = NULL;
	int64_t nrows = 0, rowcap = 0;
	int     failed = 0;
	for (;;)
	{
		if (!rd_packet(r, &payload, &plen, &seq))
		{
			failed = 1;
			bzy_db_set_error("Connection closed mid-result.");
			break;
		}

		unsigned char m = (plen >= 1) ? payload[0] : 0;
		if (m == 0xfe && plen < 9)
		{
			break;    /* EOF: end of rows. */
		}

		if (m == 0xff)
		{
			set_error_from_err(payload, plen);
			failed = 1;
			break;
		}

		void *values = bzy_array_new((int64_t)ncols, 1);
		const unsigned char *p = payload;
		const unsigned char *end = payload + plen;
		for (uint64_t c = 0; c < ncols; c++)
		{
			int isnull;
			int64_t sl;
			const char *v = lenenc_str(&p, end, &sl, &isnull);
			if (!isnull)
			{
				arr_set(values, (int64_t)c, bzy_str_new(v ? v : "", sl));
			}
		}

		void *row = bzy_db_row_new(values, colnames);
		if (nrows >= rowcap)
		{
			rowcap = rowcap ? rowcap * 2 : 16;
			rowbuf = (void**)realloc(rowbuf, (size_t)rowcap * sizeof(void*));
		}

		rowbuf[nrows++] = row;
	}

	free(r->buf);
	if (failed)
	{
		for (int64_t i = 0; i < nrows; i++)
		{
			bzy_release(rowbuf[i]);
		}

		free(rowbuf);
		bzy_release(colnames);
		return NULL;
	}

	void *rows = bzy_array_new(nrows, 1);
	for (int64_t i = 0; i < nrows; i++)
	{
		arr_set(rows, i, rowbuf[i]);
	}

	free(rowbuf);
	return bzy_db_result_new(colnames, rows, nrows);
}

/* Run a COM_QUERY and decode the result. On a server error the error sink is set
   (the codegen-emitted bzy_db_check raises it) and NULL returns. */
void *bzy_my_query(void *conn, void *sql)
{
	if (!conn)
	{
		bzy_db_set_error("Query on a null connection.");
		return NULL;
	}

	Xport xp = { *(void**)((char*)conn + MYC_SOCK), *(int*)((char*)conn + MYC_TLS) };

	const char *s = bzy_str_data(sql);
	int64_t sl = bzy_str_len(sql);
	Wbuf w = { 0 };
	w_u8(&w, 0x03);                                  /* COM_QUERY. */
	w_bytes(&w, s, sl);
	int oks = send_packet(&xp, 0, w.p, w.len);
	free(w.p);
	if (!oks)
	{
		bzy_db_set_error("Failed to send the query.");
		return NULL;
	}

	Reader r = { 0 };
	r.x = &xp;
	return my_collect(&r);
}

/* ---- the prepared-statement (binary) protocol -------------------------------- */

/* Parse a ColumnDefinition41: 6 length-encoded strings (we keep `name`, the 5th),
   a 0x0c marker, then charset(2) column_length(4) type(1) flags(2) decimals(1).
   Sets *type and *is_unsigned; returns an owned name string. */
static void *parse_col_def(const unsigned char *payload, int64_t plen, int *type, int *is_unsigned)
{
	const unsigned char *p = payload;
	const unsigned char *end = payload + plen;
	int isnull;
	int64_t sl;
	lenenc_str(&p, end, &sl, &isnull);   /* catalog. */
	lenenc_str(&p, end, &sl, &isnull);   /* schema. */
	lenenc_str(&p, end, &sl, &isnull);   /* table. */
	lenenc_str(&p, end, &sl, &isnull);   /* org_table. */
	const char *name = lenenc_str(&p, end, &sl, &isnull);   /* name. */
	int64_t namelen = sl;
	lenenc_str(&p, end, &sl, &isnull);   /* org_name. */

	int64_t adv;
	lenenc_int(p, &adv);                 /* length of the fixed-length fields (0x0c). */
	p += adv;
	*type = 0xfd;                        /* Fallback: treat as a string. */
	*is_unsigned = 0;
	if (p + 9 <= end)
	{
		p += 2;                          /* character set. */
		p += 4;                          /* column length. */
		*type = *p;
		p += 1;
		uint32_t flags = le16(p);
		*is_unsigned = (flags & 0x0020) ? 1 : 0;   /* UNSIGNED_FLAG. */
	}

	return bzy_str_new(name ? name : "", namelen);
}

/* Decode one non-NULL binary-protocol value to its text form (the shared result
   model stores text). Fixed-width numerics and temporals are formatted; every other
   type (string/blob/decimal/bit/enum/set/json/geometry) is a length-encoded string. */
static void *decode_bin_value(const unsigned char **p, const unsigned char *end, int type, int is_unsigned)
{
	char tmp[64];
	switch (type)
	{
	case 0x01:   /* TINY. */
	{
		unsigned char b = (*p)[0];
		(*p) += 1;
		if (is_unsigned)
		{
			snprintf(tmp, sizeof(tmp), "%u", (unsigned)b);
		}
		else
		{
			snprintf(tmp, sizeof(tmp), "%d", (int)(int8_t)b);
		}
		return bzy_str_new(tmp, (int64_t)strlen(tmp));
	}
	case 0x02:   /* SHORT. */
	case 0x0d:   /* YEAR. */
	{
		uint32_t u = le16(*p);
		(*p) += 2;
		if (is_unsigned)
		{
			snprintf(tmp, sizeof(tmp), "%u", u);
		}
		else
		{
			snprintf(tmp, sizeof(tmp), "%d", (int)(int16_t)u);
		}
		return bzy_str_new(tmp, (int64_t)strlen(tmp));
	}
	case 0x03:   /* LONG. */
	case 0x09:   /* INT24. */
	{
		uint32_t u = le32(*p);
		(*p) += 4;
		if (is_unsigned)
		{
			snprintf(tmp, sizeof(tmp), "%u", u);
		}
		else
		{
			snprintf(tmp, sizeof(tmp), "%d", (int)(int32_t)u);
		}
		return bzy_str_new(tmp, (int64_t)strlen(tmp));
	}
	case 0x08:   /* LONGLONG. */
	{
		uint64_t u = le64(*p);
		(*p) += 8;
		if (is_unsigned)
		{
			snprintf(tmp, sizeof(tmp), "%llu", (unsigned long long)u);
		}
		else
		{
			snprintf(tmp, sizeof(tmp), "%lld", (long long)(int64_t)u);
		}
		return bzy_str_new(tmp, (int64_t)strlen(tmp));
	}
	case 0x04:   /* FLOAT. */
	{
		uint32_t bits = le32(*p);
		(*p) += 4;
		float f;
		memcpy(&f, &bits, 4);
		snprintf(tmp, sizeof(tmp), "%g", (double)f);
		return bzy_str_new(tmp, (int64_t)strlen(tmp));
	}
	case 0x05:   /* DOUBLE. */
	{
		uint64_t bits = le64(*p);
		(*p) += 8;
		double d;
		memcpy(&d, &bits, 8);
		snprintf(tmp, sizeof(tmp), "%.17g", d);
		return bzy_str_new(tmp, (int64_t)strlen(tmp));
	}
	case 0x0a:   /* DATE. */
	case 0x07:   /* TIMESTAMP. */
	case 0x0c:   /* DATETIME. */
	{
		unsigned char L = (*p)[0];
		(*p) += 1;
		int year = 0, mon = 0, day = 0, hh = 0, mm = 0, ss = 0;
		uint32_t micro = 0;
		if (L >= 4)
		{
			year = (int)le16(*p);
			mon = (*p)[2];
			day = (*p)[3];
		}
		if (L >= 7)
		{
			hh = (*p)[4];
			mm = (*p)[5];
			ss = (*p)[6];
		}
		if (L >= 11)
		{
			micro = le32(*p + 7);
		}
		(*p) += L;
		if (L == 0)
		{
			snprintf(tmp, sizeof(tmp), "0000-00-00");
		}
		else if (L == 4)
		{
			snprintf(tmp, sizeof(tmp), "%04d-%02d-%02d", year, mon, day);
		}
		else if (L >= 11)
		{
			snprintf(tmp, sizeof(tmp), "%04d-%02d-%02d %02d:%02d:%02d.%06u", year, mon, day, hh, mm, ss, micro);
		}
		else
		{
			snprintf(tmp, sizeof(tmp), "%04d-%02d-%02d %02d:%02d:%02d", year, mon, day, hh, mm, ss);
		}
		return bzy_str_new(tmp, (int64_t)strlen(tmp));
	}
	case 0x0b:   /* TIME. */
	{
		unsigned char L = (*p)[0];
		(*p) += 1;
		int neg = 0, hh = 0, mm = 0, ss = 0;
		uint32_t days = 0, micro = 0;
		if (L >= 8)
		{
			neg = (*p)[0];
			days = le32(*p + 1);
			hh = (*p)[5];
			mm = (*p)[6];
			ss = (*p)[7];
		}
		if (L >= 12)
		{
			micro = le32(*p + 8);
		}
		(*p) += L;
		long total_h = (long)days * 24 + hh;
		if (L == 0)
		{
			snprintf(tmp, sizeof(tmp), "00:00:00");
		}
		else if (L >= 12)
		{
			snprintf(tmp, sizeof(tmp), "%s%ld:%02d:%02d.%06u", neg ? "-" : "", total_h, mm, ss, micro);
		}
		else
		{
			snprintf(tmp, sizeof(tmp), "%s%ld:%02d:%02d", neg ? "-" : "", total_h, mm, ss);
		}
		return bzy_str_new(tmp, (int64_t)strlen(tmp));
	}
	default:     /* DECIMAL/VARCHAR/VAR_STRING/STRING/BLOB/BIT/ENUM/SET/JSON/... */
	{
		int64_t adv;
		uint64_t n = lenenc_int(*p, &adv);
		(*p) += adv;
		const char *s = (const char*)*p;
		if (*p + n > end)
		{
			n = (uint64_t)(end - *p);
		}
		(*p) += n;
		return bzy_str_new(s, (int64_t)n);
	}
	}
}

/* Decode the EXECUTE response (binary-protocol resultset) into a DbResult. */
static void *my_collect_binary(Reader *r)
{
	unsigned char *payload;
	int64_t plen;
	int seq;
	if (!rd_packet(r, &payload, &plen, &seq))
	{
		bzy_db_set_error("Connection closed during the query.");
		free(r->buf);
		return NULL;
	}

	unsigned char m0 = (plen >= 1) ? payload[0] : 0xff;
	if (m0 == 0xff)
	{
		set_error_from_err(payload, plen);
		free(r->buf);
		return NULL;
	}
	if (m0 == 0x00)                                  /* OK packet: a non-row statement. */
	{
		int64_t adv;
		uint64_t affected = lenenc_int(payload + 1, &adv);
		free(r->buf);
		return bzy_db_result_new(NULL, NULL, (int64_t)affected);
	}

	int64_t adv;
	uint64_t ncols = lenenc_int(payload, &adv);
	void *colnames = bzy_array_new((int64_t)ncols, 1);
	int *types = (int*)malloc((size_t)ncols * sizeof(int));
	int *uns = (int*)malloc((size_t)ncols * sizeof(int));

	for (uint64_t c = 0; c < ncols; c++)             /* Column definitions (with types). */
	{
		if (!rd_packet(r, &payload, &plen, &seq))
		{
			bzy_db_set_error("Truncated MySQL column definitions.");
			free(types);
			free(uns);
			bzy_release(colnames);
			free(r->buf);
			return NULL;
		}

		int ty, un;
		arr_set(colnames, (int64_t)c, parse_col_def(payload, plen, &ty, &un));
		types[c] = ty;
		uns[c] = un;
	}

	rd_packet(r, &payload, &plen, &seq);             /* EOF after the column block. */

	void  **rowbuf = NULL;
	int64_t nrows = 0, rowcap = 0;
	int     failed = 0;
	for (;;)
	{
		if (!rd_packet(r, &payload, &plen, &seq))
		{
			failed = 1;
			bzy_db_set_error("Connection closed mid-result.");
			break;
		}

		unsigned char m = (plen >= 1) ? payload[0] : 0;
		if (m == 0xfe && plen < 9)
		{
			break;    /* EOF: end of rows. */
		}
		if (m == 0xff)
		{
			set_error_from_err(payload, plen);
			failed = 1;
			break;
		}

		/* Binary row: a 0x00 header, a NULL bitmap, then each non-NULL value. */
		const unsigned char *p = payload + 1;
		const unsigned char *end = payload + plen;
		int64_t nbm = ((int64_t)ncols + 7 + 2) / 8;
		const unsigned char *bitmap = p;
		p += nbm;
		void *values = bzy_array_new((int64_t)ncols, 1);
		for (uint64_t c = 0; c < ncols; c++)
		{
			int null_bit = (bitmap[(c + 2) / 8] >> ((c + 2) % 8)) & 1;
			if (!null_bit)
			{
				arr_set(values, (int64_t)c, decode_bin_value(&p, end, types[c], uns[c]));
			}
		}

		void *row = bzy_db_row_new(values, colnames);
		if (nrows >= rowcap)
		{
			rowcap = rowcap ? rowcap * 2 : 16;
			rowbuf = (void**)realloc(rowbuf, (size_t)rowcap * sizeof(void*));
		}

		rowbuf[nrows++] = row;
	}

	free(types);
	free(uns);
	free(r->buf);
	if (failed)
	{
		for (int64_t i = 0; i < nrows; i++)
		{
			bzy_release(rowbuf[i]);
		}
		free(rowbuf);
		bzy_release(colnames);
		return NULL;
	}

	void *rows = bzy_array_new(nrows, 1);
	for (int64_t i = 0; i < nrows; i++)
	{
		arr_set(rows, i, rowbuf[i]);
	}
	free(rowbuf);
	return bzy_db_result_new(colnames, rows, nrows);
}

/* Run a parameterized query via the prepared-statement protocol: PREPARE, EXECUTE
   with the values bound as text-format (VAR_STRING) parameters -- never spliced into
   SQL, so injection-safe -- then CLOSE. `params` is a managed string[]; a NULL slot
   binds SQL NULL. */
void *bzy_my_query_params(void *conn, void *sql, void *params)
{
	if (!conn)
	{
		bzy_db_set_error("Query on a null connection.");
		return NULL;
	}
	Xport xp = { *(void**)((char*)conn + MYC_SOCK), *(int*)((char*)conn + MYC_TLS) };

	int64_t nparams = params ? *(int64_t*)((char*)params + 24) : 0;
	void  **pslots = params ? (void**)((char*)params + 32) : NULL;

	/* COM_STMT_PREPARE. */
	{
		const char *s = bzy_str_data(sql);
		int64_t sl = bzy_str_len(sql);
		Wbuf w = { 0 };
		w_u8(&w, 0x16);
		w_bytes(&w, s, sl);
		int oks = send_packet(&xp, 0, w.p, w.len);
		free(w.p);
		if (!oks)
		{
			bzy_db_set_error("Failed to send the prepare.");
			return NULL;
		}
	}

	Reader r = { 0 };
	r.x = &xp;
	unsigned char *payload;
	int64_t plen;
	int seq;
	if (!rd_packet(&r, &payload, &plen, &seq))
	{
		bzy_db_set_error("Connection closed during prepare.");
		free(r.buf);
		return NULL;
	}

	if (plen >= 1 && payload[0] == 0xff)
	{
		set_error_from_err(payload, plen);
		free(r.buf);
		return NULL;
	}

	/* COM_STMT_PREPARE_OK: 0x00, statement id (4), columns (2), params (2), ... */
	uint32_t stmt_id = le32(payload + 1);
	int num_cols = (int)le16(payload + 5);
	int num_params = (int)le16(payload + 7);

	for (int i = 0; i < num_params; i++)
	{
		rd_packet(&r, &payload, &plen, &seq);    /* Param defs. */
	}
	if (num_params > 0)
	{
		rd_packet(&r, &payload, &plen, &seq);    /* EOF. */
	}
	for (int i = 0; i < num_cols; i++)
	{
		rd_packet(&r, &payload, &plen, &seq);    /* Col defs (re-read at EXECUTE). */
	}
	if (num_cols > 0)
	{
		rd_packet(&r, &payload, &plen, &seq);    /* EOF. */
	}

	/* COM_STMT_EXECUTE. */
	{
		Wbuf w = { 0 };
		w_u8(&w, 0x17);
		w_le32(&w, stmt_id);
		w_u8(&w, 0x00);                  /* Flags: no cursor. */
		w_le32(&w, 1);                   /* Iteration count. */
		if (nparams > 0)
		{
			int64_t nbm = (nparams + 7) / 8;
			for (int64_t i = 0; i < nbm; i++)
			{
				unsigned char byte = 0;
				for (int b = 0; b < 8; b++)
				{
					int64_t idx = i * 8 + b;
					if (idx < nparams && pslots[idx] == NULL)
					{
						byte |= (unsigned char)(1 << b);
					}
				}

				w_u8(&w, byte);
			}

			w_u8(&w, 0x01);              /* new_params_bound_flag. */
			for (int64_t i = 0; i < nparams; i++)
			{
				w_u8(&w, 0xfd);          /* MYSQL_TYPE_VAR_STRING. */
				w_u8(&w, 0x00);          /* Unsigned flag. */
			}

			for (int64_t i = 0; i < nparams; i++)
			{
				void *v = pslots[i];
				if (v)
				{
					int64_t vl = bzy_str_len(v);
					w_lenenc(&w, (uint64_t)vl);
					w_bytes(&w, bzy_str_data(v), vl);
				}
			}
		}

		int oks = send_packet(&xp, 0, w.p, w.len);
		free(w.p);
		if (!oks)
		{
			bzy_db_set_error("Failed to send the execute.");
			free(r.buf);
			return NULL;
		}
	}

	void *result = my_collect_binary(&r);   /* Frees r.buf. */

	/* COM_STMT_CLOSE (best effort; the result is already decoded). */
	{
		Wbuf w = { 0 };
		w_u8(&w, 0x19);
		w_le32(&w, stmt_id);
		send_packet(&xp, 0, w.p, w.len);
		free(w.p);
	}

	return result;
}
