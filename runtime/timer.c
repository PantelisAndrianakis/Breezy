#include "breezy.h"
#include <stdint.h>
#include <stdlib.h>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

/* Timer object (object_size = 56), same managed-leaf idiom as channel:
   0 vtable | 8 rc | 16 gcinfo | 24 deadline | 32 period | 40 entry | 48 cancelled.
   period 0 => one-shot. Zero traceable children, so the cycle collector treats it
   as a leaf and ARC frees it directly. */
#define T_DEADLINE(t)  (*(int64_t*)((char*)(t) + 24))
#define T_PERIOD(t)    (*(int64_t*)((char*)(t) + 32))
#define T_ENTRY(t)     (*(void(**)(void))((char*)(t) + 40))
#define T_CANCELLED(t) (*(int64_t*)((char*)(t) + 48))

/* No finalizer, zero object children (leaf). */
static int64_t g_timer_typeinfo[2] = { 0, 0 };
static int64_t g_timer_vtable[2];

static void *timer_vtable(void)
{
	g_timer_vtable[0] = (int64_t)&g_timer_typeinfo[0];
	return &g_timer_vtable[1];
}

/* The shared min-heap (binary heap over deadlines), guarded by one lock. */
static void  **g_heap;
static int64_t g_n;
static int64_t g_cap;
static SRWLOCK g_lock = SRWLOCK_INIT;

static void heap_swap(int64_t i, int64_t j)
{
	void *tmp = g_heap[i];
	g_heap[i] = g_heap[j];
	g_heap[j] = tmp;
}

static void sift_up(int64_t i)
{
	while (i > 0)
	{
		int64_t parent = (i - 1) / 2;
		if (T_DEADLINE(g_heap[parent]) <= T_DEADLINE(g_heap[i]))
		{
			break;
		}

		heap_swap(parent, i);
		i = parent;
	}
}

static void sift_down(int64_t i)
{
	for (;;)
	{
		int64_t l = 2 * i + 1, r = 2 * i + 2, small = i;
		if (l < g_n && T_DEADLINE(g_heap[l]) < T_DEADLINE(g_heap[small]))
		{
			small = l;
		}

		if (r < g_n && T_DEADLINE(g_heap[r]) < T_DEADLINE(g_heap[small]))
		{
			small = r;
		}

		if (small == i)
		{
			break;
		}

		heap_swap(i, small);
		i = small;
	}
}

/* Caller holds g_lock. */
static void heap_push(void *t)
{
	if (g_n == g_cap)
	{
		g_cap = g_cap ? g_cap * 2 : 16;
		g_heap = realloc(g_heap, (size_t)g_cap * sizeof(void*));
	}

	g_heap[g_n] = t;
	sift_up(g_n);
	g_n++;
}

/* Remove the root. Caller holds g_lock and g_n > 0. */
static void heap_pop_root(void)
{
	g_n--;
	if (g_n > 0)
	{
		g_heap[0] = g_heap[g_n];
		sift_down(0);
	}
}

void *bzy_timer_schedule(void (*entry)(void), int64_t first_deadline, int64_t period)
{
	void *t = bzy_alloc(56);
	*(void**)t = timer_vtable();
	T_DEADLINE(t) = first_deadline;
	T_PERIOD(t) = period;
	T_ENTRY(t) = entry;
	T_CANCELLED(t) = 0;

	bzy_retain(t);                 /* The heap's own reference (caller keeps the alloc +1). */
	AcquireSRWLockExclusive(&g_lock);
	heap_push(t);
	ReleaseSRWLockExclusive(&g_lock);

	bzy_sched_nudge();             /* A worker may be idle-parked; let it re-arm its timer wait. */
	return t;                      /* Owned (+1) to the caller. */
}

void *bzy_timer_after(void (*entry)(void), int64_t delay_ms)
{
	if (delay_ms < 0)
	{
		delay_ms = 0;              /* Clamp (validateDelay analog): never schedule into the past as negative. */
	}

	return bzy_timer_schedule(entry, bzy_clock_millis() + delay_ms, 0);
}

void *bzy_timer_every(void (*entry)(void), int64_t delay_ms, int64_t period_ms)
{
	if (delay_ms < 0)
	{
		delay_ms = 0;
	}

	if (period_ms < 1)
	{
		period_ms = 1;             /* A non-positive period would spin; floor at 1ms. */
	}

	return bzy_timer_schedule(entry, bzy_clock_millis() + delay_ms, period_ms);
}

void bzy_timer_cancel(void *t)
{
	if (!t)
	{
		return;
	}

	AcquireSRWLockExclusive(&g_lock);
	T_CANCELLED(t) = 1;            /* Lazy: the heap evicts cancelled entries when it reaches them. */
	ReleaseSRWLockExclusive(&g_lock);
}

/* Drop cancelled entries sitting at the root. Caller holds g_lock. */
static void drop_cancelled_root(void)
{
	while (g_n > 0 && T_CANCELLED(g_heap[0]))
	{
		void *root = g_heap[0];
		heap_pop_root();
		bzy_release(root);        /* Heap's reference. */
	}
}

int64_t bzy_timer_next_deadline(void)
{
	AcquireSRWLockExclusive(&g_lock);
	drop_cancelled_root();
	int64_t d = (g_n > 0) ? T_DEADLINE(g_heap[0]) : -1;
	ReleaseSRWLockExclusive(&g_lock);
	return d;
}

void *bzy_timer_pop_due(int64_t now)
{
	AcquireSRWLockExclusive(&g_lock);
	drop_cancelled_root();
	if (g_n == 0 || T_DEADLINE(g_heap[0]) > now)
	{
		ReleaseSRWLockExclusive(&g_lock);
		return NULL;
	}

	void *root = g_heap[0];
	heap_pop_root();
	ReleaseSRWLockExclusive(&g_lock);
	return root;                  /* Heap ref transferred to the caller (the scheduler). */
}

void bzy_timer_reinsert(void *t, int64_t now)
{
	int64_t d = T_DEADLINE(t) + T_PERIOD(t);
	while (d <= now)
	{
		d += T_PERIOD(t);         /* Fixed-rate, coalesce: jump to the next future slot in one step. */
	}

	T_DEADLINE(t) = d;
	AcquireSRWLockExclusive(&g_lock);
	heap_push(t);                 /* The caller's transferred ref becomes the heap's ref again. */
	ReleaseSRWLockExclusive(&g_lock);
}

int64_t bzy_timer_count(void)
{
	AcquireSRWLockExclusive(&g_lock);
	int64_t n = g_n;
	ReleaseSRWLockExclusive(&g_lock);
	return n;
}

void bzy_timer_reset(void)
{
	AcquireSRWLockExclusive(&g_lock);
	for (int64_t i = 0; i < g_n; i++)
	{
		bzy_release(g_heap[i]);   /* Drop every heap reference. */
	}

	g_n = 0;
	ReleaseSRWLockExclusive(&g_lock);
}
