/* PriorityQueue<T>: a binary min-heap over a doubling int64 slot array. Own TU:
   a program using no PriorityQueue drops this object. Ordering: bzy_order_cmp_for
   by elem_kind for primitives; an injected vtable thunk (obj_cmp) for objects.
   Managed slots (string/object) are ARC-retained on add, released on finalize;
   poll transfers the add-time reference to the caller, peek hands back a fresh
   owned reference -- so both return an owned managed value (like bzy_map_get).

   ARC ceiling: the slot array is a raw allocation, not a managed BzyArray child,
   so the cycle collector does not trace elements held only by the queue. Acyclic
   graphs (primitives, strings, ordinary object keys) are reclaimed correctly; an
   object cycle reachable solely through a PriorityQueue would leak. Keys are
   almost always primitive/string, where this cannot arise. */
#include "breezy.h"
#include <stdint.h>
#include <stdlib.h>

typedef struct
{
	int64_t   *a;          /* Heap array; index 0 = min. */
	int64_t    len;
	int64_t    cap;
	int64_t    elem_kind;
	bzy_cmp_fn cmp;        /* Primitive comparator, or NULL when obj_cmp is used. */
	bzy_cmp_fn obj_cmp;    /* Object vtable thunk, or NULL for primitives. */
} PQ;

/* Handle layout (object_size = 40): 0 vtable | 8 rc | 16 gcinfo | 24 PQ*. */
#define PQ_PTR(o) (*(PQ**)((char*)(o) + 24))

static int64_t g_pq_typeinfo[2];
static int64_t g_pq_vtable[2];

static int pq_shared(void *o)
{
	return (*(int64_t*)((char*)o + 16) & (1ll << 3)) != 0;
}

static int pq_managed(int64_t k)
{
	return k == 3 || k == 4;
}

static int pq_less(PQ *q, int64_t x, int64_t y)
{
	return (q->obj_cmp ? q->obj_cmp(x, y) : q->cmp(x, y)) < 0;
}

static void pq_finalize(void *o)
{
	PQ *q = PQ_PTR(o);
	if (!q)
	{
		return;
	}

	if (pq_managed(q->elem_kind))
	{
		for (int64_t i = 0; i < q->len; i++)
		{
			bzy_release((void*)q->a[i]);
		}
	}

	free(q->a);
	free(q);
	PQ_PTR(o) = NULL;
}

static void *pq_vtable(void)
{
	g_pq_typeinfo[0] = (int64_t)(void*)pq_finalize;
	g_pq_vtable[0] = (int64_t)&g_pq_typeinfo[0];
	return &g_pq_vtable[1];
}

void *bzy_pq_new(int64_t elem_kind, void *obj_cmp)
{
	PQ *q = (PQ*)calloc(1, sizeof(PQ));
	q->cap = 8;
	q->a = (int64_t*)malloc((size_t)q->cap * 8);
	q->elem_kind = elem_kind;
	q->obj_cmp = (bzy_cmp_fn)obj_cmp;
	q->cmp = obj_cmp ? (bzy_cmp_fn)0 : bzy_order_cmp_for(elem_kind);

	void *o = bzy_alloc(40);
	*(void**)o = pq_vtable();
	PQ_PTR(o) = q;
	bzy_share_crosscore(o);
	return o;
}

static void pq_add_impl(PQ *q, int64_t v)
{
	if (q->len == q->cap)
	{
		q->cap *= 2;
		q->a = (int64_t*)realloc(q->a, (size_t)q->cap * 8);
	}

	if (pq_managed(q->elem_kind))
	{
		bzy_retain((void*)v);
	}

	int64_t i = q->len++;
	q->a[i] = v;
	while (i > 0)                                   /* Sift up. */
	{
		int64_t p = (i - 1) / 2;
		if (!pq_less(q, q->a[i], q->a[p]))
		{
			break;
		}

		int64_t t = q->a[i];
		q->a[i] = q->a[p];
		q->a[p] = t;
		i = p;
	}
}

static int64_t pq_poll_impl(PQ *q)
{
	if (q->len == 0)
	{
		return 0;                                   /* No-throw sentinel. */
	}

	int64_t top = q->a[0];                          /* Transfer the add-time reference to the caller. */
	q->len--;
	q->a[0] = q->a[q->len];
	int64_t i = 0;
	while (1)                                        /* Sift down. */
	{
		int64_t l = 2 * i + 1, r = 2 * i + 2, m = i;
		if (l < q->len && pq_less(q, q->a[l], q->a[m]))
		{
			m = l;
		}

		if (r < q->len && pq_less(q, q->a[r], q->a[m]))
		{
			m = r;
		}

		if (m == i)
		{
			break;
		}

		int64_t t = q->a[i];
		q->a[i] = q->a[m];
		q->a[m] = t;
		i = m;
	}

	return top;
}

void bzy_pq_add(void *o, int64_t v)
{
	PQ *q = PQ_PTR(o);
	if (pq_shared(o))
	{
		bzy_shared_lock(o);
		pq_add_impl(q, v);
		bzy_shared_unlock(o);
		return;
	}

	pq_add_impl(q, v);
}

int64_t bzy_pq_poll(void *o)
{
	PQ *q = PQ_PTR(o);
	if (pq_shared(o))
	{
		bzy_shared_lock(o);
		int64_t r = pq_poll_impl(q);
		bzy_shared_unlock(o);
		return r;
	}

	return pq_poll_impl(q);
}

int64_t bzy_pq_peek(void *o)
{
	PQ *q = PQ_PTR(o);
	int64_t r;
	if (pq_shared(o))
	{
		bzy_shared_lock(o);
		r = q->len == 0 ? 0 : q->a[0];
		if (q->len != 0 && pq_managed(q->elem_kind))
		{
			bzy_retain((void*)r);                   /* Borrow -> owned, like map.get. */
		}

		bzy_shared_unlock(o);
		return r;
	}

	r = q->len == 0 ? 0 : q->a[0];
	if (q->len != 0 && pq_managed(q->elem_kind))
	{
		bzy_retain((void*)r);
	}

	return r;
}

int64_t bzy_pq_size(void *o)
{
	return PQ_PTR(o)->len;
}
