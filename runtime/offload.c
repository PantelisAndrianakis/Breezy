#include "breezy.h"
#include <stdlib.h>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

/* A task is a stack node on the submitting breeze's coroutine stack (stable while
   parked). The worker copies its fields out under the lock, runs fn(ctx), then
   wakes the breeze — it never touches the node again, so the node may go out of
   scope as soon as the breeze resumes. */
typedef struct OffTask
{
	void (*fn)(void*);
	void *ctx;
	void *breeze;
	struct OffTask *next;
} OffTask;

static OffTask *g_head, *g_tail;
static SRWLOCK  g_lock = SRWLOCK_INIT;
static HANDLE   g_sem;                 /* Released once per submitted task. */
static HANDLE  *g_threads;
static int      g_nthreads;
static volatile LONG g_inflight;       /* Breezes parked on an offload task. */
static volatile LONG g_shutdown;
static int      g_started;             /* Guarded by g_start_lock. */
static SRWLOCK  g_start_lock = SRWLOCK_INIT;

static DWORD WINAPI offload_worker(void *unused)
{
	(void)unused;
	for (;;)
	{
		WaitForSingleObject(g_sem, INFINITE);
		if (g_shutdown)
		{
			break;
		}

		AcquireSRWLockExclusive(&g_lock);
		OffTask *tp = g_head;
		if (tp)
		{
			g_head = tp->next;
			if (!g_head)
			{
				g_tail = NULL;
			}
		}

		ReleaseSRWLockExclusive(&g_lock);
		if (!tp)
		{
			continue;                  /* Spurious / shutdown wake. */
		}

		void (*fn)(void*) = tp->fn;    /* Copy out: tp belongs to the parked breeze. */
		void *ctx = tp->ctx;
		void *breeze = tp->breeze;
		fn(ctx);                       /* The blocking call: reads caller buffers, fills the result slot. */
		bzy_sched_wake_external(breeze);
		InterlockedDecrement(&g_inflight);
	}

	return 0;
}

static void offload_ensure_started(void)
{
	AcquireSRWLockExclusive(&g_start_lock);
	if (!g_started)
	{
		int n;
		const char *env = getenv("BZY_OFFLOAD_THREADS");
		if (env)
		{
			n = atoi(env);
		}
		else
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
			n = 64;
		}

		g_shutdown = 0;
		g_sem = CreateSemaphore(NULL, 0, 0x7fffffff, NULL);
		g_threads = calloc((size_t)n, sizeof(HANDLE));
		for (int i = 0; i < n; i++)
		{
			g_threads[i] = CreateThread(NULL, 0, offload_worker, NULL, 0, NULL);
		}

		g_nthreads = n;
		g_started = 1;
	}

	ReleaseSRWLockExclusive(&g_start_lock);
}

void bzy_offload_run(void (*fn)(void*), void *ctx)
{
	offload_ensure_started();

	OffTask t;
	t.fn = fn;
	t.ctx = ctx;
	t.breeze = bzy_sched_current();
	t.next = NULL;

	InterlockedIncrement(&g_inflight);
	AcquireSRWLockExclusive(&g_lock);
	if (g_tail)
	{
		g_tail->next = &t;
	}
	else
	{
		g_head = &t;
	}

	g_tail = &t;
	ReleaseSemaphore(g_sem, 1, NULL);
	/* Park; the scheduler releases g_lock after we switch out, so a worker cannot
	   pop &t (and read our stack node) until we are safely off the CPU. */
	bzy_sched_park_unlock(&g_lock);
	/* Resumed: the worker ran fn(ctx) and woke us. */
}

int64_t bzy_offload_inflight(void)
{
	return (int64_t)g_inflight;
}

void bzy_offload_shutdown(void)
{
	AcquireSRWLockExclusive(&g_start_lock);
	if (g_started)
	{
		g_shutdown = 1;
		ReleaseSemaphore(g_sem, g_nthreads, NULL);   /* Wake every worker to exit. */
		WaitForMultipleObjects((DWORD)g_nthreads, g_threads, TRUE, INFINITE);
		for (int i = 0; i < g_nthreads; i++)
		{
			CloseHandle(g_threads[i]);
		}

		free(g_threads);
		g_threads = NULL;
		CloseHandle(g_sem);
		g_sem = NULL;
		g_nthreads = 0;
		g_started = 0;
		g_shutdown = 0;
		g_head = NULL;
		g_tail = NULL;
	}

	ReleaseSRWLockExclusive(&g_start_lock);
}
