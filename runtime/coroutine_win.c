#include "coroutine.h"
#include <stdlib.h>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

struct BzyCoroutine
{
	void *fiber;
	BzyCoroutineFn fn;
	void *arg;
};

/* Single OS thread in 6a-1, so these are plain statics. They become
   per-thread (one scheduler per core) when 6a-3 introduces real threads. */
static BzyCoroutine *g_current;
static BzyCoroutine g_main;

void bzy_coroutine_main_init(void)
{
	g_main.fiber = ConvertThreadToFiber(NULL);
	g_main.fn = NULL;
	g_main.arg = NULL;
	g_current = &g_main;
}

static void WINAPI coroutine_trampoline(void *p)
{
	BzyCoroutine *c = (BzyCoroutine*)p;
	c->fn(c->arg);
	/* A breeze's fn must switch away before returning (the scheduler's wrapper
	   does). A fiber function that returns would terminate the thread, so guard. */
	for (;;)
	{
		bzy_coroutine_switch(&g_main);
	}
}

BzyCoroutine *bzy_coroutine_create(BzyCoroutineFn fn, void *arg)
{
	BzyCoroutine *c = malloc(sizeof(*c));
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
	DeleteFiber(c->fiber);
	free(c);
}
