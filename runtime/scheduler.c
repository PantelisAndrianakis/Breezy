#include "breezy.h"
#include "coroutine.h"
#include <stdlib.h>
#include <stdio.h>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

typedef struct Breeze
{
	BzyCoroutine *coroutine;
	void (*entry)(void);        /* Zero-arg spawn (6a-1). */
	void (*thunk)(void*);       /* Arg'd spawn: codegen-emitted per-target thunk. */
	void *arg;                  /* Arg block for the thunk. */
	int done;
	struct Breeze *next;
} Breeze;

/* One ready FIFO per worker. A worker pushes/pops its own queue and steals from
   siblings when it runs dry (6a-3 Task 2). The lock makes both safe. */
typedef struct Worker
{
	Breeze *head, *tail;
	SRWLOCK lock;
} Worker;

static Worker *g_workers;
static int     g_nworkers = 1;
static HANDLE  g_work_sem;          /* Released on every enqueue: "work may be available". */
static volatile LONG g_live;        /* Breezes created but not finished (atomic). */
static volatile LONG g_shutdown;    /* Set when g_live hits 0; unblocks idle workers so they exit. */

static __thread BzyCoroutine *t_sched;       /* This thread's scheduler coroutine. */
static __thread Breeze       *t_running;     /* Breeze currently on this CPU (NULL = scheduler). */
static __thread int           t_wid;         /* This thread's worker index. */
static __thread void         *t_park_unlock; /* SRWLOCK* the scheduler releases after a parking switch. */

static void enqueue_on(int wid, Breeze *b)
{
	Worker *w = &g_workers[wid];
	AcquireSRWLockExclusive(&w->lock);
	b->next = NULL;
	if (w->tail)
	{
		w->tail->next = b;
	}
	else
	{
		w->head = b;
	}

	w->tail = b;
	ReleaseSRWLockExclusive(&w->lock);
	ReleaseSemaphore(g_work_sem, 1, NULL);
}

static Breeze *dequeue_from(int wid)
{
	Worker *w = &g_workers[wid];
	AcquireSRWLockExclusive(&w->lock);
	Breeze *b = w->head;
	if (b)
	{
		w->head = b->next;
		if (!w->head)
		{
			w->tail = NULL;
		}
	}

	ReleaseSRWLockExclusive(&w->lock);
	return b;
}

static void breeze_run(void *p)
{
	Breeze *b = (Breeze*)p;
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
		SYSTEM_INFO si;
		GetSystemInfo(&si);
		n = (int)si.dwNumberOfProcessors;
	}

	if (n < 1)
	{
		n = 1;
	}

	if (n > 64)
	{
		n = 64;   /* WaitForMultipleObjects caps at MAXIMUM_WAIT_OBJECTS (64). */
	}

	int old = g_nworkers;
	g_workers = realloc(g_workers, (size_t)n * sizeof(Worker));
	for (int i = old; i < n; i++)
	{
		g_workers[i].head = NULL;
		g_workers[i].tail = NULL;
		InitializeSRWLock(&g_workers[i].lock);
	}

	g_nworkers = n;
}

void bzy_sched_init(void)
{
	t_sched = bzy_coroutine_thread_enter();
	t_wid = 0;
	g_live = 0;
	g_shutdown = 0;
	g_nworkers = 0;
	g_workers = NULL;
	bzy_sched_set_workers(1);        /* Worker 0 exists before any spawn; Task 2 raises this. */
	g_work_sem = CreateSemaphore(NULL, 0, 0x7fffffff, NULL);
}

static Breeze *make_breeze(void)
{
	Breeze *b = calloc(1, sizeof(*b));
	b->coroutine = bzy_coroutine_create(breeze_run, b);
	InterlockedIncrement(&g_live);
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

void *bzy_sched_current(void)      /* Opaque handle to the running breeze (for waiter lists). */
{
	return t_running;
}

void bzy_sched_park(void)          /* Suspend the running breeze; a wake() must re-enqueue it. */
{
	bzy_coroutine_switch(t_sched);
}

void bzy_sched_park_unlock(void *srwlock)   /* Park, then have the scheduler release the lock after we switch out. */
{
	t_park_unlock = srwlock;        /* Closes the wake-before-park race: the waker cannot take the lock,
	                                   and so cannot observe us as a waiter, until we are safely off the CPU. */
	bzy_coroutine_switch(t_sched);
}

void bzy_sched_wake(void *breeze)  /* Make a parked breeze ready again, on this worker; stealing rebalances. */
{
	enqueue_on(t_wid, (Breeze*)breeze);
}

void bzy_sched_wake_external(void *breeze)   /* Wake from a non-scheduler thread (e.g. an offload worker). */
{
	enqueue_on(0, (Breeze*)breeze);   /* Worker 0's queue; stealing rebalances. No thread-local state needed. */
}

void bzy_sched_nudge(void)   /* Release one semaphore count so an idle worker re-checks timers. */
{
	if (g_work_sem)
	{
		ReleaseSemaphore(g_work_sem, 1, NULL);
	}
}

void bzy_yield(void)
{
	Breeze *b = t_running;
	if (!b)
	{
		return;                 /* Called from the scheduler itself: nothing to yield. */
	}

	enqueue_on(t_wid, b);       /* Re-queue at the tail (round-robin). */
	bzy_coroutine_switch(t_sched);
}

/* Find a breeze: own queue first, then steal from siblings. NULL if none anywhere. */
static Breeze *find_work(void)
{
	Breeze *b = dequeue_from(t_wid);
	if (b)
	{
		return b;
	}

	for (int i = 1; i < g_nworkers; i++)
	{
		int victim = (t_wid + i) % g_nworkers;
		b = dequeue_from(victim);
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
			if (g_shutdown)
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

				WaitForSingleObject(g_work_sem, (DWORD)wait);   /* Sleep until the next deadline (or an enqueue). */
				continue;
			}

			/* No ready work and no pending timer. */
			if (bzy_offload_inflight() > 0 || bzy_iocp_inflight() > 0)
			{
				/* A breeze is parked on an offload or network op; a worker or the
				   completion thread will wake it. Wait instead of declaring deadlock. */
				WaitForSingleObject(g_work_sem, INFINITE);
				continue;
			}

			if (g_nworkers == 1 && g_live > 0)
			{
				/* Single worker, breezes remain parked, nothing can ever wake them. */
				fprintf(stderr, "Deadlock: all breezes blocked.\n");
				abort();
			}

			WaitForSingleObject(g_work_sem, INFINITE);
			continue;
		}

		t_running = b;
		bzy_coroutine_switch(b->coroutine);
		t_running = NULL;

		if (t_park_unlock)
		{
			ReleaseSRWLockExclusive((PSRWLOCK)t_park_unlock);   /* Hand-off: the breeze parked holding this. */
			t_park_unlock = NULL;
		}

		if (b->done)
		{
			bzy_coroutine_delete(b->coroutine);
			free(b);
			if (InterlockedDecrement(&g_live) == 0 && bzy_timer_next_deadline() < 0)
			{
				/* No breezes and no timer can ever fire again: shut down. A pending
				   timer keeps the program alive — workers fall into the timer wait. */
				g_shutdown = 1;
				ReleaseSemaphore(g_work_sem, g_nworkers, NULL);   /* Wake every idle worker to exit. */
			}
		}
	}
}

static DWORD WINAPI worker_thread_main(void *arg)
{
	t_wid = (int)(intptr_t)arg;
	t_sched = bzy_coroutine_thread_enter();   /* This thread becomes its own scheduler coroutine. */
	worker_loop();
	return 0;
}

void bzy_sched_run(void)
{
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

	CloseHandle(g_work_sem);
	g_work_sem = NULL;
	free(g_workers);
	g_workers = NULL;
	g_nworkers = 0;
}
