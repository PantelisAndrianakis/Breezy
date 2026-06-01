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
