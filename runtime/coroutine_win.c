#include "coroutine.h"
#include <stdlib.h>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

struct BzyCoroutine
{
	void *fiber;
	BzyCoroutineFn fn;
	void *arg;
	int is_thread;   /* 1 = converted from an OS thread; never DeleteFiber it. */
};

/* Per-thread (6a-3 runs one scheduler per worker thread). g_current is the
   coroutine running on this thread; g_home is this thread's scheduler coroutine,
   the one a finished/yielding breeze switches back to. Both resolve on whatever
   OS thread is executing, so a migrated breeze always returns to the right
   scheduler. */
static __thread BzyCoroutine *g_current;
static __thread BzyCoroutine *g_home;

BzyCoroutine *bzy_coroutine_thread_enter(void)
{
	if (g_home)
	{
		return g_home;   /* Idempotent: this thread is already a scheduler coroutine. */
	}

	void *fiber = ConvertThreadToFiber(NULL);
	if (!fiber)
	{
		fiber = GetCurrentFiber();   /* Already a fiber (ERROR_ALREADY_FIBER). */
	}

	BzyCoroutine *c = calloc(1, sizeof(*c));
	c->fiber = fiber;
	c->is_thread = 1;
	g_home = c;
	g_current = c;
	return c;
}

void bzy_coroutine_main_init(void)
{
	bzy_coroutine_thread_enter();   /* The main thread is just worker 0's scheduler. */
}

static void WINAPI coroutine_trampoline(void *p)
{
	BzyCoroutine *c = (BzyCoroutine*)p;
	c->fn(c->arg);
	/* A breeze's fn switches away before returning (the scheduler wrapper does).
	   A fiber function that returns would terminate the thread, so guard by
	   bouncing back to this thread's scheduler. */
	for (;;)
	{
		bzy_coroutine_switch(g_home);
	}
}

BzyCoroutine *bzy_coroutine_create(BzyCoroutineFn fn, void *arg)
{
	BzyCoroutine *c = calloc(1, sizeof(*c));
	c->fn = fn;
	c->arg = arg;
	c->fiber = CreateFiber(0, coroutine_trampoline, c);   /* 0 = default (1 MiB reserve, lazy-committed). */
	return c;
}

void bzy_coroutine_switch(BzyCoroutine *to)
{
	g_current = to;
	SwitchToFiber(to->fiber);
}

BzyCoroutine *bzy_coroutine_self(void)
{
	return g_current;
}

void bzy_coroutine_delete(BzyCoroutine *c)
{
	if (!c->is_thread)
	{
		DeleteFiber(c->fiber);   /* A thread-converted fiber is owned by its thread. */
	}

	free(c);
}
