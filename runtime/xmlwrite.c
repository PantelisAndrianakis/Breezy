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
		void *c = (void*)bzy_vec_get(kids, i); // +1.
		int ok = in_place(c, node) && NODE_I32(c, X_LEAD) == expect && clean(c);
		expect = NODE_I32(c, X_END);
		bzy_release(c);
		if (!ok)
		{
			return 0;
		}
	}

	return expect == NODE_I32(node, X_TAIL);
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

static void write_el(W *w, void *node, const char *ind, int64_t ind_len)
{
	(void)ind;
	(void)ind_len;
	if (clean(node))
	{
		const char *d = src_of(node);
		put(w, d + NODE_I32(node, X_START), NODE_I32(node, X_END) - NODE_I32(node, X_START));
		return;
	}

	bzy_xml_fail("Writing an edited XML tree is not supported.");
	w->failed = 1;
}
