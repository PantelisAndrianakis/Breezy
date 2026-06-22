/* src/lsp.c -- the `breezy --lsp` language server.

   A stdio JSON-RPC 2.0 loop. It reads Content-Length-framed LSP messages from
   stdin, dispatches by method, and publishes the compiler's own diagnostics to
   the editor on document open/save (Task 2). Diagnostics are obtained by
   re-invoking this same binary as `breezy --check <project-root>` -- a fresh
   child per check, so the compiler's first-error `exit(1)` is harmless and the
   server holds no compiler state between checks (spec Phase G, §2).

   This file is frontend tooling: it links into the compiler only, never into a
   user program, so it is exempt from the no-bloat and performance invariants. */

#include "lsp.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#ifdef _WIN32
#include <io.h>
#include <fcntl.h>
#endif

/* argv[0]: the path this server re-invokes with --check (set in lsp_main). */
static const char *g_self_exe;

/* ---- a tiny growable string builder, used to compose outgoing messages ---- */

typedef struct
{
	char  *p;
	size_t len, cap;
} Sb;

static void sb_reserve(Sb *s, size_t extra)
{
	if (s->len + extra + 1 > s->cap)
	{
		s->cap = (s->len + extra + 1) * 2;
		s->p = realloc(s->p, s->cap);
	}
}
static void sb_putn(Sb *s, const char *b, size_t n)
{
	sb_reserve(s, n);
	memcpy(s->p + s->len, b, n);
	s->len += n;
	s->p[s->len] = '\0';
}
static void sb_puts(Sb *s, const char *b)
{
	sb_putn(s, b, strlen(b));
}
static void sb_putc(Sb *s, char c)
{
	sb_reserve(s, 1);
	s->p[s->len++] = c;
	s->p[s->len] = '\0';
}

/* ---- minimal JSON reader ----

   A structural recursive-descent parser into a small tagged tree. It is
   structural, NOT substring search, because a didOpen payload carries the
   document's full text -- which may itself contain `"uri":` and friends, so the
   wanted field must be found by path, not by scanning. Each value also records
   its raw source span, which lets a request `id` (a number OR a string) be
   echoed back verbatim with no reconstruction. */

typedef enum { J_NULL, J_BOOL, J_NUM, J_STR, J_ARR, J_OBJ } JType;

typedef struct JVal JVal;
struct JVal
{
	JType       type;
	const char *raw;     /* span into the parsed buffer (for verbatim id echo). */
	int         rawlen;
	int         bval;    /* J_BOOL */
	double      num;     /* J_NUM */
	char       *str;     /* J_STR: unescaped, malloc'd. */
	JVal      **items;   /* J_ARR */
	int         nitems;
	char      **keys;    /* J_OBJ */
	JVal      **vals;
	int         npairs;
};

typedef struct
{
	const char *p;
} JParser;

static JVal *jparse_value(JParser *jp);

static void jskip(JParser *jp)
{
	while (*jp->p == ' ' || *jp->p == '\t' || *jp->p == '\n' || *jp->p == '\r')
	{
		jp->p++;
	}
}

static JVal *jval_new(JType t)
{
	JVal *v = calloc(1, sizeof(JVal));
	v->type = t;
	return v;
}

static unsigned hex4(const char *p)
{
	unsigned v = 0;
	for (int i = 0; i < 4; i++)
	{
		char c = p[i];
		v <<= 4;
		if (c >= '0' && c <= '9')
		{
			v |= (unsigned)(c - '0');
		}
		else if (c >= 'a' && c <= 'f')
		{
			v |= (unsigned)(c - 'a' + 10);
		}
		else if (c >= 'A' && c <= 'F')
		{
			v |= (unsigned)(c - 'A' + 10);
		}
	}
	return v;
}

/* Parse a JSON string starting at the opening quote; return the unescaped
   value (malloc'd), advancing past the closing quote. */
static char *jparse_string_raw(JParser *jp)
{
	jp->p++;   /* Opening quote. */
	Sb sb = {0};
	while (*jp->p && *jp->p != '"')
	{
		if (*jp->p == '\\')
		{
			jp->p++;
			char e = *jp->p;
			switch (e)
			{
			case '"':
				sb_putc(&sb, '"');
				break;
			case '\\':
				sb_putc(&sb, '\\');
				break;
			case '/':
				sb_putc(&sb, '/');
				break;
			case 'b':
				sb_putc(&sb, '\b');
				break;
			case 'f':
				sb_putc(&sb, '\f');
				break;
			case 'n':
				sb_putc(&sb, '\n');
				break;
			case 'r':
				sb_putc(&sb, '\r');
				break;
			case 't':
				sb_putc(&sb, '\t');
				break;
			case 'u':
			{
				/* \uXXXX -> UTF-8. BMP only: editor file:// URIs are
				   percent-encoded ASCII, so \u above the BMP never appears. */
				unsigned cp = hex4(jp->p + 1);
				jp->p += 4;
				if (cp < 0x80)
				{
					sb_putc(&sb, (char)cp);
				}
				else if (cp < 0x800)
				{
					sb_putc(&sb, (char)(0xC0 | (cp >> 6)));
					sb_putc(&sb, (char)(0x80 | (cp & 0x3F)));
				}
				else
				{
					sb_putc(&sb, (char)(0xE0 | (cp >> 12)));
					sb_putc(&sb, (char)(0x80 | ((cp >> 6) & 0x3F)));
					sb_putc(&sb, (char)(0x80 | (cp & 0x3F)));
				}
				break;
			}
			default:
				sb_putc(&sb, e);
				break;
			}
			if (*jp->p)
			{
				jp->p++;
			}
		}
		else
		{
			sb_putc(&sb, *jp->p);
			jp->p++;
		}
	}
	if (*jp->p == '"')
	{
		jp->p++;
	}
	if (!sb.p)
	{
		sb.p = calloc(1, 1);
	}
	return sb.p;
}

static JVal *jparse_value(JParser *jp)
{
	jskip(jp);
	const char *start = jp->p;
	char c = *jp->p;
	JVal *v;
	if (c == '"')
	{
		v = jval_new(J_STR);
		v->str = jparse_string_raw(jp);
	}
	else if (c == '{')
	{
		v = jval_new(J_OBJ);
		jp->p++;
		jskip(jp);
		while (*jp->p && *jp->p != '}')
		{
			jskip(jp);
			if (*jp->p != '"')
			{
				break;
			}
			char *key = jparse_string_raw(jp);
			jskip(jp);
			if (*jp->p == ':')
			{
				jp->p++;
			}
			JVal *val = jparse_value(jp);
			v->keys = realloc(v->keys, sizeof(char *) * (v->npairs + 1));
			v->vals = realloc(v->vals, sizeof(JVal *) * (v->npairs + 1));
			v->keys[v->npairs] = key;
			v->vals[v->npairs] = val;
			v->npairs++;
			jskip(jp);
			if (*jp->p == ',')
			{
				jp->p++;
			}
		}
		if (*jp->p == '}')
		{
			jp->p++;
		}
	}
	else if (c == '[')
	{
		v = jval_new(J_ARR);
		jp->p++;
		jskip(jp);
		while (*jp->p && *jp->p != ']')
		{
			JVal *item = jparse_value(jp);
			v->items = realloc(v->items, sizeof(JVal *) * (v->nitems + 1));
			v->items[v->nitems++] = item;
			jskip(jp);
			if (*jp->p == ',')
			{
				jp->p++;
			}
		}
		if (*jp->p == ']')
		{
			jp->p++;
		}
	}
	else if (c == 't' || c == 'f')
	{
		v = jval_new(J_BOOL);
		v->bval = (c == 't');
		while (isalpha((unsigned char)*jp->p))
		{
			jp->p++;
		}
	}
	else if (c == 'n')
	{
		v = jval_new(J_NULL);
		while (isalpha((unsigned char)*jp->p))
		{
			jp->p++;
		}
	}
	else
	{
		v = jval_new(J_NUM);
		char *end = (char *)jp->p;
		v->num = strtod(jp->p, &end);
		jp->p = end;
	}
	v->raw = start;
	v->rawlen = (int)(jp->p - start);
	return v;
}

static JVal *json_parse(const char *buf)
{
	JParser jp = { buf };
	return jparse_value(&jp);
}

static void json_free(JVal *v)
{
	if (!v)
	{
		return;
	}
	switch (v->type)
	{
	case J_STR:
		free(v->str);
		break;
	case J_ARR:
		for (int i = 0; i < v->nitems; i++)
		{
			json_free(v->items[i]);
		}
		free(v->items);
		break;
	case J_OBJ:
		for (int i = 0; i < v->npairs; i++)
		{
			free(v->keys[i]);
			json_free(v->vals[i]);
		}
		free(v->keys);
		free(v->vals);
		break;
	default:
		break;
	}
	free(v);
}

/* Look up a key in an object value; NULL if absent or not an object. */
static JVal *jobj_get(JVal *o, const char *key)
{
	if (!o || o->type != J_OBJ)
	{
		return NULL;
	}
	for (int i = 0; i < o->npairs; i++)
	{
		if (strcmp(o->keys[i], key) == 0)
		{
			return o->vals[i];
		}
	}
	return NULL;
}

/* ---- framing ---- */

/* Read one header line into buf (CR stripped, NUL-terminated). Returns the
   length, 0 for a blank line, or -1 at EOF before any newline. */
static int read_line(char *buf, int max)
{
	int i = 0, c;
	while ((c = getchar()) != EOF)
	{
		if (c == '\n')
		{
			if (i > 0 && buf[i - 1] == '\r')
			{
				i--;
			}
			buf[i] = '\0';
			return i;
		}
		if (i < max - 1)
		{
			buf[i++] = (char)c;
		}
	}
	return -1;
}

/* Read one LSP message body (the JSON after the Content-Length header block).
   Returns a malloc'd NUL-terminated buffer, or NULL at EOF / on a malformed
   header block. */
static char *read_message(void)
{
	int content_length = -1;
	char line[256];
	for (;;)
	{
		int n = read_line(line, sizeof(line));
		if (n < 0)
		{
			return NULL;   /* EOF. */
		}
		if (n == 0)
		{
			break;         /* Blank line: end of headers. */
		}
		if (strncmp(line, "Content-Length:", 15) == 0)
		{
			content_length = atoi(line + 15);
		}
		/* Any other header (e.g. Content-Type) is tolerated and ignored. */
	}
	if (content_length < 0 || content_length > (1 << 28))
	{
		return NULL;
	}
	char *body = malloc((size_t)content_length + 1);
	size_t got = fread(body, 1, (size_t)content_length, stdin);
	body[got] = '\0';
	if ((int)got != content_length)
	{
		free(body);
		return NULL;
	}
	return body;
}

/* Write a framed message: Content-Length header + the JSON body. */
static void send_framed(const char *body, size_t len)
{
	char hdr[64];
	int hn = snprintf(hdr, sizeof(hdr), "Content-Length: %d\r\n\r\n", (int)len);
	fwrite(hdr, 1, (size_t)hn, stdout);
	fwrite(body, 1, len, stdout);
	fflush(stdout);
}

/* Append a request id (echoed verbatim from its raw span, so a number or a
   string id both round-trip), or `null` when there is none. */
static void sb_put_id(Sb *sb, JVal *id)
{
	if (id)
	{
		sb_putn(sb, id->raw, (size_t)id->rawlen);
	}
	else
	{
		sb_puts(sb, "null");
	}
}

/* Reply to a request with a result object (result is a raw JSON fragment). */
static void reply_result(JVal *id, const char *result)
{
	Sb sb = {0};
	sb_puts(&sb, "{\"jsonrpc\":\"2.0\",\"id\":");
	sb_put_id(&sb, id);
	sb_puts(&sb, ",\"result\":");
	sb_puts(&sb, result);
	sb_putc(&sb, '}');
	send_framed(sb.p, sb.len);
	free(sb.p);
}

/* Reply to an unsupported request with JSON-RPC MethodNotFound. */
static void reply_method_not_found(JVal *id)
{
	Sb sb = {0};
	sb_puts(&sb, "{\"jsonrpc\":\"2.0\",\"id\":");
	sb_put_id(&sb, id);
	sb_puts(&sb, ",\"error\":{\"code\":-32601,\"message\":\"Method not found\"}}");
	send_framed(sb.p, sb.len);
	free(sb.p);
}

/* ---- server loop ---- */

int lsp_main(const char *self_exe)
{
	g_self_exe = self_exe;
#ifdef _WIN32
	/* Binary stdio so the \r\n framing bytes survive unmangled. */
	_setmode(_fileno(stdin), _O_BINARY);
	_setmode(_fileno(stdout), _O_BINARY);
#endif
	int draining = 0;   /* Set once `shutdown` is seen; `exit` then returns 0. */
	char *msg;
	while ((msg = read_message()) != NULL)
	{
		JVal *root = json_parse(msg);
		JVal *method = jobj_get(root, "method");
		JVal *id = jobj_get(root, "id");
		const char *m = (method && method->type == J_STR) ? method->str : "";

		if (strcmp(m, "initialize") == 0)
		{
			reply_result(id, "{\"capabilities\":{\"textDocumentSync\":1}}");
		}
		else if (strcmp(m, "initialized") == 0)
		{
			/* Notification, no-op. */
		}
		else if (strcmp(m, "textDocument/didOpen") == 0
				 || strcmp(m, "textDocument/didSave") == 0)
		{
			/* Task 2: check the document and publish diagnostics. */
		}
		else if (strcmp(m, "textDocument/didChange") == 0)
		{
			/* Task 2: record the dirty buffer; v1 does not check on change. */
		}
		else if (strcmp(m, "shutdown") == 0)
		{
			reply_result(id, "null");
			draining = 1;
		}
		else if (strcmp(m, "exit") == 0)
		{
			json_free(root);
			free(msg);
			return draining ? 0 : 1;
		}
		else if (id && method)
		{
			/* An unknown request (has an id): answer so the client is not left
			   waiting. A notification (no id) is simply ignored. */
			reply_method_not_found(id);
		}

		json_free(root);
		free(msg);
	}
	return draining ? 0 : 1;
}
