#include "breezy.h"
#include <string.h>
#include <stdlib.h>
#include <stdint.h>

/* Map layout (object_size = 80):
   0 vtable | 8 rc | 16 gcinfo | 24 size | 32 cap | 40 ctrl ptr (raw bytes) |
   48 keys BzyArray | 56 vals BzyArray | 64 key_kind (0=int,1=string) | 72 val_is_managed.
   Keys/vals live in managed arrays, so ARC and the cycle collector reach map
   contents through the array span descriptor; the finalizer only frees ctrl. */

enum { CTRL_EMPTY = 0x80, CTRL_DELETED = 0xFE };

static int64_t  *M_SIZE(void *m)
{
	return (int64_t*)((char*)m + 24);
}

static int64_t  *M_CAP(void *m)
{
	return (int64_t*)((char*)m + 32);
}

static uint8_t **M_CTRL(void *m)
{
	return (uint8_t**)((char*)m + 40);
}

static void    **M_KEYS(void *m)
{
	return (void**)((char*)m + 48);
}

static void    **M_VALS(void *m)
{
	return (void**)((char*)m + 56);
}

static int64_t  *M_KKIND(void *m)
{
	return (int64_t*)((char*)m + 64);
}

static int64_t  *M_VMAN(void *m)
{
	return (int64_t*)((char*)m + 72);
}

static int64_t *keys_data(void *m)
{
	return (int64_t*)((char*)(*M_KEYS(m)) + 32);
}

static int64_t *vals_data(void *m)
{
	return (int64_t*)((char*)(*M_VALS(m)) + 32);
}

static uint64_t mix64(uint64_t x)
{
	x ^= x >> 30;
	x *= 0xbf58476d1ce4e5b9ULL;
	x ^= x >> 27;
	x *= 0x94d049bb133111ebULL;
	x ^= x >> 31;
	return x;
}

static uint64_t hash_bytes(const char *p, int64_t n)
{
	uint64_t h = 0x9e3779b97f4a7c15ULL;
	for (int64_t i = 0; i < n; i++)
	{
		h = mix64(h ^ (uint8_t)p[i]);
	}

	return mix64(h ^ (uint64_t)n);
}

static uint64_t hash_key(void *m, int64_t key)
{
	if (*M_KKIND(m) == 1)
	{
		void *s = (void*)key;
		return hash_bytes(bzy_str_data(s), bzy_str_len(s));
	}

	return mix64((uint64_t)key);
}

static int key_eq(void *m, int64_t a, int64_t b)
{
	if (*M_KKIND(m) == 1)
	{
		void *x = (void*)a, *y = (void*)b;
		int64_t lx = bzy_str_len(x), ly = bzy_str_len(y);
		return lx == ly && memcmp(bzy_str_data(x), bzy_str_data(y), (size_t)lx) == 0;
	}

	return a == b;
}

static void bzy_map_finalize(void *m)
{
	free(*M_CTRL(m));   /* The two arrays are managed children, released by free_object. */
}

/* Runtime-owned vtable/descriptor (finalizer + 2 managed children at 48, 56). */
static int64_t g_map_typeinfo[4] = { 0 /* finalizer (set on first use) */, 2, 48, 56 };
static int64_t g_map_vtable[2];

static void *map_vtable(void)
{
	g_map_typeinfo[0] = (int64_t)(void*)bzy_map_finalize;
	g_map_vtable[0] = (int64_t)&g_map_typeinfo[0];
	return &g_map_vtable[1];
}

/* Triangular probe. Returns the FULL slot holding key, or -1. */
static int64_t map_find(void *m, int64_t key, uint64_t h)
{
	int64_t cap = *M_CAP(m);
	uint8_t *ctrl = *M_CTRL(m);
	int64_t *keys = keys_data(m);
	uint8_t h2 = (uint8_t)(h & 0x7f);
	int64_t i = (int64_t)((h >> 7) & (uint64_t)(cap - 1)), step = 1;
	for (;;)
	{
		uint8_t c = ctrl[i];
		if (c == CTRL_EMPTY)
		{
			return -1;
		}

		if (c == h2 && key_eq(m, keys[i], key))
		{
			return i;
		}

		i = (i + step) & (cap - 1);
		step++;
	}
}

/* Slot to insert key into (first tombstone seen, else the terminating empty). */
static int64_t map_find_insert(void *m, int64_t key, uint64_t h)
{
	int64_t cap = *M_CAP(m);
	uint8_t *ctrl = *M_CTRL(m);
	int64_t *keys = keys_data(m);
	uint8_t h2 = (uint8_t)(h & 0x7f);
	int64_t i = (int64_t)((h >> 7) & (uint64_t)(cap - 1)), step = 1, first_del = -1;
	for (;;)
	{
		uint8_t c = ctrl[i];
		if (c == CTRL_EMPTY)
		{
			return first_del >= 0 ? first_del : i;
		}

		if (c == CTRL_DELETED && first_del < 0)
		{
			first_del = i;
		}
		else if (c == h2 && key_eq(m, keys[i], key))
		{
			return i;   /* Existing key (overwrite). */
		}

		i = (i + step) & (cap - 1);
		step++;
	}
}

static void map_grow(void *m);

void bzy_map_put(void *m, int64_t key, int64_t val)
{
	if ((*M_SIZE(m) + 1) > (*M_CAP(m) * 7) / 8)
	{
		map_grow(m);
	}

	uint64_t h = hash_key(m, key);
	int64_t slot = map_find_insert(m, key, h);
	uint8_t *ctrl = *M_CTRL(m);
	int64_t *keys = keys_data(m), *vals = vals_data(m);
	int existing = (ctrl[slot] != CTRL_EMPTY && ctrl[slot] != CTRL_DELETED);

	if (existing)
	{
		if (*M_VMAN(m))
		{
			bzy_retain((void*)val);
			bzy_release((void*)vals[slot]);
		}

		vals[slot] = val;
		return;
	}

	if (*M_KKIND(m) == 1)
	{
		bzy_retain((void*)key);
	}

	if (*M_VMAN(m))
	{
		bzy_retain((void*)val);
	}

	keys[slot] = key;
	vals[slot] = val;
	ctrl[slot] = (uint8_t)(h & 0x7f);
	(*M_SIZE(m))++;
}

int64_t bzy_map_get(void *m, int64_t key)
{
	int64_t slot = map_find(m, key, hash_key(m, key));
	if (slot < 0)
	{
		return 0;
	}

	int64_t v = vals_data(m)[slot];
	if (*M_VMAN(m))
	{
		bzy_retain((void*)v);
	}

	return v;
}

int64_t bzy_map_has(void *m, int64_t key)
{
	return map_find(m, key, hash_key(m, key)) >= 0 ? 1 : 0;
}

void bzy_map_remove(void *m, int64_t key)
{
	int64_t slot = map_find(m, key, hash_key(m, key));
	if (slot < 0)
	{
		return;
	}

	int64_t *keys = keys_data(m), *vals = vals_data(m);
	if (*M_KKIND(m) == 1)
	{
		bzy_release((void*)keys[slot]);
	}

	if (*M_VMAN(m))
	{
		bzy_release((void*)vals[slot]);
	}

	keys[slot] = 0;
	vals[slot] = 0;
	(*M_CTRL(m))[slot] = CTRL_DELETED;
	(*M_SIZE(m))--;
}

int64_t bzy_map_len(void *m)
{
	return *M_SIZE(m);
}

int64_t bzy_map_iter(void *m, int64_t from)
{
	int64_t cap = *M_CAP(m);
	uint8_t *ctrl = *M_CTRL(m);
	for (int64_t i = from; i < cap; i++)
	{
		if ((ctrl[i] & 0x80) == 0)   /* FULL: top bit clear (EMPTY/DELETED set it). */
		{
			return i;
		}
	}

	return -1;
}

int64_t bzy_map_key_at(void *m, int64_t slot)
{
	return keys_data(m)[slot];   /* Borrowed: the map keeps the reference. */
}

int64_t bzy_map_val_at(void *m, int64_t slot)
{
	return vals_data(m)[slot];   /* Borrowed: the map keeps the reference. */
}

/* val_kind: 3 = string (content equality), anything else = raw 8-byte equality. */
int64_t bzy_map_contains_value(void *m, int64_t needle, int64_t val_kind)
{
	int64_t cap = *M_CAP(m);
	uint8_t *ctrl = *M_CTRL(m);
	int64_t *vals = vals_data(m);
	for (int64_t i = 0; i < cap; i++)
	{
		if ((ctrl[i] & 0x80) != 0)   /* Skip EMPTY/DELETED (top bit set). */
		{
			continue;
		}

		if (val_kind == 3 ? (bzy_str_eq((void*)vals[i], (void*)needle) != 0) : (vals[i] == needle))
		{
			return 1;
		}
	}

	return 0;
}

void *bzy_map_keys(void *m)
{
	int64_t managed = (*M_KKIND(m) == 1) ? 1 : 0;
	void *a = bzy_array_new(*M_SIZE(m), managed);   /* Owned (+1). */
	int64_t *out = (int64_t*)((char*)a + 32);
	int64_t cap = *M_CAP(m), *keys = keys_data(m);
	uint8_t *ctrl = *M_CTRL(m);
	int64_t n = 0;
	for (int64_t i = 0; i < cap; i++)
	{
		if ((ctrl[i] & 0x80) == 0)
		{
			if (managed)
			{
				bzy_retain((void*)keys[i]);
			}

			out[n++] = keys[i];
		}
	}

	return a;
}

void *bzy_map_values(void *m)
{
	int64_t managed = *M_VMAN(m);
	void *a = bzy_array_new(*M_SIZE(m), managed);
	int64_t *out = (int64_t*)((char*)a + 32);
	int64_t cap = *M_CAP(m), *vals = vals_data(m);
	uint8_t *ctrl = *M_CTRL(m);
	int64_t n = 0;
	for (int64_t i = 0; i < cap; i++)
	{
		if ((ctrl[i] & 0x80) == 0)
		{
			if (managed)
			{
				bzy_retain((void*)vals[i]);
			}

			out[n++] = vals[i];
		}
	}

	return a;
}

static void map_init_ctrl(uint8_t *ctrl, int64_t cap)
{
	for (int64_t i = 0; i < cap; i++)
	{
		ctrl[i] = CTRL_EMPTY;
	}
}

void *bzy_map_new(int64_t key_kind, int64_t val_is_managed)
{
	int64_t cap = 8;
	void *m = bzy_alloc(80);
	*(void**)m = map_vtable();
	*M_SIZE(m) = 0;
	*M_CAP(m) = cap;
	*M_CTRL(m) = (uint8_t*)malloc((size_t)cap);
	map_init_ctrl(*M_CTRL(m), cap);
	*M_KEYS(m) = bzy_array_new(cap, key_kind == 1 ? 1 : 0);   /* +1, owned by the map. */
	*M_VALS(m) = bzy_array_new(cap, val_is_managed);
	*M_KKIND(m) = key_kind;
	*M_VMAN(m) = val_is_managed;
	return m;
}

static void map_grow(void *m)
{
	int64_t oldcap = *M_CAP(m), newcap = oldcap * 2;
	uint8_t *oldctrl = *M_CTRL(m);
	void *oldkeys = *M_KEYS(m), *oldvals = *M_VALS(m);
	int64_t *okeys = (int64_t*)((char*)oldkeys + 32);
	int64_t *ovals = (int64_t*)((char*)oldvals + 32);

	uint8_t *nctrl = (uint8_t*)malloc((size_t)newcap);
	map_init_ctrl(nctrl, newcap);
	void *nkeys = bzy_array_new(newcap, *M_KKIND(m) == 1 ? 1 : 0);
	void *nvals = bzy_array_new(newcap, *M_VMAN(m));
	int64_t *nk = (int64_t*)((char*)nkeys + 32);
	int64_t *nv = (int64_t*)((char*)nvals + 32);

	*M_CTRL(m) = nctrl;        /* Publish new buffers before re-probing. */
	*M_KEYS(m) = nkeys;
	*M_VALS(m) = nvals;
	*M_CAP(m) = newcap;

	for (int64_t i = 0; i < oldcap; i++)
	{
		if (oldctrl[i] != CTRL_EMPTY && oldctrl[i] != CTRL_DELETED)
		{
			uint64_t h = hash_key(m, okeys[i]);
			int64_t s = map_find_insert(m, okeys[i], h);   /* Into the new arrays. */
			nk[s] = okeys[i];
			nv[s] = ovals[i];
			nctrl[s] = (uint8_t)(h & 0x7f);
			okeys[i] = 0;       /* Transfer ownership: blank the old slot. */
			ovals[i] = 0;
		}
	}

	free(oldctrl);
	bzy_release(oldkeys);      /* Old element slots are now NULL: frees the block only. */
	bzy_release(oldvals);
}
