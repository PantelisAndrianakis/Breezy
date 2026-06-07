#include "breezy.h"
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>   /* TlsAlloc / Interlocked* for the TEB-slot allocator TLS. */
#include <intrin.h>    /* __readgsqword / __writegsqword. */
#endif

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

/* ---- Small-object size-class free lists (per worker thread) -----------------
   Every Breezy object is its own heap block; under churn the calloc/free
   round-trip dominates allocation cost. Each thread keeps one free list per size
   class and recycles blocks locally: a free pushes onto the running thread's list
   and an allocation pops from it, so no list is ever shared between threads. This
   is lock-free and correct even as breezes migrate across workers - a block
   simply moves from one thread's pool to another's, and each list is only ever
   touched by its owner.

   The size class (1..POOL_NCLASS-1; 0 means "too big to pool") is recorded in
   gcinfo bits 4-7, which the color/buffered/shared/crc machinery never touches.
   Recycled blocks are re-zeroed to preserve calloc semantics, and per-class depth
   is capped so a producer/consumer split cannot grow a pool without bound. Pooled
   blocks are returned to the OS only implicitly at process exit.

   Both arrays live in ONE __thread struct so the hot path pays a single TLS
   resolution per call - on the MinGW target __thread is emulated (a call to
   __emutls_get_address), so minimizing distinct TLS accesses is what makes the
   pool a net win rather than a loss. */

#define POOL_MAX_SIZE 256
#define POOL_NCLASS   11        /* Index 0 = unpooled; 1..10 are real classes. */
#define POOL_CAP      256       /* Max recycled blocks held per class, per thread. */

static const int g_class_size[POOL_NCLASS] =
{
	0, 32, 48, 64, 80, 96, 128, 160, 192, 224, 256
};

/* Per-thread allocator state. `live` is this thread's share of the global
   live-object count: bzy_alloc increments the running thread's `live` and a free
   decrements the freeing thread's `live`, so the hot path never writes a shared
   cache line (the old single global g_live counter serialized every concurrent
   alloc/free - its line ping-ponged across cores). The true count is the sum of
   every thread's shard, computed only when bzy_live_count() is called; a block
   allocated on one thread and freed on another leaves the two shards unbalanced
   but their sum exact. Folding `live` into PoolTLS means the whole hot path still
   costs a single TLS resolution. */
typedef struct
{
	void    *head[POOL_NCLASS];   /* Free-list head per class (next link @ block offset 0). */
	int32_t  n[POOL_NCLASS];      /* Current depth per class. */
	int64_t  live;                /* This thread's contribution to the live-object count. */
	int      registered;          /* 1 once this shard is linked into g_shards. */
} PoolTLS;

#ifndef _WIN32
static __thread PoolTLS t_pool;   /* Linux/ELF: native fs:-relative TLS - one mov per resolution. */
#endif

/* Registry of every thread's shard, so bzy_live_count can sum them. A thread
   registers its shard once, on its first alloc or free, via a lock-free append.
   MAX_SHARDS comfortably exceeds the 64-worker cap plus the main/offload/IOCP
   threads; a thread beyond it simply is not summed (its objects are rare). */
#define MAX_SHARDS 256
static PoolTLS *g_shards[MAX_SHARDS];
static int      g_nshards;   /* Appended via __atomic; read back to bound the sum. */

/* Link a freshly created shard into the registry so bzy_live_count can sum it. */
static void register_shard(PoolTLS *p)
{
	p->registered = 1;
	int i = __atomic_fetch_add(&g_nshards, 1, __ATOMIC_SEQ_CST);
	if (i < MAX_SHARDS)
	{
		g_shards[i] = p;
	}
}

#ifdef _WIN32
/* The MinGW target emulates `__thread` as a call to __emutls_get_address on every
   resolution - and pool_tls() is on the hottest path in the runtime. Instead, hold
   the per-thread shard pointer in a reserved TEB TLS slot and read it with a single
   gs:-relative load (no call), the same shape as the native fs:-relative access the
   ELF build gets for free. TlsAlloc is taken once, on the first allocation (very
   early, so the index lands inside the inline TlsSlots[64] array); a TlsGetValue
   fallback covers the unlikely overflow case. */
#define TEB_TLS_SLOTS 0x1480   /* Offset of TlsSlots[64] in the x64 TEB. */

static volatile LONG g_slot_claim;   /* CAS gate: elects one thread to run TlsAlloc. */
static volatile LONG g_slot_ready;   /* 1 once g_pool_slot is valid. */
static DWORD         g_pool_slot;

static PoolTLS *pool_tls(void)
{
	if (!g_slot_ready)
	{
		if (InterlockedCompareExchange(&g_slot_claim, 1, 0) == 0)
		{
			g_pool_slot = TlsAlloc();
			InterlockedExchange(&g_slot_ready, 1);
		}
		else
		{
			while (!g_slot_ready)   /* Another thread is mid-TlsAlloc; brief startup-only spin. */
			{
				YieldProcessor();
			}
		}
	}

	DWORD slot = g_pool_slot;
	PoolTLS *p;
	if (slot < 64)
	{
		unsigned off = TEB_TLS_SLOTS + slot * 8u;
		p = (PoolTLS*)__readgsqword(off);
		if (!p)
		{
			p = (PoolTLS*)calloc(1, sizeof(PoolTLS));
			__writegsqword(off, (DWORD64)(uintptr_t)p);
			register_shard(p);
		}
	}
	else
	{
		p = (PoolTLS*)TlsGetValue(slot);   /* Index beyond the inline array: correct, slower path. */
		if (!p)
		{
			p = (PoolTLS*)calloc(1, sizeof(PoolTLS));
			TlsSetValue(slot, p);
			register_shard(p);
		}
	}

	return p;
}
#else
static PoolTLS *pool_tls(void)
{
	PoolTLS *p = &t_pool;   /* One fs:-relative resolution per call. */
	if (!p->registered)
	{
		register_shard(p);
	}

	return p;
}
#endif

/* Smallest class whose block fits size, or 0 when size exceeds the pooled range. */
static int class_index(int64_t size)
{
	if (size > POOL_MAX_SIZE)
	{
		return 0;
	}

	for (int c = 1; c < POOL_NCLASS; c++)
	{
		if (size <= g_class_size[c])
		{
			return c;
		}
	}

	return 0;
}

/* Account one freed object against the running thread's live shard, then return
   its block to that thread's size-class pool - or to the system allocator when it
   is too big to pool or the pool is full. The caller has already run the
   finalizer / released children. */
static void pool_free(void *o)
{
	PoolTLS *p = pool_tls();   /* One TLS resolution for the decrement and the push. */
	p->live--;

	int c = (int)((*GI(o) >> 4) & 0xF);
	if (c && c < POOL_NCLASS && p->n[c] < POOL_CAP)
	{
		*(void**)o = p->head[c];
		p->head[c] = o;
		p->n[c]++;
		return;
	}

	free(o);
}

void *bzy_alloc(int64_t size)
{
	PoolTLS *p = pool_tls();   /* One TLS resolution for the count and the pop. */
	p->live++;

	int c = class_index(size);
	void *o;
	if (c)
	{
		o = p->head[c];
		if (o)
		{
			p->head[c] = *(void**)o;
			p->n[c]--;
		}
		else
		{
			o = malloc((size_t)g_class_size[c]);
			if (!o)
			{
				abort();
			}
		}

		memset(o, 0, (size_t)g_class_size[c]);   /* Match calloc's zero-fill, on reuse and on fresh blocks. */
	}
	else
	{
		o = calloc(1, (size_t)size);
		if (!o)
		{
			abort();
		}
	}

	*RC(o) = 1;                 /* The refcount starts at one. */
	*GI(o) = (int64_t)c << 4;   /* Class nibble in bits 4-7; color BLACK, not buffered, not shared, crc zero. */
	return o;
}

/* Cross-core publication: an object handed to another worker thread (a channel
   value, a spawn argument) needs an atomic refcount, or its non-atomic count
   races across cores and is freed early (use-after-free). The SHARED bit is
   normally fixed at allocation; promoting it here is safe ONLY for a leaf (no
   managed children): a leaf is never cycle-buffered and never visited by the
   collector, and at the first cross-core handoff the object is still confined to
   this thread (an unshared object is single-core by invariant), so this thread is
   its only accessor and the plain OR cannot race. Objects WITH managed children
   can be cycle-buffered, and the roots buffer (g_roots) is shared across threads,
   so promoting them here could race a collector running on another thread; those
   are covered at compile time when their class is a channel element type (see
   types_compute_shared_set). The remaining cases -- builtin containers of objects
   crossing a channel, and spawn args that are objects with object fields -- are a
   documented gap (bench/REPORT.md P7). Idempotent and NULL/unmanaged-safe. */
void bzy_share_crosscore(void *o)
{
	if (!o || *RC(o) == 0)
	{
		return;   /* NULL or an unmanaged (stack) value: nothing to share. */
	}

	if (*GI(o) & BZY_GCINFO_SHARED)
	{
		return;   /* Already shared (idempotent). */
	}

	if (has_object_children(o))
	{
		return;   /* Not a leaf: promotion here would race the cycle collector. */
	}

	*GI(o) |= BZY_GCINFO_SHARED;
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

	pool_free(obj);   /* Decrements the freeing thread's live shard. */
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
	/* Sum every registered thread's shard. Per-shard reads are unsynchronized -
	   a concurrent alloc/free may make the total off by a few in flight - but each
	   aligned int64 read is tear-free, and with a single mutator (the usual leak
	   check at a quiescent point) it is exact. */
	int64_t n = 0;
	int k = __atomic_load_n(&g_nshards, __ATOMIC_SEQ_CST);
	if (k > MAX_SHARDS)
	{
		k = MAX_SHARDS;
	}

	for (int i = 0; i < k; i++)
	{
		n += g_shards[i]->live;
	}

	return n;
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
				pool_free(s);   /* Decrements this thread's live shard. */
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

		pool_free(g_white[i]);   /* Decrements this thread's live shard. */
	}

	g_white_n = 0;
}
