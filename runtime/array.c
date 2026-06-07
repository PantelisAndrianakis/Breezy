#include "breezy.h"
#include <stdio.h>
#include <stdlib.h>

/* Array layout (one allocation): [vtable|refcount|gcinfo|length|elem0..].
   Value arrays have no managed children. Object/string arrays use a span
   descriptor: desc[1] == -1 marks "iterate length elements from desc[2], stride
   desc[3]" (length read from the array's own field at offset 24). */

static int64_t g_array_val_typeinfo[2] = { 0 /* Finalizer. */, 0 /* Child count. */ };
static int64_t g_array_obj_typeinfo[4] = { 0 /* Finalizer. */, -1 /* SPAN. */, 32 /* Off. */, 8 /* Stride. */ };
static int64_t g_array_val_vtable[2];
static int64_t g_array_obj_vtable[2];

static void *array_val_vtable(void)
{
	g_array_val_vtable[0] = (int64_t)&g_array_val_typeinfo[0];
	return &g_array_val_vtable[1];
}

static void *array_obj_vtable(void)
{
	g_array_obj_vtable[0] = (int64_t)&g_array_obj_typeinfo[0];
	return &g_array_obj_vtable[1];
}

void *bzy_array_new_sized(int64_t n, int64_t elem_size, int64_t elem_is_managed)
{
	if (n < 0) n = 0;
	void *a = bzy_alloc(32 + n * elem_size);   /* Elements zeroed by bzy_alloc. */
	*(void**)a = elem_is_managed ? array_obj_vtable() : array_val_vtable();
	*(int64_t*)((char*)a + 24) = n;
	return a;
}

void *bzy_array_new(int64_t n, int64_t elem_is_managed)
{
	return bzy_array_new_sized(n, 8, elem_is_managed);   /* Legacy 8-byte slot arrays. */
}

int64_t bzy_array_len(void *a)
{
	return *(int64_t*)((char*)a + 24);
}

void bzy_oob_abort(int64_t index, int64_t length)
{
	fprintf(stderr, "Array index %lld out of bounds for length %lld.\n",
			(long long)index, (long long)length);
	abort();
}
