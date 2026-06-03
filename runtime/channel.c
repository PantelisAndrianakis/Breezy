#include "breezy.h"
#include <stdint.h>
#include <stdlib.h>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

/* Bounded buffered channel (object_size = 104):
   0 vtable | 8 rc | 16 gcinfo | 24 cap | 32 count | 40 head | 48 ring(int64_t*) |
   56 elem_managed | 64 send_head | 72 send_tail | 80 recv_head | 88 recv_tail |
   96 lock(SRWLOCK).
   The ring is a raw malloc'd buffer (not a managed child), so the finalizer
   frees it and releases any buffered managed elements; the cycle collector
   treats the channel as a leaf (num_obj_fields = 0).

   The SRWLOCK guards the ring + both waiter lists so a channel is safe to share
   across worker threads (6a-3). A parked breeze releases the lock through the
   scheduler (bzy_sched_park_unlock) so a waker on another core cannot observe it
   as a waiter until it is fully switched out, and every wake happens after the
   lock is dropped (channel lock -> queue lock is the only nesting order).

   Waiters are stack-local nodes on the parked breeze's coroutine stack (stable
   while parked); the counterpart unlinks and reads one, then wakes its breeze.
   No malloc/free for waiters. */

typedef struct CWaiter
{
	void *breeze;          /* The parked breeze (a bzy_sched handle). */
	int64_t val;           /* A parked sender's pending value. */
	int64_t *deliver;      /* A parked receiver's result slot. */
	struct CWaiter *next;
} CWaiter;

static int64_t  *C_CAP(void *c)
{
	return (int64_t*)((char*)c + 24);
}
static int64_t  *C_COUNT(void *c)
{
	return (int64_t*)((char*)c + 32);
}
static int64_t  *C_HEAD(void *c)
{
	return (int64_t*)((char*)c + 40);
}
static int64_t **C_RING(void *c)
{
	return (int64_t**)((char*)c + 48);
}
static int64_t  *C_EMAN(void *c)
{
	return (int64_t*)((char*)c + 56);
}
static CWaiter **C_SHEAD(void *c)
{
	return (CWaiter**)((char*)c + 64);
}
static CWaiter **C_STAIL(void *c)
{
	return (CWaiter**)((char*)c + 72);
}
static CWaiter **C_RHEAD(void *c)
{
	return (CWaiter**)((char*)c + 80);
}
static CWaiter **C_RTAIL(void *c)
{
	return (CWaiter**)((char*)c + 88);
}
static SRWLOCK  *C_LOCK(void *c)
{
	return (SRWLOCK*)((char*)c + 96);
}

static void wq_push(CWaiter **head, CWaiter **tail, CWaiter *w)
{
	w->next = NULL;
	if (*tail)
	{
		(*tail)->next = w;
	}
	else
	{
		*head = w;
	}

	*tail = w;
}

static CWaiter *wq_pop(CWaiter **head, CWaiter **tail)
{
	CWaiter *w = *head;
	if (w)
	{
		*head = w->next;
		if (!*head)
		{
			*tail = NULL;
		}
	}

	return w;
}

static void ring_push(void *c, int64_t v)
{
	int64_t cap = *C_CAP(c);
	int64_t *ring = *C_RING(c);
	ring[(*C_HEAD(c) + *C_COUNT(c)) % cap] = v;
	(*C_COUNT(c))++;
}

static int64_t ring_pop(void *c)
{
	int64_t cap = *C_CAP(c);
	int64_t *ring = *C_RING(c);
	int64_t v = ring[*C_HEAD(c)];
	*C_HEAD(c) = (*C_HEAD(c) + 1) % cap;
	(*C_COUNT(c))--;
	return v;
}

static void bzy_channel_finalize(void *c)
{
	if (*C_EMAN(c))
	{
		int64_t cap = *C_CAP(c), head = *C_HEAD(c), n = *C_COUNT(c), *ring = *C_RING(c);
		for (int64_t i = 0; i < n; i++)
		{
			bzy_release((void*)ring[(head + i) % cap]);   /* Release elements still buffered. */
		}
	}

	free(*C_RING(c));
}

/* Finalizer + zero traceable object children. */
static int64_t g_channel_typeinfo[2] = { 0 /* Finalizer (set on first use). */, 0 };
static int64_t g_channel_vtable[2];

static void *channel_vtable(void)
{
	g_channel_typeinfo[0] = (int64_t)(void*)bzy_channel_finalize;
	g_channel_vtable[0] = (int64_t)&g_channel_typeinfo[0];
	return &g_channel_vtable[1];
}

void *bzy_channel_new(int64_t cap, int64_t elem_managed)
{
	if (cap < 1)
	{
		cap = 1;   /* V1 requires a buffer of at least one slot. */
	}

	void *c = bzy_alloc(104);
	*(void**)c = channel_vtable();
	*C_CAP(c) = cap;
	*C_COUNT(c) = 0;
	*C_HEAD(c) = 0;
	*C_RING(c) = (int64_t*)malloc((size_t)cap * sizeof(int64_t));
	*C_EMAN(c) = elem_managed;
	*C_SHEAD(c) = NULL;
	*C_STAIL(c) = NULL;
	*C_RHEAD(c) = NULL;
	*C_RTAIL(c) = NULL;
	InitializeSRWLock(C_LOCK(c));
	return c;
}

void bzy_channel_send(void *c, int64_t v)
{
	AcquireSRWLockExclusive(C_LOCK(c));

	/* A receiver is already waiting (channel was empty): hand the value over. */
	CWaiter *r = wq_pop(C_RHEAD(c), C_RTAIL(c));
	if (r)
	{
		*r->deliver = v;                    /* Move: the +1 passes straight to the receiver. */
		void *rb = r->breeze;
		ReleaseSRWLockExclusive(C_LOCK(c));
		bzy_sched_wake(rb);                 /* Wake outside the channel lock (may cross cores). */
		return;
	}

	if (*C_COUNT(c) < *C_CAP(c))
	{
		ring_push(c, v);                    /* Buffer (owns the +1 until recv takes it). */
		ReleaseSRWLockExclusive(C_LOCK(c));
		return;
	}

	/* Full: park the sender with its value until a receiver drains a slot. */
	CWaiter w;
	w.breeze = bzy_sched_current();
	w.val = v;
	w.deliver = NULL;
	wq_push(C_SHEAD(c), C_STAIL(c), &w);
	bzy_sched_park_unlock(C_LOCK(c));       /* Lock dropped by the scheduler once we are off the CPU. */
	/* Resumes after a receiver moved w.val into the ring; the work is already done. */
}

int64_t bzy_channel_recv(void *c)
{
	AcquireSRWLockExclusive(C_LOCK(c));

	if (*C_COUNT(c) > 0)
	{
		int64_t v = ring_pop(c);
		/* A sender is parked because the ring was full: move its value in, wake it. */
		CWaiter *s = wq_pop(C_SHEAD(c), C_STAIL(c));
		void *sb = NULL;
		if (s)
		{
			ring_push(c, s->val);
			sb = s->breeze;
		}

		ReleaseSRWLockExclusive(C_LOCK(c));
		if (sb)
		{
			bzy_sched_wake(sb);             /* Wake outside the channel lock. */
		}

		return v;                           /* Owned move-out. */
	}

	/* Empty (and, with cap >= 1, no sender can be parked): park the receiver. */
	int64_t result = 0;
	CWaiter w;
	w.breeze = bzy_sched_current();
	w.val = 0;
	w.deliver = &result;
	wq_push(C_RHEAD(c), C_RTAIL(c), &w);
	bzy_sched_park_unlock(C_LOCK(c));       /* Lock dropped by the scheduler once we are off the CPU. */
	return result;                          /* Owned; a sender delivered into result. */
}
