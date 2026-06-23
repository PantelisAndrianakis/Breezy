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
#include <windows.h>
#include <direct.h>
#define BZY_POPEN  _popen
#define BZY_PCLOSE _pclose
#else
#include <dirent.h>
#include <sys/stat.h>
#define BZY_POPEN  popen
#define BZY_PCLOSE pclose
#endif

/* argv[0]: the path this server re-invokes with --check (set in lsp_main). */
static const char *g_self_exe;

static int same_file(const char *a, const char *b);   /* Defined below; used by the doc store. */

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

/* ---- diagnostics: re-invoke --check, translate, publish ---- */

static char *dupstr(const char *s)
{
	size_t n = strlen(s) + 1;
	char *r = malloc(n);
	memcpy(r, s, n);
	return r;
}

static int hexval(char c)
{
	if (c >= '0' && c <= '9')
	{
		return c - '0';
	}

	if (c >= 'a' && c <= 'f')
	{
		return c - 'a' + 10;
	}

	if (c >= 'A' && c <= 'F')
	{
		return c - 'A' + 10;
	}

	return 0;
}

static int has_nonspace(const char *t)
{
	for (; t && *t; t++)
	{
		if (!isspace((unsigned char)*t))
		{
			return 1;
		}
	}

	return 0;
}

/* JSON-escape a string into the builder (for uri + message values). */
static void sb_put_json_escaped(Sb *sb, const char *s)
{
	for (; s && *s; s++)
	{
		unsigned char c = (unsigned char)*s;
		switch (c)
		{
		case '"':
			sb_puts(sb, "\\\"");
			break;
		case '\\':
			sb_puts(sb, "\\\\");
			break;
		case '\n':
			sb_puts(sb, "\\n");
			break;
		case '\r':
			sb_puts(sb, "\\r");
			break;
		case '\t':
			sb_puts(sb, "\\t");
			break;
		default:
			if (c < 0x20)
			{
				char b[8];
				snprintf(b, sizeof(b), "\\u%04x", c);
				sb_puts(sb, b);
			}
			else
			{
				sb_putc(sb, (char)c);
			}
			break;
		}
	}
}

/* The open-document store: exact editor URIs + their decoded filesystem paths.
   v1 needs the open set so a clean check clears every open doc, and an error in
   one file leaves the others clear. (No didClose handling yet: the set only
   grows for the session -- negligible.) */
static char **g_uris;
static char **g_paths;
static char **g_texts;   /* The latest (possibly unsaved) buffer per doc, or NULL. */
static int    g_ndocs;

static int docs_index(const char *uri)
{
	for (int i = 0; i < g_ndocs; i++)
	{
		if (strcmp(g_uris[i], uri) == 0)
		{
			return i;
		}
	}

	return -1;
}

static void docs_add(const char *uri, const char *path)
{
	if (docs_index(uri) >= 0)
	{
		return;
	}

	g_uris = realloc(g_uris, sizeof(char *) * (g_ndocs + 1));
	g_paths = realloc(g_paths, sizeof(char *) * (g_ndocs + 1));
	g_texts = realloc(g_texts, sizeof(char *) * (g_ndocs + 1));
	g_uris[g_ndocs] = dupstr(uri);
	g_paths[g_ndocs] = dupstr(path);
	g_texts[g_ndocs] = NULL;
	g_ndocs++;
}

/* Record the document's current buffer text (full-sync didOpen / didChange). */
static void docs_set_text(const char *uri, const char *text)
{
	int i = docs_index(uri);
	if (i < 0)
	{
		return;
	}
	free(g_texts[i]);
	g_texts[i] = text ? dupstr(text) : NULL;
}

/* The buffer text for an open file path (same_file match), or NULL if not open. */
static char *docs_text_for(const char *path)
{
	for (int i = 0; i < g_ndocs; i++)
	{
		if (g_texts[i] && same_file(g_paths[i], path))
		{
			return g_texts[i];
		}
	}

	return NULL;
}

/* file:///c:/a/b -> c:/a/b ; file:///home/x -> /home/x ; percent-decode. */
static char *uri_to_path(const char *uri)
{
	const char *p = uri;
	if (strncmp(p, "file://", 7) == 0)
	{
		p += 7;
		/* Skip an authority (file://host/path) if one is present. */
		if (*p && *p != '/')
		{
			const char *slash = strchr(p, '/');
			p = slash ? slash : p + strlen(p);
		}
	}

	const char *q = p;
#ifdef _WIN32
	/* file:///c:/... -> drop the slash before the drive letter. */
	if (q[0] == '/' && isalpha((unsigned char)q[1]) && q[2] == ':')
	{
		q++;
	}

#endif
	Sb sb = {0};
	for (; *q; q++)
	{
		if (*q == '%' && isxdigit((unsigned char)q[1]) && isxdigit((unsigned char)q[2]))
		{
			sb_putc(&sb, (char)((hexval(q[1]) << 4) | hexval(q[2])));
			q += 2;
		}
		else
		{
			sb_putc(&sb, *q);
		}
	}

	if (!sb.p)
	{
		sb.p = calloc(1, 1);
	}

	return sb.p;
}

static char *path_to_uri(const char *p)
{
	Sb sb = {0};
	sb_puts(&sb, "file://");
#ifdef _WIN32
	if (isalpha((unsigned char)p[0]) && p[1] == ':')
	{
		sb_putc(&sb, '/');   /* file:///C:/... */
	}

#endif
	for (; *p; p++)
	{
		sb_putc(&sb, *p == '\\' ? '/' : *p);
	}

	if (!sb.p)
	{
		sb.p = calloc(1, 1);
	}

	return sb.p;
}

/* The directory part of a path (a copy; "." if the path has no separator). */
static char *file_dir(const char *path)
{
	char *dir = dupstr(path);
	char *slash = strrchr(dir, '/');
#ifdef _WIN32
	{
		char *b = strrchr(dir, '\\');
		if (b && (!slash || b > slash))
		{
			slash = b;
		}
	}

#endif
	if (slash)
	{
		*slash = '\0';
	}
	else
	{
		free(dir);
		return dupstr(".");
	}

	return dir;
}

static int has_breezy_toml(const char *dir)
{
	Sb t = {0};
	sb_puts(&t, dir);
	sb_puts(&t, "/breezy.toml");
	FILE *f = fopen(t.p, "rb");
	free(t.p);
	if (f)
	{
		fclose(f);
		return 1;
	}

	return 0;
}

/* The project root for a file: the nearest ancestor holding a breezy.toml, else
   the file's own directory (matches how `breezy <dir>` scopes a project). */
static char *project_root(const char *path)
{
	char *dir = file_dir(path);
	char *probe = dupstr(dir);
	for (;;)
	{
		if (has_breezy_toml(probe))
		{
			free(dir);
			return probe;
		}

		char *up = strrchr(probe, '/');
#ifdef _WIN32
		{
			char *b = strrchr(probe, '\\');
			if (b && (!up || b > up))
			{
				up = b;
			}
		}

#endif
		if (!up || up == probe)
		{
			break;
		}
		*up = '\0';
	}

	free(probe);
	return dir;
}

/* Path equality, separator- and (on Windows) case-insensitive. */
static int same_file(const char *a, const char *b)
{
	for (;;)
	{
		char ca = *a, cb = *b;
		if (ca == '\\')
		{
			ca = '/';
		}

		if (cb == '\\')
		{
			cb = '/';
		}

#ifdef _WIN32
		ca = (char)tolower((unsigned char)ca);
		cb = (char)tolower((unsigned char)cb);
#endif
		if (ca != cb)
		{
			return 0;
		}

		if (!ca)
		{
			return 1;
		}

		a++;
		b++;
	}
}

/* Best-effort 1-based line scrape from a plain-text compiler error (the parser
   paths that print "line N: ..." to stderr instead of the JSON diagnostic). */
static int scrape_line(const char *t)
{
	const char *m = strstr(t, "line ");
	if (m)
	{
		int n = atoi(m + 5);
		if (n > 0)
		{
			return n;
		}
	}

	for (const char *p = t; *p; p++)
	{
		if (*p == ':' && isdigit((unsigned char)p[1]))
		{
			int n = atoi(p + 1);
			if (n > 0)
			{
				return n;
			}
		}
	}

	return 1;
}

static char *first_line(const char *t)
{
	size_t n = 0;
	while (t[n] && t[n] != '\n' && t[n] != '\r')
	{
		n++;
	}

	char *r = malloc(n + 1);
	memcpy(r, t, n);
	r[n] = '\0';
	return r;
}

/* Publish diagnostics for one URI. msg==NULL clears (empty array); otherwise a
   single Error diagnostic. line0/char0/endchar0 are already 0-based (LSP positions);
   the squiggle spans [char0, endchar0) -- the offending token. */
static void send_diagnostics(const char *uri, int line0, int char0, int endchar0, const char *msg)
{
	if (line0 < 0)
	{
		line0 = 0;
	}

	if (char0 < 0)
	{
		char0 = 0;
	}

	if (endchar0 <= char0)
	{
		endchar0 = char0 + 1;   /* Always at least one character wide. */
	}

	Sb sb = {0};
	sb_puts(&sb, "{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/publishDiagnostics\",\"params\":{\"uri\":\"");
	sb_put_json_escaped(&sb, uri);
	sb_puts(&sb, "\",\"diagnostics\":[");
	if (msg)
	{
		char range[160];
		snprintf(range, sizeof(range),
				 "{\"range\":{\"start\":{\"line\":%d,\"character\":%d},"
				 "\"end\":{\"line\":%d,\"character\":%d}},"
				 "\"severity\":1,\"source\":\"breezy\",\"message\":\"",
				 line0, char0, line0, endchar0);
		sb_puts(&sb, range);
		sb_put_json_escaped(&sb, msg);
		sb_puts(&sb, "\"}");
	}

	sb_puts(&sb, "]}}");
	send_framed(sb.p, sb.len);
	free(sb.p);
}

/* Run `breezy --check <root>` for the document's project, translate the single
   diagnostic (or the clean result), and publish to every open document. */
/* Run `<self> <flag> "<root>" 2>&1` and return its captured stdout (malloc'd, caller
   frees; NULL if there was none). The shared subprocess driver for --check (publish)
   and --symbols (hover/definition). */
static char *run_self_capture(const char *flag, const char *root)
{
	Sb cmd = {0};
#ifdef _WIN32
	/* _popen runs `cmd /c <command>`; when <command> begins with a quote, cmd
	   strips the first and last quote, which would break the inner quoting of a
	   spaced path. Wrap the whole command in an extra quote pair so cmd strips
	   that outer layer and leaves the inner quotes intact. */
	sb_putc(&cmd, '"');
#endif
	sb_putc(&cmd, '"');
	sb_puts(&cmd, g_self_exe);
	sb_puts(&cmd, "\" ");
	sb_puts(&cmd, flag);
	sb_puts(&cmd, " \"");
	sb_puts(&cmd, root);
	sb_puts(&cmd, "\" 2>&1");   /* Capture stderr too: not every compiler error is JSON. */
#ifdef _WIN32
	sb_putc(&cmd, '"');
#endif

	Sb outp = {0};
	FILE *pp = BZY_POPEN(cmd.p, "r");
	if (pp)
	{
		char chunk[1024];
		size_t r;
		while ((r = fread(chunk, 1, sizeof(chunk), pp)) > 0)
		{
			sb_putn(&outp, chunk, r);
		}

		BZY_PCLOSE(pp);
	}

	free(cmd.p);
	return outp.p;
}

/* ---- live-as-you-type: a temp mirror of the project with dirty buffers ---- */

static void write_all(const char *dst, const char *content)
{
	FILE *f = fopen(dst, "wb");
	if (!f)
	{
		return;
	}
	size_t len = strlen(content);
	if (len)
	{
		fwrite(content, 1, len, f);
	}
	fclose(f);
}

static void copy_file(const char *src, const char *dst)
{
	FILE *in = fopen(src, "rb");
	if (!in)
	{
		return;
	}
	FILE *out = fopen(dst, "wb");
	if (!out)
	{
		fclose(in);
		return;
	}
	char buf[8192];
	size_t r;
	while ((r = fread(buf, 1, sizeof(buf), in)) > 0)
	{
		fwrite(buf, 1, r, out);
	}
	fclose(in);
	fclose(out);
}

static void make_dir(const char *path)
{
#ifdef _WIN32
	_mkdir(path);
#else
	mkdir(path, 0700);
#endif
}

static unsigned fnv1a(const char *s)
{
	unsigned h = 2166136261u;
	for (; *s; s++)
	{
		h ^= (unsigned char)*s;
		h *= 16777619u;
	}
	return h;
}

/* Mirror one project file into the temp dir: the open dirty buffer if there is one,
   else a copy of the file on disk. */
static void overlay_one(const char *real_root, const char *tmp, const char *name)
{
	Sb rf = {0};
	sb_puts(&rf, real_root);
	sb_putc(&rf, '/');
	sb_puts(&rf, name);
	Sb df = {0};
	sb_puts(&df, tmp);
	sb_putc(&df, '/');
	sb_puts(&df, name);

	char *dirty = docs_text_for(rf.p);
	if (dirty)
	{
		write_all(df.p, dirty);
	}
	else
	{
		copy_file(rf.p, df.p);
	}

	free(rf.p);
	free(df.p);
}

/* Build a temp mirror of real_root's .bzy files with any open dirty buffer
   substituted, and return the temp dir path (caller frees). One stable temp dir per
   project (FNV of the root), reused and overwritten. Breezy is whole-program, so the
   whole project is mirrored, not just the edited file. */
static char *overlay_build(const char *real_root)
{
#ifdef _WIN32
	const char *base = getenv("TEMP");
	if (!base)
	{
		base = getenv("TMP");
	}
	if (!base)
	{
		base = ".";
	}
#else
	const char *base = "/tmp";
#endif
	Sb t = {0};
	sb_puts(&t, base);
	sb_puts(&t, "/bzylsp_");
	char h[16];
	snprintf(h, sizeof(h), "%08x", fnv1a(real_root));
	sb_puts(&t, h);
	char *tmp = t.p;
	make_dir(tmp);

#ifdef _WIN32
	Sb pat = {0};
	sb_puts(&pat, real_root);
	sb_puts(&pat, "\\*.bzy");
	WIN32_FIND_DATAA fd;
	HANDLE hf = FindFirstFileA(pat.p, &fd);
	free(pat.p);
	if (hf != INVALID_HANDLE_VALUE)
	{
		do
		{
			overlay_one(real_root, tmp, fd.cFileName);
		}
		while (FindNextFileA(hf, &fd));
		FindClose(hf);
	}
#else
	DIR *d = opendir(real_root);
	if (d)
	{
		struct dirent *e;
		while ((e = readdir(d)) != NULL)
		{
			size_t nl = strlen(e->d_name);
			if (nl >= 5 && strcmp(e->d_name + nl - 4, ".bzy") == 0)
			{
				overlay_one(real_root, tmp, e->d_name);
			}
		}
		closedir(d);
	}
#endif
	return tmp;
}

/* Map a path the index reports (which is under the overlay temp dir when live, or the
   real project otherwise) to the real project path: real_root + the file's basename.
   Basenames are unique in a flat one-class-per-file project, so this round-trips a temp
   path to its real twin and leaves a real path effectively unchanged. */
static char *index_to_real(const char *real_root, const char *file)
{
	const char *bn = file + strlen(file);
	while (bn > file && bn[-1] != '/' && bn[-1] != '\\')
	{
		bn--;
	}
	Sb s = {0};
	sb_puts(&s, real_root);
	sb_putc(&s, '/');
	sb_puts(&s, bn);
	return s.p;
}

/* True if an index occurrence's file is the real trigger document. */
static int occ_in_file(const char *real_root, const char *index_file, const char *real_path)
{
	char *r = index_to_real(real_root, index_file);
	int ok = same_file(r, real_path);
	free(r);
	return ok;
}

/* Run `--symbols` for the trigger document's project, through the dirty-buffer overlay
   when the document has unsaved text, so hover / definition / references reflect edits
   that are not yet on disk. Returns the output (caller frees) and sets *real_root (the
   real project dir, for index_to_real); the check root is internal. */
static char *symbols_for_doc(const char *real_path, char **real_root_out)
{
	char *real_root = project_root(real_path);
	int live = docs_text_for(real_path) != NULL;
	char *check_root = live ? overlay_build(real_root) : dupstr(real_root);
	char *out = run_self_capture("--symbols", check_root);
	free(check_root);
	*real_root_out = real_root;
	return out;
}

/* Publish diagnostics for the trigger document's project. When `live`, the project is
   mirrored to a temp dir with the unsaved buffers substituted and checked there, so the
   squiggles reflect edits that are not yet on disk; the reported temp paths are remapped
   back to the real project. Otherwise disk is checked (open/save). */
static void check_and_publish(const char *trigger_uri, int live)
{
	char *path = uri_to_path(trigger_uri);
	char *root = project_root(path);
	char *check_root = live ? overlay_build(root) : dupstr(root);

	char *out = run_self_capture("--check", check_root);

	/* Decide: clean, a precise JSON diagnostic, or a scraped plain-text error. */
	int clean = 0, have_diag = 0;
	char *err_file = NULL;
	char *err_msg = NULL;
	int err_line = 0, err_col = 0, err_endcol = 0;   /* 1-based as the compiler reports. */

	const char *text = out ? out : "";
	const char *brace = strchr(text, '{');
	if (brace)
	{
		JVal *j = json_parse(brace);
		JVal *ok = jobj_get(j, "ok");
		JVal *jline = jobj_get(j, "line");
		JVal *jmsg = jobj_get(j, "message");
		if (ok && ok->type == J_BOOL && ok->bval)
		{
			clean = 1;
		}
		else if (jline && jmsg && jmsg->type == J_STR)
		{
			JVal *jfile = jobj_get(j, "file");
			JVal *jcol = jobj_get(j, "col");
			JVal *jend = jobj_get(j, "endCol");
			have_diag = 1;
			err_file = dupstr((jfile && jfile->type == J_STR) ? jfile->str : path);
			err_line = (int)jline->num;
			err_col = jcol ? (int)jcol->num : 0;
			err_endcol = jend ? (int)jend->num : 0;
			err_msg = dupstr(jmsg->str);
		}

		json_free(j);
	}

	if (!clean && !have_diag)
	{
		/* Non-JSON output: surface it so the error is never silently invisible. */
		if (has_nonspace(text))
		{
			have_diag = 1;
			err_file = dupstr(path);   /* Best guess: the triggering file. */
			err_msg = first_line(text);
			err_line = scrape_line(text);
			err_col = 0;
		}
		else
		{
			clean = 1;   /* No output, no error: treat as clean. */
		}
	}

	/* The overlay check reports temp-dir paths; map them back to the real project so
	   the diagnostic publishes under the file's real URI. */
	if (live && err_file && strncmp(err_file, check_root, strlen(check_root)) == 0)
	{
		Sb rm = {0};
		sb_puts(&rm, root);
		sb_puts(&rm, err_file + strlen(check_root));
		free(err_file);
		err_file = rm.p;
	}

	/* 1-based (compiler) -> 0-based (LSP). col 0 (resolve errors) maps to 0. */
	int l0 = err_line > 0 ? err_line - 1 : 0;
	int c0 = err_col > 0 ? err_col - 1 : 0;
	int ec0 = err_endcol > 0 ? err_endcol - 1 : c0 + 1;   /* Token span end; default 1 char. */

	/* Publish to every open document IN THIS PROJECT: the error's file gets the
	   diagnostic, the project's other open files are cleared. Documents in other
	   projects are left untouched (a clean check here must not wipe their
	   squiggles). */
	int matched = 0;
	for (int i = 0; i < g_ndocs; i++)
	{
		char *ri = project_root(g_paths[i]);
		int covered = same_file(ri, root);
		free(ri);
		if (!covered)
		{
			continue;
		}

		if (have_diag && same_file(g_paths[i], err_file))
		{
			send_diagnostics(g_uris[i], l0, c0, ec0, err_msg);
			matched = 1;
		}
		else
		{
			send_diagnostics(g_uris[i], 0, 0, 0, NULL);
		}
	}
	/* Error in a file that is not currently open: publish under a derived URI. */
	if (have_diag && !matched)
	{
		char *euri = path_to_uri(err_file);
		send_diagnostics(euri, l0, c0, ec0, err_msg);
		free(euri);
	}

	free(err_file);
	free(err_msg);
	free(out);
	free(check_root);
	free(root);
	free(path);
}

/* textDocument/hover: find the named occurrence whose span contains the cursor in the
   --symbols index for the file's project, and reply its "name : type". Replies null
   when the cursor is not on a known symbol. */
static void handle_hover(JVal *id, JVal *params)
{
	JVal *td  = jobj_get(params, "textDocument");
	JVal *uri = jobj_get(td, "uri");
	JVal *pos = jobj_get(params, "position");
	JVal *jl  = jobj_get(pos, "line");
	JVal *jc  = jobj_get(pos, "character");
	if (!uri || uri->type != J_STR || !jl || jl->type != J_NUM || !jc || jc->type != J_NUM)
	{
		reply_result(id, "null");
		return;
	}

	int line1 = (int)jl->num + 1;   /* LSP 0-based -> the compiler's 1-based. */
	int char1 = (int)jc->num + 1;
	char *path = uri_to_path(uri->str);
	char *real_root = NULL;
	char *out  = symbols_for_doc(path, &real_root);

	const char *text  = out ? out : "";
	const char *brace = strchr(text, '{');
	char *hovertext = NULL;   /* "name : type" of the occurrence under the cursor. */
	if (brace)
	{
		JVal *j = json_parse(brace);
		JVal *syms = jobj_get(j, "symbols");
		if (syms && syms->type == J_ARR)
		{
			for (int i = 0; i < syms->nitems; i++)
			{
				JVal *s  = syms->items[i];
				JVal *sf = jobj_get(s, "file");
				JVal *sl = jobj_get(s, "line");
				JVal *sc = jobj_get(s, "col");
				JVal *se = jobj_get(s, "endCol");
				JVal *sn = jobj_get(s, "name");
				JVal *st = jobj_get(s, "type");
				if (!sf || sf->type != J_STR || !sl || !sc || !se
						|| !sn || sn->type != J_STR || !st || st->type != J_STR)
				{
					continue;
				}
				if ((int)sl->num != line1 || char1 < (int)sc->num || char1 >= (int)se->num)
				{
					continue;
				}
				if (!occ_in_file(real_root, sf->str, path))
				{
					continue;
				}

				Sb t = {0};
				sb_puts(&t, sn->str);
				sb_puts(&t, " : ");
				sb_puts(&t, st->str);
				hovertext = dupstr(t.p ? t.p : "");
				free(t.p);
				break;
			}
		}

		json_free(j);
	}

	if (hovertext)
	{
		Sb r = {0};
		sb_puts(&r, "{\"contents\":{\"kind\":\"plaintext\",\"value\":\"");
		sb_put_json_escaped(&r, hovertext);
		sb_puts(&r, "\"}}");
		reply_result(id, r.p);
		free(r.p);
		free(hovertext);
	}
	else
	{
		reply_result(id, "null");
	}

	free(out);
	free(real_root);
	free(path);
}

/* textDocument/definition: find the occurrence under the cursor in the --symbols
   index and reply the Location of its declaration (defFile/defLine/defCol), or null
   when the occurrence has no resolvable definition (a local, a built-in, ...). */
static void handle_definition(JVal *id, JVal *params)
{
	JVal *td  = jobj_get(params, "textDocument");
	JVal *uri = jobj_get(td, "uri");
	JVal *pos = jobj_get(params, "position");
	JVal *jl  = jobj_get(pos, "line");
	JVal *jc  = jobj_get(pos, "character");
	if (!uri || uri->type != J_STR || !jl || jl->type != J_NUM || !jc || jc->type != J_NUM)
	{
		reply_result(id, "null");
		return;
	}

	int line1 = (int)jl->num + 1;
	int char1 = (int)jc->num + 1;
	char *path = uri_to_path(uri->str);
	char *real_root = NULL;
	char *out  = symbols_for_doc(path, &real_root);

	const char *text  = out ? out : "";
	const char *brace = strchr(text, '{');
	char *defuri = NULL;
	int defl = 0, defc = 0;
	if (brace)
	{
		JVal *j = json_parse(brace);
		JVal *syms = jobj_get(j, "symbols");
		if (syms && syms->type == J_ARR)
		{
			for (int i = 0; i < syms->nitems; i++)
			{
				JVal *s  = syms->items[i];
				JVal *sf = jobj_get(s, "file");
				JVal *sl = jobj_get(s, "line");
				JVal *sc = jobj_get(s, "col");
				JVal *se = jobj_get(s, "endCol");
				if (!sf || sf->type != J_STR || !sl || !sc || !se)
				{
					continue;
				}
				if ((int)sl->num != line1 || char1 < (int)sc->num || char1 >= (int)se->num)
				{
					continue;
				}
				if (!occ_in_file(real_root, sf->str, path))
				{
					continue;
				}

				JVal *df = jobj_get(s, "defFile");
				JVal *dl = jobj_get(s, "defLine");
				JVal *dc = jobj_get(s, "defCol");
				if (df && df->type == J_STR && dl && dl->type == J_NUM && dc && dc->type == J_NUM)
				{
					char *dr = index_to_real(real_root, df->str);   /* Overlay temp -> real. */
					defuri = path_to_uri(dr);
					free(dr);
					defl = (int)dl->num;
					defc = (int)dc->num;
				}

				break;
			}
		}

		json_free(j);
	}

	if (defuri)
	{
		int l0 = defl > 0 ? defl - 1 : 0;   /* 1-based (index) -> 0-based (LSP). */
		int c0 = defc > 0 ? defc - 1 : 0;
		Sb r = {0};
		sb_puts(&r, "{\"uri\":\"");
		sb_put_json_escaped(&r, defuri);
		char range[176];
		snprintf(range, sizeof(range),
				 "\",\"range\":{\"start\":{\"line\":%d,\"character\":%d},"
				 "\"end\":{\"line\":%d,\"character\":%d}}}",
				 l0, c0, l0, c0);
		sb_puts(&r, range);
		reply_result(id, r.p);
		free(r.p);
		free(defuri);
	}
	else
	{
		reply_result(id, "null");
	}

	free(out);
	free(real_root);
	free(path);
}

/* Append one LSP Location {uri, range} (1-based span -> 0-based) to the array builder,
   comma-separated via *first. */
static void append_location(Sb *r, int *first, const char *real_root, const char *file, int line1, int col1, int endcol1)
{
	int l0 = line1 > 0 ? line1 - 1 : 0;
	int c0 = col1 > 0 ? col1 - 1 : 0;
	int e0 = endcol1 > 0 ? endcol1 - 1 : c0 + 1;
	if (e0 < c0)
	{
		e0 = c0;
	}

	if (!*first)
	{
		sb_putc(r, ',');
	}
	*first = 0;

	char *real = index_to_real(real_root, file);   /* Overlay temp path -> the real file. */
	char *uri = path_to_uri(real);
	sb_puts(r, "{\"uri\":\"");
	sb_put_json_escaped(r, uri);
	char range[176];
	snprintf(range, sizeof(range),
			 "\",\"range\":{\"start\":{\"line\":%d,\"character\":%d},"
			 "\"end\":{\"line\":%d,\"character\":%d}}}",
			 l0, c0, l0, e0);
	sb_puts(r, range);
	free(uri);
	free(real);
}

/* textDocument/references: the occurrence under the cursor names a declaration (its
   def triple in the --symbols index); reply every occurrence sharing that declaration
   -- the symbol's uses -- plus the declaration itself when the client asks. A symbol
   with no def link (a local, a built-in) has no references to report. */
static void handle_references(JVal *id, JVal *params)
{
	JVal *td  = jobj_get(params, "textDocument");
	JVal *uri = jobj_get(td, "uri");
	JVal *pos = jobj_get(params, "position");
	JVal *jl  = jobj_get(pos, "line");
	JVal *jc  = jobj_get(pos, "character");
	JVal *ctx = jobj_get(params, "context");
	JVal *inc = ctx ? jobj_get(ctx, "includeDeclaration") : NULL;
	int include_decl = inc && inc->type == J_BOOL && inc->bval;
	if (!uri || uri->type != J_STR || !jl || jl->type != J_NUM || !jc || jc->type != J_NUM)
	{
		reply_result(id, "[]");
		return;
	}

	int line1 = (int)jl->num + 1;
	int char1 = (int)jc->num + 1;
	char *path = uri_to_path(uri->str);
	char *real_root = NULL;
	char *out  = symbols_for_doc(path, &real_root);

	const char *text  = out ? out : "";
	const char *brace = strchr(text, '{');
	Sb r = {0};
	sb_putc(&r, '[');
	int first = 1;
	if (brace)
	{
		JVal *j = json_parse(brace);
		JVal *syms = jobj_get(j, "symbols");
		const char *dfile = NULL, *dname = "";
		int dl = 0, dc = 0;
		if (syms && syms->type == J_ARR)
		{
			/* Pass 1: the occurrence under the cursor and its declaration triple. */
			for (int i = 0; i < syms->nitems; i++)
			{
				JVal *s  = syms->items[i];
				JVal *sf = jobj_get(s, "file");
				JVal *sl = jobj_get(s, "line");
				JVal *sc = jobj_get(s, "col");
				JVal *se = jobj_get(s, "endCol");
				if (!sf || sf->type != J_STR || !sl || !sc || !se)
				{
					continue;
				}
				if ((int)sl->num != line1 || char1 < (int)sc->num || char1 >= (int)se->num)
				{
					continue;
				}
				if (!occ_in_file(real_root, sf->str, path))
				{
					continue;
				}

				JVal *df = jobj_get(s, "defFile");
				JVal *dlv = jobj_get(s, "defLine");
				JVal *dcv = jobj_get(s, "defCol");
				JVal *nm = jobj_get(s, "name");
				if (df && df->type == J_STR && dlv && dlv->type == J_NUM && dcv && dcv->type == J_NUM)
				{
					dfile = df->str;
					dl = (int)dlv->num;
					dc = (int)dcv->num;
					dname = (nm && nm->type == J_STR) ? nm->str : "";
				}
				break;
			}

			/* Pass 2: every occurrence sharing that declaration, + the declaration. */
			if (dfile)
			{
				if (include_decl)
				{
					append_location(&r, &first, real_root, dfile, dl, dc, dc + (int)strlen(dname));
				}
				for (int i = 0; i < syms->nitems; i++)
				{
					JVal *s  = syms->items[i];
					JVal *df = jobj_get(s, "defFile");
					JVal *dlv = jobj_get(s, "defLine");
					JVal *dcv = jobj_get(s, "defCol");
					if (!df || df->type != J_STR || !dlv || dlv->type != J_NUM || !dcv || dcv->type != J_NUM)
					{
						continue;
					}
					if ((int)dlv->num != dl || (int)dcv->num != dc || !same_file(df->str, dfile))
					{
						continue;
					}

					JVal *sf = jobj_get(s, "file");
					JVal *sl = jobj_get(s, "line");
					JVal *sc = jobj_get(s, "col");
					JVal *se = jobj_get(s, "endCol");
					if (!sf || sf->type != J_STR || !sl || !sc || !se)
					{
						continue;
					}
					append_location(&r, &first, real_root, sf->str, (int)sl->num, (int)sc->num, (int)se->num);
				}
			}
		}

		json_free(j);
	}

	sb_putc(&r, ']');
	reply_result(id, r.p);
	free(r.p);
	free(out);
	free(real_root);
	free(path);
}

/* The text on line `line0` up to column `char0` (a copy; the editor's 0-based pos). */
static char *line_prefix(const char *text, int line0, int char0)
{
	const char *p = text;
	for (int ln = 0; ln < line0 && *p; p++)
	{
		if (*p == '\n')
		{
			ln++;
		}
	}
	const char *e = p;
	int c = 0;
	while (*e && *e != '\n' && c < char0)
	{
		e++;
		c++;
	}
	int len = (int)(e - p);
	char *r = malloc((size_t)len + 1);
	memcpy(r, p, (size_t)len);
	r[len] = '\0';
	return r;
}

static int starts_with(const char *s, const char *prefix)
{
	return strncmp(s, prefix, strlen(prefix)) == 0;
}

static int ident_char(char c)
{
	return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
}

/* Append one CompletionItem {label, kind, detail}. */
static void add_item(Sb *r, int *first, const char *label, int kind, const char *detail)
{
	if (!*first)
	{
		sb_putc(r, ',');
	}
	*first = 0;
	sb_puts(r, "{\"label\":\"");
	sb_put_json_escaped(r, label);
	char k[48];
	snprintf(k, sizeof(k), "\",\"kind\":%d", kind);
	sb_puts(r, k);
	if (detail && detail[0])
	{
		sb_puts(r, ",\"detail\":\"");
		sb_put_json_escaped(r, detail);
		sb_putc(r, '"');
	}
	sb_putc(r, '}');
}

/* textDocument/completion: from the dirty line up to the cursor, either complete the
   members of `receiver.` (the receiver's type -> that class's members) or, on a bare
   word, the project's class and function names. Prefix-filtered. */
static void handle_completion(JVal *id, JVal *params)
{
	JVal *td  = jobj_get(params, "textDocument");
	JVal *uri = jobj_get(td, "uri");
	JVal *pos = jobj_get(params, "position");
	JVal *jl  = jobj_get(pos, "line");
	JVal *jc  = jobj_get(pos, "character");
	if (!uri || uri->type != J_STR || !jl || jl->type != J_NUM || !jc || jc->type != J_NUM)
	{
		reply_result(id, "{\"isIncomplete\":false,\"items\":[]}");
		return;
	}

	int line0 = (int)jl->num;
	int char0 = (int)jc->num;
	char *path = uri_to_path(uri->str);
	char *buf = docs_text_for(path);   /* Borrowed: the unsaved buffer, or NULL. */
	char *prefix = buf ? line_prefix(buf, line0, char0) : dupstr("");

	/* The trailing word is the partial being typed; a `.` before it means member
	   completion, with the identifier before the dot as the receiver. */
	int n = (int)strlen(prefix);
	int pe = n;
	while (pe > 0 && ident_char(prefix[pe - 1]))
	{
		pe--;
	}
	char partial[64];
	int pl = n - pe;
	if (pl > 63)
	{
		pl = 63;
	}
	memcpy(partial, prefix + pe, (size_t)pl);
	partial[pl] = '\0';
	int member = 0;
	char receiver[64] = "";
	if (pe > 0 && prefix[pe - 1] == '.')
	{
		member = 1;
		int re = pe - 1;
		int rs = re;
		while (rs > 0 && ident_char(prefix[rs - 1]))
		{
			rs--;
		}
		int rl = re - rs;
		if (rl > 63)
		{
			rl = 63;
		}
		memcpy(receiver, prefix + rs, (size_t)rl);
		receiver[rl] = '\0';
	}
	free(prefix);

	char *real_root = NULL;
	char *out = symbols_for_doc(path, &real_root);
	if (!out || !strstr(out, "\"classes\""))
	{
		/* The dirty buffer did not parse (mid-edit, e.g. `obj.`); fall back to the
		   last-saved version on disk so completion still has the project's types. */
		free(out);
		out = run_self_capture("--symbols", real_root);
	}
	const char *text = out ? out : "";
	const char *brace = strchr(text, '{');
	Sb r = {0};
	sb_puts(&r, "{\"isIncomplete\":false,\"items\":[");
	int first = 1;
	if (brace)
	{
		JVal *j = json_parse(brace);
		if (member && receiver[0])
		{
			/* Resolve the receiver's class from an occurrence of its name. */
			const char *rtype = NULL;
			JVal *syms = jobj_get(j, "symbols");
			if (syms && syms->type == J_ARR)
			{
				for (int i = 0; i < syms->nitems; i++)
				{
					JVal *nm = jobj_get(syms->items[i], "name");
					JVal *ty = jobj_get(syms->items[i], "type");
					if (nm && nm->type == J_STR && ty && ty->type == J_STR && strcmp(nm->str, receiver) == 0)
					{
						rtype = ty->str;
						break;
					}
				}
			}
			JVal *cls = jobj_get(j, "classes");
			if (rtype && cls && cls->type == J_ARR)
			{
				for (int i = 0; i < cls->nitems; i++)
				{
					JVal *cn = jobj_get(cls->items[i], "name");
					if (!cn || cn->type != J_STR || strcmp(cn->str, rtype) != 0)
					{
						continue;
					}
					JVal *mem = jobj_get(cls->items[i], "members");
					for (int k = 0; mem && mem->type == J_ARR && k < mem->nitems; k++)
					{
						JVal *mn = jobj_get(mem->items[k], "name");
						JVal *mk = jobj_get(mem->items[k], "kind");
						JVal *mt = jobj_get(mem->items[k], "type");
						if (!mn || mn->type != J_STR || (partial[0] && !starts_with(mn->str, partial)))
						{
							continue;
						}
						int kind = (mk && mk->type == J_STR && strcmp(mk->str, "method") == 0) ? 2 : 5;
						add_item(&r, &first, mn->str, kind, (mt && mt->type == J_STR) ? mt->str : "");
					}
					break;
				}
			}
		}
		else
		{
			/* Bare word: class names + free-function names. */
			JVal *cls = jobj_get(j, "classes");
			for (int i = 0; cls && cls->type == J_ARR && i < cls->nitems; i++)
			{
				JVal *cn = jobj_get(cls->items[i], "name");
				if (cn && cn->type == J_STR && (!partial[0] || starts_with(cn->str, partial)))
				{
					add_item(&r, &first, cn->str, 7, "class");
				}
			}
			JVal *fns = jobj_get(j, "functions");
			for (int i = 0; fns && fns->type == J_ARR && i < fns->nitems; i++)
			{
				JVal *fn = jobj_get(fns->items[i], "name");
				JVal *ft = jobj_get(fns->items[i], "type");
				if (fn && fn->type == J_STR && (!partial[0] || starts_with(fn->str, partial)))
				{
					add_item(&r, &first, fn->str, 3, (ft && ft->type == J_STR) ? ft->str : "");
				}
			}
		}

		json_free(j);
	}

	sb_puts(&r, "]}");
	reply_result(id, r.p);
	free(r.p);
	free(out);
	free(real_root);
	free(path);
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
			reply_result(id, "{\"capabilities\":{\"textDocumentSync\":1,\"hoverProvider\":true,\"definitionProvider\":true,\"referencesProvider\":true,\"completionProvider\":{\"triggerCharacters\":[\".\"]}}}");
		}
		else if (strcmp(m, "initialized") == 0)
		{
			/* Notification, no-op. */
		}
		else if (strcmp(m, "textDocument/didOpen") == 0
				 || strcmp(m, "textDocument/didSave") == 0)
		{
			JVal *td = jobj_get(jobj_get(root, "params"), "textDocument");
			JVal *uri = jobj_get(td, "uri");
			if (uri && uri->type == J_STR)
			{
				char *path = uri_to_path(uri->str);
				docs_add(uri->str, path);
				free(path);
				JVal *txt = jobj_get(td, "text");   /* didOpen carries the buffer. */
				if (txt && txt->type == J_STR)
				{
					docs_set_text(uri->str, txt->str);
				}
				check_and_publish(uri->str, 0);   /* Disk is authoritative on open/save. */
			}
		}
		else if (strcmp(m, "textDocument/didChange") == 0)
		{
			/* Full sync: contentChanges[0].text is the whole new buffer. Check it live
			   through the overlay so the squiggles track unsaved edits. */
			JVal *params = jobj_get(root, "params");
			JVal *td = jobj_get(params, "textDocument");
			JVal *uri = jobj_get(td, "uri");
			if (uri && uri->type == J_STR)
			{
				char *path = uri_to_path(uri->str);
				docs_add(uri->str, path);
				free(path);
				JVal *changes = jobj_get(params, "contentChanges");
				if (changes && changes->type == J_ARR && changes->nitems > 0)
				{
					JVal *txt = jobj_get(changes->items[0], "text");
					if (txt && txt->type == J_STR)
					{
						docs_set_text(uri->str, txt->str);
					}
				}
				check_and_publish(uri->str, 1);   /* Live: the dirty-buffer overlay. */
			}
		}
		else if (strcmp(m, "textDocument/hover") == 0)
		{
			handle_hover(id, jobj_get(root, "params"));
		}
		else if (strcmp(m, "textDocument/definition") == 0)
		{
			handle_definition(id, jobj_get(root, "params"));
		}
		else if (strcmp(m, "textDocument/references") == 0)
		{
			handle_references(id, jobj_get(root, "params"));
		}
		else if (strcmp(m, "textDocument/completion") == 0)
		{
			handle_completion(id, jobj_get(root, "params"));
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
