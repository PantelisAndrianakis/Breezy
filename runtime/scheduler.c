#include "breezy.h"
#include "coroutine.h"
#include "platform.h"
#include <stdlib.h>
#include <stdio.h>
#include <stddef.h>   /* offsetof, for recovering the Breeze from its inline argbuf. */
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <pthread.h>
#include <unistd.h>   /* sysconf(_SC_NPROCESSORS_ONLN). */
#endif

#define SCHED_SPIN 1024 /* find_work() polls this many times before a worker commits to a
                           sleeping wait. Sized to outlast the gap a fan-out producer leaves
                           between bursts: a spinning worker is counted idle-free, so producers
                           skip the wake post (and its futex syscall). 64 was too short on Linux
                           - workers fell through to sem_wait and the per-task futex post/wait
                           thrash made a 50k fan-out 20-40x slower than Go (measured: 64 -> 244 ms
                           with wild variance; 1024 -> a stable 16 ms, on par with Go). Above ~1024
                           the curve is flat, so this is the efficient knee. */

/* Hint the CPU we are in a spin-wait: lets a hyperthread sibling proceed and avoids
   a memory-order-violation pipeline flush when the value finally changes. */
static inline void bzy_cpu_relax(void)
{
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__)
	__builtin_ia32_pause();
#endif
}

typedef struct Breeze
{
	BzyCoroutine *coroutine;
	void (*entry)(void);        /* Zero-arg spawn (6a-1). */
	void (*thunk)(void*);       /* Arg'd spawn: codegen-emitted per-target thunk. */
	void *arg;                  /* Arg block for the thunk (points at argbuf below). */
	int done;
	int home;                   /* Worker id this breeze last ran on (cache-affine wake target). */
	struct Breeze *next;
	int64_t argbuf[4];          /* Inline storage for the (<=4, per resolve.c) spawn args. The
	                               Breeze is pooled with its coroutine, so this replaces the
	                               per-spawn malloc/free of an arg block that otherwise sat on
	                               the single-threaded spawn loop's critical path. */
} Breeze;

/* Lock-free Chase-Lev work-stealing deque. The owning worker pushes and pops the
   bottom (LIFO for itself, which keeps freshly spawned work hot in cache); idle
   sibling workers steal from the top. No lock on any path: the owner uses plain
   loads/stores plus one CAS only when popping the last element (to race a thief),
   and thieves use a single CAS. This replaces the per-worker mutex FIFO whose lock
   serialized the producer against every stealer (the high-core collapse). */
typedef struct CLArray
{
	int64_t          cap;     /* Number of slots (power of two). */
	int64_t          mask;    /* cap - 1. */
	Breeze         **slot;    /* cap entries; indexed by (pos & mask). */
	struct CLArray  *older;   /* Previous, smaller array kept alive until deque free
	                             (a thief may still hold a pointer into it). */
} CLArray;

typedef struct
{
	int64_t  top;          /* Steal end; thieves CAS this forward. */
	char     _pad[56];     /* Keep top and bottom on separate cache lines: the owner writes
	                          bottom on every push while many thieves CAS top, and sharing one
	                          line makes each push invalidate it for all stealers (false sharing
	                          that cost ~5 ms at 20 workers on the fan-in benchmark). 64-byte
	                          separation guarantees distinct lines regardless of base alignment. */
	int64_t  bottom;       /* Owner end; only the owner moves it. */
	CLArray *array;        /* Backing store; swapped (release) by the owner on growth. */
} Deque;

typedef struct Worker
{
	Deque     dq;
	Breeze   *inj_head, *inj_tail;   /* Foreign-producer injection (MPSC under inj_lock). */
	bzy_mutex inj_lock;
	Breeze   *runnext;               /* Direct-handoff slot: a breeze woken by a channel rendezvous
	                                    on THIS worker runs next here, without nudging a stealer -
	                                    keeps a ping-pong pair local instead of bouncing cross-worker.
	                                    Owner-only (set in bzy_sched_wake, taken in find_work, both on
	                                    this worker thread); thieves never read it, so no atomics. */
} Worker;

static CLArray *cl_array_new(int64_t cap)
{
	CLArray *a = malloc(sizeof(*a));
	a->cap = cap;
	a->mask = cap - 1;
	a->slot = malloc((size_t)cap * sizeof(Breeze*));
	a->older = NULL;
	return a;
}

static void cl_init(Deque *d)
{
	d->top = 0;
	d->bottom = 0;
	d->array = cl_array_new(256);   /* Grows on demand; 256 covers most fan-outs without a resize. */
}

static void cl_free(Deque *d)
{
	CLArray *a = d->array;
	while (a)
	{
		CLArray *older = a->older;
		free(a->slot);
		free(a);
		a = older;
	}
}

/* Owner only. Doubles the backing array when full, copying the live range by
   logical index so a slot's logical position is identical in both arrays (a thief
   reading the old array through a stale pointer still sees the right value). */
static CLArray *cl_grow(Deque *d, CLArray *a, int64_t b, int64_t t)
{
	CLArray *na = cl_array_new(a->cap * 2);
	na->older = a;   /* Retain the old array; a concurrent thief may still read it. */
	for (int64_t i = t; i < b; i++)
	{
		na->slot[i & na->mask] = a->slot[i & a->mask];
	}

	__atomic_store_n(&d->array, na, __ATOMIC_RELEASE);
	return na;
}

/* Owner only (sole producer). Pushes at the bottom; never contends with consumers. */
static void cl_push(Deque *d, Breeze *x)
{
	int64_t b = __atomic_load_n(&d->bottom, __ATOMIC_RELAXED);
	int64_t t = __atomic_load_n(&d->top, __ATOMIC_ACQUIRE);
	CLArray *a = __atomic_load_n(&d->array, __ATOMIC_RELAXED);
	if (b - t > a->cap - 1)
	{
		a = cl_grow(d, a, b, t);
	}

	__atomic_store_n(&a->slot[b & a->mask], x, __ATOMIC_RELAXED);
	__atomic_thread_fence(__ATOMIC_RELEASE);          /* Publish the slot before bottom. */
	__atomic_store_n(&d->bottom, b + 1, __ATOMIC_RELAXED);
}

/* Any consumer (the owning worker draining its own queue, or a thief stealing).
   Both take from the top via CAS, which makes the queue FIFO in arrival order -
   the documented "deterministic FIFO interleaving on a single scheduler" - while
   the producer runs lock-free at the bottom. NULL if empty or the CAS lost the
   slot to a racing consumer (the caller treats both as "no work here"). */
static Breeze *cl_take(Deque *d)
{
	int64_t t = __atomic_load_n(&d->top, __ATOMIC_ACQUIRE);
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	int64_t b = __atomic_load_n(&d->bottom, __ATOMIC_ACQUIRE);
	if (t >= b)
	{
		return NULL;   /* Empty. */
	}

	CLArray *a = __atomic_load_n(&d->array, __ATOMIC_ACQUIRE);
	Breeze *x = __atomic_load_n(&a->slot[t & a->mask], __ATOMIC_RELAXED);
	if (!__atomic_compare_exchange_n(&d->top, &t, t + 1, 0, __ATOMIC_SEQ_CST, __ATOMIC_RELAXED))
	{
		return NULL;   /* A sibling (or the owner) took this slot first. */
	}

	return x;
}

#define CL_STEAL_MAX 256   /* Cap on how many a thief lifts in one CAS (bounds the copy). */

/* Steal up to half a victim's items in a single CAS, into out[0..return). The caller
   runs one and pushes the rest onto its OWN deque. Without batching, a single producer
   fanning out to many idle stealers serializes them all on the victim's `top` cacheline
   (one CAS per task); taking a chunk at a time amortizes that ~chunk-fold, which is what
   lets the scheduler scale to many workers. Order within the batch stays FIFO. */
static int cl_steal_batch(Deque *d, Breeze **out, int max)
{
	int64_t t = __atomic_load_n(&d->top, __ATOMIC_ACQUIRE);
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	int64_t b = __atomic_load_n(&d->bottom, __ATOMIC_ACQUIRE);
	int64_t n = b - t;
	if (n <= 0)
	{
		return 0;   /* Empty. */
	}

	n = (n + 1) / 2;   /* Take about half; round up so a lone item is still taken. */
	if (n > max)
	{
		n = max;
	}

	CLArray *a = __atomic_load_n(&d->array, __ATOMIC_ACQUIRE);
	for (int64_t i = 0; i < n; i++)
	{
		out[i] = __atomic_load_n(&a->slot[(t + i) & a->mask], __ATOMIC_RELAXED);
	}

	/* The CAS commits all n at once: if it succeeds, no other consumer could have
	   taken any of [t, t+n) (that would have moved top past t and failed us), so the
	   copied slots are valid. If it fails, we took nothing. */
	if (!__atomic_compare_exchange_n(&d->top, &t, t + n, 0, __ATOMIC_SEQ_CST, __ATOMIC_RELAXED))
	{
		return 0;
	}

	return (int)n;
}

static Worker *g_workers;
static int     g_nworkers = 1;
static bzy_sem g_work_sem;          /* Released on every enqueue: "work may be available". */
static int     g_sem_ready;         /* 1 once g_work_sem is initialized (nudge guard pre-run). */
static int     g_live;              /* Breezes created but not finished (accessed via __atomic). */
static int     g_shutdown;          /* Set when g_live hits 0; unblocks idle workers so they exit. */
static int     g_idle;              /* Workers blocked (or about to block) on g_work_sem. enqueue_on
                                       only posts the semaphore when this is > 0, so a spawn/wake onto
                                       a busy pool costs no syscall - the work is found by polling. */

static __thread BzyCoroutine *t_sched;       /* This thread's scheduler coroutine. */
static __thread Breeze       *t_running;     /* Breeze currently on this CPU (NULL = scheduler). */
static __thread int           t_wid;         /* This thread's worker index. */
static __thread void         *t_park_unlock; /* bzy_mutex* the scheduler releases after a parking switch. */
static __thread int           t_requeue;     /* bzy_yield sets this; the scheduler re-enqueues the breeze
                                                after the switch, when it is safely off its own CPU. */
static __thread int           t_is_worker;   /* 1 on a scheduler worker thread (and the main thread acting
                                                as worker 0); 0 on offload/reactor/foreign threads. */
static __thread int           t_in_callback; /* FFI: depth of foreign (C) callbacks running on this thread.
                                                A callback runs synchronously inside a C stack frame, so it
                                                must not park/yield (would unwind the C frame) or throw across
                                                the C boundary. Non-zero => park/throw is a fatal misuse. */

/* Bracket a foreign callback: codegen emits enter before, leave after, the extern
   call that receives a Breezy function pointer. */
void bzy_callback_enter(void)
{
	t_in_callback++;
}

void bzy_callback_leave(void)
{
	t_in_callback--;
}

int bzy_in_callback(void)
{
	return t_in_callback;
}

int bzy_current_wid(void)
{
	return t_wid;   /* 0 on the pre-scheduler/single-worker main thread. */
}

int bzy_on_worker(void)
{
	return t_is_worker;   /* Distinguishes a real worker context from a foreign thread. */
}

/* Wake an idle worker if any is parked on the semaphore. A busy worker finds new
   work by polling/stealing, so a spawn or wake onto a running pool pays no syscall.
   The matching re-poll in worker_loop (after it bumps g_idle) closes the race. */
static void wake_one(void)
{
	if (__atomic_load_n(&g_idle, __ATOMIC_SEQ_CST) > 0)
	{
		bzy_sem_post(&g_work_sem, 1);
	}
}

/* Push onto the current worker's own deque (owner-only path: spawn and wake both
   run on a worker thread enqueuing locally; work-stealing rebalances). */
static void enqueue_on(int wid, Breeze *b)
{
	cl_push(&g_workers[wid].dq, b);
	wake_one();
}

/* Per-worker foreign-producer injection: a non-worker thread (offload / reactor /
   IOCP completion) cannot push a Chase-Lev deque (owner-only), so external wakes
   land on a target worker's own short-locked MPSC queue. Sharding the queue per
   worker (was a single global lock) removes the serialization of every cross-thread
   wake - the network reactor's bottleneck at high connection counts. The reactor
   targets a breeze's home worker (cache affinity); find_work drains the local queue
   first, then steals from siblings so nothing strands behind a busy home worker. */
static void inject_to(int wid, Breeze *b)
{
	Worker *wk = &g_workers[wid];
	bzy_mutex_lock(&wk->inj_lock);
	b->next = NULL;
	if (wk->inj_tail)
	{
		wk->inj_tail->next = b;
	}
	else
	{
		wk->inj_head = b;
	}

	wk->inj_tail = b;
	bzy_mutex_unlock(&wk->inj_lock);
	wake_one();
}

static Breeze *inject_drain(int wid)
{
	Worker *wk = &g_workers[wid];
	if (!__atomic_load_n(&wk->inj_head, __ATOMIC_RELAXED))
	{
		return NULL;   /* Common case: nothing injected, no lock taken. */
	}

	bzy_mutex_lock(&wk->inj_lock);
	Breeze *b = wk->inj_head;
	if (b)
	{
		wk->inj_head = b->next;
		if (!wk->inj_head)
		{
			wk->inj_tail = NULL;
		}
	}

	bzy_mutex_unlock(&wk->inj_lock);
	return b;
}

static void breeze_run(void *p)
{
	(void)p;   /* The Breeze rides on the coroutine's attach slot (pooled together). */
	Breeze *b = bzy_coroutine_attach(bzy_coroutine_self());
	if (b->thunk)
	{
		b->thunk(b->arg);            /* Arg'd spawn: the thunk loads args, calls the target, frees the block. */
	}
	else
	{
		b->entry();
	}

	b->done = 1;
	bzy_coroutine_switch(t_sched);   /* TLS: the scheduler of whatever thread runs us now. */
}

/* An uncaught exception in the running breeze (called by bzy_throw): mark the
   breeze done and switch back to the scheduler. A plain fiber switch — the dead
   breeze's stack is abandoned (worker_loop deletes the fiber), so no unwind of
   its SEH-less NASM frames is attempted. Never returns to the caller. */
void bzy_sched_breeze_uncaught(void)
{
	if (t_running)
	{
		t_running->done = 1;
	}

	bzy_coroutine_switch(t_sched);
}

void bzy_sched_set_workers(int n)    /* Call before bzy_sched_run. n <= 0 => auto (logical core count). */
{
	if (n <= 0)
	{
#ifdef _WIN32
		SYSTEM_INFO si;
		GetSystemInfo(&si);
		n = (int)si.dwNumberOfProcessors;
#else
		n = (int)sysconf(_SC_NPROCESSORS_ONLN);
#endif
	}

	if (n < 1)
	{
		n = 1;
	}

	if (n > 64)
	{
		n = 64;   /* Windows WaitForMultipleObjects caps at MAXIMUM_WAIT_OBJECTS (64). */
	}

	int old = g_nworkers;
	g_workers = realloc(g_workers, (size_t)n * sizeof(Worker));
	for (int i = old; i < n; i++)
	{
		cl_init(&g_workers[i].dq);
		g_workers[i].inj_head = NULL;
		g_workers[i].inj_tail = NULL;
		g_workers[i].runnext = NULL;
		bzy_mutex_init(&g_workers[i].inj_lock);
	}

	g_nworkers = n;
}

void bzy_sched_init(void)
{
	t_sched = bzy_coroutine_thread_enter();
	t_wid = 0;
	t_is_worker = 1;   /* The main thread acts as worker 0 before/with the pool. */
	g_live = 0;
	g_shutdown = 0;
	g_nworkers = 0;
	g_workers = NULL;
	bzy_sched_set_workers(1);        /* Worker 0 exists before any spawn; Task 2 raises this. */
	bzy_sem_init(&g_work_sem);
	g_sem_ready = 1;
}

static Breeze *make_breeze(void)
{
	/* Recycle the Breeze along with the coroutine: a pooled coroutine carries its
	   Breeze on its attach slot, so a reused coroutine brings the Breeze back with no
	   allocation. This matters because make_breeze runs on the single-threaded spawn
	   loop, the measured bottleneck of fan-out; allocating a Breeze per spawn (and
	   freeing it cross-thread on completion) dominated that loop. */
	BzyCoroutine *co = bzy_coroutine_create(breeze_run, NULL);
	Breeze *b = bzy_coroutine_attach(co);
	if (b)
	{
		b->entry = NULL;
		b->thunk = NULL;
		b->arg = NULL;
		b->done = 0;
		b->next = NULL;
	}
	else
	{
		b = calloc(1, sizeof(*b));
		b->coroutine = co;
		bzy_coroutine_set_attach(co, b);
	}

	__atomic_add_fetch(&g_live, 1, __ATOMIC_SEQ_CST);
	return b;
}

void bzy_spawn(void (*entry)(void))
{
	Breeze *b = make_breeze();
	b->entry = entry;
	enqueue_on(t_wid, b);            /* Enqueue locally; work-stealing balances (Task 2). */
}

void bzy_spawn_args(void (*thunk)(void*), void *arg)
{
	Breeze *b = make_breeze();
	b->thunk = thunk;
	b->arg = arg;
	enqueue_on(t_wid, b);
}

/* Inline-argument spawn (codegen fast path). begin() hands back the pooled Breeze's
   own argbuf for the caller to fill (no malloc); commit() enqueues. The Breeze is
   recovered from the buffer pointer since argbuf is a fixed-offset field, so no extra
   state is threaded between the two calls (nested spawns during arg evaluation stay
   independent - each has its own Breeze and buffer on the native stack). */
void *bzy_spawn_args_begin(void (*thunk)(void*))
{
	Breeze *b = make_breeze();
	b->thunk = thunk;
	b->arg = b->argbuf;
	return b->argbuf;
}

void bzy_spawn_args_commit(void *argbuf)
{
	Breeze *b = (Breeze*)((char*)argbuf - offsetof(Breeze, argbuf));
	enqueue_on(t_wid, b);
}

void *bzy_sched_current(void)      /* Opaque handle to the running breeze (for waiter lists). */
{
	return t_running;
}

/* True if the current worker already has other runnable breezes on its own
   deque. A spin-before-park caller (socket recv) consults this: when ready work
   exists - e.g. a peer handler that must run to produce our reply - hogging the
   core with a spin starves it, so the caller should park immediately and yield.
   A racy read of top/bottom is fine; this is a scheduling hint, not a barrier. */
int bzy_sched_local_runnable(void)
{
	if (!t_is_worker)
	{
		return 0;
	}

	Deque *d = &g_workers[t_wid].dq;
	return (d->bottom - d->top) > 0;
}

/* Abort if called from within a foreign callback: parking/throwing there would
   corrupt the live C stack frame the callback runs inside. Fail loud, not silent. */
static void bzy_callback_guard(const char *op)
{
	if (t_in_callback)
	{
		fprintf(stderr, "A foreign callback may not %s (it runs inside a C stack frame).\n", op);
		abort();
	}
}

void bzy_sched_park(void)          /* Suspend the running breeze; a wake() must re-enqueue it. */
{
	bzy_callback_guard("park");
	bzy_coroutine_switch(t_sched);
}

void bzy_sched_park_unlock(void *lock)   /* Park, then have the scheduler release the lock after we switch out. */
{
	bzy_callback_guard("park");
	t_park_unlock = lock;           /* bzy_mutex*. Closes the wake-before-park race: the waker cannot take the lock,
	                                   and so cannot observe us as a waiter, until we are safely off the CPU. */
	bzy_coroutine_switch(t_sched);
}

void bzy_sched_wake(void *breeze)  /* Make a parked breeze ready again via the direct-handoff slot. */
{
	/* Direct handoff: place the woken breeze in this worker's runnext so it runs
	   next HERE when the current breeze parks/yields, with no wake_one() nudge -
	   the partner in a channel ping-pong stays on one worker instead of being
	   stolen onto an idle sibling (a cross-worker inject + futex + cache bounce
	   per round-trip). Any breeze already in runnext is displaced to the deque via
	   the normal enqueue (which DOES nudge), so at most one breeze is owner-pinned
	   and nothing strands: the current breeze always yields eventually, and
	   find_work then takes runnext. Owner-thread only, so a plain store is safe. */
	if (t_is_worker)
	{
		Breeze *prev = g_workers[t_wid].runnext;
		g_workers[t_wid].runnext = (Breeze*)breeze;
		if (prev)
		{
			enqueue_on(t_wid, prev);
		}

		return;
	}

	enqueue_on(t_wid, (Breeze*)breeze);   /* Not on a worker (rare): fall back to the deque. */
}

void bzy_sched_wake_external(void *breeze)   /* Wake from a non-scheduler thread (e.g. an offload worker). */
{
	Breeze *b = (Breeze*)breeze;
	int wid = b->home;
	if (wid < 0 || wid >= g_nworkers)
	{
		wid = 0;   /* Unscheduled fallback (a breeze that has not run yet). */
	}

	inject_to(wid, b);   /* Not a worker thread: target the home worker's injection queue. */
}

void bzy_sched_nudge(void)   /* Release one semaphore count so an idle worker re-checks timers. */
{
	if (g_sem_ready)
	{
		bzy_sem_post(&g_work_sem, 1);
	}
}

void bzy_yield(void)
{
	bzy_callback_guard("yield");
	Breeze *b = t_running;
	if (!b)
	{
		return;                 /* Called from the scheduler itself: nothing to yield. */
	}

	/* Do NOT enqueue here: that would make b stealable while its fiber is still on
	   this CPU, letting another worker SwitchToFiber the same fiber concurrently.
	   Defer the re-enqueue to the scheduler, which runs it once b is off-CPU. */
	t_requeue = 1;
	bzy_coroutine_switch(t_sched);
}

/* Find a breeze: own deque first, then steal from siblings, then the foreign
   injection queue. NULL if none anywhere. */
static Breeze *find_work(void)
{
	Breeze *rn = g_workers[t_wid].runnext;   /* Direct-handoff partner runs before anything else. */
	if (rn)
	{
		g_workers[t_wid].runnext = NULL;
		return rn;
	}

	Breeze *b = cl_take(&g_workers[t_wid].dq);
	if (b)
	{
		return b;
	}

	Breeze *batch[CL_STEAL_MAX];
	for (int i = 1; i < g_nworkers; i++)
	{
		int victim = (t_wid + i) % g_nworkers;
		int n = cl_steal_batch(&g_workers[victim].dq, batch, CL_STEAL_MAX);
		if (n > 0)
		{
			/* Keep the first to run now; deposit the rest on our own deque (we own
			   it, so the push is the uncontended producer path) for later/stealing. */
			for (int j = 1; j < n; j++)
			{
				cl_push(&g_workers[t_wid].dq, batch[j]);
			}

			return batch[0];
		}
	}

	Breeze *ij = inject_drain(t_wid);   /* Our own injection queue (reactor wakes land here). */
	if (ij)
	{
		return ij;
	}

	for (int i = 1; i < g_nworkers; i++)   /* Steal a stranded wake off a busy sibling's queue. */
	{
		Breeze *b = inject_drain((t_wid + i) % g_nworkers);
		if (b)
		{
			return b;
		}
	}

	return NULL;
}

/* The scheduler loop, run by every worker thread. */
static void worker_loop(void)
{
	for (;;)
	{
		Breeze *b = find_work();
		if (!b)
		{
			bzy_cycle_slice(t_wid);   /* Bounded, per-worker; other workers keep running. */

			if (__atomic_load_n(&g_shutdown, __ATOMIC_SEQ_CST))
			{
				break;
			}

			/* Fire every timer due now: spawn its target, re-insert periodics. */
			int64_t now = bzy_clock_millis();
			int fired = 0;
			void *t;
			while ((t = bzy_timer_pop_due(now)) != NULL)
			{
				void (*entry)(void) = *(void(**)(void))((char*)t + 40);   /* Timer.entry. */
				int64_t period = *(int64_t*)((char*)t + 32);              /* Timer.period. */
				bzy_spawn(entry);
				fired++;
				if (period > 0)
				{
					bzy_timer_reinsert(t, now);   /* Periodic: keep the heap's ref. */
				}
				else
				{
					bzy_release(t);               /* One-shot: drop the heap's ref. */
				}
			}

			if (fired)
			{
				continue;                         /* Pick up the freshly spawned breezes. */
			}

			/* Spin-poll before sleeping. With one producer feeding many workers
			   (fan-out), a worker that drains the queue finds fresh work again within
			   a few hundred cycles; parking on the semaphore here instead would force a
			   post/wait syscall pair per burst, and that churn - not lock contention -
			   is what made throughput collapse as workers were added. Spinning briefly
			   (still counted idle-free so producers skip the post) catches the work in
			   userspace. Only genuinely idle workers fall through to the real sleep.
			   (Capping the number of spinners was tried and is worse here: the parked
			   workers then need a wake syscall, which is the very churn we are avoiding.) */
			for (int spun = 0; spun < SCHED_SPIN; spun++)
			{
				b = find_work();
				if (b)
				{
					goto run;
				}

				bzy_cpu_relax();
			}

			/* About to block. Register as idle and re-poll: an enqueue that raced our
			   earlier find_work() is now either visible here, or sees g_idle > 0 and
			   posts. Either way no wakeup is lost. g_idle is dropped on the way out. */
			__atomic_add_fetch(&g_idle, 1, __ATOMIC_SEQ_CST);
			b = find_work();
			if (b)
			{
				__atomic_sub_fetch(&g_idle, 1, __ATOMIC_SEQ_CST);
				goto run;
			}

			int64_t nd = bzy_timer_next_deadline();
			if (nd >= 0)
			{
				int64_t wait = nd - bzy_clock_millis();
				if (wait < 1)
				{
					wait = 1;
				}

				if (wait > 0x7fffffff)
				{
					wait = 0x7fffffff;
				}

				bzy_sem_wait_ms(&g_work_sem, wait);   /* Sleep until the next deadline (or an enqueue). */
			}
			else if (bzy_offload_inflight() > 0 || bzy_iocp_inflight() > 0)
			{
				/* A breeze is parked on an offload or network op; a worker or the
				   completion thread will wake it. Wait instead of declaring deadlock. */
				bzy_sem_wait(&g_work_sem);
			}
			else if (g_nworkers == 1 && __atomic_load_n(&g_live, __ATOMIC_SEQ_CST) > 0)
			{
				/* Single worker, breezes remain parked, nothing can ever wake them.
				   (The re-poll above already covered an offload/IOCP wake that raced
				   the inflight==0 read.) */
				__atomic_sub_fetch(&g_idle, 1, __ATOMIC_SEQ_CST);
				fprintf(stderr, "Deadlock: all breezes blocked.\n");
				abort();
			}
			else
			{
				bzy_sem_wait(&g_work_sem);
			}

			__atomic_sub_fetch(&g_idle, 1, __ATOMIC_SEQ_CST);
			continue;
		}

run:
		t_running = b;
		b->home = t_wid;                       /* Cache-affine wake target for the reactor. */
		bzy_coroutine_switch(b->coroutine);
		t_running = NULL;

		if (t_park_unlock)
		{
			bzy_mutex_unlock((bzy_mutex*)t_park_unlock);   /* Hand-off: the breeze parked holding this. */
			t_park_unlock = NULL;
		}

		if (t_requeue)
		{
			t_requeue = 0;
			enqueue_on(t_wid, b);   /* Yielded: b is off-CPU now, so it is safe to make it stealable. */
		}
		else if (b->done)
		{
			bzy_coroutine_delete(b->coroutine);   /* Pools the coroutine AND its attached Breeze; do not free b. */
			if (__atomic_sub_fetch(&g_live, 1, __ATOMIC_SEQ_CST) == 0 && bzy_timer_next_deadline() < 0)
			{
				/* No breezes and no timer can ever fire again: shut down. A pending
				   timer keeps the program alive — workers fall into the timer wait. */
				__atomic_store_n(&g_shutdown, 1, __ATOMIC_SEQ_CST);
				bzy_sem_post(&g_work_sem, g_nworkers);   /* Wake every idle worker to exit. */
			}
		}
	}
}

/* Worker entry: become this thread's own scheduler coroutine, then run the loop.
   The two ABIs differ only in the wrapper signature/return. */
static void worker_main(int wid)
{
	t_wid = wid;
	t_is_worker = 1;
	t_sched = bzy_coroutine_thread_enter();   /* This thread becomes its own scheduler coroutine. */
	worker_loop();
}

#ifdef _WIN32
static DWORD WINAPI worker_thread_main(void *arg)
{
	worker_main((int)(intptr_t)arg);
	return 0;
}
#else
static void *worker_thread_main(void *arg)
{
	worker_main((int)(intptr_t)arg);
	return NULL;
}
#endif

void bzy_sched_run(void)
{
#ifdef _WIN32
	HANDLE *threads = NULL;
	if (g_nworkers > 1)
	{
		threads = calloc((size_t)(g_nworkers - 1), sizeof(HANDLE));
		for (int i = 1; i < g_nworkers; i++)
		{
			threads[i - 1] = CreateThread(NULL, 0, worker_thread_main, (void*)(intptr_t)i, 0, NULL);
		}
	}

	worker_loop();   /* This thread is worker 0. */

	if (threads)
	{
		WaitForMultipleObjects((DWORD)(g_nworkers - 1), threads, TRUE, INFINITE);
		for (int i = 0; i < g_nworkers - 1; i++)
		{
			CloseHandle(threads[i]);
		}

		free(threads);
	}
#else
	pthread_t *threads = NULL;
	if (g_nworkers > 1)
	{
		threads = calloc((size_t)(g_nworkers - 1), sizeof(pthread_t));
		for (int i = 1; i < g_nworkers; i++)
		{
			pthread_create(&threads[i - 1], NULL, worker_thread_main, (void*)(intptr_t)i);
		}
	}

	worker_loop();   /* This thread is worker 0. */

	if (threads)
	{
		for (int i = 0; i < g_nworkers - 1; i++)
		{
			pthread_join(threads[i], NULL);
		}

		free(threads);
	}
#endif

	bzy_sem_destroy(&g_work_sem);
	g_sem_ready = 0;
	for (int i = 0; i < g_nworkers; i++)
	{
		cl_free(&g_workers[i].dq);
	}

	free(g_workers);
	g_workers = NULL;
	g_nworkers = 0;
}
