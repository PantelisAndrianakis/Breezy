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
