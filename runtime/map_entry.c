#include "breezy.h"
#include <stdint.h>

/* A map Entry: a 2-field managed object holding one key/value pair.
   Layout (object_size = 56): 0 vtable | 8 rc | 16 gcinfo | 24 key | 32 value |
   40 key_managed | 48 val_managed.

   Typeinfo: [finalizer][num_obj_fields][off0...]. The four variants enumerate
   which of {key@24, value@32} are managed object fields, so free_object and the
   cycle collector reach exactly the managed children (finalizer 0 -- nothing to
   free beyond the children). The vtable points one slot past the typeinfo, so
   the descriptor sits at vtable - 8, matching the array/map convention. */
static int64_t ti_00[2] = { 0, 0 };
static int64_t ti_k0[3] = { 0, 1, 24 };
static int64_t ti_0v[3] = { 0, 1, 32 };
static int64_t ti_kv[4] = { 0, 2, 24, 32 };
static int64_t vt_00[2], vt_k0[2], vt_0v[2], vt_kv[2];
static int g_inited;

static void entry_init(void)
{
	vt_00[0] = (int64_t)&ti_00[0];
	vt_k0[0] = (int64_t)&ti_k0[0];
	vt_0v[0] = (int64_t)&ti_0v[0];
	vt_kv[0] = (int64_t)&ti_kv[0];
	g_inited = 1;
}

void *bzy_entry_new(int64_t key, int64_t val, int64_t key_managed, int64_t val_managed)
{
	if (!g_inited)
	{
		entry_init();
	}

	void *e = bzy_alloc(56);                 /* Owned (+1); fields zeroed. */
	int64_t *vt = key_managed
				  ? (val_managed ? vt_kv : vt_k0)
				  : (val_managed ? vt_0v : vt_00);
	*(void**)e = &vt[1];                      /* vtable points past the typeinfo slot. */
	if (key_managed)
	{
		bzy_retain((void*)key);
	}

	if (val_managed)
	{
		bzy_retain((void*)val);
	}

	*(int64_t*)((char*)e + 24) = key;
	*(int64_t*)((char*)e + 32) = val;
	*(int64_t*)((char*)e + 40) = key_managed;
	*(int64_t*)((char*)e + 48) = val_managed;
	return e;
}

int64_t bzy_entry_key(void *e)
{
	int64_t v = *(int64_t*)((char*)e + 24);
	if (*(int64_t*)((char*)e + 40))
	{
		bzy_retain((void*)v);                 /* Return owned, like map.get. */
	}

	return v;
}

int64_t bzy_entry_val(void *e)
{
	int64_t v = *(int64_t*)((char*)e + 32);
	if (*(int64_t*)((char*)e + 48))
	{
		bzy_retain((void*)v);
	}

	return v;
}
