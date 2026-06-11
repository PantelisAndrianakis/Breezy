#include "breezy.h"
#include <string.h>
#include <stdlib.h>
#include <stdint.h>

/* Map layout (object_size = 80):
   0 vtable | 8 rc | 16 gcinfo | 24 size | 32 cap | 40 ctrl ptr (raw bytes) |
   48 keys BzyArray | 56 vals BzyArray | 64 key_kind | 72 val_is_managed.
   key_kind: 0=int family, 1=string, 2=object identity,
             3=record value (key's vtable slot 0 = hashCode, slot 1 = equals).
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

/* Caching the per-slot hash only pays off for keys whose hash is expensive to
   recompute on grow: string content (kind 1, FNV + a pointer chase into the key)
   and record value (kind 3, a user hashCode call). Integer (0) and object
   identity (2) keys hash in a couple of instructions, so caching them would only
   add a per-insert store to a separate cache line - a measured net loss. */
static int hash_is_cached(void *m)
{
	return *M_KKIND(m) == 1 || *M_KKIND(m) == 3;
}

/* When cached, the per-slot full hash lives in the same block as ctrl: cap
   control bytes followed by cap 8-byte hashes. map_grow then re-places every
   entry without recomputing - no re-hash and no pointer chase into each
   scattered key object (the dominant string-key insert cost). Only valid when
   hash_is_cached(m); the hashes region is not allocated otherwise. */
static uint64_t *map_hashes(void *m)
{
	return (uint64_t*)((*M_CTRL(m)) + *M_CAP(m));
}

/* A key slot holds a managed reference (retain/release applies) for every kind
   except the raw integer family. Kept separate from the string-vs-identity
   hashing decision so the two never re-tangle. */
static int key_managed(void *m)
{
	return *M_KKIND(m) != 0;
}

/* Shared-receiver gate. Every public map op funnels through one: a map that
   crossed cores (SHARED gcinfo bit) runs whole-op under its stripe; a confined
   map skips the branch entirely. The user hashCode (record keys) and the insert
   barrier both run BEFORE the stripe is taken - user code under a container
   lock could touch another shared container and, on a stripe collision,
   deadlock. The user equals during a locked probe is the residual exception: a
   user equals that touches the SAME shared map is documented user error (as in
   Java). Displaced/removed occupants are released AFTER the stripe is dropped
   so destructor cascades never run under the lock. */
static int map_is_shared(void *m)
{
	return (*(int64_t*)((char*)m + 16) & (1ll << 3)) != 0;
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

/* Identity hash of an object pointer, NULL-safe. Used by synthesized record
   hashCode for plain-object fields (compared by identity, hashed by address). */
int64_t bzy_ptr_hash(void *p)
{
	return p ? (int64_t)(uint32_t)mix64((uint64_t)p) : 0;
}

static uint64_t hash_bytes(const char *p, int64_t n)
{
	uint64_t h = 0x9e3779b97f4a7c15ULL;
	int64_t i = 0;
	for (; i + 8 <= n; i += 8)        /* Eight bytes per mix instead of one. */
	{
		uint64_t w;
		memcpy(&w, p + i, 8);          /* memcpy: no alignment assumption; GCC emits a mov. */
		h = mix64(h ^ w);
	}

	uint64_t tail = 0;                 /* Remaining 0-7 bytes, low-to-high. */
	for (int64_t j = 0; i + j < n; j++)
	{
		tail |= (uint64_t)(uint8_t)p[i + j] << (j * 8);
	}

	h = mix64(h ^ tail);
	return mix64(h ^ (uint64_t)n);     /* Length-mix preserved (distinguishes trailing zeros). */
}

static uint64_t hash_key(void *m, int64_t key)
{
	/* kind 1 = string content; kind 3 = record value (vtable slot 0 = hashCode);
	   kind 2 (object) falls through to raw pointer identity. */
	if (*M_KKIND(m) == 3)
	{
		void *k = (void*)key;
		int64_t (*hc)(void*) = (int64_t(*)(void*))(*(void***)k)[0];   /* vtable slot 0. */
		return mix64((uint64_t)(uint32_t)hc(k));
	}

	if (*M_KKIND(m) == 1)
	{
		/* String content hash, cached on the string object so a reused key hashes
		   only once. The slot is 0 until computed; if a real hash happens to be 0
		   it is stored as 1 (a one-in-2^64 case that costs only a hair more
		   collisions for that one string, never correctness). */
		void *s = (void*)key;
		void *slot = bzy_str_hashslot(s);
		uint64_t h;
		memcpy(&h, slot, sizeof h);
		if (h == 0)
		{
			h = hash_bytes(bzy_str_data(s), bzy_str_len(s));
			if (h == 0)
			{
				h = 1;
			}

			memcpy(slot, &h, sizeof h);
		}

		return h;
	}

	return mix64((uint64_t)key);
}

static int key_eq(void *m, int64_t a, int64_t b)
{
	/* kind 1 = string content; kind 3 = record value (vtable slot 1 = equals);
	   kind 2 (object) falls through to raw pointer identity. */
	if (*M_KKIND(m) == 3)
	{
		if (a == b)
		{
			return 1;   /* Identity fast path (also covers both-null). */
		}

		void *x = (void*)a;
		int64_t (*eq)(void*, void*) = (int64_t(*)(void*, void*))(*(void***)x)[1];   /* vtable slot 1. */
		return eq(x, (void*)b) != 0;
	}

	if (*M_KKIND(m) == 1)
	{
		if (a == b)
		{
			return 1;   /* Same string object (e.g. a reused key): equal, skip the memcmp. */
		}

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
static int64_t g_map_typeinfo[4] = { 0 /* Finalizer (set on first use). */, 2, 48, 56 };
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
	if (cap == 0)
	{
		return -1;   /* Never-grown map: nothing to find. */
	}

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

/* Returns the displaced value (0 when the key was new or values are unmanaged)
   so callers can release it outside the stripe. */
static int64_t map_put_impl(void *m, int64_t key, int64_t val, uint64_t h)
{
	if ((*M_SIZE(m) + 1) > (*M_CAP(m) * 7) / 8)
	{
		map_grow(m);
	}

	int64_t slot = map_find_insert(m, key, h);
	uint8_t *ctrl = *M_CTRL(m);
	int64_t *keys = keys_data(m), *vals = vals_data(m);
	int existing = (ctrl[slot] != CTRL_EMPTY && ctrl[slot] != CTRL_DELETED);

	if (existing)
	{
		int64_t old = 0;
		if (*M_VMAN(m))
		{
			bzy_retain((void*)val);
			old = vals[slot];
		}

		vals[slot] = val;
		return old;
	}

	if (key_managed(m))
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
	if (hash_is_cached(m))
	{
		map_hashes(m)[slot] = h;
	}

	(*M_SIZE(m))++;
	return 0;
}

void bzy_map_put(void *m, int64_t key, int64_t val)
{
	if (map_is_shared(m))
	{
		/* Insert barrier and the (possibly user) hash both before the stripe. */
		if (key_managed(m))
		{
			bzy_share_crosscore((void*)key);
		}

		if (*M_VMAN(m))
		{
			bzy_share_crosscore((void*)val);
		}

		uint64_t h = hash_key(m, key);
		bzy_shared_lock(m);
		int64_t old = map_put_impl(m, key, val, h);
		bzy_shared_unlock(m);
		bzy_release((void*)old);
		return;
	}

	int64_t old = map_put_impl(m, key, val, hash_key(m, key));
	if (old)
	{
		bzy_release((void*)old);
	}
}

static int64_t map_get_impl(void *m, int64_t key, uint64_t h)
{
	int64_t slot = map_find(m, key, h);
	if (slot < 0)
	{
		return 0;
	}

	int64_t v = vals_data(m)[slot];
	if (*M_VMAN(m))
	{
		bzy_retain((void*)v);   /* Retain inside the stripe on the shared path. */
	}

	return v;
}

int64_t bzy_map_get(void *m, int64_t key)
{
	if (map_is_shared(m))
	{
		uint64_t h = hash_key(m, key);
		bzy_shared_lock(m);
		int64_t v = map_get_impl(m, key, h);
		bzy_shared_unlock(m);
		return v;
	}

	return map_get_impl(m, key, hash_key(m, key));
}

int64_t bzy_map_has(void *m, int64_t key)
{
	if (map_is_shared(m))
	{
		uint64_t h = hash_key(m, key);
		bzy_shared_lock(m);
		int64_t found = map_find(m, key, h) >= 0 ? 1 : 0;
		bzy_shared_unlock(m);
		return found;
	}

	return map_find(m, key, hash_key(m, key)) >= 0 ? 1 : 0;
}

/* Writes the removed key/value (or 0s) to the out-params so callers can release
   them outside the stripe. */
static void map_remove_impl(void *m, int64_t key, uint64_t h, int64_t *out_key, int64_t *out_val)
{
	*out_key = 0;
	*out_val = 0;
	int64_t slot = map_find(m, key, h);
	if (slot < 0)
	{
		return;
	}

	int64_t *keys = keys_data(m), *vals = vals_data(m);
	if (key_managed(m))
	{
		*out_key = keys[slot];
	}

	if (*M_VMAN(m))
	{
		*out_val = vals[slot];
	}

	keys[slot] = 0;
	vals[slot] = 0;
	(*M_CTRL(m))[slot] = CTRL_DELETED;
	(*M_SIZE(m))--;
}

void bzy_map_remove(void *m, int64_t key)
{
	int64_t okey, oval;
	if (map_is_shared(m))
	{
		uint64_t h = hash_key(m, key);
		bzy_shared_lock(m);
		map_remove_impl(m, key, h, &okey, &oval);
		bzy_shared_unlock(m);
	}
	else
	{
		map_remove_impl(m, key, hash_key(m, key), &okey, &oval);
	}

	bzy_release((void*)okey);
	bzy_release((void*)oval);
}

int64_t bzy_map_len(void *m)
{
	if (map_is_shared(m))
	{
		bzy_shared_lock(m);
		int64_t n = *M_SIZE(m);
		bzy_shared_unlock(m);
		return n;
	}

	return *M_SIZE(m);
}

static int64_t map_iter_impl(void *m, int64_t from)
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

int64_t bzy_map_iter(void *m, int64_t from)
{
	if (map_is_shared(m))
	{
		bzy_shared_lock(m);
		int64_t i = map_iter_impl(m, from);
		bzy_shared_unlock(m);
		return i;
	}

	return map_iter_impl(m, from);
}

int64_t bzy_map_key_at(void *m, int64_t slot)
{
	if (map_is_shared(m))
	{
		bzy_shared_lock(m);
		int64_t k = keys_data(m)[slot];
		bzy_shared_unlock(m);
		return k;   /* Borrowed: the map keeps the reference. */
	}

	return keys_data(m)[slot];   /* Borrowed: the map keeps the reference. */
}

int64_t bzy_map_val_at(void *m, int64_t slot)
{
	if (map_is_shared(m))
	{
		bzy_shared_lock(m);
		int64_t v = vals_data(m)[slot];
		bzy_shared_unlock(m);
		return v;   /* Borrowed: the map keeps the reference. */
	}

	return vals_data(m)[slot];   /* Borrowed: the map keeps the reference. */
}

/* val_kind: 3 = string (content equality), anything else = raw 8-byte equality. */
static int64_t map_contains_value_impl(void *m, int64_t needle, int64_t val_kind)
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

int64_t bzy_map_contains_value(void *m, int64_t needle, int64_t val_kind)
{
	if (map_is_shared(m))
	{
		bzy_shared_lock(m);
		int64_t r = map_contains_value_impl(m, needle, val_kind);
		bzy_shared_unlock(m);
		return r;
	}

	return map_contains_value_impl(m, needle, val_kind);
}

/* Write a 64-bit value v into a packed output buffer at byte offset (n * esize),
   truncating to esize bytes. Managed elements are always 8-byte pointers. */
static void pack_write(char *base, int64_t n, int64_t esize, int64_t v)
{
	char *p = base + n * esize;
	switch (esize)
	{
	case 1:
		*(int8_t*)p = (int8_t)v;
		break;
	case 2:
		*(int16_t*)p = (int16_t)v;
		break;
	case 4:
		*(int32_t*)p = (int32_t)v;
		break;
	default:
		*(int64_t*)p = v;
		break;
	}
}

static void *map_keys_impl(void *m, int64_t elem_size)
{
	int64_t managed = key_managed(m) ? 1 : 0;
	void *a = bzy_array_new_sized(*M_SIZE(m), elem_size, managed);   /* Owned (+1). */
	char *out = (char*)a + 32;
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

			pack_write(out, n++, elem_size, keys[i]);
		}
	}

	return a;
}

void *bzy_map_keys(void *m, int64_t elem_size)
{
	if (map_is_shared(m))
	{
		bzy_shared_lock(m);
		void *a = map_keys_impl(m, elem_size);
		bzy_shared_unlock(m);
		return a;
	}

	return map_keys_impl(m, elem_size);
}

static void *map_values_impl(void *m, int64_t elem_size)
{
	int64_t managed = *M_VMAN(m);
	void *a = bzy_array_new_sized(*M_SIZE(m), elem_size, managed);   /* Owned (+1). */
	char *out = (char*)a + 32;
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

			pack_write(out, n++, elem_size, vals[i]);
		}
	}

	return a;
}

void *bzy_map_values(void *m, int64_t elem_size)
{
	if (map_is_shared(m))
	{
		bzy_shared_lock(m);
		void *a = map_values_impl(m, elem_size);
		bzy_shared_unlock(m);
		return a;
	}

	return map_values_impl(m, elem_size);
}

static void *map_entries_impl(void *m)
{
	void *a = bzy_array_new(*M_SIZE(m), 1);   /* Object array (entries are managed). */
	void **out = (void**)((char*)a + 32);
	int64_t cap = *M_CAP(m), *keys = keys_data(m), *vals = vals_data(m);
	uint8_t *ctrl = *M_CTRL(m);
	int64_t kman = (*M_KKIND(m) == 1) ? 1 : 0, vman = *M_VMAN(m), n = 0;
	for (int64_t i = 0; i < cap; i++)
	{
		if ((ctrl[i] & 0x80) == 0)
		{
			out[n++] = bzy_entry_new(keys[i], vals[i], kman, vman);   /* +1, transferred to the array. */
		}
	}

	return a;
}

void *bzy_map_entries(void *m)
{
	if (map_is_shared(m))
	{
		bzy_shared_lock(m);
		void *a = map_entries_impl(m);
		bzy_shared_unlock(m);
		return a;
	}

	return map_entries_impl(m);
}

/* Foreach support. A confined map is iterated in place (identity, retained so
   the caller's release at loop end is uniform). A SHARED map is cloned under
   its stripe and the frozen clone is iterated lock-free: slot cursors survive
   because nothing rehashes the clone, and the contract is per-op (the view may
   be stale by the time the body runs - safe, not snapshot-consistent for the
   original). The clone is born confined; it never escapes the iterating
   breeze. Cached per-slot hashes are reused, so no user hashCode runs under
   the stripe. */
void *bzy_map_iter_snapshot(void *m)
{
	if (!map_is_shared(m))
	{
		bzy_retain(m);
		return m;
	}

	void *c = bzy_map_new(*M_KKIND(m), *M_VMAN(m));   /* Owned (+1). */
	bzy_shared_lock(m);
	int64_t cap = *M_CAP(m);
	if (cap > 0)
	{
		uint8_t *ctrl = *M_CTRL(m);
		int64_t *keys = keys_data(m), *vals = vals_data(m);
		int cached = hash_is_cached(m);
		for (int64_t s = 0; s < cap; s++)
		{
			if ((ctrl[s] & 0x80) == 0)
			{
				uint64_t h = cached ? map_hashes(m)[s] : hash_key(m, keys[s]);
				map_put_impl(c, keys[s], vals[s], h);   /* Retains key and value into the clone. */
			}
		}
	}
	bzy_shared_unlock(m);
	return c;
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
	void *m = bzy_alloc(80);
	*(void**)m = map_vtable();
	/* The backing store (ctrl + keys + vals) is allocated lazily on the first put,
	   so an unused map costs only its header. map_grow handles the cap == 0 case. */
	*M_KKIND(m) = key_kind;
	*M_VMAN(m) = val_is_managed;
	return m;   /* size, cap, ctrl, keys and vals are left zero by bzy_alloc. */
}

static void map_grow(void *m)
{
	int64_t oldcap = *M_CAP(m), newcap = oldcap ? oldcap * 2 : 8;
	int cached = hash_is_cached(m);
	uint8_t *oldctrl = *M_CTRL(m);
	uint64_t *oldhashes = (cached && oldctrl) ? (uint64_t*)(oldctrl + oldcap) : NULL;
	void *oldkeys = *M_KEYS(m), *oldvals = *M_VALS(m);
	int64_t *okeys = oldkeys ? (int64_t*)((char*)oldkeys + 32) : NULL;
	int64_t *ovals = oldvals ? (int64_t*)((char*)oldvals + 32) : NULL;

	/* When caching, one block holds newcap control bytes + newcap 8-byte hashes;
	   otherwise just the control bytes (cheap-hash keys recompute on grow). */
	uint8_t *nctrl = (uint8_t*)malloc((size_t)newcap + (cached ? (size_t)newcap * 8 : 0));
	map_init_ctrl(nctrl, newcap);
	uint64_t *nhashes = cached ? (uint64_t*)(nctrl + newcap) : NULL;
	void *nkeys = bzy_array_new(newcap, key_managed(m) ? 1 : 0);
	void *nvals = bzy_array_new(newcap, *M_VMAN(m));
	int64_t *nk = (int64_t*)((char*)nkeys + 32);
	int64_t *nv = (int64_t*)((char*)nvals + 32);

	/* A shared map's fresh backing arrays inherit the SHARED bit: they are
	   newborn and sole-referenced here, so a plain OR is race-free (and the
	   roots lock must NOT be taken while holding a stripe). */
	if (map_is_shared(m))
	{
		*(int64_t*)((char*)nkeys + 16) |= (1ll << 3);
		*(int64_t*)((char*)nvals + 16) |= (1ll << 3);
	}

	*M_CTRL(m) = nctrl;        /* Publish new buffers before re-probing. */
	*M_KEYS(m) = nkeys;
	*M_VALS(m) = nvals;
	*M_CAP(m) = newcap;

	for (int64_t i = 0; i < oldcap; i++)
	{
		if (oldctrl[i] != CTRL_EMPTY && oldctrl[i] != CTRL_DELETED)
		{
			/* Reuse the cached hash (no recompute, no key pointer-chase); for
			   cheap-hash keys recompute - it costs only a couple of instructions. */
			uint64_t h = cached ? oldhashes[i] : hash_key(m, okeys[i]);
			int64_t s = map_find_insert(m, okeys[i], h);   /* Into the new arrays. */
			nk[s] = okeys[i];
			nv[s] = ovals[i];
			nctrl[s] = (uint8_t)(h & 0x7f);
			if (cached)
			{
				nhashes[s] = h;
			}

			okeys[i] = 0;       /* Transfer ownership: blank the old slot. */
			ovals[i] = 0;
		}
	}

	free(oldctrl);             /* Frees the (combined, when cached) ctrl block. */
	bzy_release(oldkeys);      /* Old element slots are now NULL: frees the block only. */
	bzy_release(oldvals);
}
