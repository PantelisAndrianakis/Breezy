/* Ordering comparators for the ordered containers (PriorityQueue, TreeMap,
   TreeSet). Own TU: a program using no ordered container drops this object.
   Slots are uniform int64 (int/long/object-ptr/string-ptr direct; double as its
   bit pattern; float in the low 32 bits). elem_kind selects the comparator:
   0 int/long/bool, 1 float, 2 double, 3 string. Object keys (4) are compared by
   an emitted vtable thunk, not here. */
#include "breezy.h"
#include <stdint.h>
#include <string.h>

static int cmp_i64(int64_t a, int64_t b)
{
	return (a > b) - (a < b);
}

static int cmp_f64(int64_t a, int64_t b)
{
	double x, y;
	memcpy(&x, &a, 8);
	memcpy(&y, &b, 8);
	return (x > y) - (x < y);   /* NaN keys are not expected; NaN sorts ambiguously. */
}

static int cmp_f32(int64_t a, int64_t b)
{
	float x, y;
	uint32_t ua = (uint32_t)a, ub = (uint32_t)b;
	memcpy(&x, &ua, 4);
	memcpy(&y, &ub, 4);
	return (x > y) - (x < y);
}

static int cmp_str(int64_t a, int64_t b)
{
	/* Lexicographic over UTF-8 bytes; shorter-is-less on a shared prefix. */
	const char *sa = bzy_str_data((void*)a);
	const char *sb = bzy_str_data((void*)b);
	int64_t la = bzy_str_len((void*)a);
	int64_t lb = bzy_str_len((void*)b);
	int64_t n = la < lb ? la : lb;
	int c = memcmp(sa, sb, (size_t)n);
	if (c != 0)
	{
		return c < 0 ? -1 : 1;
	}

	return (la > lb) - (la < lb);
}

/* Order two object keys by calling their Comparable.compareTo through the vtable.
   A Breezy method is C-ABI (receiver in arg0, argument in arg1) and the vtable
   pointer sits at object offset 0, so the call needs no per-class thunk -- just
   the compareTo slot, passed by codegen at construction. Returns <0/0/>0. */
int64_t bzy_obj_compare(void *a, void *b, int64_t slot)
{
	void **vt = *(void***)a;
	int (*fn)(void *, void *) = (int (*)(void *, void *))vt[slot];
	return (int64_t)fn(a, b);   /* int return, sign-extended. */
}

/* Return the primitive comparator for an elem_kind, or NULL for objects (4). */
bzy_cmp_fn bzy_order_cmp_for(int64_t elem_kind)
{
	switch (elem_kind)
	{
		case 1:
			return cmp_f32;
		case 2:
			return cmp_f64;
		case 3:
			return cmp_str;
		case 4:
			return (bzy_cmp_fn)0;   /* Objects use an injected vtable thunk. */
		default:
			return cmp_i64;         /* 0 int / long / bool. */
	}
}
