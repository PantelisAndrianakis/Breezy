/* PriorityQueue<T>: a binary min-heap. Storage is a managed BzyArray child (the
   heap's one child, declared in the type descriptor), so ARC and the cycle
   collector reach elements through the array span exactly as the vector does --
   an object cycle held only through a PriorityQueue is reclaimed. The heap is a
   flat 0..len-1 array (no ring); the sift walks the array slots directly, so the
   common case stays a bare load/compare/store with no per-access call. Own TU:
   a program using no PriorityQueue drops this object.

   Ordering: bzy_order_cmp_for by elem_kind for primitives; an injected vtable
   thunk (obj_cmp) for objects. Managed elements are retained on add; poll
   transfers the add-time reference out (blanking the slot); peek hands back a
   fresh owned reference -- so both return an owned managed value (like
   bzy_map_get). No explicit finalizer: freeing the heap releases the child
   array, which releases its managed slots.

   Handle layout (object_size = 72): 0 vtable | 8 rc | 16 gcinfo | 24 len |
   32 cap | 40 data (managed array child) | 48 elem_kind | 56 cmp | 64 obj_slot.
   obj_slot >= 0 is the Comparable.compareTo vtable slot for object keys; -1 for
   primitives (which use cmp). */
#include "breezy.h"
#include <stdint.h>
#include <stdlib.h>

static int64_t  *PQ_LEN(void *o)
{
	return (int64_t*)((char*)o + 24);
}

static int64_t  *PQ_CAP(void *o)
{
	return (int64_t*)((char*)o + 32);
}

static void    **PQ_DATA(void *o)
{
	return (void**)((char*)o + 40);
}

static int64_t  *PQ_KIND(void *o)
{
	return (int64_t*)((char*)o + 48);
}

static bzy_cmp_fn *PQ_CMP(void *o)
{
	return (bzy_cmp_fn*)((char*)o + 56);
}

static int64_t    *PQ_OBJSLOT(void *o)
{
	return (int64_t*)((char*)o + 64);
}

static int64_t *pq_slots(void *o)
{
	return (int64_t*)((char*)(*PQ_DATA(o)) + 32);   /* BzyArray data at +32. */
}

/* Descriptor: no finalizer, one managed child at offset 40 (the data array). */
static int64_t g_pq_typeinfo[3] = { 0, 1, 40 };
static int64_t g_pq_vtable[2];

static void *pq_vtable(void)
{
	g_pq_vtable[0] = (int64_t)&g_pq_typeinfo[0];
	return &g_pq_vtable[1];
}

static int pq_shared(void *o)
{
	return (*(int64_t*)((char*)o + 16) & (1ll << 3)) != 0;
}

static int pq_managed(void *o)
{
	return *PQ_KIND(o) >= 3;
}

static int pq_less(void *o, int64_t x, int64_t y)
{
	int64_t slot = *PQ_OBJSLOT(o);
	int64_t c = slot >= 0 ? bzy_obj_compare((void*)x, (void*)y, slot) : (*PQ_CMP(o))(x, y);
	return c < 0;
}

/* The hot case -- an int/long/bool heap (elem_kind 0) -- orders by a plain signed
   compare, identical to the cmp_i64 the comparator would return. The sift loops
   hoist this once (prim) so each comparison is an inline < instead of an indirect
   comparator call; float/string/object keys keep the general pq_less path. The
   branch is loop-invariant, so it predicts away. */
#define PQ_LESS(o, prim, x, y) ((prim) ? (x) < (y) : pq_less((o), (x), (y)))

void *bzy_pq_new(int64_t elem_kind, int64_t obj_slot)
{
	void *o = bzy_alloc(72);
	*(void**)o = pq_vtable();
	*PQ_KIND(o) = elem_kind;
	*PQ_OBJSLOT(o) = obj_slot;
	*PQ_CMP(o) = obj_slot >= 0 ? (bzy_cmp_fn)0 : bzy_order_cmp_for(elem_kind);
	/* len, cap, data left zero by bzy_alloc; the array is allocated on first add. */
	return o;
}

static void pq_grow(void *o)
{
	int64_t oldcap = *PQ_CAP(o), newcap = oldcap ? oldcap * 2 : 8, n = *PQ_LEN(o);
	void *olddata = *PQ_DATA(o);
	void *newdata = bzy_array_new(newcap, pq_managed(o) ? 1 : 0);
	if (n > 0)
	{
		int64_t *os = (int64_t*)((char*)olddata + 32);
		int64_t *ns = (int64_t*)((char*)newdata + 32);
		for (int64_t i = 0; i < n; i++)
		{
			ns[i] = os[i];
			os[i] = 0;                          /* Transfer ownership; blank old slot. */
		}
	}

	if (pq_shared(o))
	{
		*(int64_t*)((char*)newdata + 16) |= (1ll << 3);   /* Newborn array inherits SHARED. */
	}

	*PQ_DATA(o) = newdata;
	*PQ_CAP(o) = newcap;
	if (olddata)
	{
		bzy_release(olddata);                   /* Old slots blanked: frees the block only. */
	}
}

static void pq_add_impl(void *o, int64_t v)
{
	if (*PQ_LEN(o) + 1 > *PQ_CAP(o))
	{
		pq_grow(o);
	}

	if (pq_managed(o))
	{
		bzy_retain((void*)v);
	}

	int prim = (*PQ_KIND(o) == 0);
	int64_t *a = pq_slots(o);
	int64_t i = (*PQ_LEN(o))++;
	a[i] = v;
	while (i > 0)                                   /* Sift up. */
	{
		int64_t p = (i - 1) / 2;
		if (!PQ_LESS(o, prim, a[i], a[p]))
		{
			break;
		}

		int64_t t = a[i];
		a[i] = a[p];
		a[p] = t;
		i = p;
	}
}

static int64_t pq_poll_impl(void *o)
{
	int64_t n = *PQ_LEN(o);
	if (n == 0)
	{
		return 0;                                   /* No-throw sentinel. */
	}

	int64_t *a = pq_slots(o);
	int64_t top = a[0];                             /* Transfer the add-time reference out. */
	n--;
	a[0] = a[n];
	a[n] = 0;                                       /* Blank the vacated slot (no double-release). */
	*PQ_LEN(o) = n;
	int prim = (*PQ_KIND(o) == 0);
	int64_t i = 0;
	while (1)                                        /* Sift down. */
	{
		int64_t l = 2 * i + 1, r = 2 * i + 2, m = i;
		if (l < n && PQ_LESS(o, prim, a[l], a[m]))
		{
			m = l;
		}

		if (r < n && PQ_LESS(o, prim, a[r], a[m]))
		{
			m = r;
		}

		if (m == i)
		{
			break;
		}

		int64_t t = a[i];
		a[i] = a[m];
		a[m] = t;
		i = m;
	}

	return top;
}

static int64_t pq_peek_impl(void *o)
{
	if (*PQ_LEN(o) == 0)
	{
		return 0;
	}

	int64_t r = pq_slots(o)[0];
	if (pq_managed(o))
	{
		bzy_retain((void*)r);                       /* Borrow -> owned, like map.get. */
	}

	return r;
}

void bzy_pq_add(void *o, int64_t v)
{
	if (pq_shared(o))
	{
		if (pq_managed(o))
		{
			bzy_share_crosscore((void*)v);          /* Insert barrier, before the stripe. */
		}

		bzy_shared_lock(o);
		pq_add_impl(o, v);
		bzy_shared_unlock(o);
		return;
	}

	pq_add_impl(o, v);
}

int64_t bzy_pq_poll(void *o)
{
	if (pq_shared(o))
	{
		bzy_shared_lock(o);
		int64_t r = pq_poll_impl(o);
		bzy_shared_unlock(o);
		return r;
	}

	return pq_poll_impl(o);
}

int64_t bzy_pq_peek(void *o)
{
	if (pq_shared(o))
	{
		bzy_shared_lock(o);
		int64_t r = pq_peek_impl(o);
		bzy_shared_unlock(o);
		return r;
	}

	return pq_peek_impl(o);
}

int64_t bzy_pq_size(void *o)
{
	if (pq_shared(o))
	{
		bzy_shared_lock(o);
		int64_t n = *PQ_LEN(o);
		bzy_shared_unlock(o);
		return n;
	}

	return *PQ_LEN(o);
}
