#include "breezy.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

/* Immutable string layout (one allocation):
   0: vtable ptr  8: refcount  16: gcinfo  24: length  32: bytes[length+1].
   The runtime owns the string's vtable/descriptor in C, since codegen never
   emits one. The descriptor has no finalizer and no object fields, so a string
   is reclaimed by a plain free and is invisible to the cycle collector. */

static int64_t g_string_typeinfo[2] = { 0 /* finalizer */, 0 /* object-field count */ };
static int64_t g_string_vtable[2];   /* [0] = typeinfo back-pointer (becomes vtable[-1]); [1] unused. */

static void *string_vtable(void)
{
	g_string_vtable[0] = (int64_t)&g_string_typeinfo[0];
	return &g_string_vtable[1];
}

void *bzy_str_new(const char *bytes, int64_t len)
{
	void *s = bzy_alloc(32 + len + 1);
	*(void**)s = string_vtable();
	*(int64_t*)((char*)s + 24) = len;
	char *data = (char*)s + 32;
	if (len && bytes)
	{
		memcpy(data, bytes, (size_t)len);
	}

	data[len] = '\0';
	return s;
}

int64_t bzy_str_len(void *s)
{
	return *(int64_t*)((char*)s + 24);
}

const char *bzy_str_data(void *s)
{
	return (const char*)s + 32;
}

void *bzy_str_concat(void *a, void *b)
{
	int64_t la = bzy_str_len(a), lb = bzy_str_len(b);
	void *c = bzy_str_new(NULL, la + lb);
	char *d = (char*)c + 32;
	memcpy(d, bzy_str_data(a), (size_t)la);
	memcpy(d + la, bzy_str_data(b), (size_t)lb);
	d[la + lb] = '\0';
	return c;
}

int64_t bzy_str_eq(void *a, void *b)
{
	if (a == b)
	{
		return 1;
	}

	if (!a || !b)
	{
		return 0;
	}

	int64_t la = bzy_str_len(a), lb = bzy_str_len(b);
	return (la == lb && memcmp(bzy_str_data(a), bzy_str_data(b), (size_t)la) == 0) ? 1 : 0;
}

/* First index of needle in s, or -1. An empty needle matches at 0. */
static int64_t str_find(const char *s, int64_t sl, const char *n, int64_t nl)
{
	if (nl == 0)
	{
		return 0;
	}

	if (nl > sl)
	{
		return -1;
	}

	for (int64_t i = 0; i + nl <= sl; i++)
	{
		if (memcmp(s + i, n, (size_t)nl) == 0)
		{
			return i;
		}
	}

	return -1;
}

int64_t bzy_str_index_of(void *s, void *needle)
{
	return str_find(bzy_str_data(s), bzy_str_len(s), bzy_str_data(needle), bzy_str_len(needle));
}

int64_t bzy_str_contains(void *s, void *needle)
{
	return bzy_str_index_of(s, needle) >= 0 ? 1 : 0;
}

int64_t bzy_str_starts_with(void *s, void *pre)
{
	int64_t sl = bzy_str_len(s), pl = bzy_str_len(pre);
	return (pl <= sl && memcmp(bzy_str_data(s), bzy_str_data(pre), (size_t)pl) == 0) ? 1 : 0;
}

int64_t bzy_str_ends_with(void *s, void *suf)
{
	int64_t sl = bzy_str_len(s), fl = bzy_str_len(suf);
	return (fl <= sl && memcmp(bzy_str_data(s) + sl - fl, bzy_str_data(suf), (size_t)fl) == 0) ? 1 : 0;
}

void *bzy_str_substring(void *s, int64_t start, int64_t end)
{
	int64_t sl = bzy_str_len(s);
	if (start < 0)
	{
		start = 0;
	}

	if (end > sl)
	{
		end = sl;
	}

	if (start > end)
	{
		start = end;
	}

	return bzy_str_new(bzy_str_data(s) + start, end - start);
}

void *bzy_str_replace(void *s, void *from, void *to)
{
	const char *t = bzy_str_data(s);
	int64_t tl = bzy_str_len(s);
	const char *f = bzy_str_data(from);
	int64_t fl = bzy_str_len(from);
	const char *r = bzy_str_data(to);
	int64_t rl = bzy_str_len(to);
	if (fl == 0)
	{
		return bzy_str_new(t, tl);                 /* Empty needle: no-op (avoid looping). */
	}

	int64_t cap = tl + 16, n = 0;
	char *buf = malloc((size_t)cap);
	int64_t pos = 0;
	while (pos <= tl - fl)
	{
		if (memcmp(t + pos, f, (size_t)fl) == 0)
		{
			if (n + rl > cap)
			{
				while (n + rl > cap)
				{
					cap *= 2;
				}

				buf = realloc(buf, (size_t)cap);
			}

			memcpy(buf + n, r, (size_t)rl);
			n += rl;
			pos += fl;
		}
		else
		{
			if (n + 1 > cap)
			{
				cap *= 2;
				buf = realloc(buf, (size_t)cap);
			}

			buf[n++] = t[pos++];
		}
	}

	int64_t tail = tl - pos;
	if (n + tail > cap)
	{
		cap = n + tail;
		buf = realloc(buf, (size_t)cap);
	}

	memcpy(buf + n, t + pos, (size_t)tail);        /* Trailing bytes after the last match. */
	n += tail;
	void *out = bzy_str_new(buf, n);
	free(buf);
	return out;
}

static int is_ws(char c)
{
	return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}

void *bzy_str_trim(void *s)
{
	const char *t = bzy_str_data(s);
	int64_t a = 0, b = bzy_str_len(s);
	while (a < b && is_ws(t[a]))
	{
		a++;
	}

	while (b > a && is_ws(t[b - 1]))
	{
		b--;
	}

	return bzy_str_new(t + a, b - a);
}

void *bzy_str_to_upper(void *s)
{
	int64_t sl = bzy_str_len(s);
	const char *t = bzy_str_data(s);
	char *buf = malloc((size_t)sl + 1);
	for (int64_t i = 0; i < sl; i++)
	{
		char c = t[i];
		buf[i] = (c >= 'a' && c <= 'z') ? (char)(c - 32) : c;
	}

	void *o = bzy_str_new(buf, sl);
	free(buf);
	return o;
}

void *bzy_str_to_lower(void *s)
{
	int64_t sl = bzy_str_len(s);
	const char *t = bzy_str_data(s);
	char *buf = malloc((size_t)sl + 1);
	for (int64_t i = 0; i < sl; i++)
	{
		char c = t[i];
		buf[i] = (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
	}

	void *o = bzy_str_new(buf, sl);
	free(buf);
	return o;
}

void bzy_print_str(void *s)
{
	fwrite(bzy_str_data(s), 1, (size_t)bzy_str_len(s), stdout);
	fputc('\n', stdout);
}

/* StringBuilder layout (one object; owns a separate doubling buffer):
   0: vtable  8: refcount  16: gcinfo  24: length  32: capacity  40: buf ptr.
   The descriptor carries a finalizer that frees buf before the object is freed;
   it has no object fields (buf is raw bytes), so the cycle collector ignores it. */

static int64_t *SB_LEN(void *sb)
{
	return (int64_t*)((char*)sb + 24);
}

static int64_t *SB_CAP(void *sb)
{
	return (int64_t*)((char*)sb + 32);
}

static char **SB_BUF(void *sb)
{
	return (char**)((char*)sb + 40);
}

static void bzy_sb_finalize(void *sb)
{
	free(*SB_BUF(sb));
}

static int64_t g_sb_typeinfo[2] = { 0 /* finalizer (set on first use) */, 0 /* object-field count */ };
static int64_t g_sb_vtable[2];   /* [0] = typeinfo back-pointer (becomes vtable[-1]); [1] unused. */

static void *sb_vtable(void)
{
	g_sb_typeinfo[0] = (int64_t)(void*)bzy_sb_finalize;
	g_sb_vtable[0] = (int64_t)&g_sb_typeinfo[0];
	return &g_sb_vtable[1];
}

void *bzy_sb_new(void)
{
	void *sb = bzy_alloc(48);   /* length/capacity/buf are zeroed by bzy_alloc. */
	*(void**)sb = sb_vtable();
	return sb;
}

void bzy_sb_append_cstr(void *sb, const char *bytes, int64_t len)
{
	int64_t used = *SB_LEN(sb), cap = *SB_CAP(sb);
	if (used + len > cap)
	{
		int64_t ncap = cap ? cap : 16;
		while (ncap < used + len)
		{
			ncap *= 2;
		}

		char *nb = realloc(*SB_BUF(sb), (size_t)ncap);
		if (!nb)
		{
			abort();
		}

		*SB_BUF(sb) = nb;
		*SB_CAP(sb) = ncap;
	}

	if (len)
	{
		memcpy(*SB_BUF(sb) + used, bytes, (size_t)len);
	}

	*SB_LEN(sb) = used + len;
}

void bzy_sb_append(void *sb, void *s)
{
	bzy_sb_append_cstr(sb, bzy_str_data(s), bzy_str_len(s));
}

void *bzy_sb_to_string(void *sb)
{
	return bzy_str_new(*SB_BUF(sb), *SB_LEN(sb));
}
