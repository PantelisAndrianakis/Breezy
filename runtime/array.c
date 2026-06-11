#include "breezy.h"
#include <stdio.h>
#include <stdlib.h>

/* Array layout (one allocation): [vtable|refcount|gcinfo|length|elem0..].
   Value arrays store each element at its natural width (byte=1, short=2,
   int/float=4, long/double=8) and have no managed children. Object/string arrays
   hold 8-byte pointers and use a span descriptor: desc[1] == -1 marks "iterate
   length elements from desc[2], stride desc[3]" (length read from the array's own
   field at offset 24). The element width is chosen by the compiler at each
   allocation site (bzy_array_new_sized); it is not stored in the header. */

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
	if (n < 0)
	{
		n = 0;
	}
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

/* Shared-array element access. Codegen reaches these only from the out-of-line
   stub behind a SHARED-bit test, and only for managed-element arrays after the
   inline bounds check has passed (so no bounds logic here - the catchable
   subscript exception is raised by the unchanged inline path). Value arrays
   never come here: aligned native loads/stores are already atomic, so a shared
   value array's fast path is the ordinary inline code. */
int64_t bzy_array_get_shared(void *a, int64_t i)
{
	bzy_shared_lock(a);
	void *e = *(void**)((char*)a + 32 + i * 8);
	bzy_retain(e);   /* Retain inside the stripe, or a concurrent overwrite could release it first. */
	bzy_shared_unlock(a);
	return (int64_t)e;
}

void bzy_array_set_shared(void *a, int64_t i, int64_t v)
{
	/* Insert barrier: anything stored into a shared container is itself shared.
	   Deep-share BEFORE taking the stripe (lock ordering: the roots lock and a
	   stripe are never held together). */
	bzy_share_crosscore((void*)v);
	bzy_retain((void*)v);
	bzy_shared_lock(a);
	void *old = *(void**)((char*)a + 32 + i * 8);
	*(void**)((char*)a + 32 + i * 8) = (void*)v;
	bzy_shared_unlock(a);
	bzy_release(old);   /* Outside the stripe: a destructor cascade must not hold it. */
}

void bzy_oob_abort(int64_t index, int64_t length)
{
	fprintf(stderr, "Array index %lld out of bounds for length %lld.\n",
			(long long)index, (long long)length);
	abort();
}
