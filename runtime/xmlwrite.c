/**
 * runtime/xmlwrite.c -- XML output: Xml.stringify and XmlWriter.
 * Own TU: a program that never writes XML links none of this.
 * Untouched elements are copied byte-for-byte from the document they were parsed from; only edited or new content is generated, in the document's own style.
 */
#include "breezy.h"
#include "xml_node.h"
#include <stdlib.h>
#include <string.h>

void    bzy_sb_append_cstr(void *sb, const char *bytes, int64_t len);
void   *bzy_sb_to_string(void *sb);

/* State of one Xml.stringify call. */
typedef struct
{
	void       *sb; // The output (a StringBuilder).
	const char *nl; // Line ending for added lines.
	int64_t     nl_len;
	const char *unit; // One level of added indentation.
	int64_t     unit_len;
	int         multiline; // 0 for a one-line or built document: nothing is added.
	void      **path; // Elements being written, outermost first (loop detection).
	int64_t     depth, cap;
	int         failed;
} W;

static void put(W *w, const char *p, int64_t n)
{
	if (n > 0)
	{
		bzy_sb_append_cstr(w->sb, p, n);
	}
}

static void put_str(W *w, void *s)
{
	put(w, bzy_str_data(s), bzy_str_len(s));
}

/* s with & < > escaped for text (quote 0), or & < and the quote for an attribute value. */
static void put_escaped(W *w, void *s, int quote)
{
	const char *p = bzy_str_data(s);
	int64_t n = bzy_str_len(s), run = 0;
	for (int64_t i = 0; i < n; i++)
	{
		const char *rep = NULL;
		if (p[i] == '&')
		{
			rep = "&amp;";
		}
		else if (p[i] == '<')
		{
			rep = "&lt;";
		}
		else if (p[i] == '>' && !quote)
		{
			rep = "&gt;";
		}
		else if (quote && p[i] == quote)
		{
			rep = quote == '"' ? "&quot;" : "&apos;";
		}

		if (rep)
		{
			put(w, p + run, i - run);
			put(w, rep, (int64_t)strlen(rep));
			run = i + 1;
		}
	}

	put(w, p + run, n - run);
}

static const char *src_of(void *node)
{
	void *src = NODE_GET(node, X_SRC);
	return src ? bzy_str_data(src) : NULL;
}

/* 1 when child still sits under the element it was parsed in, so its source gap applies. */
static int in_place(void *child, void *parent)
{
	return NODE_GET(child, X_SRC) && NODE_GET(parent, X_SRC) && NODE_I64(child, X_PID) == NODE_I64(parent, X_ID);
}

/* A parsed element's indentation: the spaces and tabs after the last line break of its leading gap; -1 when the gap has no line break. */
static int64_t src_indent(void *node, const char **out)
{
	const char *d = src_of(node);
	int64_t lead = NODE_I32(node, X_LEAD), start = NODE_I32(node, X_START);
	for (int64_t i = start - 1; i >= lead; i--)
	{
		if (d[i] == '\n')
		{
			for (int64_t j = i + 1; j < start; j++)
			{
				if (d[j] != ' ' && d[j] != '\t')
				{
					return -1;
				}
			}

			*out = d + i + 1;
			return start - i - 1;
		}
	}

	return -1;
}

/* 1 when the element and its whole subtree can be copied from the source unchanged. */
static int clean(void *node)
{
	if (!NODE_GET(node, X_SRC) || NODE_I64(node, X_DIRTY))
	{
		return 0;
	}

	void *kids = NODE_GET(node, X_KIDS);
	int64_t n = kids ? bzy_vec_len(kids) : 0;
	int64_t expect = NODE_I32(node, X_STAG); // Where the next original child's gap must begin.
	for (int64_t i = 0; i < n; i++)
	{
		void *c = xml_kid(kids, i); // Borrowed.
		int ok = in_place(c, node) && NODE_I32(c, X_LEAD) == expect && clean(c);
		expect = NODE_I32(c, X_END);
		if (!ok)
		{
			return 0;
		}
	}

	return expect == NODE_I32(node, X_TAIL);
}

/* Length of the name starting at p (the reader's name characters). */
static int64_t name_len(const char *p)
{
	int64_t n = 0;
	while ((p[n] >= 'a' && p[n] <= 'z') || (p[n] >= 'A' && p[n] <= 'Z') || (p[n] >= '0' && p[n] <= '9')
			|| p[n] == '_' || p[n] == ':' || p[n] == '-' || p[n] == '.')
	{
		n++;
	}

	return n;
}

/* One attribute as written in the source start tag. */
typedef struct
{
	const char *ws; // Leading whitespace.
	const char *name;
	int64_t     name_len;
	const char *value; // Raw value, between the quotes.
	int64_t     value_len;
	char        quote;
	const char *end; // Just past the closing quote.
} SrcAttr;

/**
 * Split the source start tag into its attributes.
 * The tag was accepted by the reader, so the scan needs no error handling.
 * Returns the count; *tail points at the whitespace after the last attribute (up to '/' or '>').
 */
static int64_t scan_attrs(const char *p, SrcAttr **out, const char **tail)
{
	p += 1 + name_len(p + 1); // Past '<' and the tag name.
	int64_t n = 0, cap = 0;
	SrcAttr *a = NULL;
	for (;;)
	{
		const char *ws = p;
		while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')
		{
			p++;
		}

		if (*p == '/' || *p == '>')
		{
			*tail = ws;
			break;
		}

		if (n == cap)
		{
			cap = cap ? cap * 2 : 8;
			a = (SrcAttr*)realloc(a, (size_t)cap * sizeof(SrcAttr));
		}

		a[n].ws = ws;
		a[n].name = p;
		a[n].name_len = name_len(p);
		p += a[n].name_len;
		while (*p != '"' && *p != '\'')
		{
			p++; // Whitespace and '='.
		}

		a[n].quote = *p;
		a[n].value = ++p;
		while (*p != a[n].quote)
		{
			p++;
		}

		a[n].value_len = p - a[n].value;
		a[n].end = ++p;
		n++;
	}

	*out = a;
	return n;
}

/* Attributes of an element with a source: unchanged ones as written, changed ones in their original quotes, new ones appended. */
static void write_src_attrs(W *w, void *node, const char *d)
{
	SrcAttr *sa = NULL;
	const char *tail = NULL;
	int64_t ns = scan_attrs(d + NODE_I32(node, X_START), &sa, &tail);
	char newq = ns > 0 ? '\'' : '"';
	for (int64_t i = 0; i < ns; i++)
	{
		if (sa[i].quote != '\'')
		{
			newq = '"';
		}
	}

	int64_t n = NODE_I64(node, X_ACOUNT);
	void **slots = n ? (void**)((char*)NODE_GET(node, X_ATTRS) + 32) : NULL;
	for (int64_t i = 0; i < n; i++)
	{
		void *name = slots[2 * i], *value = slots[2 * i + 1];
		SrcAttr *o = NULL;
		for (int64_t j = 0; j < ns && !o; j++)
		{
			if (sa[j].name_len == bzy_str_len(name) && memcmp(sa[j].name, bzy_str_data(name), (size_t)sa[j].name_len) == 0)
			{
				o = &sa[j];
			}
		}

		if (!o)
		{
			put(w, " ", 1);
			put_str(w, name);
			put(w, newq == '"' ? "=\"" : "='", 2);
			put_escaped(w, value, newq);
			put(w, &newq, 1);
			continue;
		}

		void *was = bzy_xml_decode_value(o->value, o->value_len);
		if (bzy_str_eq(was, value))
		{
			put(w, o->ws, o->end - o->ws); // Exactly as written.
		}
		else
		{
			put(w, o->ws, o->value - o->ws); // Whitespace, name, '=' and the opening quote.
			put_escaped(w, value, o->quote);
			put(w, &o->quote, 1);
		}

		bzy_release(was);
	}

	free(sa);
	put(w, tail, (d + NODE_I32(node, X_STAG)) - tail - ((NODE_I32(node, X_END) == NODE_I32(node, X_STAG)) ? 2 : 1)); // Spacing before '/>' or '>'.
}

/* The first extra indentation a parsed child has over its parent, searched in document order; 0 when there is none. */
static int64_t find_unit(void *node, const char *ind, int64_t ind_len, const char **out)
{
	void *kids = NODE_GET(node, X_KIDS);
	int64_t n = kids ? bzy_vec_len(kids) : 0;
	int64_t found = 0;
	for (int64_t i = 0; i < n && !found; i++)
	{
		void *c = xml_kid(kids, i); // Borrowed.
		if (in_place(c, node))
		{
			const char *ci = NULL;
			int64_t cl = src_indent(c, &ci);
			if (cl > ind_len && memcmp(ci, ind, (size_t)ind_len) == 0)
			{
				*out = ci + ind_len;
				found = cl - ind_len;
			}
			else if (cl >= 0)
			{
				found = find_unit(c, ci, cl, out);
			}
		}

	}

	return found;
}

static void write_el(W *w, void *node, const char *ind, int64_t ind_len);

static void *stringify(void *node, int declaration)
{
	W w;
	memset(&w, 0, sizeof w);
	w.sb = bzy_sb_new();
	w.nl = "\n";
	w.nl_len = 1;
	w.unit = "\t";
	w.unit_len = 1;
	const char *d = src_of(node);
	int64_t doc_len = d ? bzy_str_len(NODE_GET(node, X_SRC)) : 0;
	int root = d && NODE_I64(node, X_PID) == 0;
	if (d)
	{
		w.multiline = memchr(d, '\n', (size_t)doc_len) != NULL;
		for (int64_t i = 0; i + 1 < doc_len; i++)
		{
			if (d[i] == '\r' && d[i + 1] == '\n')
			{
				w.nl = "\r\n";
				w.nl_len = 2;
				break;
			}
		}
	}

	int has_decl = root && NODE_I32(node, X_START) >= 5 && memcmp(d, "<?xml", 5) == 0;
	if (declaration && !has_decl)
	{
		put(&w, "<?xml version=\"1.0\" encoding=\"UTF-8\"?>", 38);
		put(&w, w.nl, w.nl_len);
	}

	const char *ind = "";
	int64_t ind_len = 0;
	if (d)
	{
		int64_t l = src_indent(node, &ind);
		ind_len = l < 0 ? 0 : l;
		if (l < 0)
		{
			ind = "";
		}

		const char *u = NULL;
		int64_t ul = find_unit(node, ind, ind_len, &u);
		if (ul > 0)
		{
			w.unit = u;
			w.unit_len = ul;
		}
	}

	if (root)
	{
		put(&w, d, NODE_I32(node, X_START)); // Declaration and comments before the root.
	}

	write_el(&w, node, ind, ind_len);
	if (root)
	{
		put(&w, d + NODE_I32(node, X_END), doc_len - NODE_I32(node, X_END)); // Comments after the root.
	}

	free(w.path);
	void *out = bzy_sb_to_string(w.sb);
	bzy_release(w.sb);
	if (w.failed)
	{
		bzy_release(out);
		return NULL;
	}

	return out;
}

void *bzy_xml_stringify(void *node)
{
	return stringify(node, 0);
}

void *bzy_xml_stringify_decl(void *node, int64_t declaration)
{
	return stringify(node, declaration != 0);
}

/* Writes a line break and indentation of depth ind + extra units of w->unit, when the document is multi-line. */
static void put_break(W *w, const char *ind, int64_t ind_len, int extra)
{
	if (!w->multiline)
	{
		return;
	}

	put(w, w->nl, w->nl_len);
	put(w, ind, ind_len);
	for (int i = 0; i < extra; i++)
	{
		put(w, w->unit, w->unit_len);
	}
}

static void write_el(W *w, void *node, const char *ind, int64_t ind_len)
{
	for (int64_t i = 0; i < w->depth; i++)
	{
		if (w->path[i] == node)
		{
			bzy_xml_fail("The XML tree contains a loop: a node is inside itself.");
			w->failed = 1;
		}
	}

	if (w->failed)
	{
		return;
	}

	if (clean(node))
	{
		const char *d = src_of(node);
		put(w, d + NODE_I32(node, X_START), NODE_I32(node, X_END) - NODE_I32(node, X_START));
		return;
	}

	if (w->depth == w->cap)
	{
		w->cap = w->cap ? w->cap * 2 : 16;
		w->path = (void**)realloc(w->path, (size_t)w->cap * sizeof(void*));
	}

	w->path[w->depth++] = node;

	const char *d = src_of(node);
	int64_t dirty = NODE_I64(node, X_DIRTY);
	void *kids = NODE_GET(node, X_KIDS);
	int64_t nk = kids ? bzy_vec_len(kids) : 0;
	void *text = NODE_GET(node, X_TEXT);
	int self_closing = d && NODE_I32(node, X_END) == NODE_I32(node, X_STAG);
	int text_set = (dirty & XD_TEXT) != 0;

	// An element writes "/>" when it has nothing inside and was self-closing (or has no source).
	int closed = nk == 0 && ((text_set || !d) ? bzy_str_len(text) == 0 : self_closing) && (!d || self_closing);

	// Start tag.
	if (d && !(dirty & (XD_NAME | XD_ATTRS)))
	{
		int64_t s0 = NODE_I32(node, X_START), s1 = NODE_I32(node, X_STAG);
		if (self_closing && !closed)
		{
			put(w, d + s0, s1 - 2 - s0); // Up to the '/' of "/>".
			put(w, ">", 1);
		}
		else
		{
			put(w, d + s0, s1 - s0);
		}
	}
	else
	{
		put(w, "<", 1);
		put_str(w, NODE_GET(node, X_NAME));
		if (d)
		{
			write_src_attrs(w, node, d);
		}
		else
		{
			int64_t n = NODE_I64(node, X_ACOUNT);
			void **slots = n ? (void**)((char*)NODE_GET(node, X_ATTRS) + 32) : NULL;
			for (int64_t i = 0; i < n; i++)
			{
				put(w, " ", 1);
				put_str(w, slots[2 * i]);
				put(w, "=\"", 2);
				put_escaped(w, slots[2 * i + 1], '"');
				put(w, "\"", 1);
			}
		}

		put(w, closed ? "/>" : ">", closed ? 2 : 1);
	}

	if (closed)
	{
		w->depth--;
		return;
	}

	// Content: replaced or built text first, then each child behind its gap.
	if (text_set || !d)
	{
		put_escaped(w, text, 0);
	}

	const char *sib = NULL; // Indentation of a sibling still in place, reused for new children.
	int64_t sib_len = -1;
	for (int64_t i = 0; i < nk && sib_len < 0 && d && !text_set; i++)
	{
		void *c = xml_kid(kids, i); // Borrowed.
		if (in_place(c, node))
		{
			sib_len = src_indent(c, &sib);
		}

	}

	char *deeper = NULL; // ind + one unit, built on demand.
	for (int64_t i = 0; i < nk && !w->failed; i++)
	{
		void *c = xml_kid(kids, i); // Borrowed.
		const char *ci = NULL;
		int64_t cl = -1;
		if (d && !text_set && in_place(c, node))
		{
			const char *cd = src_of(c);
			put(w, cd + NODE_I32(c, X_LEAD), NODE_I32(c, X_START) - NODE_I32(c, X_LEAD));
			cl = src_indent(c, &ci);
		}
		else if (sib_len >= 0)
		{
			put_break(w, sib, sib_len, 0);
			ci = sib;
			cl = sib_len;
		}
		else
		{
			put_break(w, ind, ind_len, 1);
		}

		if (cl < 0)
		{
			if (!deeper)
			{
				deeper = (char*)malloc((size_t)(ind_len + w->unit_len) + 1);
				memcpy(deeper, ind, (size_t)ind_len);
				memcpy(deeper + ind_len, w->unit, (size_t)w->unit_len);
			}

			ci = deeper;
			cl = ind_len + w->unit_len;
		}

		write_el(w, c, ci, cl);
	}

	free(deeper);

	// Closing gap: the source's own, or a line break back to this element's indentation.
	if (d && !text_set)
	{
		const char *t0 = d + NODE_I32(node, X_TAIL);
		int64_t tl = NODE_I32(node, X_ETAG) - NODE_I32(node, X_TAIL);
		int blank = 1;
		for (int64_t i = 0; i < tl; i++)
		{
			if (t0[i] != ' ' && t0[i] != '\t' && t0[i] != '\r' && t0[i] != '\n')
			{
				blank = 0;
			}
		}

		if (nk > 0 && NODE_I32(node, X_TAIL) == NODE_I32(node, X_STAG) && blank)
		{
			put_break(w, ind, ind_len, 0); // First children of an element that had none.
		}
		else if (!self_closing)
		{
			put(w, t0, tl);
		}
	}
	else if (nk > 0)
	{
		put_break(w, ind, ind_len, 0);
	}

	// End tag.
	if (d && !self_closing && !(dirty & XD_NAME))
	{
		put(w, d + NODE_I32(node, X_ETAG), NODE_I32(node, X_END) - NODE_I32(node, X_ETAG));
	}
	else if (d && !self_closing)
	{
		int64_t e0 = NODE_I32(node, X_ETAG);
		int64_t old = name_len(d + e0 + 2);
		put(w, "</", 2);
		put_str(w, NODE_GET(node, X_NAME));
		put(w, d + e0 + 2 + old, NODE_I32(node, X_END) - (e0 + 2 + old)); // Original spacing and '>'.
	}
	else
	{
		put(w, "</", 2);
		put_str(w, NODE_GET(node, X_NAME));
		put(w, ">", 1);
	}

	w->depth--;
}

/* ---- XmlWriter ---------------------------------------------------------------- */

/* XmlWriter layout: vtable|rc|gcinfo|sb@24|open@32|depth@40|stack@48|cap@56|roots@64. stack is a malloc'd array of owned element names. */
#define XW_SB    24
#define XW_OPEN  32 // 1 while a start tag still takes attributes.
#define XW_DEPTH 40
#define XW_STACK 48
#define XW_CAP   56
#define XW_ROOTS 64 // Root elements already closed.
#define XW_SIZE  72

static void xmlw_finalize(void *w)
{
	void **stack = (void**)NODE_GET(w, XW_STACK);
	for (int64_t i = 0; i < NODE_I64(w, XW_DEPTH); i++)
	{
		bzy_release(stack[i]);
	}

	free(stack);
}

static int64_t g_xmlw_ti[3] = { 0 /* Finalizer (set on first use). */, 1, XW_SB };
static int64_t g_xmlw_vt[2];

static void *xmlw_vtable(void)
{
	g_xmlw_ti[0] = (int64_t)(void*)xmlw_finalize;
	g_xmlw_vt[0] = (int64_t)&g_xmlw_ti[0];
	return &g_xmlw_vt[1];
}

void *bzy_xmlw_new(void)
{
	void *w = bzy_alloc(XW_SIZE); // Zeroed: nothing open, no stack.
	*(void**)w = xmlw_vtable();
	NODE_SET(w, XW_SB, bzy_sb_new());
	return w;
}

static void xw_put(void *w, const char *p, int64_t n)
{
	bzy_sb_append_cstr(NODE_GET(w, XW_SB), p, n);
}

/* Close a start tag that still takes attributes. */
static void xw_close_open(void *w)
{
	if (NODE_I64(w, XW_OPEN))
	{
		xw_put(w, ">", 1);
		NODE_I64(w, XW_OPEN) = 0;
	}
}

static void xw_escaped(void *w, void *s, int quote)
{
	W tmp;
	memset(&tmp, 0, sizeof tmp);
	tmp.sb = NODE_GET(w, XW_SB);
	put_escaped(&tmp, s, quote);
}

void bzy_xmlw_start(void *w, void *name)
{
	if (!bzy_xml_valid_name(name))
	{
		bzy_xml_fail("Invalid XML element name.");
		return;
	}

	int64_t depth = NODE_I64(w, XW_DEPTH);
	if (depth == 0 && NODE_I64(w, XW_ROOTS) > 0)
	{
		bzy_xml_fail("An XML document has a single root element.");
		return;
	}

	xw_close_open(w);
	xw_put(w, "<", 1);
	bzy_sb_append(NODE_GET(w, XW_SB), name);
	if (depth == NODE_I64(w, XW_CAP))
	{
		int64_t cap = depth ? depth * 2 : 8;
		NODE_SET(w, XW_STACK, realloc(NODE_GET(w, XW_STACK), (size_t)cap * sizeof(void*)));
		NODE_I64(w, XW_CAP) = cap;
	}

	bzy_retain(name);
	((void**)NODE_GET(w, XW_STACK))[depth] = name;
	NODE_I64(w, XW_DEPTH) = depth + 1;
	NODE_I64(w, XW_OPEN) = 1;
}

void bzy_xmlw_attr(void *w, void *name, void *value)
{
	if (!NODE_I64(w, XW_OPEN))
	{
		bzy_xml_fail("XmlWriter.attr must follow start, before any content.");
		return;
	}

	if (!bzy_xml_valid_name(name))
	{
		bzy_xml_fail("Invalid XML attribute name.");
		return;
	}

	if (!bzy_xml_valid_chars(value))
	{
		bzy_xml_fail("An XML attribute value cannot contain control characters other than tab, line feed and carriage return.");
		return;
	}

	xw_put(w, " ", 1);
	bzy_sb_append(NODE_GET(w, XW_SB), name);
	xw_put(w, "=\"", 2);
	xw_escaped(w, value, '"');
	xw_put(w, "\"", 1);
}

void bzy_xmlw_text(void *w, void *text)
{
	if (!bzy_xml_valid_chars(text))
	{
		bzy_xml_fail("XML text cannot contain control characters other than tab, line feed and carriage return.");
		return;
	}

	if (NODE_I64(w, XW_DEPTH) == 0)
	{
		const char *p = bzy_str_data(text);
		for (int64_t i = 0; i < bzy_str_len(text); i++)
		{
			if (p[i] != ' ' && p[i] != '\t' && p[i] != '\r' && p[i] != '\n')
			{
				bzy_xml_fail("XmlWriter.text outside an element may only be whitespace.");
				return;
			}
		}
	}

	xw_close_open(w);
	xw_escaped(w, text, 0);
}

void bzy_xmlw_comment(void *w, void *text)
{
	const char *p = bzy_str_data(text);
	int64_t n = bzy_str_len(text);
	int bad = n > 0 && p[n - 1] == '-';
	for (int64_t i = 0; i + 1 < n && !bad; i++)
	{
		bad = p[i] == '-' && p[i + 1] == '-';
	}

	if (bad)
	{
		bzy_xml_fail("An XML comment cannot contain \"--\" or end with \"-\".");
		return;
	}

	if (!bzy_xml_valid_chars(text))
	{
		bzy_xml_fail("XML text cannot contain control characters other than tab, line feed and carriage return.");
		return;
	}

	xw_close_open(w);
	xw_put(w, "<!--", 4);
	xw_put(w, p, n);
	xw_put(w, "-->", 3);
}

void bzy_xmlw_end(void *w)
{
	int64_t depth = NODE_I64(w, XW_DEPTH);
	if (depth == 0)
	{
		bzy_xml_fail("XmlWriter.end has no open element to close.");
		return;
	}

	void *name = ((void**)NODE_GET(w, XW_STACK))[depth - 1];
	if (NODE_I64(w, XW_OPEN))
	{
		xw_put(w, "/>", 2);
		NODE_I64(w, XW_OPEN) = 0;
	}
	else
	{
		xw_put(w, "</", 2);
		bzy_sb_append(NODE_GET(w, XW_SB), name);
		xw_put(w, ">", 1);
	}

	bzy_release(name);
	NODE_I64(w, XW_DEPTH) = depth - 1;
	if (depth == 1)
	{
		NODE_I64(w, XW_ROOTS)++;
	}
}

void *bzy_xmlw_to_string(void *w)
{
	if (NODE_I64(w, XW_DEPTH) > 0)
	{
		bzy_xml_fail("XmlWriter.toString with an element still open.");
		return NULL;
	}

	return bzy_sb_to_string(NODE_GET(w, XW_SB));
}
