#include "test_framework.h"
#include "breezy.h"
#include <stdint.h>
#include <string.h>

/* A descriptor for one object field at offset 24, preceded by the finalizer
   slot. The layout in memory is [finalizer][n][off0][typeinfo-pointer][vtable...],
   so we hand out vtable = &slot[4] and the runtime reads the descriptor (which the
   typeinfo-pointer word points at) at vtable - 8. */
static int64_t g_desc_with_field[] = { 0 /* Finalizer. */, 1, 24, 0 /* Typeinfo pointer. */, 0 /* Vtable slot zero. */ };
static int64_t g_desc_no_field[]   = { 0 /* Finalizer. */, 0, 0 /* Typeinfo pointer. */, 0 /* Vtable slot zero. */ };

static void *vtable_with_field(void)
{
	g_desc_with_field[3] = (int64_t)&g_desc_with_field[0];
	return &g_desc_with_field[4];
}

static void *vtable_no_field(void)
{
	g_desc_no_field[2] = (int64_t)&g_desc_no_field[0];
	return &g_desc_no_field[3];
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
	void *parent = bzy_alloc(32);                 /* Header is 24 bytes plus one object field at offset 24. */
	void *child  = bzy_alloc(24);
	*(void**)parent = vtable_with_field();        /* Give the parent a descriptor with a field at offset 24. */
	*(void**)child  = vtable_no_field();
	*(void**)((char*)parent + 24) = child;        /* The parent owns the child. */
	ASSERT_INT(bzy_live_count(), before + 2);
	bzy_release(parent);                          /* Releasing the parent should cascade to the child. */
	ASSERT_INT(bzy_live_count(), before);
}

static void test_loop_reuse_is_bounded(void)
{
	/* Simulate `c = new Cell()` in a loop: each store retains the new object and
	   releases the previous occupant, so only one object stays alive. */
	int64_t before = bzy_live_count();
	void *slot = NULL;
	for (int i=0; i<1000; i++)
	{
		void *o = bzy_alloc(24);
		*(void**)o = vtable_no_field();
		bzy_retain(o);       /* The slot takes ownership. */
		bzy_release(slot);   /* Release the previous occupant. */
		slot = o;
		bzy_release(o);      /* Drop the allocation's initial reference. */
	}

	ASSERT_INT(bzy_live_count(), before + 1);
	bzy_release(slot);
	ASSERT_INT(bzy_live_count(), before);
}

static void test_unmanaged_object_ignored(void)
{
	/* A refcount-0 object models a stack allocation: retain and release must
	   leave it untouched and never free it. */
	int64_t buf[4] = {0,0,0,0};
	void *o = buf;
	int64_t before = bzy_live_count();
	bzy_retain(o);
	bzy_release(o);
	ASSERT_INT(buf[1], 0);
	ASSERT_INT(bzy_live_count(), before);
}

/* A node descriptor: one object field at offset 24 (the widened-header base),
   preceded by the finalizer slot. */
static int64_t g_node_desc[] = { 0 /* Finalizer. */, 1, 24, 0 /* Typeinfo pointer. */, 0 /* Vtable slot zero. */ };

static void *node_vtable(void)
{
	g_node_desc[3] = (int64_t)&g_node_desc[0];
	return &g_node_desc[4];
}

/* A no-field descriptor carrying a finalizer, to prove the finalizer fires on free. */
static int g_fin_calls = 0;
static void test_finalizer(void *obj)
{
	(void)obj;
	g_fin_calls++;
}

static int64_t g_fin_desc[] = { 0 /* Finalizer. */, 0, 0 /* Typeinfo pointer. */, 0 /* Vtable slot zero. */ };
static void *fin_vtable(void)
{
	g_fin_desc[0] = (int64_t)(void*)test_finalizer;
	g_fin_desc[2] = (int64_t)&g_fin_desc[0];
	return &g_fin_desc[3];
}

static void test_string_new_and_len(void)
{
	int64_t before = bzy_live_count();
	void *s = bzy_str_new("hello", 5);
	ASSERT_INT(bzy_str_len(s), 5);
	ASSERT(strcmp(bzy_str_data(s), "hello") == 0);
	bzy_release(s);
	ASSERT_INT(bzy_live_count(), before);
}

static void test_string_concat(void)
{
	int64_t before = bzy_live_count();
	void *a = bzy_str_new("foo", 3), *b = bzy_str_new("bar", 3);
	void *c = bzy_str_concat(a, b);
	ASSERT_INT(bzy_str_len(c), 6);
	ASSERT(strcmp(bzy_str_data(c), "foobar") == 0);
	bzy_release(a);
	bzy_release(b);
	bzy_release(c);
	ASSERT_INT(bzy_live_count(), before);
}

static void test_array_value_roundtrip(void)
{
	int64_t before = bzy_live_count();
	void *a = bzy_array_new(3, 0);          /* value array, 3 slots */
	ASSERT_INT(bzy_array_len(a), 3);
	*(int64_t*)((char*)a + 32 + 1*8) = 42;  /* a[1] = 42 */
	ASSERT_INT(*(int64_t*)((char*)a + 32 + 1*8), 42);
	bzy_release(a);
	ASSERT_INT(bzy_live_count(), before);
}

static void test_array_object_elements_released(void)
{
	int64_t before = bzy_live_count();
	void *a = bzy_array_new(2, 1);          /* managed-element array */
	void *o = bzy_alloc(24);                /* a plain refcount-1 object */
	*(void**)o = vtable_no_field();
	*(void**)((char*)a + 32 + 0*8) = o;     /* a[0] = o (store the pointer) */
	/* The array owns one reference to o; releasing the array must release o. */
	bzy_release(a);
	ASSERT_INT(bzy_live_count(), before);   /* both a and o reclaimed */
}

static void test_map_int_keys(void)
{
	int64_t before = bzy_live_count();
	void *m = bzy_map_new(0, 0);          /* key_kind=int, val unmanaged */
	bzy_map_put(m, 10, 100);
	bzy_map_put(m, 20, 200);
	bzy_map_put(m, 10, 111);              /* overwrite */
	ASSERT_INT(bzy_map_len(m), 2);
	ASSERT_INT(bzy_map_get(m, 10), 111);
	ASSERT_INT(bzy_map_get(m, 20), 200);
	ASSERT_INT(bzy_map_has(m, 99), 0);
	ASSERT_INT(bzy_map_get(m, 99), 0);    /* missing -> 0 */
	bzy_map_remove(m, 10);
	ASSERT_INT(bzy_map_has(m, 10), 0);
	ASSERT_INT(bzy_map_len(m), 1);
	bzy_release(m);
	ASSERT_INT(bzy_live_count(), before);
}

static void test_map_string_keys_and_grow(void)
{
	int64_t before = bzy_live_count();
	void *m = bzy_map_new(1, 0);          /* key_kind=string */
	for (int i = 0; i < 50; i++)          /* forces several grows */
	{
		char buf[16];
		int n = sprintf(buf, "k%d", i);
		void *k = bzy_str_new(buf, n);
		bzy_map_put(m, (int64_t)k, i);
		bzy_release(k);                   /* the map retained its own copy */
	}
	ASSERT_INT(bzy_map_len(m), 50);
	void *probe = bzy_str_new("k37", 3);
	ASSERT_INT(bzy_map_get(m, (int64_t)probe), 37);   /* content-equality lookup */
	bzy_release(probe);
	bzy_release(m);
	ASSERT_INT(bzy_live_count(), before); /* all 50 string keys reclaimed */
}

static void test_map_object_values_released(void)
{
	int64_t before = bzy_live_count();
	void *m = bzy_map_new(0, 1);          /* int keys, managed values */
	void *a = bzy_alloc(24);
	*(void**)a = vtable_no_field();
	bzy_map_put(m, 1, (int64_t)a);        /* map retains a */
	bzy_release(a);                        /* drop our ref; map still holds it */
	void *got = (void*)bzy_map_get(m, 1); /* get retains -> +1 */
	ASSERT(got == a);
	bzy_release(got);
	bzy_release(m);                        /* releases the value array -> releases a */
	ASSERT_INT(bzy_live_count(), before);
}

static void test_map_iteration(void)
{
	int64_t before = bzy_live_count();
	void *m = bzy_map_new(0, 0);           /* int keys, unmanaged values */
	bzy_map_put(m, 5, 50);
	bzy_map_put(m, 7, 70);
	bzy_map_put(m, 9, 90);
	int64_t keys_sum = 0, vals_sum = 0, count = 0;
	for (int64_t c = bzy_map_iter(m, 0); c >= 0; c = bzy_map_iter(m, c + 1))
	{
		int64_t k = bzy_map_key_at(m, c);
		keys_sum += k;
		vals_sum += bzy_map_get(m, k);
		count++;
	}

	ASSERT_INT(count, 3);
	ASSERT_INT(keys_sum, 21);              /* 5 + 7 + 9 */
	ASSERT_INT(vals_sum, 210);             /* 50 + 70 + 90 */
	bzy_release(m);
	ASSERT_INT(bzy_live_count(), before);
}

static void test_builder_append_tostring(void)
{
	int64_t before = bzy_live_count();
	void *sb = bzy_sb_new();
	bzy_sb_append_cstr(sb, "ab", 2);
	bzy_sb_append_cstr(sb, "cd", 2);
	void *s = bzy_sb_to_string(sb);
	ASSERT(strcmp(bzy_str_data(s), "abcd") == 0);
	bzy_release(s);
	bzy_release(sb);
	ASSERT_INT(bzy_live_count(), before);   /* The finalizer freed sb's buffer. */
}

static void test_finalizer_runs_on_free(void)
{
	int64_t before = bzy_live_count();
	g_fin_calls = 0;
	void *o = bzy_alloc(24);
	*(void**)o = fin_vtable();
	bzy_release(o);
	ASSERT_INT(g_fin_calls, 1);
	ASSERT_INT(bzy_live_count(), before);
}

static void test_cycle_is_collected(void)
{
	int64_t before = bzy_live_count();
	void *a = bzy_alloc(32);
	void *b = bzy_alloc(32);
	*(void**)a = node_vtable();
	*(void**)b = node_vtable();
	*(void**)((char*)a + 24) = b;
	bzy_retain(b);
	*(void**)((char*)b + 24) = a;
	bzy_retain(a);
	ASSERT_INT(bzy_live_count(), before + 2);
	/* Drop both external references: the cycle keeps each refcount at one. */
	bzy_release(a);
	bzy_release(b);
	ASSERT_INT(bzy_live_count(), before + 2);
	bzy_collect_cycles();
	ASSERT_INT(bzy_live_count(), before);
}

static void test_self_cycle_collected(void)
{
	int64_t before = bzy_live_count();
	void *a = bzy_alloc(32);
	*(void**)a = node_vtable();
	*(void**)((char*)a + 24) = a;
	bzy_retain(a);
	bzy_release(a);
	ASSERT_INT(bzy_live_count(), before + 1);
	bzy_collect_cycles();
	ASSERT_INT(bzy_live_count(), before);
}

static void test_live_cycle_kept(void)
{
	int64_t before = bzy_live_count();
	void *a = bzy_alloc(32);       /* a.rc=1 models a's external owner. */
	void *b = bzy_alloc(32);
	*(void**)a = node_vtable();
	*(void**)b = node_vtable();
	*(void**)((char*)a + 24) = b;
	bzy_retain(b);                 /* b.rc=2. */
	*(void**)((char*)b + 24) = a;
	bzy_retain(a);                 /* a.rc=2. */
	/* Drop only b's external owner; a's reference still reaches the cycle. */
	bzy_release(b);                /* b.rc=1, buffered. */
	bzy_collect_cycles();
	ASSERT_INT(bzy_live_count(), before + 2);   /* Externally reachable: kept. */
	/* Now drop a's external owner too and the cycle is unreachable. */
	bzy_release(a);
	bzy_collect_cycles();
	ASSERT_INT(bzy_live_count(), before);
}

int main(void)
{
	printf("Runtime (ARC) tests\n");
	RUN(test_alloc_sets_refcount_and_live);
	RUN(test_retain_release_balance);
	RUN(test_null_is_safe);
	RUN(test_release_frees_owned_field);
	RUN(test_loop_reuse_is_bounded);
	RUN(test_unmanaged_object_ignored);
	RUN(test_string_new_and_len);
	RUN(test_string_concat);
	RUN(test_array_value_roundtrip);
	RUN(test_array_object_elements_released);
	RUN(test_map_int_keys);
	RUN(test_map_string_keys_and_grow);
	RUN(test_map_object_values_released);
	RUN(test_map_iteration);
	RUN(test_builder_append_tostring);
	RUN(test_finalizer_runs_on_free);
	RUN(test_cycle_is_collected);
	RUN(test_self_cycle_collected);
	RUN(test_live_cycle_kept);
	SUMMARY();
	return 0;
}
