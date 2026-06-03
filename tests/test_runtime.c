#include "test_framework.h"
#include "breezy.h"
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>

/* Exception registry stubs: no compiled Breezy program is linked into this unit-test binary. */
void *__bzy_exception_funcs[1] = { 0 };
long long __bzy_exception_func_count = 0;
void *__bzy_vtable_parents[1] = { 0 };
long long __bzy_vtable_parent_count = 0;
char __vtable_IndexOutOfBounds[8] = { 0 };

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

static void test_map_contains_value(void)
{
	void *m = bzy_map_new(0, 0);          /* int->int, value unmanaged. */
	bzy_map_put(m, 1, 100);
	bzy_map_put(m, 2, 200);
	ASSERT_INT(bzy_map_contains_value(m, 200, 0), 1);   /* val_kind 0 = int. */
	ASSERT_INT(bzy_map_contains_value(m, 999, 0), 0);
	ASSERT_INT(bzy_map_val_at(m, bzy_map_iter(m, 0)) >= 100, 1);  /* val_at reads a slot. */
	bzy_release(m);
}

static void test_map_keys_values(void)
{
	void *m = bzy_map_new(0, 0);
	bzy_map_put(m, 10, 1);
	bzy_map_put(m, 20, 2);
	void *ks = bzy_map_keys(m);                 /* int[] (value array). */
	void *vs = bzy_map_values(m);
	ASSERT_INT(bzy_array_len(ks), 2);
	ASSERT_INT(bzy_array_len(vs), 2);
	int64_t ksum = *(int64_t*)((char*)ks + 32) + *(int64_t*)((char*)ks + 40);
	int64_t vsum = *(int64_t*)((char*)vs + 32) + *(int64_t*)((char*)vs + 40);
	ASSERT_INT(ksum, 30);
	ASSERT_INT(vsum, 3);
	bzy_release(ks);
	bzy_release(vs);
	bzy_release(m);
}

static void test_entry(void)
{
	int64_t before = bzy_live_count();
	void *k = bzy_str_new("key", 3);
	void *e = bzy_entry_new((int64_t)k, 42, 1, 0);   /* key managed, value plain int. */
	bzy_release(k);                                  /* The entry retained its own key. */
	void *gk = (void*)bzy_entry_key(e);              /* Owned (+1) string. */
	ASSERT(strcmp(bzy_str_data(gk), "key") == 0);
	ASSERT_INT(bzy_entry_val(e), 42);
	bzy_release(gk);
	bzy_release(e);                                  /* Frees the entry and its retained key. */
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

static void test_str_eq(void)
{
	int64_t before = bzy_live_count();
	void *a = bzy_str_new("hello", 5);
	void *b = bzy_str_new("hello", 5);
	void *c = bzy_str_new("world", 5);
	ASSERT_INT(bzy_str_eq(a, b), 1);   /* Content-equal, distinct objects. */
	ASSERT_INT(bzy_str_eq(a, c), 0);
	ASSERT_INT(bzy_str_eq(a, a), 1);   /* Identity. */
	bzy_release(a);
	bzy_release(b);
	bzy_release(c);
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

static void test_vec_value_back(void)
{
	int64_t before = bzy_live_count();
	void *v = bzy_vec_new(0);              /* int elements */
	bzy_vec_push_back(v, 10);
	bzy_vec_push_back(v, 20);
	bzy_vec_push_back(v, 30);
	ASSERT_INT(bzy_vec_len(v), 3);
	ASSERT_INT(bzy_vec_get(v, 1), 20);
	bzy_vec_set(v, 1, 99);
	ASSERT_INT(bzy_vec_get(v, 1), 99);
	ASSERT_INT(bzy_vec_pop_back(v), 30);
	ASSERT_INT(bzy_vec_len(v), 2);
	bzy_release(v);                         /* vector + data array */
	ASSERT_INT(bzy_live_count(), before);
}

static void test_vec_grow(void)
{
	int64_t before = bzy_live_count();
	void *v = bzy_vec_new(0);
	for (int i = 0; i < 50; i++)             /* forces several doublings */
	{
		bzy_vec_push_back(v, i);
	}

	ASSERT_INT(bzy_vec_len(v), 50);
	ASSERT_INT(bzy_vec_get(v, 0), 0);
	ASSERT_INT(bzy_vec_get(v, 49), 49);
	bzy_release(v);
	ASSERT_INT(bzy_live_count(), before);
}

static void test_vec_object_released(void)
{
	int64_t before = bzy_live_count();
	void *v = bzy_vec_new(4);                /* object elements */
	void *a = bzy_alloc(24);
	*(void**)a = vtable_no_field();
	bzy_vec_push_back(v, (int64_t)a);        /* vector retains a */
	bzy_release(a);                           /* drop our ref; vector holds it */
	void *got = (void*)bzy_vec_get(v, 0);    /* get retains -> +1 */
	ASSERT(got == a);
	bzy_release(got);
	bzy_release(v);                           /* releases data array -> releases a */
	ASSERT_INT(bzy_live_count(), before);
}

static void test_vec_ring(void)
{
	int64_t before = bzy_live_count();
	void *v = bzy_vec_new(0);
	bzy_vec_push_back(v, 2);
	bzy_vec_push_front(v, 1);     /* [1, 2] */
	bzy_vec_push_back(v, 3);      /* [1, 2, 3] */
	ASSERT_INT(bzy_vec_peek_front(v), 1);
	ASSERT_INT(bzy_vec_peek_back(v), 3);
	ASSERT_INT(bzy_vec_pop_front(v), 1);   /* [2, 3] */
	ASSERT_INT(bzy_vec_pop_back(v), 3);    /* [2] */
	ASSERT_INT(bzy_vec_len(v), 1);
	ASSERT_INT(bzy_vec_get(v, 0), 2);
	bzy_release(v);
	ASSERT_INT(bzy_live_count(), before);
}

static void test_vec_contains_and_remove(void)
{
	int64_t before = bzy_live_count();
	void *v = bzy_vec_new(0);
	bzy_vec_push_back(v, 5);
	bzy_vec_push_back(v, 6);
	bzy_vec_push_back(v, 7);
	ASSERT_INT(bzy_vec_index_of(v, 6), 1);
	ASSERT_INT(bzy_vec_contains(v, 7), 1);
	ASSERT_INT(bzy_vec_contains(v, 9), 0);
	bzy_vec_remove_at(v, 1);      /* [5, 7] */
	ASSERT_INT(bzy_vec_len(v), 2);
	ASSERT_INT(bzy_vec_get(v, 1), 7);
	bzy_release(v);
	ASSERT_INT(bzy_live_count(), before);
}

static void test_vec_contains_string(void)
{
	int64_t before = bzy_live_count();
	void *v = bzy_vec_new(3);                /* string elements */
	void *a = bzy_str_new("ab", 2);
	bzy_vec_push_back(v, (int64_t)a);
	bzy_release(a);
	void *probe = bzy_str_new("ab", 2);
	ASSERT_INT(bzy_vec_contains(v, (int64_t)probe), 1);   /* content equality */
	bzy_release(probe);
	bzy_release(v);
	ASSERT_INT(bzy_live_count(), before);
}

static void test_rnd(void)
{
	for (int i=0; i<2000; i++)
	{
		int64_t v = bzy_rnd_get_i(10);
		ASSERT(v >= 0 && v < 10);
	}

	for (int i=0; i<2000; i++)
	{
		int64_t v = bzy_rnd_get_ii(5, 8);   /* inclusive */
		ASSERT(v >= 5 && v <= 8);
	}

	for (int i=0; i<2000; i++)
	{
		double d = bzy_rnd_double();
		ASSERT(d >= 0.0 && d < 1.0);
	}

	for (int i=0; i<2000; i++)
	{
		float f = bzy_rnd_float();
		ASSERT(f >= 0.0f && f < 1.0f);
	}

	int seen0=0, seen1=0;
	for (int i=0; i<2000; i++)
	{
		int64_t b = bzy_rnd_bool();
		ASSERT(b==0 || b==1);
		if (b==0)
		{
			seen0=1;
		}
		else
		{
			seen1=1;
		}
	}

	ASSERT(seen0 && seen1);                 /* both outcomes appear */

	double g = bzy_rnd_gaussian();
	ASSERT(g == g);                          /* not NaN */

	void *arr = bzy_array_new(8, 0);
	bzy_rnd_bytes(arr);
	int64_t *slots = (int64_t*)((char*)arr + 32);
	for (int i=0; i<8; i++)
	{
		ASSERT(slots[i] >= 0 && slots[i] <= 255);
	}

	bzy_release(arr);
}

static void test_clock(void)
{
	int64_t m = bzy_clock_millis();
	ASSERT(m > 0);                         /* Wall-clock ms since 1970 is large/positive. */
	int64_t a = bzy_clock_nanos();
	int64_t b = bzy_clock_nanos();
	ASSERT(b >= a);                        /* Monotonic non-decreasing. */
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

static void test_regex_matches_basic(void)
{
	ASSERT_INT(bzy_regex_matches(bzy_str_new("a+b", 3), bzy_str_new("aaab", 4)), 1);
	ASSERT_INT(bzy_regex_matches(bzy_str_new("a+b", 3), bzy_str_new("aaa", 3)), 0);   /* not full */
	ASSERT_INT(bzy_regex_matches(bzy_str_new("a|aa", 4), bzy_str_new("aa", 2)), 1);   /* alt full match */
	ASSERT_INT(bzy_regex_matches(bzy_str_new("[0-9]{2,4}", 10), bzy_str_new("123", 3)), 1);
	ASSERT_INT(bzy_regex_matches(bzy_str_new("[0-9]{2,4}", 10), bzy_str_new("1", 1)), 0);
	ASSERT_INT(bzy_regex_matches(bzy_str_new("\\d+", 3), bzy_str_new("42", 2)), 1);
	ASSERT_INT(bzy_regex_matches(bzy_str_new("(ab)+", 5), bzy_str_new("abab", 4)), 1);
}

static void test_regex_test_search(void)
{
	ASSERT_INT(bzy_regex_test(bzy_str_new("a+b", 3), bzy_str_new("xxaaabyy", 8)), 1);   /* found inside */
	ASSERT_INT(bzy_regex_test(bzy_str_new("^a+b$", 5), bzy_str_new("xxaaabyy", 8)), 0); /* anchored, no */
	ASSERT_INT(bzy_regex_test(bzy_str_new("z", 1), bzy_str_new("abc", 3)), 0);
}

static void test_str_query(void)
{
	ASSERT_INT(bzy_str_contains(bzy_str_new("hello world", 11), bzy_str_new("o w", 3)), 1);
	ASSERT_INT(bzy_str_contains(bzy_str_new("hello", 5), bzy_str_new("z", 1)), 0);
	ASSERT_INT(bzy_str_starts_with(bzy_str_new("/api/x", 6), bzy_str_new("/api", 4)), 1);
	ASSERT_INT(bzy_str_starts_with(bzy_str_new("/x", 2), bzy_str_new("/api", 4)), 0);
	ASSERT_INT(bzy_str_ends_with(bzy_str_new("file.bzy", 8), bzy_str_new(".bzy", 4)), 1);
	ASSERT_INT(bzy_str_index_of(bzy_str_new("abcabc", 6), bzy_str_new("bc", 2)), 1);
	ASSERT_INT(bzy_str_index_of(bzy_str_new("abc", 3), bzy_str_new("z", 1)), -1);
}

static void test_str_transform(void)
{
	void *sub = bzy_str_substring(bzy_str_new("hello", 5), 1, 4);
	ASSERT_INT(bzy_str_len(sub), 3);
	ASSERT_INT(memcmp(bzy_str_data(sub), "ell", 3), 0);
	void *rep = bzy_str_replace(bzy_str_new("a.b.c", 5), bzy_str_new(".", 1), bzy_str_new("/", 1));
	ASSERT_INT(memcmp(bzy_str_data(rep), "a/b/c", 5), 0);
	void *tr = bzy_str_trim(bzy_str_new("  hi \t", 6));
	ASSERT_INT(bzy_str_len(tr), 2);
	ASSERT_INT(memcmp(bzy_str_data(tr), "hi", 2), 0);
	void *up = bzy_str_to_upper(bzy_str_new("aB3", 3));
	ASSERT_INT(memcmp(bzy_str_data(up), "AB3", 3), 0);
	void *lo = bzy_str_to_lower(bzy_str_new("aB3", 3));
	ASSERT_INT(memcmp(bzy_str_data(lo), "ab3", 3), 0);
}

static void test_str_more(void)
{
	ASSERT_INT(bzy_str_equals_ignore_case(bzy_str_new("HeLLo", 5), bzy_str_new("hello", 5)), 1);
	ASSERT_INT(bzy_str_equals_ignore_case(bzy_str_new("a", 1), bzy_str_new("ab", 2)), 0);
	ASSERT_INT(bzy_str_is_empty(bzy_str_new("", 0)), 1);
	ASSERT_INT(bzy_str_is_empty(bzy_str_new("x", 1)), 0);
	ASSERT_INT(bzy_str_char_at(bzy_str_new("abc", 3), 1), 'b');
	ASSERT_INT(bzy_str_char_at(bzy_str_new("abc", 3), 9), -1);
	ASSERT_INT(bzy_str_last_index_of(bzy_str_new("abcabc", 6), bzy_str_new("bc", 2)), 4);
	void *rp = bzy_str_repeat(bzy_str_new("ab", 2), 3);
	ASSERT_INT(bzy_str_len(rp), 6);
	ASSERT_INT(memcmp(bzy_str_data(rp), "ababab", 6), 0);
}

static void test_clock_date(void)
{
	_putenv("TZ=UTC0");
	_tzset();
	void *s = bzy_clock_date(0);                 /* Epoch in UTC -> 1970-01-01 00:00:00. */
	ASSERT_INT(bzy_str_len(s), 19);
	ASSERT_INT(memcmp(bzy_str_data(s), "1970-01-01 00:00:00", 19), 0);
	void *f = bzy_clock_date_fmt(0, bzy_str_new("yyyy/MM/dd HH:mm:ss", 19));
	ASSERT_INT(memcmp(bzy_str_data(f), "1970/01/01 00:00:00", 19), 0);
}

static void test_str_split(void)
{
	void *a = bzy_str_split(bzy_str_new("a,bb,c", 6), bzy_str_new(",", 1));
	ASSERT_INT(bzy_array_len(a), 3);
	void **e = (void**)((char*)a + 32);
	ASSERT_INT(bzy_str_len(e[0]), 1);
	ASSERT_INT(memcmp(bzy_str_data(e[0]), "a", 1), 0);
	ASSERT_INT(bzy_str_len(e[1]), 2);
	ASSERT_INT(memcmp(bzy_str_data(e[1]), "bb", 2), 0);
	ASSERT_INT(memcmp(bzy_str_data(e[2]), "c", 1), 0);
}

static void test_regex_find(void)
{
	void *m = bzy_regex_find(bzy_str_new("[0-9]+", 6), bzy_str_new("abc123def", 9));
	ASSERT_INT(bzy_str_len(m), 3);
	ASSERT_INT(memcmp(bzy_str_data(m), "123", 3), 0);
	void *none = bzy_regex_find(bzy_str_new("z+", 2), bzy_str_new("abc", 3));
	ASSERT_INT(bzy_str_len(none), 0);
}

static void test_regex_replace(void)
{
	void *r = bzy_regex_replace(bzy_str_new("a+", 2), bzy_str_new("xaayaaaz", 8), bzy_str_new("-", 1));
	ASSERT_INT(bzy_str_len(r), 5);
	ASSERT_INT(memcmp(bzy_str_data(r), "x-y-z", 5), 0);
}

static int g_breeze_log[8];
static int g_breeze_n;
static void worker_a(void)
{
	g_breeze_log[g_breeze_n++] = 1;
	bzy_yield();
	g_breeze_log[g_breeze_n++] = 3;
}
static void worker_b(void)
{
	g_breeze_log[g_breeze_n++] = 2;
	bzy_yield();
	g_breeze_log[g_breeze_n++] = 4;
}

static int64_t g_sa_sum;
static void sa_thunk(void *blk)
{
	int64_t *a = (int64_t*)blk;
	g_sa_sum = a[0] + a[1];
	free(blk);
}

static void test_spawn_args(void)
{
	g_sa_sum = 0;
	bzy_sched_init();
	int64_t *blk = malloc(2 * sizeof(int64_t));
	blk[0] = 30;
	blk[1] = 12;
	bzy_spawn_args(sa_thunk, blk);
	bzy_sched_run();
	ASSERT_INT(g_sa_sum, 42);
}

static void test_scheduler_roundrobin(void)
{
	g_breeze_n = 0;
	bzy_sched_init();
	bzy_spawn(worker_a);
	bzy_spawn(worker_b);
	bzy_sched_run();
	ASSERT_INT(g_breeze_n, 4);
	ASSERT_INT(g_breeze_log[0], 1);   /* a runs first... */
	ASSERT_INT(g_breeze_log[1], 2);   /* ...yields, b runs... */
	ASSERT_INT(g_breeze_log[2], 3);   /* ...a resumes... */
	ASSERT_INT(g_breeze_log[3], 4);   /* ...b resumes. */
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
	RUN(test_map_contains_value);
	RUN(test_map_keys_values);
	RUN(test_entry);
	RUN(test_map_string_keys_and_grow);
	RUN(test_map_object_values_released);
	RUN(test_map_iteration);
	RUN(test_str_eq);
	RUN(test_vec_value_back);
	RUN(test_vec_grow);
	RUN(test_vec_object_released);
	RUN(test_vec_ring);
	RUN(test_vec_contains_and_remove);
	RUN(test_vec_contains_string);
	RUN(test_rnd);
	RUN(test_clock);
	RUN(test_builder_append_tostring);
	RUN(test_finalizer_runs_on_free);
	RUN(test_cycle_is_collected);
	RUN(test_self_cycle_collected);
	RUN(test_live_cycle_kept);
	RUN(test_regex_matches_basic);
	RUN(test_regex_test_search);
	RUN(test_str_query);
	RUN(test_str_transform);
	RUN(test_str_more);
	RUN(test_str_split);
	RUN(test_clock_date);
	RUN(test_regex_find);
	RUN(test_regex_replace);
	RUN(test_scheduler_roundrobin);
	RUN(test_spawn_args);
	SUMMARY();
	return 0;
}
