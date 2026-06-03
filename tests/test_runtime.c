#include "test_framework.h"
#include "breezy.h"
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <windows.h>

/* Exception registry stubs: no compiled Breezy program is linked into this unit-test binary. */
void *__bzy_exception_funcs[1] = { 0 };
long long __bzy_exception_func_count = 0;
void *__bzy_vtable_parents[1] = { 0 };
long long __bzy_vtable_parent_count = 0;
char __vtable_IndexOutOfBounds[8] = { 0 };
char __vtable_IOException[8] = { 0 };

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

static void *g_ch;
static int64_t g_recv_sum;
static void channel_producer(void)
{
	bzy_channel_send(g_ch,1);
	bzy_channel_send(g_ch,2);
	bzy_channel_send(g_ch,3);
}
static void channel_consumer(void)
{
	g_recv_sum = bzy_channel_recv(g_ch) + bzy_channel_recv(g_ch) + bzy_channel_recv(g_ch);
}

static void *g_mc_ch;
static int64_t g_mc_sum;
static void mc_worker(void)
{
	bzy_channel_send(g_mc_ch, 1);
}
static void mc_collector(void)
{
	int64_t s = 0;
	for (int i = 0; i < 50; i++)
	{
		s += bzy_channel_recv(g_mc_ch);
	}

	g_mc_sum = s;
}

static void *g_sh_obj;
static void sh_hammer(void)
{
	for (int i = 0; i < 1000; i++)
	{
		bzy_retain(g_sh_obj);    /* Atomic: g_sh_obj carries the SHARED bit. */
		bzy_release(g_sh_obj);
	}
}

static void test_shared_atomic_refcount(void)
{
	/* Eight breezes across four workers hammer retain/release on one shared
	   object. main holds a reference throughout, so it is never freed mid-run;
	   if the ops were not atomic the refcount would drift off 1. */
	int64_t base = bzy_live_count();
	bzy_sched_init();
	bzy_sched_set_workers(4);
	g_sh_obj = bzy_alloc(24);                                /* rc = 1, no vtable (leaf). */
	*(int64_t*)((char*)g_sh_obj + 16) |= BZY_GCINFO_SHARED;  /* Mark it shared (codegen does this at `new`). */
	for (int i = 0; i < 8; i++)
	{
		bzy_spawn(sh_hammer);
	}

	bzy_sched_run();
	ASSERT_INT(*(int64_t*)((char*)g_sh_obj + 8), 1);         /* Every retain matched its release. */
	bzy_release(g_sh_obj);                                   /* Drop main's ref -> free. */
	ASSERT_INT(bzy_live_count(), base);
}

static void test_scheduler_multicore(void)
{
	/* Four workers, 50 senders, one collector: the sum is order-independent and
	   must come out to 50 every run. Exercises cross-thread channel send/recv,
	   work-stealing, and the park-unlock hand-off under real parallelism. */
	int64_t base = bzy_live_count();
	g_mc_sum = 0;
	bzy_sched_init();
	bzy_sched_set_workers(4);
	g_mc_ch = bzy_channel_new(8, 0);
	for (int i = 0; i < 50; i++)
	{
		bzy_spawn(mc_worker);
	}

	bzy_spawn(mc_collector);
	bzy_sched_run();
	ASSERT_INT(g_mc_sum, 50);
	bzy_release(g_mc_ch);
	ASSERT_INT(bzy_live_count(), base);
}

static void test_channel_roundtrip(void)
{
	g_recv_sum = 0;
	bzy_sched_init();
	g_ch = bzy_channel_new(2, 0);     /* cap 2: the 3rd send parks until the consumer drains. */
	bzy_spawn(channel_producer);
	bzy_spawn(channel_consumer);
	bzy_sched_run();
	ASSERT_INT(g_recv_sum, 6);
	bzy_release(g_ch);
}

static int g_timer_hits;            /* A timer target for tests that only need a side effect. */
static void timer_test_target(void)
{
	g_timer_hits++;
}

static void test_timer_heap_orders_by_deadline(void)
{
	bzy_timer_reset();
	void *a = bzy_timer_schedule(timer_test_target, 300, 0);
	void *b = bzy_timer_schedule(timer_test_target, 100, 0);
	void *c = bzy_timer_schedule(timer_test_target, 200, 0);
	ASSERT_INT(bzy_timer_count(), 3);
	ASSERT_INT(bzy_timer_next_deadline(), 100);   /* Min, regardless of insertion order. */

	ASSERT(bzy_timer_pop_due(50) == NULL);         /* Nothing due before 100. */
	void *d1 = bzy_timer_pop_due(150);
	ASSERT(d1 == b);                               /* Earliest deadline pops first. */
	bzy_release(d1);                               /* One-shot: drop the heap's transferred ref. */
	ASSERT_INT(bzy_timer_next_deadline(), 200);

	bzy_release(a);
	bzy_release(b);
	bzy_release(c);/* Drop the caller refs returned by schedule. */
	bzy_timer_reset();
}

static void test_timer_periodic_coalesces_missed_ticks(void)
{
	bzy_timer_reset();
	void *t = bzy_timer_schedule(timer_test_target, 100, 10);  /* first=100, period=10. */
	void *due = bzy_timer_pop_due(135);
	ASSERT(due == t);
	bzy_timer_reinsert(t, 135);                    /* 100->110->120->130->140 (first slot > 135). */
	ASSERT_INT(bzy_timer_next_deadline(), 140);    /* Coalesced: one future slot, not a backlog. */
	bzy_release(t);
	bzy_timer_reset();
}

static void test_timer_cancel_is_skipped(void)
{
	bzy_timer_reset();
	void *t = bzy_timer_schedule(timer_test_target, 100, 0);
	bzy_timer_cancel(t);
	ASSERT_INT(bzy_timer_next_deadline(), -1);     /* Cancelled entries are evicted lazily, not returned. */
	ASSERT(bzy_timer_pop_due(1000) == NULL);
	bzy_release(t);                                /* Caller ref; heap ref already dropped on eviction. */
	ASSERT_INT(bzy_timer_count(), 0);
	bzy_timer_reset();
}

static void test_timer_refcount_lifecycle(void)
{
	bzy_timer_reset();
	int64_t before = bzy_live_count();
	void *t = bzy_timer_schedule(timer_test_target, 100, 0);
	ASSERT_INT(bzy_live_count(), before + 1);      /* One live Timer object. */
	ASSERT_INT(*(int64_t*)((char*)t + 8), 2);      /* refcount 2: caller + heap. */
	void *due = bzy_timer_pop_due(150);
	ASSERT(due == t);
	bzy_release(due);                              /* Heap ref gone -> rc 1. */
	ASSERT_INT(*(int64_t*)((char*)t + 8), 1);
	bzy_release(t);                                /* Caller ref gone -> freed. */
	ASSERT_INT(bzy_live_count(), before);
	bzy_timer_reset();
}

static int g_sched_timer_ran;
static void sched_timer_target(void)
{
	g_sched_timer_ran = 1;
}

static void test_scheduler_runs_one_shot_timer(void)
{
	bzy_timer_reset();
	g_sched_timer_ran = 0;
	bzy_sched_init();                                  /* Single worker, deterministic. */
	/* No breezes spawned: only a pending one-shot keeps the scheduler alive. */
	bzy_timer_schedule(sched_timer_target, bzy_clock_millis() + 5, 0);
	bzy_sched_run();                                   /* Fires the timer, runs the breeze, then exits. */
	ASSERT_INT(g_sched_timer_ran, 1);                  /* Returning at all proves it did not deadlock-abort. */
}

static void test_file_predicates(void)
{
	FILE *f = fopen("bzy_test_tmp.txt", "wb");
	fputs("hi", f);
	fclose(f);
	void *p = bzy_str_new("bzy_test_tmp.txt", 16);
	ASSERT_INT(bzy_file_exists(p), 1);
	ASSERT_INT(bzy_file_is_file(p), 1);
	ASSERT_INT(bzy_file_is_folder(p), 0);
	void *none = bzy_str_new("bzy_no_such.txt", 15);
	ASSERT_INT(bzy_file_exists(none), 0);
	remove("bzy_test_tmp.txt");
	bzy_release(p);
	bzy_release(none);
}

static void test_file_read_write(void)
{
	void *p = bzy_str_new("bzy_rw_tmp.txt", 14);
	void *content = bzy_str_new("alpha\nbeta\n", 11);
	bzy_file_write_text(p, content);
	void *back = bzy_file_read_text(p);
	ASSERT(strcmp(bzy_str_data(back), "alpha\nbeta\n") == 0);
	void *lines = bzy_file_read_lines(p);
	ASSERT_INT(bzy_array_len(lines), 2);                 /* "alpha", "beta"; trailing \n drops empty. */
	void *l0 = *(void**)((char*)lines + 32);
	ASSERT(strcmp(bzy_str_data(l0), "alpha") == 0);

	void *bp = bzy_str_new("bzy_rw_bin.bin", 14);
	void *data = bzy_array_new(3, 0);
	int64_t *s = (int64_t*)((char*)data + 32);
	s[0] = 1;
	s[1] = 254;
	s[2] = 0;
	bzy_file_write_bytes(bp, data);
	void *rb = bzy_file_read_bytes(bp);
	ASSERT_INT(bzy_array_len(rb), 3);
	ASSERT_INT(*(int64_t*)((char*)rb + 32 + 8), 254);

	remove("bzy_rw_tmp.txt");
	remove("bzy_rw_bin.bin");
	bzy_release(p);
	bzy_release(content);
	bzy_release(back);
	bzy_release(lines);
	bzy_release(bp);
	bzy_release(data);
	bzy_release(rb);
}

static void test_file_search(void)
{
	void *dir = bzy_str_new("bzy_search_dir", 14);
	bzy_file_create_folder(dir);
	void *fa = bzy_str_new("bzy_search_dir/a.txt", 20);
	void *fb = bzy_str_new("bzy_search_dir/b.txt", 20);
	void *fc = bzy_str_new("bzy_search_dir/c.log", 20);
	bzy_file_create_file(fa);
	bzy_file_create_file(fb);
	bzy_file_create_file(fc);

	void *pat = bzy_str_new("*.txt", 5);
	void *res = bzy_file_search(dir, pat);
	ASSERT_INT(bzy_array_len(res), 2);            /* a.txt, b.txt */
	void *all = bzy_file_list(dir);
	ASSERT_INT(bzy_array_len(all), 3);            /* a.txt, b.txt, c.log */

	bzy_file_delete(fa);
	bzy_file_delete(fb);
	bzy_file_delete(fc);
	bzy_file_delete(dir);
	bzy_release(dir);
	bzy_release(fa);
	bzy_release(fb);
	bzy_release(fc);
	bzy_release(pat);
	bzy_release(res);
	bzy_release(all);
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

static int g_off_ran;
static void off_fn(void *p)
{
	int *v = (int*)p;    /* Runs on an offload thread. */
	*v += 1;
	g_off_ran = 1;
}
static int g_off_result;
static void off_breeze(void)
{
	int v = 41;
	bzy_offload_run(off_fn, &v);   /* Parks here; resumes after the worker mutated v. */
	g_off_result = v;
}

static void test_offload_runs_and_resumes(void)
{
	g_off_ran = 0;
	g_off_result = 0;
	bzy_sched_init();
	bzy_spawn(off_breeze);
	bzy_sched_run();                 /* Returns only once the breeze finished (proves it resumed). */
	ASSERT_INT(g_off_ran, 1);        /* The task ran on the pool. */
	ASSERT_INT(g_off_result, 42);    /* The mutation is visible after resume. */
	bzy_offload_shutdown();
}

static int g_file_ok;
static void file_breeze(void)
{
	void *p = bzy_str_new("bzy_off_tmp.txt", 15);
	void *c = bzy_str_new("hello", 5);
	bzy_file_write_text(p, c);              /* Offloaded: we are inside a breeze. */
	void *back = bzy_file_read_text(p);     /* Offloaded. */
	g_file_ok = (strcmp(bzy_str_data(back), "hello") == 0);
	remove("bzy_off_tmp.txt");
	bzy_release(p);
	bzy_release(c);
	bzy_release(back);
}

static void test_file_ops_offload_in_breeze(void)
{
	g_file_ok = 0;
	bzy_sched_init();
	bzy_spawn(file_breeze);
	bzy_sched_run();
	ASSERT_INT(g_file_ok, 1);               /* Offloaded write+read round-trips correctly. */
	bzy_offload_shutdown();
}

static int64_t g_shell_code;
static void shell_breeze(void)
{
	void *cmd = bzy_str_new("exit 7", 6);   /* "cmd /c exit 7" -> exit code 7. */
	g_shell_code = bzy_system_shell(cmd, 1); /* Wait -> returns the exit code; parks via offload. */
	bzy_release(cmd);
}

static void test_system_shell_wait_exit_code(void)
{
	g_shell_code = -1;
	bzy_sched_init();
	bzy_spawn(shell_breeze);
	bzy_sched_run();
	ASSERT_INT((int)g_shell_code, 7);        /* The child's exit code came back through the offload park. */
	bzy_offload_shutdown();
}

static char g_iocp_opbuf[BZY_IOCP_OP_SIZE];
static int  g_iocp_resumed;
static unsigned long g_iocp_bytes;

static DWORD WINAPI poke_completion(void *unused)
{
	(void)unused;
	Sleep(20);                                   /* Let the breeze issue + park. */
	IocpOp *op = (IocpOp*)g_iocp_opbuf;
	/* Post a fake completion carrying 7 bytes to the breeze's overlapped. */
	extern void *bzy_iocp_test_port(void);       /* Test hook (declared in iocp.c). */
	PostQueuedCompletionStatus((HANDLE)bzy_iocp_test_port(), 7, 0,
							   (OVERLAPPED*)bzy_iocp_op_overlapped(op));
	return 0;
}

static void iocp_breeze(void)
{
	IocpOp *op = (IocpOp*)g_iocp_opbuf;
	bzy_iocp_ensure();
	bzy_iocp_op_reset(op);                        /* Records the running breeze. */
	HANDLE th = CreateThread(NULL, 0, poke_completion, NULL, 0, NULL);
	bzy_iocp_park(op);                            /* Resumes when poke_completion posts. */
	WaitForSingleObject(th, INFINITE);
	CloseHandle(th);
	g_iocp_bytes = bzy_iocp_op_bytes(op);
	g_iocp_resumed = 1;
}

static void test_iocp_completion_wakes_breeze(void)
{
	g_iocp_resumed = 0;
	g_iocp_bytes = 0;
	bzy_sched_init();
	bzy_spawn(iocp_breeze);
	bzy_sched_run();
	ASSERT_INT(g_iocp_resumed, 1);                /* The breeze resumed after the completion. */
	ASSERT_INT((int)g_iocp_bytes, 7);             /* The posted byte count transferred through. */
	bzy_iocp_shutdown();
}

static int g_tcp_ok;
static void *g_tcp_listener;       /* Shared Listener handle (set by server, read by client). */

static void tcp_server(void)
{
	void *cli = bzy_listener_accept(g_tcp_listener);   /* Parks until the client connects. */
	void *in = bzy_socket_read(cli, 64);               /* Parks until bytes arrive. */
	bzy_socket_write(cli, in);                          /* Echo the same byte[] back. */
	bzy_release(in);
	bzy_socket_close(cli);
	bzy_release(cli);
}

static void tcp_client(void)
{
	void *host = bzy_str_new("127.0.0.1", 9);
	void *s = bzy_socket_connect(host, bzy_listener_port(g_tcp_listener));
	bzy_release(host);
	void *msg = bzy_str_new("ping", 4);
	bzy_socket_write_text(s, msg);
	void *back = bzy_socket_read_text(s, 64);
	g_tcp_ok = (strcmp(bzy_str_data(back), "ping") == 0);
	bzy_release(msg);
	bzy_release(back);
	bzy_socket_close(s);
	bzy_release(s);
}

static void test_tcp_loopback_echo(void)
{
	g_tcp_ok = 0;
	bzy_sched_init();
	g_tcp_listener = bzy_listener_new(0);   /* Ephemeral port; bound before any breeze runs. */
	bzy_spawn(tcp_server);
	bzy_spawn(tcp_client);
	bzy_sched_run();
	ASSERT_INT(g_tcp_ok, 1);                /* The client got its bytes echoed back. */
	bzy_listener_close(g_tcp_listener);
	bzy_release(g_tcp_listener);
	bzy_iocp_shutdown();
}

static int g_tcp_null_ok;
static void tcp_timeout_breeze(void)
{
	void *l = bzy_listener_new(0);
	void *t = bzy_listener_accept_timeout(l, 30);   /* No client connects: must time out -> NULL. */
	void *y = bzy_listener_try_accept(l);           /* Nothing pending: must be NULL, no park. */
	g_tcp_null_ok = (t == NULL && y == NULL);
	bzy_listener_close(l);
	bzy_release(l);
}

static void test_tcp_accept_timeout_and_try(void)
{
	g_tcp_null_ok = 0;
	bzy_sched_init();
	bzy_spawn(tcp_timeout_breeze);
	bzy_sched_run();
	ASSERT_INT(g_tcp_null_ok, 1);
	bzy_iocp_shutdown();
}

static int g_udp_ok;
static void *g_udp_server;     /* Shared UdpSocket handle. */
static int64_t g_udp_server_port;

static void udp_server(void)
{
	void *dg = bzy_udp_receive(g_udp_server);          /* Parks until a datagram arrives. */
	void *payload = bzy_dgram_data(dg);
	void *host = bzy_dgram_host(dg);
	bzy_udp_send_to(g_udp_server, host, bzy_dgram_port(dg), payload);  /* Echo back. */
	bzy_release(host);
	bzy_release(payload);
	bzy_release(dg);
}

static void udp_client(void)
{
	void *u = bzy_udp_new(0);
	void *host = bzy_str_new("127.0.0.1", 9);
	void *msg = bzy_str_new("hey", 3);
	bzy_udp_send_text_to(u, host, g_udp_server_port, msg);
	void *dg = bzy_udp_receive(u);
	void *back = bzy_dgram_text(dg);
	g_udp_ok = (strcmp(bzy_str_data(back), "hey") == 0);
	bzy_release(back);
	bzy_release(dg);
	bzy_release(msg);
	bzy_release(host);
	bzy_udp_close(u);
	bzy_release(u);
}

static void test_udp_loopback_echo(void)
{
	g_udp_ok = 0;
	bzy_sched_init();
	g_udp_server = bzy_udp_new(0);
	g_udp_server_port = bzy_udp_port(g_udp_server);
	bzy_spawn(udp_server);
	bzy_spawn(udp_client);
	bzy_sched_run();
	ASSERT_INT(g_udp_ok, 1);
	bzy_udp_close(g_udp_server);
	bzy_release(g_udp_server);
	bzy_iocp_shutdown();
}

static int g_fc_ok;

/* Build a value byte[] from a C string's bytes (one byte per 8-byte slot). */
static void *fc_bytes(const char *s, int n)
{
	void *arr = bzy_array_new(n, 0);
	int64_t *slots = (int64_t*)((char*)arr + 32);
	for (int i = 0; i < n; i++)
	{
		slots[i] = (unsigned char)s[i];
	}

	return arr;
}

static void fc_breeze(void)
{
	void *path = bzy_str_new("fc_unit.tmp", 11);
	void *ch = bzy_filechannel_open(path);

	void *a = fc_bytes("hello", 5);
	void *b = fc_bytes("world", 5);
	bzy_filechannel_write_at(ch, 0, a);     /* Parks on the IOCP write. */
	bzy_filechannel_write_at(ch, 5, b);
	bzy_filechannel_sync(ch);               /* Durability barrier (offloaded). */

	void *back = bzy_filechannel_read_at(ch, 0, 10);   /* Parks on the IOCP read. */
	int64_t *slots = (int64_t*)((char*)back + 32);
	const char *want = "helloworld";
	int ok = (bzy_array_len(back) == 10);
	for (int i = 0; ok && i < 10; i++)
	{
		ok = (slots[i] == (unsigned char)want[i]);
	}

	g_fc_ok = ok && (bzy_filechannel_size(ch) == 10);

	bzy_filechannel_close(ch);
	bzy_release(back);
	bzy_release(a);
	bzy_release(b);
	bzy_release(ch);
	bzy_release(path);
}

static void test_filechannel_positioned_io(void)
{
	g_fc_ok = 0;
	bzy_sched_init();
	bzy_spawn(fc_breeze);
	bzy_sched_run();
	ASSERT_INT(g_fc_ok, 1);              /* Positioned writes + sync + read round-trip and size are correct. */
	bzy_offload_shutdown();
	bzy_iocp_shutdown();
	remove("fc_unit.tmp");
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
	RUN(test_shared_atomic_refcount);
	RUN(test_scheduler_multicore);
	RUN(test_channel_roundtrip);
	RUN(test_timer_heap_orders_by_deadline);
	RUN(test_timer_periodic_coalesces_missed_ticks);
	RUN(test_timer_cancel_is_skipped);
	RUN(test_timer_refcount_lifecycle);
	RUN(test_scheduler_runs_one_shot_timer);
	RUN(test_file_predicates);
	RUN(test_file_read_write);
	RUN(test_file_search);
	RUN(test_offload_runs_and_resumes);
	RUN(test_file_ops_offload_in_breeze);
	RUN(test_system_shell_wait_exit_code);
	RUN(test_iocp_completion_wakes_breeze);
	RUN(test_tcp_loopback_echo);
	RUN(test_tcp_accept_timeout_and_try);
	RUN(test_udp_loopback_echo);
	RUN(test_filechannel_positioned_io);
	SUMMARY();
	return 0;
}
