#include "breezy.h"
#include <stdint.h>
#include <stdlib.h>

/* Bounded buffered channel (object_size = 96):
   0 vtable | 8 rc | 16 gcinfo | 24 cap | 32 count | 40 head | 48 ring(int64_t*) |
   56 elem_managed | 64 send_head | 72 send_tail | 80 recv_head | 88 recv_tail.
   The ring is a raw malloc'd buffer (not a managed child), so the finalizer
   frees it and releases any buffered managed elements; the cycle collector
   treats the channel as a leaf (num_obj_fields = 0).

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
static int64_t g_channel_typeinfo[2] = { 0 /* finalizer (set on first use) */, 0 };
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
		cap = 1;   /* v1 requires a buffer of at least one slot. */
	}

	void *c = bzy_alloc(96);
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
	return c;
}

void bzy_channel_send(void *c, int64_t v)
{
	/* A receiver is already waiting (channel was empty): hand the value over. */
	CWaiter *r = wq_pop(C_RHEAD(c), C_RTAIL(c));
	if (r)
	{
		*r->deliver = v;                    /* Move: the +1 passes straight to the receiver. */
		bzy_sched_wake(r->breeze);
		return;
	}

	if (*C_COUNT(c) < *C_CAP(c))
	{
		ring_push(c, v);                    /* Buffer (owns the +1 until recv takes it). */
		return;
	}

	/* Full: park the sender with its value until a receiver drains a slot. */
	CWaiter w;
	w.breeze = bzy_sched_current();
	w.val = v;
	w.deliver = NULL;
	wq_push(C_SHEAD(c), C_STAIL(c), &w);
	bzy_sched_park();                       /* Resumes after a receiver moved w.val into the ring. */
}

int64_t bzy_channel_recv(void *c)
{
	if (*C_COUNT(c) > 0)
	{
		int64_t v = ring_pop(c);
		/* A sender is parked because the ring was full: move its value in, wake it. */
		CWaiter *s = wq_pop(C_SHEAD(c), C_STAIL(c));
		if (s)
		{
			ring_push(c, s->val);
			bzy_sched_wake(s->breeze);
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
	bzy_sched_park();                       /* Resumes after a sender delivered into result. */
	return result;                          /* Owned. */
}
