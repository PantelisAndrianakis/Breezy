/**
 * runtime/xml.c -- the XML reader and in-place tree edits.
 * Own TU: a program that never calls Xml.parse or Xml.element links none of this.
 * The parser builds a tree of managed XmlNode objects; ARC + the cycle collector reclaim it via the per-type typeinfo.
 * Each node also records where it sits in its source text, so runtime/xmlwrite.c can copy untouched parts back byte-for-byte.
 */
#include "breezy.h"
#include "xml_node.h"
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

/* Emitted per-program by codegen; bzy_xml_check stamps a thrown XmlException with it (the bzy_number_check pattern). */
extern char __vtable_XmlException[];
extern void bzy_throw(void *exc, int64_t pc, int64_t frame);

/* Managed-node typeinfo: {finalizer=0, child_count=5, name,text,attrs,kids,src}. The counts, offsets and ids are plain integers. */
static int64_t g_xml_ti[7] = { 0, 5, X_NAME, X_TEXT, X_ATTRS, X_KIDS, X_SRC };
static int64_t g_xml_vt[2];
static int     g_xml_vt_built;
static int64_t g_xml_next_id; // Last id handed to a parsed element (atomic: documents parse on any worker).

static void *xml_vtable(void)
{
	if (!g_xml_vt_built)
	{
		g_xml_vt[0] = (int64_t)&g_xml_ti[0];
		g_xml_vt_built = 1;
	}

	return &g_xml_vt[1]; // The object stores this; [stored-8] == &g_xml_ti[0].
}

static void *node_new(void)
{
	void *n = bzy_alloc(X_SIZE); // Zeroed; rc=1; gcinfo set by alloc.
	*(void**)n = xml_vtable();
	return n;
}

/* Store an owned (+1) string into a managed array slot, transferring ownership (the array's SPAN typeinfo releases the slot when the array is freed). */
static void arr_set(void *arr, int64_t i, void *str)
{
	((void**)((char*)arr + 32))[i] = str;
}

/* ---- scanner ---------------------------------------------------------------- */

#define XML_NAMES 32

typedef struct
{
	const char *p, *end;
	const char *base; // Start of the document text (offsets are relative to it).
	void *doc; // The document string, retained by every node parsed from it.
	int64_t line, col;
	const char *err;
	void *names[XML_NAMES]; // Interned element and attribute names (one reference each).
	int nnames;
} Scan;

static void fail(Scan *s, const char *msg)
{
	if (!s->err)
	{
		s->err = msg;
	}
}

static int at_end(Scan *s)
{
	return s->p >= s->end || s->err != NULL;
}

static int peek(Scan *s)
{
	return s->p < s->end ? (unsigned char)*s->p : -1;
}

static int peek2(Scan *s)
{
	return (s->p + 1) < s->end ? (unsigned char)s->p[1] : -1;
}

static void advance(Scan *s)
{
	if (s->p < s->end)
	{
		if (*s->p == '\n')
		{
			s->line++;
			s->col = 1;
		}
		else
		{
			s->col++;
		}

		s->p++;
	}
}

static void skip_ws(Scan *s)
{
	while (!at_end(s))
	{
		int c = peek(s);
		if (c == ' ' || c == '\t' || c == '\r' || c == '\n')
		{
			advance(s);
		}
		else
		{
			break;
		}
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
/* Scans a name and returns its length (its bytes start at the old s->p), or -1. */
static int64_t scan_name(Scan *s)
{
	const char *start = s->p;
	if (!is_name_start(peek(s)))
	{
		fail(s, "Expected a name.");
		return -1;
	}

	while (!at_end(s) && is_name_char(peek(s)))
	{
		advance(s);
	}

	return (int64_t)(s->p - start);
}

/**
 * An owned (+1) string for a name.
 * Names repeat on every record of a document, so each distinct one is allocated once per parse and shared; strings are immutable, so sharing is invisible. ponytail: linear scan of XML_NAMES entries, names past that are allocated per use; a hash table if documents with many distinct names need it.
 */
static void *intern_name(Scan *s, const char *p, int64_t len)
{
	for (int i = 0; i < s->nnames; i++)
	{
		void *n = s->names[i];
		if (bzy_str_len(n) == len && memcmp(bzy_str_data(n), p, (size_t)len) == 0)
		{
			bzy_retain(n);
			return n;
		}
	}

	void *n = bzy_str_new(p, len);
	if (s->nnames < XML_NAMES)
	{
		bzy_retain(n); // The table's own reference.
		s->names[s->nnames++] = n;
	}

	return n;
}

static void *parse_name(Scan *s)
{
	const char *start = s->p;
	int64_t len = scan_name(s);
	return len < 0 ? NULL : intern_name(s, start, len);
}

/* ---- a growable text buffer (one per element's direct character data) ------- */

typedef struct
{
	char *data;
	size_t len, cap;
} TextBuf;

static void tb_push(TextBuf *t, const char *bytes, size_t n)
{
	if (t->len + n > t->cap)
	{
		size_t nc = t->cap ? t->cap : 32;
		while (nc < t->len + n)
		{
			nc *= 2;
		}

		t->data = (char*)realloc(t->data, nc);
		t->cap = nc;
	}

	memcpy(t->data + t->len, bytes, n);
	t->len += n;
}

/* ---- a growable temp list of owned attribute name/value string pointers ------ */

typedef struct
{
	void **names;
	void **vals;
	int count, cap;
} AttrTmp;

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

/* Record where an element sits in the document, as offsets from s->base. */
static void set_span(void *node, Scan *s, const char *lead, const char *start, const char *stag, const char *tail, const char *etag, const char *end)
{
	NODE_I32(node, X_LEAD) = (int32_t)(lead - s->base);
	NODE_I32(node, X_START) = (int32_t)(start - s->base);
	NODE_I32(node, X_STAG) = (int32_t)(stag - s->base);
	NODE_I32(node, X_TAIL) = (int32_t)(tail - s->base);
	NODE_I32(node, X_ETAG) = (int32_t)(etag - s->base);
	NODE_I32(node, X_END) = (int32_t)(end - s->base);
}

static void *parse_element(Scan *s, const char *lead, int64_t parent_id);

/* Append a Unicode code point to the buffer as UTF-8 (1-4 bytes). */
static void append_utf8(TextBuf *t, long cp)
{
	unsigned char b[4];
	int n;
	if (cp < 0x80)
	{
		b[0] = (unsigned char)cp;
		n = 1;
	}
	else if (cp < 0x800)
	{
		b[0] = 0xC0 | (cp >> 6);
		b[1] = 0x80 | (cp & 0x3F);
		n = 2;
	}
	else if (cp < 0x10000)
	{
		b[0] = 0xE0 | (cp >> 12);
		b[1] = 0x80 | ((cp >> 6) & 0x3F);
		b[2] = 0x80 | (cp & 0x3F);
		n = 3;
	}
	else
	{
		b[0] = 0xF0 | (cp >> 18);
		b[1] = 0x80 | ((cp >> 12) & 0x3F);
		b[2] = 0x80 | ((cp >> 6) & 0x3F);
		b[3] = 0x80 | (cp & 0x3F);
		n = 4;
	}

	tb_push(t, (char*)b, (size_t)n);
}

/**
 * Decode one entity reference at the cursor (positioned on '&') into the buffer: the five built-ins, or a numeric character reference &#NN; / &#xHH;.
 * Returns 1 on success, 0 (with the error set) on an unknown or malformed reference.
 */
static int decode_entity(Scan *s, TextBuf *t)
{
	advance(s); // '&'.
	if (peek(s) == '#')
	{
		advance(s);
		int hex = 0;
		if (peek(s) == 'x' || peek(s) == 'X')
		{
			hex = 1;
			advance(s);
		}

		long cp = 0;
		int digits = 0;
		while (!at_end(s) && peek(s) != ';')
		{
			int c = peek(s), d;
			if (c >= '0' && c <= '9')
			{
				d = c - '0';
			}
			else if (hex && c >= 'a' && c <= 'f')
			{
				d = c - 'a' + 10;
			}
			else if (hex && c >= 'A' && c <= 'F')
			{
				d = c - 'A' + 10;
			}
			else
			{
				fail(s, "Bad numeric character reference.");
				return 0;
			}

			cp = cp * (hex ? 16 : 10) + d;
			digits++;
			advance(s);
		}

		if (peek(s) != ';' || digits == 0)
		{
			fail(s, "Bad numeric character reference.");
			return 0;
		}

		advance(s); // ';'.
		append_utf8(t, cp);
		return 1;
	}

	const char *nm = s->p;
	while (!at_end(s) && peek(s) != ';' && is_name_char(peek(s)))
	{
		advance(s);
	}

	if (peek(s) != ';')
	{
		fail(s, "Unterminated entity reference.");
		return 0;
	}

	size_t len = (size_t)(s->p - nm);
	char rep;
	if (len == 2 && !memcmp(nm, "lt", 2))
	{
		rep = '<';
	}
	else if (len == 2 && !memcmp(nm, "gt", 2))
	{
		rep = '>';
	}
	else if (len == 3 && !memcmp(nm, "amp", 3))
	{
		rep = '&';
	}
	else if (len == 4 && !memcmp(nm, "quot", 4))
	{
		rep = '"';
	}
	else if (len == 4 && !memcmp(nm, "apos", 4))
	{
		rep = '\'';
	}
	else
	{
		fail(s, "Unknown entity reference.");
		return 0;
	}

	advance(s); // ';'.
	tb_push(t, &rep, 1);
	return 1;
}

/* If the bytes at the cursor equal `lit`, consume them and return 1; else 0. */
static int match_lit(Scan *s, const char *lit)
{
	size_t n = strlen(lit);
	if ((size_t)(s->end - s->p) < n || memcmp(s->p, lit, n) != 0)
	{
		return 0;
	}

	for (size_t i = 0; i < n; i++)
	{
		advance(s);
	}

	return 1;
}

/* Advance past `term`, consuming it; fail at EOF. */
static void skip_until(Scan *s, const char *term)
{
	while (!at_end(s) && !match_lit(s, term))
	{
		advance(s);
	}
}

/**
 * Document-level: skip whitespace, comments, the <?xml?> declaration, and other processing instructions, stopping at an element start or end of input.
 * A DOCTYPE is rejected.
 */
static void skip_misc(Scan *s)
{
	for (;;)
	{
		skip_ws(s);
		if (at_end(s) || peek(s) != '<')
		{
			break;
		}

		if (peek2(s) == '?')
		{
			match_lit(s, "<?");
			skip_until(s, "?>");
		}
		else if (s->p + 4 <= s->end && memcmp(s->p, "<!--", 4) == 0)
		{
			match_lit(s, "<!--");
			skip_until(s, "-->");
		}
		else if (peek2(s) == '!')
		{
			fail(s, "DTD / DOCTYPE is not supported.");
			break;
		}
		else
		{
			break; // An element start.
		}
	}
}

/**
 * Parse the attributes of a start tag into the node (after the tag name, before '>' or '/>').
 * Leaves the cursor at '>' or '/'.
 */
static void parse_attrs(Scan *s, void *node)
{
	AttrTmp a = { 0 };
	for (;;)
	{
		skip_ws(s);
		int c = peek(s);
		if (c == '>' || c == '/' || c < 0)
		{
			break;
		}

		void *aname = parse_name(s);
		if (s->err)
		{
			break;
		}

		skip_ws(s);
		if (peek(s) != '=')
		{
			fail(s, "Expected '=' after attribute name.");
			break;
		}

		advance(s);
		skip_ws(s);
		int q = peek(s);
		if (q != '"' && q != '\'')
		{
			fail(s, "Expected a quoted attribute value.");
			break;
		}

		advance(s);
		TextBuf vb = { 0 };
		while (!at_end(s) && peek(s) != q)
		{
			const char *seg = s->p;
			while (!at_end(s) && peek(s) != q && peek(s) != '&')
			{
				advance(s);
			}

			if (s->p > seg)
			{
				tb_push(&vb, seg, (size_t)(s->p - seg));
			}

			if (peek(s) == '&' && !decode_entity(s, &vb))
			{
				break;
			}
		}

		if (s->err || peek(s) != q)
		{
			free(vb.data);
			bzy_release(aname);
			if (!s->err)
			{
				fail(s, "Unterminated attribute value.");
			}
			break;
		}

		void *aval = bzy_str_new(vb.data ? vb.data : "", (int64_t)vb.len);
		free(vb.data);
		advance(s); // Closing quote.
		at_push(&a, aname, aval);
	}

	if (!s->err && a.count > 0)
	{
		void *attrs = bzy_array_new(2 * a.count, 1);
		for (int i = 0; i < a.count; i++)
		{
			arr_set(attrs, 2 * i, a.names[i]); // Transfer owned.
			arr_set(attrs, 2 * i + 1, a.vals[i]);
		}

		NODE_SET(node, X_ATTRS, attrs);
		*(int64_t*)((char*)node + X_ACOUNT) = a.count;
	}
	else
	{
		// On error, release whatever was collected so far; on the 0-attr path there is nothing to free.
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
static void *parse_element(Scan *s, const char *lead, int64_t parent_id)
{
	if (peek(s) != '<')
	{
		fail(s, "Expected '<'.");
		return NULL;
	}

	const char *el_start = s->p;
	advance(s);

	void *node = node_new();
	int64_t id = __atomic_add_fetch(&g_xml_next_id, 1, __ATOMIC_RELAXED);
	NODE_I64(node, X_ID) = id;
	NODE_I64(node, X_PID) = parent_id;
	bzy_retain(s->doc);
	NODE_SET(node, X_SRC, s->doc);
	void *name = parse_name(s);
	if (s->err)
	{
		bzy_release(node);
		return NULL;
	}

	NODE_SET(node, X_NAME, name);

	parse_attrs(s, node);
	if (s->err)
	{
		bzy_release(node);
		return NULL;
	}

	skip_ws(s);
	if (peek(s) == '/') // Self-closing <tag/>.
	{
		advance(s);
		if (peek(s) != '>')
		{
			fail(s, "Expected '>' after '/'.");
			bzy_release(node);
			return NULL;
		}

		advance(s);
		set_span(node, s, lead, el_start, s->p, s->p, s->p, s->p);
		NODE_SET(node, X_TEXT, bzy_str_new("", 0));
		return node; // No children list until something asks for it (bzy_xml_children).
	}

	if (peek(s) != '>')
	{
		fail(s, "Expected '>'.");
		bzy_release(node);
		return NULL;
	}

	advance(s);
	const char *stag = s->p;
	const char *gap = s->p; // Where the next child's leading gap begins.
	const char *etag = s->p;

	// Content: direct text runs (into tb) interleaved with child elements.
	// The children list is made on the first child: a leaf keeps the slot NULL, and bzy_xml_children makes an empty list only if a program asks for one.
	TextBuf tb = { 0 };
	void *kids = NULL; // Object elements, created with the first child.

	for (;;)
	{
		if (at_end(s))
		{
			fail(s, "Unexpected end of input inside element.");
			break;
		}

		if (peek(s) == '<')
		{
			if (peek2(s) == '/') // End tag </name>.
			{
				etag = s->p;
				advance(s); // '<'.
				advance(s); // '/'.
				const char *estart = s->p;
				int64_t elen = scan_name(s); // Compared in place: no string for the end tag.
				if (s->err)
				{
					break;
				}

				skip_ws(s);
				if (peek(s) != '>')
				{
					fail(s, "Expected '>' in end tag.");
					break;
				}

				advance(s);
				if (elen != bzy_str_len(name) || memcmp(estart, bzy_str_data(name), (size_t)elen) != 0)
				{
					fail(s, "Mismatched end tag.");
					break;
				}

				break;
			}

			if (peek2(s) == '!') // CDATA, comment, or a rejected DTD.
			{
				if (match_lit(s, "<![CDATA[")) // Literal content, no decode.
				{
					const char *cs = s->p;
					while (!at_end(s) && !(s->end - s->p >= 3 && memcmp(s->p, "]]>", 3) == 0))
					{
						advance(s);
					}

					if (s->end - s->p < 3)
					{
						fail(s, "Unterminated CDATA section.");
						break;
					}

					tb_push(&tb, cs, (size_t)(s->p - cs));
					match_lit(s, "]]>");
					continue;
				}

				if (s->end - s->p >= 4 && memcmp(s->p, "<!--", 4) == 0)
				{
					match_lit(s, "<!--");
					skip_until(s, "-->");
					continue;
				}

				fail(s, "DTD / DOCTYPE is not supported.");
				break;
			}

			if (peek2(s) == '?') // Processing instruction: skip.
			{
				match_lit(s, "<?");
				skip_until(s, "?>");
				continue;
			}

			if (!is_name_start(peek2(s)))
			{
				fail(s, "Unsupported markup.");
				break;
			}

			void *child = parse_element(s, gap, id);
			if (s->err)
			{
				break;
			}

			gap = s->p;
			if (!kids)
			{
				kids = bzy_vec_new(4);
			}

			bzy_vec_push_back(kids, (int64_t)child); // Retains.
			bzy_release(child); // Drop our +1; the vector owns it.
		}
		else // A run of character data (entities decoded).
		{
			while (!at_end(s) && peek(s) != '<')
			{
				const char *seg = s->p;
				while (!at_end(s) && peek(s) != '<' && peek(s) != '&')
				{
					advance(s);
				}

				if (s->p > seg)
				{
					tb_push(&tb, seg, (size_t)(s->p - seg));
				}

				if (peek(s) == '&' && !decode_entity(s, &tb))
				{
					break;
				}
			}
		}
	}

	if (s->err)
	{
		free(tb.data);
		if (kids)
		{
			bzy_release(kids);
		}

		bzy_release(node);
		return NULL;
	}

	NODE_SET(node, X_TEXT, bzy_str_new(tb.data ? tb.data : "", (int64_t)tb.len));
	free(tb.data);
	set_span(node, s, lead, el_start, stag, gap, etag, s->p);
	if (kids)
	{
		NODE_SET(node, X_KIDS, kids);
	}

	return node;
}

/**
 * Parse a whole document: optional leading whitespace, a single root element, trailing whitespace only.
 * Returns the owned (+1) root, or NULL with the error message + line + column written through the out-params.
 */
void *bzy_xml_parse_impl(void *src, const char **errmsg, int64_t *line, int64_t *col)
{
	Scan s;
	s.p = bzy_str_data(src);
	s.end = s.p + bzy_str_len(src);
	s.line = 1;
	s.col = 1;
	s.err = NULL;
	s.nnames = 0;
	s.base = s.p;
	s.doc = src;
	if (bzy_str_len(src) >= 0x7fffffff)
	{
		*errmsg = "Document is too large: 2 GiB or more.";
		*line = 1;
		*col = 1;
		return NULL;
	}

	skip_misc(&s); // Leading whitespace, comments, <?xml?> declaration, PIs.
	if (!s.err && (peek(&s) != '<' || !is_name_start(peek2(&s))))
	{
		fail(&s, "expected a root element");
	}

	void *root = s.err ? NULL : parse_element(&s, s.base, 0);
	if (!s.err)
	{
		skip_misc(&s); // Trailing whitespace, comments, PIs.
		if (s.p != s.end)
		{
			fail(&s, "trailing content after the root element");
		}
	}

	for (int i = 0; i < s.nnames; i++)
	{
		bzy_release(s.names[i]); // The table's references; nodes keep their own.
	}

	if (s.err)
	{
		if (root)
		{
			bzy_release(root);
		}
		*errmsg = s.err;
		*line = s.line;
		*col = s.col;
		return NULL;
	}

	return root;
}

/* ---- attribute accessors ---------------------------------------------------- */

/* The attribute value for `name` (owned +1), or an owned empty string if absent. */
/**
 * node.children: the node's child list, borrowed like a field read.
 * A leaf has none until asked, which keeps a large parsed document from carrying an empty list per leaf; the list made here is the node's own, so changes to it stick.
 * A node shared across cores makes it under the node's lock, so two readers cannot both make one.
 */
void *bzy_xml_children(void *node)
{
	void *kids = NODE_GET(node, X_KIDS);
	if (kids)
	{
		return kids;
	}

	int shared = (*(int64_t*)((char*)node + 16) & BZY_GCINFO_SHARED) != 0;
	if (shared)
	{
		bzy_shared_lock(node);
		kids = NODE_GET(node, X_KIDS);
	}

	if (!kids)
	{
		kids = bzy_vec_new(4);
		if (shared)
		{
			*(int64_t*)((char*)kids + 16) |= BZY_GCINFO_SHARED; // Newborn and unpublished: a plain OR is race-free.
		}

		NODE_SET(node, X_KIDS, kids); // The node owns it.
	}

	if (shared)
	{
		bzy_shared_unlock(node);
	}

	return kids;
}

void *bzy_xml_attr(void *node, void *name)
{
	int64_t n = *(int64_t*)((char*)node + X_ACOUNT);
	void **attrs = n ? (void**)((char*)NODE_GET(node, X_ATTRS) + 32) : NULL;
	for (int64_t i = 0; i < n; i++)
	{
		if (bzy_str_eq(attrs[2 * i], name))
		{
			void *v = attrs[2 * i + 1];
			bzy_retain(v);
			return v;
		}
	}

	return bzy_str_new("", 0);
}

int64_t bzy_xml_has_attr(void *node, void *name)
{
	int64_t n = *(int64_t*)((char*)node + X_ACOUNT);
	void **attrs = n ? (void**)((char*)NODE_GET(node, X_ATTRS) + 32) : NULL;
	for (int64_t i = 0; i < n; i++)
	{
		if (bzy_str_eq(attrs[2 * i], name))
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
		bzy_oob_abort(i, n); // No return.
	}

	void *nm = ((void**)((char*)NODE_GET(node, X_ATTRS) + 32))[2 * i];
	bzy_retain(nm);
	return nm;
}

/**
 * ---- descendants --------------------------------------------------------------
 *
 * Walk a node's subtree pre-order, pushing every descendant element onto `out`.
 * The children list is parser-built (only push_back, so head is 0); read its slots directly. push_back retains each element into the result.
 */
static void collect_descendants(void *node, void *out)
{
	void *kids = NODE_GET(node, X_KIDS);
	if (!kids)
	{
		return;
	}

	int64_t len = *(int64_t*)((char*)kids + 24); // length@24.
	void *data = NODE_GET(kids, 48); // data array@48.
	if (!data)
	{
		return;
	}

	for (int64_t i = 0; i < len; i++)
	{
		void *child = ((void**)((char*)data + 32))[i]; // Borrowed slot.
		bzy_vec_push_back(out, (int64_t)child); // Retains into the result.
		collect_descendants(child, out);
	}
}

/* All descendant elements, pre-order (the receiver excluded), as an owned (+1) List<XmlNode>. */
void *bzy_xml_descendants(void *node)
{
	void *out = bzy_vec_new(4);
	collect_descendants(node, out);
	return out;
}

/* ---- Breezy entry + catchable-error plumbing (the bzy_number_check pattern) -- */

static __thread const char *g_xml_error; // Set by bzy_xml_parse; consumed by bzy_xml_check.

/* Called from Breezy: returns the owned (+1) root in rax, or NULL with the error message buffered for the codegen-emitted bzy_xml_check at the call site. */
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
	*(void**)((char*)exc + 24) = msg; // Exception.message.
	bzy_throw(exc, pc, frame); // Never returns.
}
