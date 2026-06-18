/* runtime/json.c -- JSON read + serialize (RFC 8259). Own TU: a program that
   never names Json links none of this. The parser builds a tree of managed
   JsonValue nodes; arrays are List<JsonValue>, objects map<string,JsonValue>.

   This file is Task 2 of the Phase E.4 plan: the node descriptor + the
   recursive-descent parser. The Breezy entry, accessors, lifters, and the
   serializer land in Tasks 3-8. */
#include "breezy.h"
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <math.h>

/* Emitted per-program by codegen; bzy_json_check stamps a thrown JsonException
   with it (the bzy_number_check pattern). */
extern char __vtable_JsonException[];
extern void bzy_throw(void *exc, int64_t pc, int64_t frame);

/* JsonValue layout: vtable|rc|gcinfo|kind@24|payload_managed@32|payload_scalar@40. */
#define J_KIND 24
#define J_MAN  32
#define J_SCA  40
#define J_SIZE 48
enum { JK_NULL = 0, JK_BOOL = 1, JK_INT = 2, JK_DBL = 3, JK_STR = 4, JK_ARR = 5, JK_OBJ = 6 };

#define JSET_MAN(v, p) (*(void**)((char*)(v) + J_MAN) = (void*)(p))
#define JGET_MAN(v)    (*(void**)((char*)(v) + J_MAN))
#define JKIND(v)       (*(int64_t*)((char*)(v) + J_KIND))
#define JSCA(v)        (*(int64_t*)((char*)(v) + J_SCA))

/* Managed-node typeinfo: {finalizer=0, child_count=1, managed payload @32}. The
   scalar slot is a plain word, not a managed child, so ARC + the cycle collector
   trace only the one payload reference (NULL-safely). */
static int64_t g_json_ti[3] = { 0, 1, J_MAN };
static int64_t g_json_vt[2];
static int     g_json_vt_built;

static void *json_vtable(void)
{
	if (!g_json_vt_built)
	{
		g_json_vt[0] = (int64_t)&g_json_ti[0];
		g_json_vt_built = 1;
	}

	return &g_json_vt[1];   /* The object stores this; [stored-8] == &g_json_ti[0]. */
}

static void *jv_new(int kind)
{
	void *v = bzy_alloc(J_SIZE);            /* Zeroed; rc=1; gcinfo set by alloc. */
	*(void**)v = json_vtable();
	JKIND(v) = kind;
	return v;
}

/* Store a double's bit pattern into the scalar slot (and read it back). */
static void jv_set_double(void *v, double d)
{
	union { double d; int64_t i; } u;
	u.d = d;
	JSCA(v) = u.i;
}

/* ---- scanner (mirrors runtime/xml.c's Scan) --------------------------------- */

typedef struct
{
	const char *p, *end;
	int64_t line, col;
	const char *err;
} Scan;

static void fail(Scan *s, const char *msg)
{
	if (!s->err)
	{
		s->err = msg;
	}
}

static int at_end(Scan *s) { return s->p >= s->end || s->err != NULL; }

static int peek(Scan *s) { return s->p < s->end ? (unsigned char)*s->p : -1; }

static void advance(Scan *s)
{
	if (s->p < s->end)
	{
		if (*s->p == '\n') { s->line++; s->col = 1; }
		else { s->col++; }
		s->p++;
	}
}

static void skip_ws(Scan *s)
{
	while (!at_end(s))
	{
		int c = peek(s);
		if (c == ' ' || c == '\t' || c == '\r' || c == '\n') { advance(s); }
		else { break; }
	}
}

/* ---- a growable text buffer (for decoded strings) --------------------------- */

typedef struct { char *data; size_t len, cap; } TextBuf;

static void tb_push(TextBuf *t, const char *bytes, size_t n)
{
	if (t->len + n > t->cap)
	{
		size_t nc = t->cap ? t->cap : 32;
		while (nc < t->len + n) { nc *= 2; }
		t->data = (char*)realloc(t->data, nc);
		t->cap = nc;
	}

	memcpy(t->data + t->len, bytes, n);
	t->len += n;
}

/* Append a Unicode code point to the buffer as UTF-8 (1-4 bytes). */
static void append_utf8(TextBuf *t, long cp)
{
	unsigned char b[4];
	int n;
	if (cp < 0x80) { b[0] = (unsigned char)cp; n = 1; }
	else if (cp < 0x800) { b[0] = 0xC0 | (cp >> 6); b[1] = 0x80 | (cp & 0x3F); n = 2; }
	else if (cp < 0x10000) { b[0] = 0xE0 | (cp >> 12); b[1] = 0x80 | ((cp >> 6) & 0x3F); b[2] = 0x80 | (cp & 0x3F); n = 3; }
	else { b[0] = 0xF0 | (cp >> 18); b[1] = 0x80 | ((cp >> 12) & 0x3F); b[2] = 0x80 | ((cp >> 6) & 0x3F); b[3] = 0x80 | (cp & 0x3F); n = 4; }
	tb_push(t, (char*)b, (size_t)n);
}

/* Read four hex digits at the cursor into a code unit; -1 on a non-hex digit. */
static int parse_hex4(Scan *s)
{
	int v = 0;
	for (int i = 0; i < 4; i++)
	{
		int c = peek(s), d;
		if (c >= '0' && c <= '9') { d = c - '0'; }
		else if (c >= 'a' && c <= 'f') { d = c - 'a' + 10; }
		else if (c >= 'A' && c <= 'F') { d = c - 'A' + 10; }
		else { return -1; }
		v = v * 16 + d;
		advance(s);
	}

	return v;
}

static void *parse_value(Scan *s);   /* Forward. */

/* Parse a JSON string at the opening '"'. Returns an owned (+1) Breezy string,
   or NULL with the error set. Decodes the JSON escapes incl. \uXXXX surrogate
   pairs into UTF-8. */
static void *parse_string(Scan *s)
{
	if (peek(s) != '"') { fail(s, "Expected a string."); return NULL; }
	advance(s);   /* '"'. */

	TextBuf tb = { 0 };
	for (;;)
	{
		if (at_end(s)) { free(tb.data); fail(s, "Unterminated string."); return NULL; }
		int c = peek(s);
		if (c == '"') { advance(s); break; }
		if (c == '\\')
		{
			advance(s);
			int e = peek(s);
			char rep;
			switch (e)
			{
				case '"':  rep = '"';  break;
				case '\\': rep = '\\'; break;
				case '/':  rep = '/';  break;
				case 'b':  rep = '\b'; break;
				case 'f':  rep = '\f'; break;
				case 'n':  rep = '\n'; break;
				case 'r':  rep = '\r'; break;
				case 't':  rep = '\t'; break;
				case 'u':
				{
					advance(s);   /* 'u'. */
					int u = parse_hex4(s);
					if (u < 0) { free(tb.data); fail(s, "Bad \\u escape."); return NULL; }
					long cp = u;
					if (u >= 0xD800 && u <= 0xDBFF)   /* High surrogate: expect a low one. */
					{
						if (peek(s) != '\\') { free(tb.data); fail(s, "Unpaired surrogate."); return NULL; }
						advance(s);
						if (peek(s) != 'u') { free(tb.data); fail(s, "Unpaired surrogate."); return NULL; }
						advance(s);
						int lo = parse_hex4(s);
						if (lo < 0xDC00 || lo > 0xDFFF) { free(tb.data); fail(s, "Unpaired surrogate."); return NULL; }
						cp = 0x10000 + (((long)u - 0xD800) << 10) + (lo - 0xDC00);
					}

					append_utf8(&tb, cp);
					continue;   /* The escape consumed its own bytes. */
				}
				default: free(tb.data); fail(s, "Bad escape."); return NULL;
			}

			tb_push(&tb, &rep, 1);
			advance(s);   /* The escaped char. */
		}
		else if ((unsigned char)c < 0x20)
		{
			free(tb.data);
			fail(s, "Control character in string.");
			return NULL;
		}
		else
		{
			const char *seg = s->p;
			while (!at_end(s) && peek(s) != '"' && peek(s) != '\\' && (unsigned char)peek(s) >= 0x20) { advance(s); }
			tb_push(&tb, seg, (size_t)(s->p - seg));
		}
	}

	void *str = bzy_str_new(tb.data ? tb.data : "", (int64_t)tb.len);
	free(tb.data);
	return str;
}

/* Parse a JSON number: a JK_INT node when there is no '.'/'e'/'E', else JK_DBL.
   Returns an owned (+1) node, or NULL with the error set. */
static void *parse_number(Scan *s)
{
	const char *start = s->p;
	int is_dbl = 0;
	if (peek(s) == '-') { advance(s); }
	while (!at_end(s))
	{
		int c = peek(s);
		if (c >= '0' && c <= '9') { advance(s); }
		else if (c == '.' || c == 'e' || c == 'E' || c == '+' || c == '-')
		{
			if (c == '.' || c == 'e' || c == 'E') { is_dbl = 1; }
			advance(s);
		}
		else { break; }
	}

	size_t n = (size_t)(s->p - start);
	if (n == 0) { fail(s, "Expected a number."); return NULL; }

	char buf[64];
	if (n >= sizeof buf) { fail(s, "Number too long."); return NULL; }
	memcpy(buf, start, n);
	buf[n] = '\0';

	char *endp = NULL;
	if (is_dbl)
	{
		double d = strtod(buf, &endp);
		if (endp != buf + n) { fail(s, "Malformed number."); return NULL; }
		void *v = jv_new(JK_DBL);
		jv_set_double(v, d);
		return v;
	}

	long long ll = strtoll(buf, &endp, 10);
	if (endp != buf + n) { fail(s, "Malformed number."); return NULL; }
	void *v = jv_new(JK_INT);
	JSCA(v) = (int64_t)ll;
	return v;
}

/* Parse a JSON array at '['. Returns an owned (+1) JK_ARR node whose payload is
   a List<JsonValue> (object-kind vector), or NULL with the error set. */
static void *parse_array(Scan *s)
{
	advance(s);   /* '['. */
	void *items = bzy_vec_new(4);   /* Object elements. */
	skip_ws(s);
	if (peek(s) == ']') { advance(s); void *v = jv_new(JK_ARR); JSET_MAN(v, items); return v; }

	for (;;)
	{
		void *el = parse_value(s);
		if (s->err) { bzy_release(items); return NULL; }
		bzy_vec_push_back(items, (int64_t)el);   /* Retains. */
		bzy_release(el);                          /* Drop our +1; the vector owns it. */
		skip_ws(s);
		int c = peek(s);
		if (c == ',') { advance(s); skip_ws(s); continue; }
		if (c == ']') { advance(s); break; }
		bzy_release(items);
		fail(s, "Expected ',' or ']' in array.");
		return NULL;
	}

	void *v = jv_new(JK_ARR);
	JSET_MAN(v, items);
	return v;
}

/* Parse a JSON object at '{'. Returns an owned (+1) JK_OBJ node whose payload is
   a map<string,JsonValue>, or NULL with the error set. Duplicate keys last-wins. */
static void *parse_object(Scan *s)
{
	advance(s);   /* '{'. */
	void *m = bzy_map_new(1, 1);   /* String keys, managed values. */
	skip_ws(s);
	if (peek(s) == '}') { advance(s); void *v = jv_new(JK_OBJ); JSET_MAN(v, m); return v; }

	for (;;)
	{
		skip_ws(s);
		if (peek(s) != '"') { bzy_release(m); fail(s, "Expected a string key."); return NULL; }
		void *key = parse_string(s);
		if (s->err) { bzy_release(m); return NULL; }
		skip_ws(s);
		if (peek(s) != ':') { bzy_release(key); bzy_release(m); fail(s, "Expected ':' after key."); return NULL; }
		advance(s);
		void *val = parse_value(s);
		if (s->err) { bzy_release(key); bzy_release(m); return NULL; }
		bzy_map_put(m, (int64_t)key, (int64_t)val);   /* Retains both. */
		bzy_release(key);
		bzy_release(val);
		skip_ws(s);
		int c = peek(s);
		if (c == ',') { advance(s); continue; }
		if (c == '}') { advance(s); break; }
		bzy_release(m);
		fail(s, "Expected ',' or '}' in object.");
		return NULL;
	}

	void *v = jv_new(JK_OBJ);
	JSET_MAN(v, m);
	return v;
}

/* Parse one JSON value: dispatch on the first non-whitespace byte. Returns an
   owned (+1) node, or NULL with the error set. */
static void *parse_value(Scan *s)
{
	skip_ws(s);
	if (at_end(s)) { fail(s, "Unexpected end of input."); return NULL; }
	int c = peek(s);
	switch (c)
	{
		case '{': return parse_object(s);
		case '[': return parse_array(s);
		case '"':
		{
			void *str = parse_string(s);
			if (s->err) { return NULL; }
			void *v = jv_new(JK_STR);
			JSET_MAN(v, str);
			return v;
		}
		case 't':
			if (s->end - s->p >= 4 && memcmp(s->p, "true", 4) == 0)
			{
				for (int i = 0; i < 4; i++) { advance(s); }
				void *v = jv_new(JK_BOOL);
				JSCA(v) = 1;
				return v;
			}
			fail(s, "Unexpected character.");
			return NULL;
		case 'f':
			if (s->end - s->p >= 5 && memcmp(s->p, "false", 5) == 0)
			{
				for (int i = 0; i < 5; i++) { advance(s); }
				void *v = jv_new(JK_BOOL);
				JSCA(v) = 0;
				return v;
			}
			fail(s, "Unexpected character.");
			return NULL;
		case 'n':
			if (s->end - s->p >= 4 && memcmp(s->p, "null", 4) == 0)
			{
				for (int i = 0; i < 4; i++) { advance(s); }
				return jv_new(JK_NULL);
			}
			fail(s, "Unexpected character.");
			return NULL;
		default:
			if (c == '-' || (c >= '0' && c <= '9')) { return parse_number(s); }
			fail(s, "Unexpected character.");
			return NULL;
	}
}

/* Parse a whole document: optional leading whitespace, a single top-level value,
   trailing whitespace only. Returns the owned (+1) root, or NULL with the error
   message + line + column written through the out-params. */
void *bzy_json_parse_impl(void *src, const char **errmsg, int64_t *line, int64_t *col)
{
	Scan s;
	s.p = bzy_str_data(src);
	s.end = s.p + bzy_str_len(src);
	s.line = 1;
	s.col = 1;
	s.err = NULL;

	void *root = parse_value(&s);
	if (!s.err)
	{
		skip_ws(&s);
		if (s.p != s.end) { fail(&s, "Trailing content after the value."); }
	}

	if (s.err)
	{
		if (root) { bzy_release(root); }
		*errmsg = s.err;
		*line = s.line;
		*col = s.col;
		return NULL;
	}

	return root;
}

/* ---- Breezy entry + catchable-error plumbing (the bzy_number_check pattern) -- */

static __thread const char *g_json_error;   /* Set by the runtime; consumed by bzy_json_check. */

/* Called from Breezy: returns the owned (+1) root in rax, or NULL with the error
   message buffered for the codegen-emitted bzy_json_check at the call site. */
void *bzy_json_parse(void *src)
{
	const char *err = NULL;
	int64_t line = 0, col = 0;
	void *root = bzy_json_parse_impl(src, &err, &line, &col);
	if (err)
	{
		static __thread char buf[192];
		snprintf(buf, sizeof buf, "JSON parse error at line %lld, column %lld: %s",
				 (long long)line, (long long)col, err);
		g_json_error = buf;
		return NULL;
	}

	return root;
}

void bzy_json_check(int64_t pc, int64_t frame)
{
	if (!g_json_error)
	{
		return;
	}

	void *msg = bzy_str_new(g_json_error, (int64_t)strlen(g_json_error));
	g_json_error = NULL;
	void *exc = bzy_alloc(32);
	*(void**)exc = (void*)__vtable_JsonException;
	*(void**)((char*)exc + 24) = msg;          /* Exception.message. */
	bzy_throw(exc, pc, frame);                  /* Never returns. */
}

/* Buffer a strict-extraction type-mismatch message for the next bzy_json_check
   (shared with the parse path's error slot). */
static void json_type_fail(const char *msg)
{
	if (!g_json_error)
	{
		g_json_error = msg;
	}
}

/* ---- kind tests (never throw) ----------------------------------------------- */

int64_t bzy_json_is_null(void *v)   { return JKIND(v) == JK_NULL; }
int64_t bzy_json_is_bool(void *v)   { return JKIND(v) == JK_BOOL; }
int64_t bzy_json_is_number(void *v) { int64_t k = JKIND(v); return k == JK_INT || k == JK_DBL; }
int64_t bzy_json_is_string(void *v) { return JKIND(v) == JK_STR; }
int64_t bzy_json_is_array(void *v)  { return JKIND(v) == JK_ARR; }
int64_t bzy_json_is_object(void *v) { return JKIND(v) == JK_OBJ; }

/* An owned (+1) string naming the kind. */
void *bzy_json_type_name(void *v)
{
	const char *t;
	switch (JKIND(v))
	{
		case JK_NULL: t = "null"; break;
		case JK_BOOL: t = "bool"; break;
		case JK_INT:  t = "number"; break;
		case JK_DBL:  t = "number"; break;
		case JK_STR:  t = "string"; break;
		case JK_ARR:  t = "array"; break;
		default:      t = "object"; break;
	}

	return bzy_str_new(t, (int64_t)strlen(t));
}

/* ---- scalar extraction (strict: a wrong kind sets the error for bzy_json_check) */

int64_t bzy_json_as_long(void *v)
{
	int64_t k = JKIND(v);
	if (k == JK_INT) { return JSCA(v); }
	if (k == JK_DBL) { union { double d; int64_t i; } u; u.i = JSCA(v); return (int64_t)u.d; }
	json_type_fail("asLong() on a non-number JSON value.");
	return 0;
}

double bzy_json_as_double(void *v)
{
	int64_t k = JKIND(v);
	if (k == JK_DBL) { union { double d; int64_t i; } u; u.i = JSCA(v); return u.d; }
	if (k == JK_INT) { return (double)JSCA(v); }
	json_type_fail("asDouble() on a non-number JSON value.");
	return 0.0;
}

void *bzy_json_as_string(void *v)
{
	if (JKIND(v) == JK_STR) { void *s = JGET_MAN(v); bzy_retain(s); return s; }
	json_type_fail("asString() on a non-string JSON value.");
	return bzy_str_new("", 0);
}

int64_t bzy_json_as_bool(void *v)
{
	if (JKIND(v) == JK_BOOL) { return JSCA(v); }
	json_type_fail("asBool() on a non-bool JSON value.");
	return 0;
}

/* ---- forgiving object navigation -------------------------------------------- */

/* A process-lifetime shared JK_NULL node. get() on a missing key / non-object
   returns it (+1) so navigation chains never crash. */
static void *g_json_null;

static void *json_null_retained(void)
{
	/* ponytail: single-init race on first use is benign -- a duplicate singleton
	   is just another valid JK_NULL node; both live forever by design. */
	if (!g_json_null) { g_json_null = jv_new(JK_NULL); }
	bzy_retain(g_json_null);
	return g_json_null;
}

/* The value at `key` (owned +1) for an object; a shared null value for a missing
   key or a non-object receiver. */
void *bzy_json_get(void *v, void *key)
{
	if (JKIND(v) == JK_OBJ)
	{
		int64_t hit = bzy_map_get(JGET_MAN(v), (int64_t)key);   /* Retains a managed value. */
		if (hit) { return (void*)hit; }
	}

	return json_null_retained();
}

/* 1 if the receiver is an object with `key` present (distinguishes an absent key
   from a present null), else 0. */
int64_t bzy_json_has(void *v, void *key)
{
	if (JKIND(v) == JK_OBJ) { return bzy_map_has(JGET_MAN(v), (int64_t)key); }
	return 0;
}

/* An owned (+1) List<string> of the object's keys; an empty list for a non-object. */
void *bzy_json_keys(void *v)
{
	void *list = bzy_vec_new(3);   /* String elements. */
	if (JKIND(v) == JK_OBJ)
	{
		void *m = JGET_MAN(v);
		for (int64_t s = bzy_map_iter(m, 0); s >= 0; s = bzy_map_iter(m, s + 1))
		{
			bzy_vec_push_back(list, bzy_map_key_at(m, s));   /* Borrowed key; push retains. */
		}
	}

	return list;
}

/* ---- array navigation ------------------------------------------------------- */

/* The array's elements as an owned (+1) List<JsonValue>; an empty list for a
   non-array (so .items().forEach is always safe). */
void *bzy_json_items(void *v)
{
	if (JKIND(v) == JK_ARR) { void *l = JGET_MAN(v); bzy_retain(l); return l; }
	return bzy_vec_new(4);   /* Object elements. */
}

/* The element at index i (owned +1); OOB or a non-array sets the error for the
   post-call bzy_json_check (returns a null value as the unused placeholder). */
void *bzy_json_at(void *v, int64_t i)
{
	if (JKIND(v) == JK_ARR)
	{
		void *l = JGET_MAN(v);
		int64_t n = *(int64_t*)((char*)l + 24);   /* length@24. */
		void *data = *(void**)((char*)l + 48);    /* data array@48. */
		if (i >= 0 && i < n && data)
		{
			void *el = ((void**)((char*)data + 32))[i];   /* Borrowed slot. */
			bzy_retain(el);
			return el;
		}

		json_type_fail("at() index out of range.");
		return json_null_retained();
	}

	json_type_fail("at() on a non-array JSON value.");
	return json_null_retained();
}

/* Element count (array) or key count (object); 0 for any other kind. */
int64_t bzy_json_size(void *v)
{
	int64_t k = JKIND(v);
	if (k == JK_ARR) { void *l = JGET_MAN(v); return l ? *(int64_t*)((char*)l + 24) : 0; }
	if (k == JK_OBJ) { void *m = JGET_MAN(v); return m ? bzy_map_len(m) : 0; }
	return 0;
}

/* ---- Json.of lifters (each returns an owned +1 JsonValue) -------------------- */

void *bzy_json_of_long(int64_t n)
{
	void *v = jv_new(JK_INT);
	JSCA(v) = n;
	return v;
}

void *bzy_json_of_double(double d)
{
	void *v = jv_new(JK_DBL);
	jv_set_double(v, d);
	return v;
}

void *bzy_json_of_string(void *s)
{
	void *v = jv_new(JK_STR);
	bzy_retain(s);
	JSET_MAN(v, s);
	return v;
}

void *bzy_json_of_bool(int64_t b)
{
	void *v = jv_new(JK_BOOL);
	JSCA(v) = b ? 1 : 0;
	return v;
}

void *bzy_json_null(void)
{
	return json_null_retained();
}

void *bzy_json_of_array(void *list)
{
	void *v = jv_new(JK_ARR);
	bzy_retain(list);
	JSET_MAN(v, list);
	return v;
}

void *bzy_json_of_object(void *map)
{
	void *v = jv_new(JK_OBJ);
	bzy_retain(map);
	JSET_MAN(v, map);
	return v;
}

/* ---- serializer (compact RFC 8259) ------------------------------------------ */

/* Emit a JSON string literal: quotes + the escapes "" \\ \n \t \r \b \f, other
   control bytes as \uXXXX, raw UTF-8 otherwise. */
static void json_escape_string(TextBuf *t, void *s)
{
	const char *p = bzy_str_data(s);
	int64_t n = bzy_str_len(s);
	tb_push(t, "\"", 1);
	for (int64_t i = 0; i < n; i++)
	{
		unsigned char c = (unsigned char)p[i];
		switch (c)
		{
			case '"':  tb_push(t, "\\\"", 2); break;
			case '\\': tb_push(t, "\\\\", 2); break;
			case '\n': tb_push(t, "\\n", 2); break;
			case '\t': tb_push(t, "\\t", 2); break;
			case '\r': tb_push(t, "\\r", 2); break;
			case '\b': tb_push(t, "\\b", 2); break;
			case '\f': tb_push(t, "\\f", 2); break;
			default:
				if (c < 0x20) { char u[8]; int k = snprintf(u, sizeof u, "\\u%04x", c); tb_push(t, u, (size_t)k); }
				else { tb_push(t, (char*)&p[i], 1); }
		}
	}

	tb_push(t, "\"", 1);
}

/* Walk v into the buffer. A non-finite double sets the error (the post-call
   bzy_json_check then throws); once set, the walk unwinds without appending. */
static void serialize_value(TextBuf *t, void *v)
{
	if (g_json_error) { return; }

	switch (JKIND(v))
	{
		case JK_NULL: tb_push(t, "null", 4); break;
		case JK_BOOL: if (JSCA(v)) { tb_push(t, "true", 4); } else { tb_push(t, "false", 5); } break;
		case JK_INT:
		{
			char b[32];
			int k = snprintf(b, sizeof b, "%lld", (long long)JSCA(v));
			tb_push(t, b, (size_t)k);
			break;
		}
		case JK_DBL:
		{
			union { double d; int64_t i; } u;
			u.i = JSCA(v);
			if (!isfinite(u.d)) { json_type_fail("Cannot stringify a non-finite number."); return; }
			void *s = bzy_str_from_f64(u.d);   /* Breezy's canonical double text (matches print). */
			tb_push(t, bzy_str_data(s), (size_t)bzy_str_len(s));
			bzy_release(s);
			break;
		}
		case JK_STR: json_escape_string(t, JGET_MAN(v)); break;
		case JK_ARR:
		{
			void *l = JGET_MAN(v);
			int64_t n = l ? *(int64_t*)((char*)l + 24) : 0;
			void *data = l ? *(void**)((char*)l + 48) : NULL;
			tb_push(t, "[", 1);
			for (int64_t i = 0; i < n; i++)
			{
				if (i) { tb_push(t, ",", 1); }
				serialize_value(t, ((void**)((char*)data + 32))[i]);
				if (g_json_error) { return; }
			}

			tb_push(t, "]", 1);
			break;
		}
		default:   /* JK_OBJ. */
		{
			void *m = JGET_MAN(v);
			tb_push(t, "{", 1);
			int first = 1;
			for (int64_t s = bzy_map_iter(m, 0); s >= 0; s = bzy_map_iter(m, s + 1))
			{
				if (!first) { tb_push(t, ",", 1); }
				first = 0;
				json_escape_string(t, (void*)bzy_map_key_at(m, s));
				tb_push(t, ":", 1);
				serialize_value(t, (void*)bzy_map_val_at(m, s));
				if (g_json_error) { return; }
			}

			tb_push(t, "}", 1);
			break;
		}
	}
}

/* Json.stringify(v) -> an owned (+1) compact JSON string. */
void *bzy_json_stringify(void *v)
{
	TextBuf t = { 0 };
	serialize_value(&t, v);
	void *out = bzy_str_new(t.data ? t.data : "", (int64_t)t.len);
	free(t.data);
	return out;
}
