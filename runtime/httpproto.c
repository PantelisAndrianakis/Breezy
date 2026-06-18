/* runtime/httpproto.c -- HTTP/1.1 message codec (RFC 7230) over a Breezy Socket.
   Own TU: a program that never names Http links none of this. Parses requests and
   responses into managed HttpRequest/HttpResponse nodes; headers are parallel
   string arrays scanned case-insensitively. Distinct from runtime/http.c (readUrl).

   This file is Task 2 of the Phase E.5 plan: the node descriptors, the buffered
   socket reader, and the request parser. The Breezy entry, accessors, builders,
   and the response parser land in Tasks 3-7. */
#include "breezy.h"
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>

/* Reactor-parked raw byte I/O on a plain Socket (shared from socket.c). */
extern int bzy_sock_recv(void *s, char *buf, int max, int64_t timeout_ms);
extern int64_t bzy_sock_send_all(void *s, const char *buf, int64_t len);

/* HttpRequest:  method@24 path@32 hnames@40 hvals@48 body@56 version@64, size 72.
   HttpResponse: status@24 reason@32 hnames@40 hvals@48 body@56,           size 72. */
#define H_METHOD 24
#define H_PATH   32
#define H_STATUS 24
#define H_REASON 32
#define H_HNAMES 40
#define H_HVALS  48
#define H_BODY   56
#define H_VERSION 64
#define H_SIZE   72

#define HSET(n, off, v) (*(void**)((char*)(n) + (off)) = (void*)(v))
#define HGET(n, off)    (*(void**)((char*)(n) + (off)))

/* Request typeinfo: 6 managed children. Response: 4 (status@24 is a plain int). */
static int64_t g_req_ti[8] = { 0, 6, H_METHOD, H_PATH, H_HNAMES, H_HVALS, H_BODY, H_VERSION };
static int64_t g_req_vt[2];
static int     g_req_vt_built;
static int64_t g_resp_ti[6] = { 0, 4, H_REASON, H_HNAMES, H_HVALS, H_BODY };
static int64_t g_resp_vt[2];
static int     g_resp_vt_built;

static void *req_vtable(void)
{
	if (!g_req_vt_built) { g_req_vt[0] = (int64_t)&g_req_ti[0]; g_req_vt_built = 1; }
	return &g_req_vt[1];
}

static void *resp_vtable(void)
{
	if (!g_resp_vt_built) { g_resp_vt[0] = (int64_t)&g_resp_ti[0]; g_resp_vt_built = 1; }
	return &g_resp_vt[1];
}

static void *req_new(void)  { void *n = bzy_alloc(H_SIZE); *(void**)n = req_vtable();  return n; }
static void *resp_new(void) { void *n = bzy_alloc(H_SIZE); *(void**)n = resp_vtable(); return n; }

static void arr_set(void *arr, int64_t i, void *p) { ((void**)((char*)arr + 32))[i] = p; }

/* ---- the buffered socket reader --------------------------------------------- */

typedef struct { void *sock; char *buf; int64_t len, cap, pos; int eof; const char *err; } Reader;

/* Append more bytes from the socket; sets eof at a clean close. Returns 1 if any
   bytes were added, 0 at EOF. */
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

/* Index of the next CRLF at/after `from`, filling as needed; -1 at EOF before a
   line completes. */
static int64_t rd_find_crlf(Reader *r, int64_t from)
{
	int64_t scan = from;
	for (;;)
	{
		for (int64_t i = scan; i + 1 < r->len; i++)
		{
			if (r->buf[i] == '\r' && r->buf[i + 1] == '\n') { return i; }
		}

		scan = r->len > 0 ? r->len - 1 : 0;   /* A CR may sit at the fill boundary. */
		if (!rd_fill(r)) { return -1; }
	}
}

/* Ensure at least `need` bytes are buffered from pos; 1 ok, 0 EOF. */
static int rd_ensure(Reader *r, int64_t need)
{
	while (r->len - r->pos < need)
	{
		if (!rd_fill(r)) { return 0; }
	}

	return 1;
}

/* ---- header collection (parallel owned-string arrays, XML-attr style) -------- */

typedef struct { void **names; void **vals; int count, cap; } Hdrs;

static void hdr_push(Hdrs *h, void *name, void *val)
{
	if (h->count >= h->cap)
	{
		h->cap = h->cap ? h->cap * 2 : 8;
		h->names = (void**)realloc(h->names, h->cap * sizeof(void*));
		h->vals  = (void**)realloc(h->vals,  h->cap * sizeof(void*));
	}

	h->names[h->count] = name;
	h->vals[h->count] = val;
	h->count++;
}

static void hdr_free(Hdrs *h)
{
	for (int i = 0; i < h->count; i++) { bzy_release(h->names[i]); bzy_release(h->vals[i]); }
	free(h->names);
	free(h->vals);
}

/* Move collected headers into a node's hnames/hvals arrays (transfers ownership). */
static void hdr_store(void *node, Hdrs *h)
{
	if (h->count > 0)
	{
		void *names = bzy_array_new(h->count, 1);
		void *vals  = bzy_array_new(h->count, 1);
		for (int i = 0; i < h->count; i++) { arr_set(names, i, h->names[i]); arr_set(vals, i, h->vals[i]); }
		HSET(node, H_HNAMES, names);
		HSET(node, H_HVALS, vals);
	}

	free(h->names);   /* Spines only; the slots moved into the arrays. */
	free(h->vals);
}

/* Case-insensitive header lookup over a node's parallel arrays; -1 if absent. */
static int64_t hdr_index(void *node, void *name)
{
	void *names = HGET(node, H_HNAMES);
	if (!names) { return -1; }
	int64_t n = *(int64_t*)((char*)names + 24);
	void **slots = (void**)((char*)names + 32);
	const char *k = bzy_str_data(name);
	int64_t kl = bzy_str_len(name);
	for (int64_t i = 0; i < n; i++)
	{
		if (bzy_str_len(slots[i]) == kl)
		{
			const char *p = bzy_str_data(slots[i]);
			int eq = 1;
			for (int64_t j = 0; j < kl; j++)
			{
				if (tolower((unsigned char)p[j]) != tolower((unsigned char)k[j])) { eq = 0; break; }
			}

			if (eq) { return i; }
		}
	}

	return -1;
}

/* The header value string at index i (borrowed). */
static void *hdr_val_at(void *node, int64_t i)
{
	return ((void**)((char*)HGET(node, H_HVALS) + 32))[i];
}

/* ---- parsing a message (shared header + body machinery) --------------------- */

/* Parse the header block from pos (after the start line) into h; advance pos past
   the terminating blank line. Returns 1 ok, 0 on error (r->err set). */
static int parse_headers(Reader *r, Hdrs *h)
{
	for (;;)
	{
		int64_t cr = rd_find_crlf(r, r->pos);
		if (cr < 0) { r->err = "Unterminated header block."; return 0; }
		if (cr == r->pos) { r->pos = cr + 2; return 1; }   /* Blank line: end of headers. */

		int64_t colon = -1;
		for (int64_t i = r->pos; i < cr; i++) { if (r->buf[i] == ':') { colon = i; break; } }
		if (colon < 0) { r->err = "Malformed header line."; return 0; }

		void *name = bzy_str_new(r->buf + r->pos, colon - r->pos);
		int64_t vs = colon + 1, ve = cr;
		while (vs < ve && (r->buf[vs] == ' ' || r->buf[vs] == '\t')) { vs++; }   /* Trim OWS. */
		while (ve > vs && (r->buf[ve - 1] == ' ' || r->buf[ve - 1] == '\t')) { ve--; }
		void *val = bzy_str_new(r->buf + vs, ve - vs);
		hdr_push(h, name, val);
		r->pos = cr + 2;
	}
}

/* True if the value string contains "chunked" (case-insensitive, ASCII). */
static int has_chunked(void *val)
{
	const char *p = bzy_str_data(val);
	int64_t n = bzy_str_len(val);
	for (int64_t i = 0; i + 7 <= n; i++)
	{
		if (tolower((unsigned char)p[i]) == 'c'
			&& tolower((unsigned char)p[i + 1]) == 'h'
			&& tolower((unsigned char)p[i + 2]) == 'u'
			&& tolower((unsigned char)p[i + 3]) == 'n'
			&& tolower((unsigned char)p[i + 4]) == 'k'
			&& tolower((unsigned char)p[i + 5]) == 'e'
			&& tolower((unsigned char)p[i + 6]) == 'd')
		{
			return 1;
		}
	}

	return 0;
}

/* Read the body into an owned string per the node's framing headers: chunked
   transfer-encoding, else Content-Length, else empty. Returns the owned string, or
   NULL with r->err set. */
static void *parse_body(Reader *r, void *node)
{
	void *kte = bzy_str_new("Transfer-Encoding", 17);
	void *kcl = bzy_str_new("Content-Length", 14);
	int64_t ite = hdr_index(node, kte), icl = hdr_index(node, kcl);
	void *te = ite >= 0 ? hdr_val_at(node, ite) : NULL;
	void *cl = icl >= 0 ? hdr_val_at(node, icl) : NULL;
	bzy_release(kte);
	bzy_release(kcl);

	if (te && has_chunked(te))
	{
		char *out = NULL;
		size_t olen = 0, ocap = 0;
		for (;;)
		{
			int64_t cr = rd_find_crlf(r, r->pos);
			if (cr < 0) { free(out); r->err = "Bad chunk size."; return NULL; }
			long sz = strtol(r->buf + r->pos, NULL, 16);
			r->pos = cr + 2;
			if (sz < 0) { free(out); r->err = "Bad chunk size."; return NULL; }
			if (sz == 0) { break; }
			if (!rd_ensure(r, sz + 2)) { free(out); r->err = "Truncated chunk."; return NULL; }
			if (olen + (size_t)sz > ocap) { ocap = (olen + (size_t)sz) * 2 + 16; out = (char*)realloc(out, ocap); }
			memcpy(out + olen, r->buf + r->pos, (size_t)sz);
			olen += (size_t)sz;
			r->pos += sz + 2;   /* Chunk data + its trailing CRLF. */
		}

		int64_t cr = rd_find_crlf(r, r->pos);   /* CRLF after the 0-chunk (trailers ignored). */
		if (cr >= 0) { r->pos = cr + 2; }
		void *s = bzy_str_new(out ? out : "", (int64_t)olen);
		free(out);
		return s;
	}

	int64_t want = 0;
	if (cl) { want = strtoll(bzy_str_data(cl), NULL, 10); if (want < 0) { want = 0; } }
	if (want == 0) { return bzy_str_new("", 0); }
	if (!rd_ensure(r, want)) { r->err = "Body shorter than Content-Length."; return NULL; }
	void *s = bzy_str_new(r->buf + r->pos, want);
	r->pos += want;
	return s;
}

/* Parse one request: METHOD SP PATH SP VERSION CRLF, headers, body. Returns an
   owned HttpRequest, NULL on a clean boundary EOF (*was_eof=1), or NULL with
   r->err set on a malformed message. */
static void *parse_request(Reader *r, int *was_eof)
{
	*was_eof = 0;
	int64_t cr = rd_find_crlf(r, 0);
	if (cr < 0)
	{
		if (r->len == 0) { *was_eof = 1; return NULL; }   /* Clean close at a boundary. */
		r->err = "Unterminated request line.";
		return NULL;
	}

	int64_t sp1 = -1, sp2 = -1;
	for (int64_t i = 0; i < cr; i++)
	{
		if (r->buf[i] == ' ')
		{
			if (sp1 < 0) { sp1 = i; }
			else { sp2 = i; break; }
		}
	}

	if (sp1 < 0 || sp2 < 0) { r->err = "Malformed request line."; return NULL; }

	void *node = req_new();
	HSET(node, H_METHOD,  bzy_str_new(r->buf, sp1));
	HSET(node, H_PATH,    bzy_str_new(r->buf + sp1 + 1, sp2 - sp1 - 1));
	HSET(node, H_VERSION, bzy_str_new(r->buf + sp2 + 1, cr - sp2 - 1));
	r->pos = cr + 2;

	Hdrs h = { 0 };
	if (!parse_headers(r, &h)) { hdr_free(&h); bzy_release(node); return NULL; }
	hdr_store(node, &h);

	void *body = parse_body(r, node);
	if (!body) { bzy_release(node); return NULL; }
	HSET(node, H_BODY, body);
	return node;
}

/* Exported so the not-yet-written Breezy entry (Task 3) references it -- keeps the
   parser + helpers reachable (no unused-static warnings) until then. */
void *bzy_http_parse_request_reader(void *sock, int *was_eof, const char **err)
{
	Reader r = { 0 };
	r.sock = sock;
	void *node = parse_request(&r, was_eof);
	*err = r.err;
	free(r.buf);
	return node;
}

/* Touch resp_new so the response vtable path is not flagged unused before Task 7. */
void *bzy_http_new_response_stub(void)
{
	return resp_new();
}
