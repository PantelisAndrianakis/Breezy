#include "breezy.h"
#include <stdlib.h>

static int64_t g_live = 0;

void *bzy_alloc(int64_t size)
{
	void *o = calloc(1, (size_t)size);
	if (!o)
	{
		abort();
	}

	*(int64_t*)((char*)o + 8) = 1;     /* The refcount starts at one. */
	g_live++;
	return o;
}

void bzy_retain(void *obj)
{
	if (!obj)
	{
		return;
	}

	(*(int64_t*)((char*)obj + 8))++;
}

void bzy_release(void *obj)
{
	if (!obj)
	{
		return;
	}

	int64_t *rc = (int64_t*)((char*)obj + 8);
	if (--(*rc) != 0)
	{
		return;
	}

	void **vtable = *(void***)obj;            /* Header word zero is the vtable pointer. */
	if (vtable)
	{
		int64_t *ti = ((int64_t**)vtable)[-1];   /* The type descriptor lives at vtable minus eight. */
		int64_t n = ti[0];
		for (int64_t i = 0; i < n; i++)
		{
			void *child = *(void**)((char*)obj + ti[1 + i]);
			bzy_release(child);
		}
	}

	g_live--;
	free(obj);
}

int64_t bzy_live_count(void)
{
	return g_live;
}
