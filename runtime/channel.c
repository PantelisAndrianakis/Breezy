#include "breezy.h"
#include "platform.h"
#include <stdint.h>
#include <stdlib.h>

/* Bounded buffered channel with a lock-free fast path.

   The buffer is a Vyukov bounded MPMC queue: a ring of per-cell sequence numbers
   that lets any number of senders and receivers enqueue/dequeue with a single CAS
   each and NO shared lock. That removes the per-message mutex that used to serialize
   every send and recv onto one channel (the high-core fan-in collapse).

   The mutex survives only at the blocking edges: a sender parks when the ring is
   full, a receiver parks when it is empty. Parking touches the waiter lists under
   the lock; the lock-free ring is never held under it. The wake handshake mirrors
   the scheduler's idle-worker protocol (g_idle): a waiter registers (bumping a
   waiter count) and then RE-CHECKS the ring before committing to sleep, and the
   counterpart checks that count after its ring op, with seq_cst fences between, so
   no wakeup is ever lost. A parked breeze releases the lock through the scheduler
   (bzy_sched_park_unlock) so a waker on another core cannot observe it as a waiter
   until it is fully switched out.

   Capacity is exact: the ring holds precisely `cap` values (the documented "ring of
   N slots"). cap is used as a mask when it is a power of two (every real use), and
   as a modulus otherwise. */

typedef struct
{
	int64_t seq;
	int64_t val;
} Cell;

typedef struct CWaiter
{
	void            *breeze;   /* The parked breeze (a bzy_sched handle). */
	struct CWaiter  *next;
} CWaiter;

typedef struct Channel
{
	void     *vtable;        /* 0  GC header. */
	int64_t   rc;            /* 8  */
	int64_t   gcinfo;        /* 16 */
	int64_t   cap;           /* 24 Exact capacity (also the ring length). */
	int64_t   mask;          /* 32 cap-1 when cap is a power of two, else 0 (use modulo). */
	Cell     *cells;         /* 40 Ring of cap cells. */
	int64_t   elem_managed;  /* 48 Elements are managed pointers (refcount across cores). */
	int64_t   enq_pos;       /* 56 Next enqueue ticket (atomic). */
	int64_t   deq_pos;       /* 64 Next dequeue ticket (atomic). */
	int64_t   recv_waiters;  /* 72 Parked receivers (atomic; fast "should I wake?" check). */
	int64_t   send_waiters;  /* 80 Parked senders (atomic). */
	CWaiter  *send_head;     /* 88 */
	CWaiter  *send_tail;     /* 96 */
	CWaiter  *recv_head;     /* 104 */
	CWaiter  *recv_tail;     /* 112 */
	bzy_mutex lock;          /* 120 Guards the waiter lists only. */
} Channel;

static int64_t ring_index(Channel *c, int64_t pos)
{
	return c->mask ? (pos & c->mask) : (pos % c->cap);
}

/* Lock-free enqueue. 1 on success, 0 if the ring is full. */
static int ring_enqueue(Channel *c, int64_t v)
{
	int64_t pos = __atomic_load_n(&c->enq_pos, __ATOMIC_RELAXED);
	for (;;)
	{
		Cell *cell = &c->cells[ring_index(c, pos)];
		int64_t seq = __atomic_load_n(&cell->seq, __ATOMIC_ACQUIRE);
		int64_t dif = seq - pos;
		if (dif == 0)
		{
			/* Cell is free and ours to claim. */
			if (__atomic_compare_exchange_n(&c->enq_pos, &pos, pos + 1, 1,
			                                __ATOMIC_RELAXED, __ATOMIC_RELAXED))
			{
				cell->val = v;
				__atomic_store_n(&cell->seq, pos + 1, __ATOMIC_RELEASE);   /* Publish to receivers. */
				return 1;
			}
			/* CAS reloaded pos; retry. */
		}
		else if (dif < 0)
		{
			return 0;   /* Cell not yet drained: the ring is full. */
		}
		else
		{
			pos = __atomic_load_n(&c->enq_pos, __ATOMIC_RELAXED);   /* Fell behind; reload. */
		}
	}
}

/* Lock-free dequeue. 1 and *out set on success, 0 if the ring is empty. */
static int ring_dequeue(Channel *c, int64_t *out)
{
	int64_t pos = __atomic_load_n(&c->deq_pos, __ATOMIC_RELAXED);
	for (;;)
	{
		Cell *cell = &c->cells[ring_index(c, pos)];
		int64_t seq = __atomic_load_n(&cell->seq, __ATOMIC_ACQUIRE);
		int64_t dif = seq - (pos + 1);
		if (dif == 0)
		{
			if (__atomic_compare_exchange_n(&c->deq_pos, &pos, pos + 1, 1,
			                                __ATOMIC_RELAXED, __ATOMIC_RELAXED))
			{
				*out = cell->val;
				__atomic_store_n(&cell->seq, pos + c->cap, __ATOMIC_RELEASE);   /* Free the cell one lap on. */
				return 1;
			}
		}
		else if (dif < 0)
		{
			return 0;   /* Producer has not published here: the ring is empty. */
		}
		else
		{
			pos = __atomic_load_n(&c->deq_pos, __ATOMIC_RELAXED);
		}
	}
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

/* Pop one parked receiver and wake it (it will re-run ring_dequeue). The count is
   only an optimization for the fast-path check; the list under the lock is truth. */
static void wake_one_recv(Channel *c)
{
	bzy_mutex_lock(&c->lock);
	CWaiter *w = wq_pop(&c->recv_head, &c->recv_tail);
	if (w)
	{
		__atomic_sub_fetch(&c->recv_waiters, 1, __ATOMIC_SEQ_CST);
	}

	bzy_mutex_unlock(&c->lock);
	if (w)
	{
		bzy_sched_wake(w->breeze);   /* Wake outside the lock (may cross cores). */
	}
}

static void wake_one_send(Channel *c)
{
	bzy_mutex_lock(&c->lock);
	CWaiter *w = wq_pop(&c->send_head, &c->send_tail);
	if (w)
	{
		__atomic_sub_fetch(&c->send_waiters, 1, __ATOMIC_SEQ_CST);
	}

	bzy_mutex_unlock(&c->lock);
	if (w)
	{
		bzy_sched_wake(w->breeze);
	}
}

static void bzy_channel_finalize(void *p)
{
	Channel *c = (Channel*)p;
	if (c->elem_managed)
	{
		/* Release every value still buffered (positions [deq_pos, enq_pos)). */
		for (int64_t pos = c->deq_pos; pos < c->enq_pos; pos++)
		{
			bzy_release((void*)c->cells[ring_index(c, pos)].val);
		}
	}

	free(c->cells);
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

	Channel *c = bzy_alloc(sizeof(Channel));
	c->vtable = channel_vtable();
	/* A channel is shared between breezes on different worker threads, so its own
	   refcount must be atomic: mark it SHARED at allocation (the bit is fixed here,
	   never raced). 8 = BZY_GCINFO_SHARED (bit 3). */
	c->gcinfo |= BZY_GCINFO_SHARED;
	c->cap = cap;
	c->mask = ((cap & (cap - 1)) == 0) ? (cap - 1) : 0;   /* Power-of-two fast path. */
	c->cells = (Cell*)malloc((size_t)cap * sizeof(Cell));
	for (int64_t i = 0; i < cap; i++)
	{
		c->cells[i].seq = i;   /* Cell i starts ready for enqueue ticket i. */
		c->cells[i].val = 0;
	}

	c->elem_managed = elem_managed;
	c->enq_pos = 0;
	c->deq_pos = 0;
	c->recv_waiters = 0;
	c->send_waiters = 0;
	c->send_head = NULL;
	c->send_tail = NULL;
	c->recv_head = NULL;
	c->recv_tail = NULL;
	bzy_mutex_init(&c->lock);
	return c;
}

void bzy_channel_send(void *p, int64_t v)
{
	Channel *c = (Channel*)p;
	/* The value is about to cross to a receiver that may run on another worker
	   thread. If it is a managed leaf, promote it to an atomic refcount before it is
	   published -- it is still confined to this thread here, so it cannot race. */
	if (c->elem_managed)
	{
		bzy_share_crosscore((void*)v);
	}

	for (;;)
	{
		if (ring_enqueue(c, v))
		{
			/* Buffered. If a receiver is parked on an empty ring, wake one. The
			   fence orders our publish before the waiter-count read (vs the receiver
			   that bumps the count then re-checks the ring). */
			__atomic_thread_fence(__ATOMIC_SEQ_CST);
			if (__atomic_load_n(&c->recv_waiters, __ATOMIC_SEQ_CST) > 0)
			{
				wake_one_recv(c);
			}

			return;
		}

		/* Full. Take the lock and REGISTER BEFORE the final re-try: a receiver that
		   frees a slot after our re-try is then guaranteed to see the bumped waiter
		   count (its release-publish + seq_cst fence + seq_cst load cannot miss our
		   seq_cst increment AND have our re-try miss its publish), so it wakes us; a
		   receiver that freed a slot earlier is caught by the re-try itself. Checking
		   before registering left a window where the counterpart published, read the
		   count as zero, skipped the wake, and we parked forever (~1%% of mc_sum
		   runs). On the found-a-slot path the registration is rolled back; a stale
		   wake_one_* pops nothing and is harmless. */
		bzy_mutex_lock(&c->lock);
		__atomic_add_fetch(&c->send_waiters, 1, __ATOMIC_SEQ_CST);
		if (ring_enqueue(c, v))
		{
			__atomic_sub_fetch(&c->send_waiters, 1, __ATOMIC_SEQ_CST);
			bzy_mutex_unlock(&c->lock);
			__atomic_thread_fence(__ATOMIC_SEQ_CST);
			if (__atomic_load_n(&c->recv_waiters, __ATOMIC_SEQ_CST) > 0)
			{
				wake_one_recv(c);
			}

			return;
		}

		CWaiter w;
		w.breeze = bzy_sched_current();
		wq_push(&c->send_head, &c->send_tail, &w);
		bzy_sched_park_unlock(&c->lock);   /* Scheduler drops the lock once we are off-CPU. */
		/* Resumed because a receiver freed a slot: loop and retry the enqueue. */
	}
}

int64_t bzy_channel_recv(void *p)
{
	Channel *c = (Channel*)p;
	for (;;)
	{
		int64_t v;
		if (ring_dequeue(c, &v))
		{
			/* Freed a slot. If a sender is parked on a full ring, wake one. */
			__atomic_thread_fence(__ATOMIC_SEQ_CST);
			if (__atomic_load_n(&c->send_waiters, __ATOMIC_SEQ_CST) > 0)
			{
				wake_one_send(c);
			}

			return v;
		}

		/* Empty. Same register-then-re-try protocol as the sender (see above). */
		bzy_mutex_lock(&c->lock);
		__atomic_add_fetch(&c->recv_waiters, 1, __ATOMIC_SEQ_CST);
		if (ring_dequeue(c, &v))
		{
			__atomic_sub_fetch(&c->recv_waiters, 1, __ATOMIC_SEQ_CST);
			bzy_mutex_unlock(&c->lock);
			__atomic_thread_fence(__ATOMIC_SEQ_CST);
			if (__atomic_load_n(&c->send_waiters, __ATOMIC_SEQ_CST) > 0)
			{
				wake_one_send(c);
			}

			return v;
		}

		CWaiter w;
		w.breeze = bzy_sched_current();
		wq_push(&c->recv_head, &c->recv_tail, &w);
		bzy_sched_park_unlock(&c->lock);
		/* Resumed because a sender published a value: loop and retry the dequeue. */
	}
}
