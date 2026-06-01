#include "breezy.h"
#include <string.h>
#include <stdio.h>

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
