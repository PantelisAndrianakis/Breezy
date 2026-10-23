/**
 * The XmlNode layout and the helpers runtime/xml.c shares with runtime/xmlwrite.c.
 * Private to the runtime: compiled programs reach nodes only through bzy_xml_* calls.
 */
#ifndef BZY_XML_NODE_H
#define BZY_XML_NODE_H
#include <stdint.h>

/**
 * XmlNode layout: vtable|rc|gcinfo|name@24|text@32|attrs@40|acount@48|children@56|src@64|offsets@72..95|id@96|parent_id@104|dirty@112.
 * attrs is one array of 2 * acount strings: name, value, name, value, ...
 * The six 32-bit offsets index src, the document the node was parsed from (NULL for a node made by Xml.element).
 */
#define X_NAME   24
#define X_TEXT   32
#define X_ATTRS  40
#define X_ACOUNT 48
#define X_KIDS   56
#define X_SRC    64
#define X_LEAD   72 // Start of the gap in front of this element.
#define X_START  76 // This element's '<'.
#define X_STAG   80 // Just past the start tag's '>'.
#define X_TAIL   84 // Just past the last original child (X_STAG when there was none).
#define X_ETAG   88 // The end tag's '<' (X_STAG for an empty or self-closing element).
#define X_END    92 // Just past the element (X_STAG for a self-closing tag).
#define X_ID     96 // Unique per parsed element; 0 for Xml.element nodes.
#define X_PID    104 // The id of the element this one was parsed under; 0 for a root.
#define X_DIRTY  112 // XD_* bits.
#define X_SIZE   120

#define XD_NAME  1 // rename.
#define XD_ATTRS 2 // setAttr / removeAttr.
#define XD_TEXT  4 // setText.

#define NODE_SET(n, off, v) (*(void**)((char*)(n) + (off)) = (void*)(v))
#define NODE_GET(n, off)    (*(void**)((char*)(n) + (off)))
#define NODE_I32(n, off)    (*(int32_t*)((char*)(n) + (off)))
#define NODE_I64(n, off)    (*(int64_t*)((char*)(n) + (off)))

/**
 * Child i of a children list, borrowed: no retain and no release.
 * Walking a tree with bzy_vec_get would retain then release every node, and each release that leaves a node alive makes it a cycle candidate.
 * List layout: len@24 cap@32 head@40 data@48 (a ring; slots start 32 bytes into the data array).
 */
static inline void *xml_kid(void *kids, int64_t i)
{
	int64_t cap = *(int64_t*)((char*)kids + 32), head = *(int64_t*)((char*)kids + 40);
	return ((void**)((char*)*(void**)((char*)kids + 48) + 32))[(head + i) & (cap - 1)];
}

void bzy_xml_fail(const char *msg); // Buffer an XmlException message for the call site's bzy_xml_check.
int  bzy_xml_valid_name(void *s); // 1 when s is a non-empty XML name the reader accepts.
int  bzy_xml_valid_chars(void *s); // 1 when s holds no character XML 1.0 forbids.
void *bzy_xml_decode_value(const char *p, int64_t len); // Owned (+1) string: raw attribute text with entities decoded.
#endif
