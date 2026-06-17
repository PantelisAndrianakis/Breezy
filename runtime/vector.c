#include "breezy.h"
#include <string.h>
#include <stdlib.h>
#include <stdint.h>

/* A growable ring-buffer vector. The data buffer is a managed BzyArray (the
   vector's one child), so ARC + the cycle collector reach elements through the
   array span; unused ring slots stay 0 so that span is NULL-safe.
   Layout (object_size = 64): 0 vtable | 8 rc | 16 gcinfo | 24 length | 32 cap |
   40 head | 48 data array | 56 elem_kind. */

static int64_t  *V_LEN(void *v)
{
	return (int64_t*)((char*)v + 24);
}

static int64_t  *V_CAP(void *v)
{
	return (int64_t*)((char*)v + 32);
}

static int64_t  *V_HEAD(void *v)
{
	return (int64_t*)((char*)v + 40);
}

static void    **V_DATA(void *v)
{
	return (void**)((char*)v + 48);
}

static int64_t  *V_KIND(void *v)
{
	return (int64_t*)((char*)v + 56);
}

static int64_t *vec_slots(void *v)
{
	return (int64_t*)((char*)(*V_DATA(v)) + 32);   /* BzyArray data at +32. */
}

static int vec_managed(void *v)
{
	return *V_KIND(v) >= 3;
}

static int64_t vec_phys(void *v, int64_t i)
{
	return (*V_HEAD(v) + i) & (*V_CAP(v) - 1);   /* cap is always a power of two, so mask == modulo. */
}

/* Runtime-owned vtable/descriptor: no finalizer, one managed child at offset 48. */
static int64_t g_vec_typeinfo[3] = { 0, 1, 48 };
static int64_t g_vec_vtable[2];

static void *vec_vtable(void)
{
	g_vec_vtable[0] = (int64_t)&g_vec_typeinfo[0];
	return &g_vec_vtable[1];
}

void *bzy_vec_new(int64_t elem_kind)
{
	void *v = bzy_alloc(64);
	*(void**)v = vec_vtable();
	/* The backing array is allocated lazily on the first push, so an unused vector
	   costs only its header. vec_grow handles the cap == 0 case. */
	*V_KIND(v) = elem_kind;
	return v;   /* len, cap, head and data are left zero by bzy_alloc. */
}

/* Shared-receiver gate. Every public vector op funnels through one of these: a
   vector that crossed cores (SHARED gcinfo bit) takes its stripe for the whole
   op - len/head/cap/data mutate together, so per-op locking is the unit of
   atomicity. A confined vector skips the branch entirely. The deep-share of an
   incoming managed element happens BEFORE the stripe is taken, and old-occupant
   releases happen AFTER it is dropped (lock ordering: the roots lock, a stripe,
   and a destructor cascade are never held together). */
static int vec_is_shared(void *v)
{
	return (*(int64_t*)((char*)v + 16) & (1ll << 3)) != 0;
}

int64_t bzy_vec_len(void *v)
{
	if (vec_is_shared(v))
	{
		bzy_shared_lock(v);
		int64_t n = *V_LEN(v);
		bzy_shared_unlock(v);
		return n;
	}

	return *V_LEN(v);
}

static void vec_grow(void *v)
{
	int64_t oldcap = *V_CAP(v), newcap = oldcap ? oldcap * 2 : 8, n = *V_LEN(v);
	void *olddata = *V_DATA(v);
	int64_t *os = (int64_t*)((char*)olddata + 32);
	void *newdata = bzy_array_new(newcap, vec_managed(v) ? 1 : 0);
	int64_t *ns = (int64_t*)((char*)newdata + 32);
	for (int64_t i = 0; i < n; i++)        /* Re-normalize ring into logical order at head 0. */
	{
		int64_t p = vec_phys(v, i);
		ns[i] = os[p];
		os[p] = 0;                          /* Transfer ownership; blank the old slot. */
	}

	/* A shared vector's fresh backing array inherits the SHARED bit: it is
	   newborn and sole-referenced here, so a plain OR is race-free (and the
	   roots lock must NOT be taken while holding a stripe). */
	if (vec_is_shared(v))
	{
		*(int64_t*)((char*)newdata + 16) |= (1ll << 3);
	}

	*V_DATA(v) = newdata;
	*V_CAP(v) = newcap;
	*V_HEAD(v) = 0;
	bzy_release(olddata);                   /* Old slots blanked: frees the block only. */
}

/* Grow the backing array once so the vector can hold at least `want` elements
   without reallocating. A single allocation to the next power-of-two capacity >=
   want, re-normalizing any existing ring into logical order at head 0 -- unlike
   repeated push-driven doubling, which would copy O(n) elements many times while
   filling a known-size result. A no-op when the current capacity already suffices. */
void bzy_vec_reserve(void *v, int64_t want)
{
	if (vec_is_shared(v))
	{
		bzy_shared_lock(v);
	}

	if (want > *V_CAP(v))
	{
		int64_t newcap = *V_CAP(v) ? *V_CAP(v) : 8;
		while (newcap < want)
		{
			newcap *= 2;
		}

		int64_t n = *V_LEN(v);
		void *olddata = *V_DATA(v);
		void *newdata = bzy_array_new(newcap, vec_managed(v) ? 1 : 0);
		if (olddata)
		{
			int64_t *os = (int64_t*)((char*)olddata + 32);
			int64_t *ns = (int64_t*)((char*)newdata + 32);
			for (int64_t i = 0; i < n; i++)
			{
				int64_t p = vec_phys(v, i);
				ns[i] = os[p];
				os[p] = 0;
			}
		}

		if (vec_is_shared(v))
		{
			*(int64_t*)((char*)newdata + 16) |= (1ll << 3);
		}

		*V_DATA(v) = newdata;
		*V_CAP(v) = newcap;
		*V_HEAD(v) = 0;
		if (olddata)
		{
			bzy_release(olddata);
		}
	}

	if (vec_is_shared(v))
	{
		bzy_shared_unlock(v);
	}
}

static void vec_push_back_impl(void *v, int64_t val)
{
	if (*V_LEN(v) + 1 > *V_CAP(v))
	{
		vec_grow(v);
	}

	if (vec_managed(v))
	{
		bzy_retain((void*)val);
	}

	vec_slots(v)[vec_phys(v, *V_LEN(v))] = val;
	(*V_LEN(v))++;
}

void bzy_vec_push_back(void *v, int64_t val)
{
	if (vec_is_shared(v))
	{
		if (vec_managed(v))
		{
			bzy_share_crosscore((void*)val);   /* Insert barrier, before the stripe. */
		}

		bzy_shared_lock(v);
		vec_push_back_impl(v, val);
		bzy_shared_unlock(v);
		return;
	}

	vec_push_back_impl(v, val);
}

static int64_t vec_pop_back_impl(void *v)
{
	if (*V_LEN(v) == 0)
	{
		bzy_oob_abort(-1, 0);
	}

	int64_t p = vec_phys(v, *V_LEN(v) - 1);
	int64_t val = vec_slots(v)[p];
	vec_slots(v)[p] = 0;                    /* Transfer out (owned): no release. */
	(*V_LEN(v))--;
	return val;
}

int64_t bzy_vec_pop_back(void *v)
{
	if (vec_is_shared(v))
	{
		bzy_shared_lock(v);
		int64_t val = vec_pop_back_impl(v);
		bzy_shared_unlock(v);
		return val;
	}

	return vec_pop_back_impl(v);
}

static int64_t vec_get_impl(void *v, int64_t i)
{
	if (i < 0 || i >= *V_LEN(v))
	{
		bzy_oob_abort(i, *V_LEN(v));
	}

	int64_t val = vec_slots(v)[vec_phys(v, i)];
	if (vec_managed(v))
	{
		bzy_retain((void*)val);   /* Retain inside the stripe on the shared path. */
	}

	return val;
}

int64_t bzy_vec_get(void *v, int64_t i)
{
	if (vec_is_shared(v))
	{
		bzy_shared_lock(v);
		int64_t val = vec_get_impl(v, i);
		bzy_shared_unlock(v);
		return val;
	}

	return vec_get_impl(v, i);
}

/* Returns the displaced occupant (0 for value kinds) so callers can release it
   outside the stripe - a destructor cascade must never run under the lock. */
static int64_t vec_set_impl(void *v, int64_t i, int64_t val)
{
	if (i < 0 || i >= *V_LEN(v))
	{
		bzy_oob_abort(i, *V_LEN(v));
	}

	int64_t p = vec_phys(v, i);
	int64_t old = 0;
	if (vec_managed(v))
	{
		bzy_retain((void*)val);
		old = vec_slots(v)[p];
	}

	vec_slots(v)[p] = val;
	return old;
}

void bzy_vec_set(void *v, int64_t i, int64_t val)
{
	if (vec_is_shared(v))
	{
		if (vec_managed(v))
		{
			bzy_share_crosscore((void*)val);   /* Insert barrier, before the stripe. */
		}

		bzy_shared_lock(v);
		int64_t old = vec_set_impl(v, i, val);
		bzy_shared_unlock(v);
		bzy_release((void*)old);
		return;
	}

	bzy_release((void*)vec_set_impl(v, i, val));
}

static int64_t vec_peek_back_impl(void *v)
{
	if (*V_LEN(v) == 0)
	{
		bzy_oob_abort(-1, 0);
	}

	int64_t val = vec_slots(v)[vec_phys(v, *V_LEN(v) - 1)];
	if (vec_managed(v))
	{
		bzy_retain((void*)val);
	}

	return val;
}

int64_t bzy_vec_peek_back(void *v)
{
	if (vec_is_shared(v))
	{
		bzy_shared_lock(v);
		int64_t val = vec_peek_back_impl(v);
		bzy_shared_unlock(v);
		return val;
	}

	return vec_peek_back_impl(v);
}

static void vec_push_front_impl(void *v, int64_t val)
{
	if (*V_LEN(v) + 1 > *V_CAP(v))
	{
		vec_grow(v);
	}

	if (vec_managed(v))
	{
		bzy_retain((void*)val);
	}

	int64_t cap = *V_CAP(v);
	*V_HEAD(v) = (*V_HEAD(v) - 1 + cap) & (cap - 1);   /* Power-of-two cap: mask == modulo. */
	vec_slots(v)[*V_HEAD(v)] = val;
	(*V_LEN(v))++;
}

void bzy_vec_push_front(void *v, int64_t val)
{
	if (vec_is_shared(v))
	{
		if (vec_managed(v))
		{
			bzy_share_crosscore((void*)val);   /* Insert barrier, before the stripe. */
		}

		bzy_shared_lock(v);
		vec_push_front_impl(v, val);
		bzy_shared_unlock(v);
		return;
	}

	vec_push_front_impl(v, val);
}

static int64_t vec_pop_front_impl(void *v)
{
	if (*V_LEN(v) == 0)
	{
		bzy_oob_abort(-1, 0);
	}

	int64_t p = *V_HEAD(v);
	int64_t val = vec_slots(v)[p];
	vec_slots(v)[p] = 0;                    /* Transfer out (owned). */
	*V_HEAD(v) = (p + 1) & (*V_CAP(v) - 1);   /* Power-of-two cap: mask == modulo. */
	(*V_LEN(v))--;
	return val;
}

int64_t bzy_vec_pop_front(void *v)
{
	if (vec_is_shared(v))
	{
		bzy_shared_lock(v);
		int64_t val = vec_pop_front_impl(v);
		bzy_shared_unlock(v);
		return val;
	}

	return vec_pop_front_impl(v);
}

static int64_t vec_peek_front_impl(void *v)
{
	if (*V_LEN(v) == 0)
	{
		bzy_oob_abort(-1, 0);
	}

	int64_t val = vec_slots(v)[*V_HEAD(v)];
	if (vec_managed(v))
	{
		bzy_retain((void*)val);
	}

	return val;
}

int64_t bzy_vec_peek_front(void *v)
{
	if (vec_is_shared(v))
	{
		bzy_shared_lock(v);
		int64_t val = vec_peek_front_impl(v);
		bzy_shared_unlock(v);
		return val;
	}

	return vec_peek_front_impl(v);
}

/* Returns the removed occupant (0 for value kinds) so callers can release it
   outside the stripe. */
static int64_t vec_remove_at_impl(void *v, int64_t i)
{
	if (i < 0 || i >= *V_LEN(v))
	{
		bzy_oob_abort(i, *V_LEN(v));
	}

	int64_t *s = vec_slots(v);
	int64_t old = 0;
	if (vec_managed(v))
	{
		old = s[vec_phys(v, i)];
	}

	for (int64_t j = i; j < *V_LEN(v) - 1; j++)   /* Shift logical j+1 -> j. */
	{
		s[vec_phys(v, j)] = s[vec_phys(v, j + 1)];
	}

	s[vec_phys(v, *V_LEN(v) - 1)] = 0;
	(*V_LEN(v))--;
	return old;
}

void bzy_vec_remove_at(void *v, int64_t i)
{
	if (vec_is_shared(v))
	{
		bzy_shared_lock(v);
		int64_t old = vec_remove_at_impl(v, i);
		bzy_shared_unlock(v);
		bzy_release((void*)old);
		return;
	}

	bzy_release((void*)vec_remove_at_impl(v, i));
}

static int vec_eq(void *v, int64_t a, int64_t b)
{
	switch (*V_KIND(v))
	{
	case 1:
	{
		float fa, fb;
		uint32_t ua = (uint32_t)a, ub = (uint32_t)b;
		memcpy(&fa, &ua, 4);
		memcpy(&fb, &ub, 4);
		return fa == fb;
	}
	case 2:
	{
		double da, db;
		memcpy(&da, &a, 8);
		memcpy(&db, &b, 8);
		return da == db;
	}
	case 3:
		return bzy_str_eq((void*)a, (void*)b) ? 1 : 0;
	default:
		return a == b;   /* int/bool (0) and object identity (4). */
	}
}

static int64_t vec_index_of_impl(void *v, int64_t needle)
{
	int64_t *s = vec_slots(v);
	for (int64_t i = 0; i < *V_LEN(v); i++)
	{
		if (vec_eq(v, s[vec_phys(v, i)], needle))
		{
			return i;
		}
	}

	return -1;
}

int64_t bzy_vec_index_of(void *v, int64_t needle)
{
	if (vec_is_shared(v))
	{
		bzy_shared_lock(v);
		int64_t i = vec_index_of_impl(v, needle);
		bzy_shared_unlock(v);
		return i;
	}

	return vec_index_of_impl(v, needle);
}

int64_t bzy_vec_contains(void *v, int64_t needle)
{
	/* Delegates to the gated index_of; taking the stripe here too would
	   self-deadlock on the same stripe (non-reentrant locks). */
	return bzy_vec_index_of(v, needle) >= 0 ? 1 : 0;
}
