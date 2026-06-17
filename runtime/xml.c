/* runtime/xml.c -- read-only XML reader (pragmatic subset). Own TU: a program that
   never calls Xml.parse links none of this. The parser builds a tree of managed
   XmlNode objects; ARC + the cycle collector reclaim it via the per-type typeinfo.

   This file is Task 2 of the Phase E.3 plan: the well-formed scanner (elements,
   attributes, text) + the managed node. Entity/numeric-ref/CDATA decoding and the
   comment/PI skipping land in Task 7; the Xml.parse entry + accessors in 3-8. */
#include "breezy.h"
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

/* Emitted per-program by codegen; bzy_xml_check stamps a thrown XmlException with
   it (the bzy_number_check pattern). */
extern char __vtable_XmlException[];
extern void bzy_throw(void *exc, int64_t pc, int64_t frame);

/* XmlNode layout: vtable|rc|gcinfo|name@24|text@32|anames@40|avals@48|acount@56|children@64. */
#define X_NAME   24
#define X_TEXT   32
#define X_ANAMES 40
#define X_AVALS  48
#define X_ACOUNT 56
#define X_KIDS   64
#define X_SIZE   72

#define NODE_SET(n, off, v) (*(void**)((char*)(n) + (off)) = (void*)(v))
#define NODE_GET(n, off)    (*(void**)((char*)(n) + (off)))

/* Managed-node typeinfo: {finalizer=0, child_count=5, name,text,anames,avals,kids}.
   attr_count@56 is a plain int, not a managed child. */
static int64_t g_xml_ti[7] = { 0, 5, X_NAME, X_TEXT, X_ANAMES, X_AVALS, X_KIDS };
static int64_t g_xml_vt[2];
static int     g_xml_vt_built;

static void *xml_vtable(void)
{
	if (!g_xml_vt_built)
	{
		g_xml_vt[0] = (int64_t)&g_xml_ti[0];
		g_xml_vt_built = 1;
	}

	return &g_xml_vt[1];   /* The object stores this; [stored-8] == &g_xml_ti[0]. */
}

static void *node_new(void)
{
	void *n = bzy_alloc(X_SIZE);            /* Zeroed; rc=1; gcinfo set by alloc. */
	*(void**)n = xml_vtable();
	return n;
}

/* Store an owned (+1) string into a managed array slot, transferring ownership
   (the array's SPAN typeinfo releases the slot when the array is freed). */
static void arr_set(void *arr, int64_t i, void *str)
{
	((void**)((char*)arr + 32))[i] = str;
}

/* ---- scanner ---------------------------------------------------------------- */

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

static int peek2(Scan *s) { return (s->p + 1) < s->end ? (unsigned char)s->p[1] : -1; }

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

static int is_name_start(int c)
{
	return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c == ':';
}

static int is_name_char(int c)
{
	return is_name_start(c) || (c >= '0' && c <= '9') || c == '-' || c == '.';
}

/* Read a tag/attribute name; returns an owned (+1) string, or NULL on error. */
static void *parse_name(Scan *s)
{
	const char *start = s->p;
	if (!is_name_start(peek(s)))
	{
		fail(s, "expected a name");
		return NULL;
	}

	while (!at_end(s) && is_name_char(peek(s))) { advance(s); }
	return bzy_str_new(start, (int64_t)(s->p - start));
}

/* ---- a growable text buffer (one per element's direct character data) ------- */

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

/* ---- a growable temp list of owned attribute name/value string pointers ------ */

typedef struct { void **names; void **vals; int count, cap; } AttrTmp;

static void at_push(AttrTmp *a, void *name, void *val)
{
	if (a->count >= a->cap)
	{
		a->cap = a->cap ? a->cap * 2 : 8;
		a->names = (void**)realloc(a->names, a->cap * sizeof(void*));
		a->vals  = (void**)realloc(a->vals,  a->cap * sizeof(void*));
	}

	a->names[a->count] = name;
	a->vals[a->count] = val;
	a->count++;
}

static void *parse_element(Scan *s);

/* Parse the attributes of a start tag into the node (after the tag name, before
   '>' or '/>'). Leaves the cursor at '>' or '/'. */
static void parse_attrs(Scan *s, void *node)
{
	AttrTmp a = { 0 };
	for (;;)
	{
		skip_ws(s);
		int c = peek(s);
		if (c == '>' || c == '/' || c < 0) { break; }

		void *aname = parse_name(s);
		if (s->err) { break; }
		skip_ws(s);
		if (peek(s) != '=') { fail(s, "expected '=' after attribute name"); break; }
		advance(s);
		skip_ws(s);
		int q = peek(s);
		if (q != '"' && q != '\'') { fail(s, "expected a quoted attribute value"); break; }
		advance(s);
		const char *vstart = s->p;
		while (!at_end(s) && peek(s) != q) { advance(s); }
		if (peek(s) != q) { fail(s, "unterminated attribute value"); break; }
		void *aval = bzy_str_new(vstart, (int64_t)(s->p - vstart));   /* Raw; decode in Task 7. */
		advance(s);   /* Closing quote. */
		at_push(&a, aname, aval);
	}

	if (!s->err && a.count > 0)
	{
		void *names = bzy_array_new(a.count, 1);
		void *vals  = bzy_array_new(a.count, 1);
		for (int i = 0; i < a.count; i++)
		{
			arr_set(names, i, a.names[i]);   /* Transfer owned. */
			arr_set(vals,  i, a.vals[i]);
		}

		NODE_SET(node, X_ANAMES, names);
		NODE_SET(node, X_AVALS, vals);
		*(int64_t*)((char*)node + X_ACOUNT) = a.count;
	}
	else
	{
		/* On error, release whatever was collected so far; on the 0-attr path
		   there is nothing to free. */
		for (int i = 0; i < a.count; i++)
		{
			bzy_release(a.names[i]);
			bzy_release(a.vals[i]);
		}
	}

	free(a.names);
	free(a.vals);
}

/* Parse one element starting at '<'. Returns the owned (+1) node, or NULL. */
static void *parse_element(Scan *s)
{
	if (peek(s) != '<') { fail(s, "expected '<'"); return NULL; }
	advance(s);

	void *node = node_new();
	void *name = parse_name(s);
	if (s->err) { bzy_release(node); return NULL; }
	NODE_SET(node, X_NAME, name);

	parse_attrs(s, node);
	if (s->err) { bzy_release(node); return NULL; }

	skip_ws(s);
	if (peek(s) == '/')          /* Self-closing <tag/>. */
	{
		advance(s);
		if (peek(s) != '>') { fail(s, "expected '>' after '/'"); bzy_release(node); return NULL; }
		advance(s);
		NODE_SET(node, X_TEXT, bzy_str_new("", 0));
		NODE_SET(node, X_KIDS, bzy_vec_new(4));   /* An empty children list. */
		return node;
	}

	if (peek(s) != '>') { fail(s, "expected '>'"); bzy_release(node); return NULL; }
	advance(s);

	/* Content: direct text runs (into tb) interleaved with child elements. The
	   children list is always present (empty for a leaf) so `node.children` is a
	   plain borrowed field read -- no NULL case for the collection combinators. */
	TextBuf tb = { 0 };
	void *kids = bzy_vec_new(4);   /* Object elements. */

	for (;;)
	{
		if (at_end(s)) { fail(s, "unexpected end of input inside element"); break; }

		if (peek(s) == '<')
		{
			if (peek2(s) == '/')                 /* End tag </name>. */
			{
				advance(s);                       /* '<'. */
				advance(s);                       /* '/'. */
				void *ename = parse_name(s);
				if (s->err) { break; }
				skip_ws(s);
				if (peek(s) != '>') { fail(s, "expected '>' in end tag"); bzy_release(ename); break; }
				advance(s);
				if (!bzy_str_eq(ename, name)) { fail(s, "mismatched end tag"); bzy_release(ename); break; }
				bzy_release(ename);
				break;
			}

			/* A child element (Task 7 will also accept <!-- / <![CDATA / <?). */
			if (!is_name_start(peek2(s))) { fail(s, "unsupported markup"); break; }

			void *child = parse_element(s);
			if (s->err) { break; }
			bzy_vec_push_back(kids, (int64_t)child);  /* Retains. */
			bzy_release(child);                       /* Drop our +1; the vector owns it. */
		}
		else                                     /* A run of character data. */
		{
			const char *tstart = s->p;
			while (!at_end(s) && peek(s) != '<') { advance(s); }
			tb_push(&tb, tstart, (size_t)(s->p - tstart));
		}
	}

	if (s->err)
	{
		free(tb.data);
		if (kids) { bzy_release(kids); }
		bzy_release(node);
		return NULL;
	}

	NODE_SET(node, X_TEXT, bzy_str_new(tb.data ? tb.data : "", (int64_t)tb.len));
	free(tb.data);
	if (kids) { NODE_SET(node, X_KIDS, kids); }
	return node;
}

/* Parse a whole document: optional leading whitespace, a single root element,
   trailing whitespace only. Returns the owned (+1) root, or NULL with the error
   message + line + column written through the out-params. */
void *bzy_xml_parse_impl(void *src, const char **errmsg, int64_t *line, int64_t *col)
{
	Scan s;
	s.p = bzy_str_data(src);
	s.end = s.p + bzy_str_len(src);
	s.line = 1;
	s.col = 1;
	s.err = NULL;

	skip_ws(&s);
	if (peek(&s) != '<') { s.err = "expected a root element"; }
	void *root = s.err ? NULL : parse_element(&s);
	if (!s.err)
	{
		skip_ws(&s);
		if (s.p != s.end) { fail(&s, "trailing content after the root element"); }
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

/* ---- attribute accessors ---------------------------------------------------- */

/* The attribute value for `name` (owned +1), or an owned empty string if absent. */
void *bzy_xml_attr(void *node, void *name)
{
	int64_t n = *(int64_t*)((char*)node + X_ACOUNT);
	void *names = NODE_GET(node, X_ANAMES);
	void *vals  = NODE_GET(node, X_AVALS);
	for (int64_t i = 0; i < n; i++)
	{
		void *an = ((void**)((char*)names + 32))[i];
		if (bzy_str_eq(an, name))
		{
			void *v = ((void**)((char*)vals + 32))[i];
			bzy_retain(v);
			return v;
		}
	}

	return bzy_str_new("", 0);
}

int64_t bzy_xml_has_attr(void *node, void *name)
{
	int64_t n = *(int64_t*)((char*)node + X_ACOUNT);
	void *names = NODE_GET(node, X_ANAMES);
	for (int64_t i = 0; i < n; i++)
	{
		if (bzy_str_eq(((void**)((char*)names + 32))[i], name))
		{
			return 1;
		}
	}

	return 0;
}

/* The attribute name at index i (owned +1); aborts on out-of-range. */
void *bzy_xml_attr_name_at(void *node, int64_t i)
{
	int64_t n = *(int64_t*)((char*)node + X_ACOUNT);
	if (i < 0 || i >= n)
	{
		bzy_oob_abort(i, n);   /* No return. */
	}

	void *names = NODE_GET(node, X_ANAMES);
	void *nm = ((void**)((char*)names + 32))[i];
	bzy_retain(nm);
	return nm;
}

/* ---- Breezy entry + catchable-error plumbing (the bzy_number_check pattern) -- */

static __thread const char *g_xml_error;   /* Set by bzy_xml_parse; consumed by bzy_xml_check. */

/* Called from Breezy: returns the owned (+1) root in rax, or NULL with the error
   message buffered for the codegen-emitted bzy_xml_check at the call site. */
void *bzy_xml_parse(void *src)
{
	const char *err = NULL;
	int64_t line = 0, col = 0;
	void *root = bzy_xml_parse_impl(src, &err, &line, &col);
	if (err)
	{
		static __thread char buf[192];
		snprintf(buf, sizeof buf, "XML parse error at line %lld, column %lld: %s",
				 (long long)line, (long long)col, err);
		g_xml_error = buf;
		return NULL;
	}

	return root;
}

void bzy_xml_check(int64_t pc, int64_t frame)
{
	if (!g_xml_error)
	{
		return;
	}

	void *msg = bzy_str_new(g_xml_error, (int64_t)strlen(g_xml_error));
	g_xml_error = NULL;
	void *exc = bzy_alloc(32);
	*(void**)exc = (void*)__vtable_XmlException;
	*(void**)((char*)exc + 24) = msg;          /* Exception.message. */
	bzy_throw(exc, pc, frame);                  /* Never returns. */
}
