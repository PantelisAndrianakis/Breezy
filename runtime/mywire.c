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

/* Client capability flags we advertise. */
#define CLIENT_LONG_PASSWORD     0x00000001u
#define CLIENT_LONG_FLAG         0x00000004u
#define CLIENT_CONNECT_WITH_DB   0x00000008u
#define CLIENT_PROTOCOL_41       0x00000200u
#define CLIENT_TRANSACTIONS      0x00002000u
#define CLIENT_SECURE_CONNECTION 0x00008000u
#define CLIENT_PLUGIN_AUTH       0x00080000u

/* ---- little-endian readers --------------------------------------------------- */

static uint32_t le24(const unsigned char *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16); }

/* (le16/le32 + the length-encoded-integer decoder arrive with the query decode,
   Task 6; w_le16 with the prepared-param types, Task 7.) */

/* ---- the buffered packet reader ---------------------------------------------- */

typedef struct
{
	void   *sock;
	char   *buf;
	int64_t len;
	int64_t cap;
	int64_t pos;
	int     eof;
} Reader;

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

static int rd_need(Reader *r, int64_t need)
{
	while (r->len - r->pos < need)
	{
		if (!rd_fill(r)) { return 0; }
	}

	return 1;
}

/* Read one packet: a 4-byte header (3-byte LE length + 1-byte sequence) then the
   payload. *payload points into the reader buffer (valid until the next rd_packet);
   *plen is the payload length, *seq the sequence number. 0 at EOF. */
static int rd_packet(Reader *r, unsigned char **payload, int64_t *plen, int *seq)
{
	if (!rd_need(r, 4)) { return 0; }
	const unsigned char *h = (const unsigned char*)(r->buf + r->pos);
	int64_t L = le24(h);
	*seq = h[3];
	if (!rd_need(r, 4 + L)) { return 0; }
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

static void w_u8(Wbuf *w, unsigned char v) { w_bytes(w, &v, 1); }

static void w_le32(Wbuf *w, uint32_t v)
{
	unsigned char b[4] = { (unsigned char)v, (unsigned char)(v >> 8), (unsigned char)(v >> 16), (unsigned char)(v >> 24) };
	w_bytes(w, b, 4);
}

static void w_cstr(Wbuf *w, const char *s) { w_bytes(w, s, (int64_t)strlen(s) + 1); }

static void w_zero(Wbuf *w, int n)
{
	for (int i = 0; i < n; i++) { w_u8(w, 0); }
}

/* Send a payload as one packet: a 3-byte LE length + the sequence byte + payload. */
static int send_packet(void *sock, int seq, const char *payload, int64_t plen)
{
	Wbuf h = { 0 };
	w_u8(&h, (unsigned char)plen);
	w_u8(&h, (unsigned char)(plen >> 8));
	w_u8(&h, (unsigned char)(plen >> 16));
	w_u8(&h, (unsigned char)seq);
	w_bytes(&h, payload, plen);
	int64_t rc = bzy_sock_send_all(sock, h.p, h.len);
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
	if (msglen > (int64_t)sizeof(buf) - n - 1) { msglen = (int64_t)sizeof(buf) - n - 1; }
	if (msglen < 0) { msglen = 0; }
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

	if (plen < 1 || *p != 10) { return 0; }        /* Protocol version 10. */
	p++;
	while (p < end && *p) { p++; }                 /* server version cstr. */
	p++;
	if (p + 4 > end) { return 0; }
	p += 4;                                        /* connection id. */
	if (p + 8 > end) { return 0; }
	memcpy(hs->scramble, p, 8);                    /* auth-plugin-data part 1. */
	p += 8;
	p += 1;                                        /* filler. */
	if (p + 2 > end) { return 1; }                 /* No extended part; scramble is short. */
	p += 2;                                        /* capability flags lower. */

	if (p + 1 > end) { return 1; }
	p += 1;                                        /* character set. */
	if (p + 2 > end) { return 1; }
	p += 2;                                        /* status flags. */
	if (p + 2 > end) { return 1; }
	p += 2;                                        /* capability flags upper. */
	int auth_data_len = (p < end) ? *p : 0;
	p += 1;
	p += 10;                                       /* reserved. */

	int part2 = auth_data_len ? auth_data_len - 8 : 13;
	int take = part2 - 1;                          /* Drop the trailing NUL. */
	if (take > 12) { take = 12; }
	if (take < 0) { take = 0; }
	if (p + take <= end) { memcpy(hs->scramble + 8, p, take); }
	p += part2;

	if (p < end)                                   /* auth plugin name cstr. */
	{
		size_t i = 0;
		while (p < end && *p && i < sizeof(hs->plugin) - 1) { hs->plugin[i++] = (char)*p++; }
		hs->plugin[i] = '\0';
	}

	return 1;
}

/* Build + send the Handshake Response. `auth` is the computed auth-response (empty
   until Task 5 fills it per the plugin); `seq` is the handshake packet's seq + 1. */
static int send_handshake_response(void *sock, int seq, const char *user, const char *db,
                                   const char *plugin, const unsigned char *auth, int authlen)
{
	uint32_t caps = CLIENT_LONG_PASSWORD | CLIENT_LONG_FLAG | CLIENT_PROTOCOL_41
	              | CLIENT_TRANSACTIONS | CLIENT_SECURE_CONNECTION | CLIENT_PLUGIN_AUTH;
	if (db && db[0]) { caps |= CLIENT_CONNECT_WITH_DB; }

	Wbuf w = { 0 };
	w_le32(&w, caps);
	w_le32(&w, 0x01000000);          /* Max packet size 16 MiB. */
	w_u8(&w, 45);                    /* utf8mb4_general_ci. */
	w_zero(&w, 23);                  /* Reserved. */
	w_cstr(&w, user);
	w_u8(&w, (unsigned char)authlen);   /* CLIENT_SECURE_CONNECTION: 1-byte length + data. */
	if (authlen) { w_bytes(&w, auth, authlen); }
	if (db && db[0]) { w_cstr(&w, db); }
	w_cstr(&w, plugin);

	int ok = send_packet(sock, seq, w.p, w.len);
	free(w.p);
	return ok;
}

/* Run the handshake on a connected socket: read the server greeting, send the
   response, read OK/ERR. Returns 1 on success, 0 with the error sink set. Real
   auth (a non-empty auth-response + AuthSwitch/AuthMoreData) lands in Task 5; here
   the auth-response is empty, which a no-password account accepts. */
int bzy_my_run_handshake(void *sock, const char *user, const char *password, const char *db)
{
	(void)password;
	Reader r = { 0 };
	r.sock = sock;

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

	unsigned char empty[1] = { 0 };
	if (!send_handshake_response(sock, seq + 1, user, db, hs.plugin, empty, 0))
	{
		bzy_db_set_error("Failed to send the MySQL handshake response.");
		free(r.buf);
		return 0;
	}

	int ok = 0;
	if (rd_packet(&r, &payload, &plen, &seq))
	{
		if (plen >= 1 && payload[0] == 0x00) { ok = 1; }                 /* OK. */
		else if (plen >= 1 && payload[0] == 0xff) { set_error_from_err(payload, plen); }
		else { bzy_db_set_error("MySQL authentication required (Task 5)."); }
	}
	else
	{
		bzy_db_set_error("Connection closed during the MySQL handshake.");
	}

	free(r.buf);
	return ok;
}

/* ---- the MyConnection node + the Breezy entry points -------------------------- */

/* MyConnection: sock@24, size 32, typeinfo {0,1,24} (one managed slot). */
#define MYC_SOCK 24
#define MYC_SIZE 32

static int64_t g_myc_ti[3] = { 0, 1, MYC_SOCK };
static int64_t g_myc_vt[2];
static int     g_myc_vt_built;

static void *myc_vtable(void)
{
	if (!g_myc_vt_built) { g_myc_vt[0] = (int64_t)&g_myc_ti[0]; g_myc_vt_built = 1; }
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

	if (!bzy_my_run_handshake(sock, bzy_str_data(user), bzy_str_data(pass), bzy_str_data(db)))
	{
		bzy_socket_close(sock);
		bzy_release(sock);
		return NULL;
	}

	void *n = bzy_alloc(MYC_SIZE);
	*(void**)n = myc_vtable();
	*(void**)((char*)n + MYC_SOCK) = sock;   /* Transfer the +1 from bzy_socket_connect. */
	return n;
}

/* Send COM_QUIT and close the socket (idempotent; the node keeps its +1 Socket
   reference, released when the MyConnection is released). */
void bzy_my_close(void *conn)
{
	if (!conn) { return; }
	void *sock = *(void**)((char*)conn + MYC_SOCK);
	if (sock)
	{
		char quit[1] = { 0x01 };   /* COM_QUIT, packet seq 0. */
		send_packet(sock, 0, quit, 1);
		bzy_socket_close(sock);
	}
}
