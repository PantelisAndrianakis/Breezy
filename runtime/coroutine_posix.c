/* POSIX coroutine backend (System V ucontext), the Linux counterpart to
   coroutine_win.c (Windows fibers). Implements the platform-agnostic 6-function
   interface in coroutine.h. Selected by the Makefile on a Linux host. */
#include "coroutine.h"
#include <ucontext.h>
#include <stdlib.h>

#define BZY_CO_STACK (256 * 1024)   /* Per-breeze stack. */

struct BzyCoroutine
{
	ucontext_t     ctx;
	void          *stack;   /* NULL for a thread-promoted (scheduler) coroutine. */
	BzyCoroutineFn fn;
	void          *arg;
};

static __thread BzyCoroutine *t_current;
static __thread BzyCoroutine  t_main;     /* This thread's own (scheduler) coroutine. */

void bzy_coroutine_main_init(void)
{
	t_current = &t_main;
}

BzyCoroutine *bzy_coroutine_thread_enter(void)
{
	if (!t_current)
	{
		t_current = &t_main;
	}

	return t_current;
}

/* Entry trampoline: the fn/arg ride on the struct (via t_current), so makecontext
   needs no integer arguments. bzy_coroutine_switch sets t_current to the new
   coroutine before swapcontext, so this reads the right one. */
static void co_trampoline(void)
{
	BzyCoroutine *c = t_current;
	for (;;)
	{
		c->fn(c->arg);                                       /* Runs to completion; a breeze body returns here. */
		bzy_coroutine_switch(bzy_coroutine_thread_enter());  /* Park: back to THIS thread's scheduler. */
		/* Resumed only after the scheduler re-armed c->fn/c->arg. t_current is c again, so the
		   next iteration reads the freshly-armed fn/arg. */
	}
}

BzyCoroutine *bzy_coroutine_create(BzyCoroutineFn fn, void *arg)
{
	BzyCoroutine *c = calloc(1, sizeof(*c));
	c->stack = malloc(BZY_CO_STACK);
	c->fn = fn;
	c->arg = arg;
	getcontext(&c->ctx);
	c->ctx.uc_stack.ss_sp = c->stack;
	c->ctx.uc_stack.ss_size = BZY_CO_STACK;
	c->ctx.uc_link = NULL;
	makecontext(&c->ctx, co_trampoline, 0);
	return c;
}

void bzy_coroutine_rearm(BzyCoroutine *c, BzyCoroutineFn fn, void *arg)
{
	c->fn = fn;     /* The coroutine is parked at the trampoline's switch; next resume runs fn. */
	c->arg = arg;
}

void bzy_coroutine_switch(BzyCoroutine *to)
{
	BzyCoroutine *from = t_current;
	if (from == to)
	{
		return;
	}

	t_current = to;
	swapcontext(&from->ctx, &to->ctx);
}

BzyCoroutine *bzy_coroutine_self(void)
{
	return t_current;
}

void bzy_coroutine_delete(BzyCoroutine *c)
{
	if (c && c->stack)
	{
		free(c->stack);
		free(c);
	}
}
