#include "breezy.h"
#include <string.h>
#include <stdlib.h>
#include <stdint.h>

/* See docs .../04f-collections.md for the layout. The data buffer is a managed
   BzyArray (the vector's one child), so ARC + the cycle collector reach elements
   through the array span; unused ring slots stay 0 so that span is NULL-safe.
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
	return (*V_HEAD(v) + i) % *V_CAP(v);
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
	int64_t cap = 8;
	void *v = bzy_alloc(64);
	*(void**)v = vec_vtable();
	*V_LEN(v) = 0;
	*V_CAP(v) = cap;
	*V_HEAD(v) = 0;
	*V_DATA(v) = bzy_array_new(cap, elem_kind >= 3 ? 1 : 0);   /* +1, owned by the vector. */
	*V_KIND(v) = elem_kind;
	return v;
}

int64_t bzy_vec_len(void *v)
{
	return *V_LEN(v);
}

static void vec_grow(void *v)
{
	int64_t oldcap = *V_CAP(v), newcap = oldcap * 2, n = *V_LEN(v);
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

	*V_DATA(v) = newdata;
	*V_CAP(v) = newcap;
	*V_HEAD(v) = 0;
	bzy_release(olddata);                   /* Old slots blanked: frees the block only. */
}

void bzy_vec_push_back(void *v, int64_t val)
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

int64_t bzy_vec_pop_back(void *v)
{
	if (*V_LEN(v) == 0)
	{
		bzy_oob(-1, 0);
	}

	int64_t p = vec_phys(v, *V_LEN(v) - 1);
	int64_t val = vec_slots(v)[p];
	vec_slots(v)[p] = 0;                    /* Transfer out (owned): no release. */
	(*V_LEN(v))--;
	return val;
}

int64_t bzy_vec_get(void *v, int64_t i)
{
	if (i < 0 || i >= *V_LEN(v))
	{
		bzy_oob(i, *V_LEN(v));
	}

	int64_t val = vec_slots(v)[vec_phys(v, i)];
	if (vec_managed(v))
	{
		bzy_retain((void*)val);
	}

	return val;
}

void bzy_vec_set(void *v, int64_t i, int64_t val)
{
	if (i < 0 || i >= *V_LEN(v))
	{
		bzy_oob(i, *V_LEN(v));
	}

	int64_t p = vec_phys(v, i);
	if (vec_managed(v))
	{
		bzy_retain((void*)val);
		bzy_release((void*)vec_slots(v)[p]);
	}

	vec_slots(v)[p] = val;
}

int64_t bzy_vec_peek_back(void *v)
{
	if (*V_LEN(v) == 0)
	{
		bzy_oob(-1, 0);
	}

	int64_t val = vec_slots(v)[vec_phys(v, *V_LEN(v) - 1)];
	if (vec_managed(v))
	{
		bzy_retain((void*)val);
	}

	return val;
}

void bzy_vec_push_front(void *v, int64_t val)
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
	*V_HEAD(v) = (*V_HEAD(v) - 1 + cap) % cap;
	vec_slots(v)[*V_HEAD(v)] = val;
	(*V_LEN(v))++;
}

int64_t bzy_vec_pop_front(void *v)
{
	if (*V_LEN(v) == 0)
	{
		bzy_oob(-1, 0);
	}

	int64_t p = *V_HEAD(v);
	int64_t val = vec_slots(v)[p];
	vec_slots(v)[p] = 0;                    /* Transfer out (owned). */
	*V_HEAD(v) = (p + 1) % *V_CAP(v);
	(*V_LEN(v))--;
	return val;
}

int64_t bzy_vec_peek_front(void *v)
{
	if (*V_LEN(v) == 0)
	{
		bzy_oob(-1, 0);
	}

	int64_t val = vec_slots(v)[*V_HEAD(v)];
	if (vec_managed(v))
	{
		bzy_retain((void*)val);
	}

	return val;
}

void bzy_vec_remove_at(void *v, int64_t i)
{
	if (i < 0 || i >= *V_LEN(v))
	{
		bzy_oob(i, *V_LEN(v));
	}

	int64_t *s = vec_slots(v);
	if (vec_managed(v))
	{
		bzy_release((void*)s[vec_phys(v, i)]);
	}

	for (int64_t j = i; j < *V_LEN(v) - 1; j++)   /* Shift logical j+1 -> j. */
	{
		s[vec_phys(v, j)] = s[vec_phys(v, j + 1)];
	}

	s[vec_phys(v, *V_LEN(v) - 1)] = 0;
	(*V_LEN(v))--;
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

int64_t bzy_vec_index_of(void *v, int64_t needle)
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

int64_t bzy_vec_contains(void *v, int64_t needle)
{
	return bzy_vec_index_of(v, needle) >= 0 ? 1 : 0;
}
