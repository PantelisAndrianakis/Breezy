#include "breezy.h"
#include <stdlib.h>

static int64_t g_live = 0;

/* Cycle-collector colors, stored in the low two bits of gcinfo at offset 16. */
enum { BLACK = 0, GRAY = 1, WHITE = 2, PURPLE = 3 };

static int64_t *RC(void *o)
{
	return (int64_t*)((char*)o + 8);
}

static int64_t *GI(void *o)
{
	return (int64_t*)((char*)o + 16);
}

static int color_of(void *o)
{
	return (int)(*GI(o) & 3);
}

static void set_color(void *o, int c)
{
	*GI(o) = (*GI(o) & ~(int64_t)3) | (c & 3);
}

static int buffered(void *o)
{
	return (int)((*GI(o) >> 2) & 1);
}

static void set_buffered(void *o, int b)
{
	*GI(o) = (*GI(o) & ~(int64_t)4) | ((b ? 1 : 0) << 2);
}

static int64_t crc_of(void *o)
{
	return *GI(o) >> 8;
}

static void set_crc(void *o, int64_t v)
{
	*GI(o) = (*GI(o) & 0xFF) | (v << 8);
}

/* The type descriptor sits one word before the vtable; it is NULL only for
   objects whose vtable has not been set (some hand-built test fixtures). */
static int64_t *descriptor_of(void *o)
{
	void **vtable = *(void***)o;
	if (!vtable)
	{
		return NULL;
	}

	return ((int64_t**)vtable)[-1];
}

static int has_object_children(void *o)
{
	int64_t *ti = descriptor_of(o);
	if (!ti)
	{
		return 0;
	}

	if (ti[1] == -1)
	{
		return *(int64_t*)((char*)o + 24) > 0;   /* Array span: any elements. */
	}

	return ti[1] > 0;
}

/* The class finalizer (descriptor word 0, at vtable - 16), or NULL when the
   class declares none. Runs before the object's memory is freed. */
static void (*finalizer_of(void *o))(void *)
{
	int64_t *d = descriptor_of(o);
	return d ? (void(*)(void*))d[0] : NULL;
}

/* Visit each managed child pointer of obj. Handles both the fixed offset list
   (desc[1] = count, offsets at desc[2+i]) and the array span (desc[1] = -1,
   first-element offset desc[2], stride desc[3], count = obj length @ offset 24).
   Children with refcount 0 (unmanaged/stack) are skipped. */
#define FOR_EACH_CHILD(obj, child, body)                                       \
	do {                                                                       \
		int64_t *_ti = descriptor_of(obj);                                     \
		if (_ti)                                                               \
		{                                                                      \
			int _span = (_ti[1] == -1);                                        \
			int64_t _n = _span ? *(int64_t*)((char*)(obj) + 24) : _ti[1];      \
			for (int64_t _i = 0; _i < _n; _i++)                                \
			{                                                                  \
				int64_t _o = _span ? (_ti[2] + _i*_ti[3]) : _ti[2+_i];         \
				void *child = *(void**)((char*)(obj) + _o);                    \
				if (child && *RC(child) != 0)                                  \
				{                                                              \
					body;                                                      \
				}                                                              \
			}                                                                  \
		}                                                                      \
	} while (0)

/* The roots buffer holds objects whose refcount was decremented to a positive
   value and which could therefore be the root of a dead cycle. It is allocated
   lazily and grows geometrically, so a program that never buffers a candidate
   pays nothing and one that buffers many is never silently truncated. */
static void   **g_roots = NULL;
static int64_t g_roots_n = 0;
static int64_t g_roots_cap = 0;

static void roots_push(void *o)
{
	if (g_roots_n == g_roots_cap)
	{
		int64_t ncap = g_roots_cap ? g_roots_cap * 2 : 1024;
		void **nb = realloc(g_roots, (size_t)ncap * sizeof(void*));
		if (!nb)
		{
			return;   /* Out of memory: drop this candidate rather than abort. */
		}

		g_roots = nb;
		g_roots_cap = ncap;
	}

	g_roots[g_roots_n++] = o;
}

void *bzy_alloc(int64_t size)
{
	void *o = calloc(1, (size_t)size);
	if (!o)
	{
		abort();
	}

	*RC(o) = 1;        /* The refcount starts at one. */
	*GI(o) = 0;        /* BLACK, not buffered, crc zero. */
	__atomic_add_fetch(&g_live, 1, __ATOMIC_RELAXED);   /* Objects alloc on any worker thread. */
	return o;
}

void bzy_retain(void *obj)
{
	if (!obj)
	{
		return;
	}

	if (*GI(obj) & BZY_GCINFO_SHARED)
	{
		/* Cross-core object: atomic increment, and no cycle color (shared objects
		   are not cycle-buffered; the bit is fixed at allocation, never racing). */
		__atomic_add_fetch(RC(obj), 1, __ATOMIC_RELAXED);
		return;
	}

	int64_t *rc = RC(obj);
	if (*rc == 0)
	{
		return;       /* Unmanaged (stack) object: the refcount stays zero. */
	}

	(*rc)++;
	set_color(obj, BLACK);
}

/* Release the object's owned fields and free its memory. Called when an object
   is known to be acyclic garbage (refcount reached zero). */
static void free_object(void *obj)
{
	void (*fin)(void*) = finalizer_of(obj);
	if (fin)
	{
		fin(obj);
	}

	FOR_EACH_CHILD(obj, child,
	{
		bzy_release(child);
	});
	set_color(obj, BLACK);
	if (buffered(obj))
	{
		/* The roots buffer still points at this node, so freeing its memory now
		   would leave a dangling candidate for the cycle collector to dereference.
		   Leave it BLACK with a zero refcount; bzy_collect_cycles reclaims it. */
		return;
	}

	__atomic_sub_fetch(&g_live, 1, __ATOMIC_RELAXED);
	free(obj);
}

void bzy_release(void *obj)
{
	if (!obj)
	{
		return;
	}

	if (*GI(obj) & BZY_GCINFO_SHARED)
	{
		/* Cross-core object: atomic decrement. At zero no other breeze can hold a
		   reference, so the ordinary free path is safe; shared objects are never
		   cycle-buffered, so their children (also shared) just recurse here. */
		if (__atomic_sub_fetch(RC(obj), 1, __ATOMIC_ACQ_REL) == 0)
		{
			free_object(obj);
		}

		return;
	}

	int64_t *rc = RC(obj);
	if (*rc == 0)
	{
		return;       /* Unmanaged (stack) object: never freed. */
	}

	(*rc)--;
	if (*rc == 0)
	{
		free_object(obj);
		return;
	}

	/* The object survives this release. If it can reference other objects it may
	   be the root of a dead cycle, so buffer it as a candidate. */
	if (has_object_children(obj))
	{
		set_color(obj, PURPLE);
		if (!buffered(obj))
		{
			set_buffered(obj, 1);
			roots_push(obj);

			/* Reclaim opportunistically once enough candidates accumulate, so
			   long-running programs bound cycle garbage without explicit calls. */
			if (g_roots_n >= 10000)
			{
				bzy_collect_cycles();
			}
		}
	}
}

int64_t bzy_live_count(void)
{
	return __atomic_load_n(&g_live, __ATOMIC_RELAXED);
}

int64_t bzy_roots_buffered(void)
{
	return g_roots_n;
}

/* Trial deletion (Bacon-Rajan), operating on the per-object crc scratch so the
   real refcount is never disturbed: a heap object always keeps refcount >= 1
   while reachable, so a child with refcount 0 is unambiguously unmanaged (a
   stack object) and is skipped everywhere. Each candidate subgraph is grayed
   while trial-decrementing crc across internal edges; a node whose crc stays
   positive is reachable from outside the set, so scan_black recolors it and its
   subgraph BLACK; the rest are WHITE garbage. White nodes are gathered into a
   list and freed only after the whole walk, so no traversal ever dereferences
   freed memory. */

static void mark_gray(void *s)
{
	if (color_of(s) == GRAY)
	{
		return;
	}

	set_color(s, GRAY);
	set_crc(s, *RC(s));
	FOR_EACH_CHILD(s, t,
	{
		if (color_of(t) != GRAY)
		{
			mark_gray(t);
		}

		set_crc(t, crc_of(t) - 1);
	});
}

static void scan_black(void *s)
{
	set_color(s, BLACK);
	FOR_EACH_CHILD(s, t,
	{
		if (color_of(t) != BLACK)
		{
			scan_black(t);
		}
	});
}

static void scan(void *s)
{
	if (color_of(s) != GRAY)
	{
		return;
	}

	if (crc_of(s) > 0)
	{
		scan_black(s);
		return;
	}

	set_color(s, WHITE);
	FOR_EACH_CHILD(s, t,
	{
		scan(t);
	});
}

/* Gather the white subgraph into g_white without freeing, recoloring to BLACK so
   each node is collected exactly once and cycles terminate. Freeing is deferred
   to bzy_collect_cycles so no walk dereferences a freed node. */
static void   **g_white = NULL;
static int64_t g_white_n = 0;
static int64_t g_white_cap = 0;

static void white_push(void *s)
{
	if (g_white_n == g_white_cap)
	{
		int64_t ncap = g_white_cap ? g_white_cap * 2 : 1024;
		void **nb = realloc(g_white, (size_t)ncap * sizeof(void*));
		if (!nb)
		{
			return;   /* Out of memory: drop this node from the gather pass. */
		}

		g_white = nb;
		g_white_cap = ncap;
	}

	g_white[g_white_n++] = s;
}

static void gather_white(void *s)
{
	if (color_of(s) != WHITE || buffered(s))
	{
		return;
	}

	set_color(s, BLACK);
	white_push(s);
	FOR_EACH_CHILD(s, t,
	{
		gather_white(t);
	});
}

void bzy_collect_cycles(void)
{
	/* Mark: gray every still-PURPLE candidate; drop the stale ones. */
	int64_t kept = 0;
	for (int64_t i = 0; i < g_roots_n; i++)
	{
		void *s = g_roots[i];
		if (color_of(s) == PURPLE)
		{
			mark_gray(s);
			g_roots[kept++] = s;
		}
		else
		{
			set_buffered(s, 0);
			if (*RC(s) == 0)
			{
				/* A node freed while still buffered (deferred by free_object):
				   reclaim its memory now that it leaves the roots buffer. */
				__atomic_sub_fetch(&g_live, 1, __ATOMIC_RELAXED);
				free(s);
			}
		}
	}

	g_roots_n = kept;

	/* Scan: restore externally-reachable subgraphs, whiten the dead ones. */
	for (int64_t i = 0; i < g_roots_n; i++)
	{
		scan(g_roots[i]);
	}

	/* Clear buffered flags so the white subgraph can be gathered, then gather. */
	for (int64_t i = 0; i < g_roots_n; i++)
	{
		set_buffered(g_roots[i], 0);
	}

	g_white_n = 0;
	for (int64_t i = 0; i < g_roots_n; i++)
	{
		gather_white(g_roots[i]);
	}

	g_roots_n = 0;

	/* Free the gathered garbage in a flat pass: no traversal touches it now.
	   Each node's finalizer runs just before its memory is reclaimed. */
	for (int64_t i = 0; i < g_white_n; i++)
	{
		void (*fin)(void*) = finalizer_of(g_white[i]);
		if (fin)
		{
			fin(g_white[i]);
		}

		__atomic_sub_fetch(&g_live, 1, __ATOMIC_RELAXED);
		free(g_white[i]);
	}

	g_white_n = 0;
}
