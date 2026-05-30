#include "test_framework.h"
#include "breezy.h"
#include <stdint.h>

/* A descriptor for one object field at offset 16, followed by the vtable word.
   The layout in memory is [n][off0][typeinfo-pointer][vtable...], so we hand
   out vtable = &slot[3] and the runtime reads the descriptor at vtable - 8. */
static int64_t g_desc_with_field[] = { 1, 16, 0 /* Typeinfo pointer. */, 0 /* Vtable slot zero. */ };
static int64_t g_desc_no_field[]   = { 0, 0 /* Typeinfo pointer. */, 0 /* Vtable slot zero. */ };

static void *vtable_with_field(void)
{
	g_desc_with_field[2] = (int64_t)&g_desc_with_field[0];
	return &g_desc_with_field[3];
}

static void *vtable_no_field(void)
{
	g_desc_no_field[1] = (int64_t)&g_desc_no_field[0];
	return &g_desc_no_field[2];
}

static void test_alloc_sets_refcount_and_live(void)
{
	int64_t before = bzy_live_count();
	void *o = bzy_alloc(24);
	ASSERT_INT(bzy_live_count(), before + 1);
	ASSERT_INT(*(int64_t*)((char*)o + 8), 1);    /* The refcount is one. */
	bzy_release(o);
	ASSERT_INT(bzy_live_count(), before);
}

static void test_retain_release_balance(void)
{
	int64_t before = bzy_live_count();
	void *o = bzy_alloc(24);
	bzy_retain(o);
	bzy_retain(o);                 /* The refcount is now three. */
	bzy_release(o);
	bzy_release(o);               /* The refcount is one and the object is still alive. */
	ASSERT_INT(bzy_live_count(), before + 1);
	bzy_release(o);                               /* The refcount reaches zero and the object is freed. */
	ASSERT_INT(bzy_live_count(), before);
}

static void test_null_is_safe(void)
{
	bzy_retain(NULL);
	bzy_release(NULL);          /* Neither call may crash. */
	ASSERT(1);
}

static void test_release_frees_owned_field(void)
{
	int64_t before = bzy_live_count();
	void *parent = bzy_alloc(24);                 /* The parent has one object field at offset 16. */
	void *child  = bzy_alloc(16);
	*(void**)parent = vtable_with_field();        /* Give the parent a descriptor with a field at offset 16. */
	*(void**)child  = vtable_no_field();
	*(void**)((char*)parent + 16) = child;        /* The parent owns the child. */
	ASSERT_INT(bzy_live_count(), before + 2);
	bzy_release(parent);                          /* Releasing the parent should cascade to the child. */
	ASSERT_INT(bzy_live_count(), before);
}

int main(void)
{
	printf("Runtime (ARC) tests\n");
	RUN(test_alloc_sets_refcount_and_live);
	RUN(test_retain_release_balance);
	RUN(test_null_is_safe);
	RUN(test_release_frees_owned_field);
	SUMMARY();
	return 0;
}
