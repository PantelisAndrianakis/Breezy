/* Smoke test for the coroutine backend: two coroutines hand control back and
   forth, appending to a log, proving create/switch/self work and stacks are
   independent. Built per-host (coroutine_posix.c on Linux, coroutine_win.c on
   Windows). */
#include "test_framework.h"
#include "coroutine.h"
#include <stdint.h>

static char g_log[64];
static int  g_n;
static BzyCoroutine *g_main;
static BzyCoroutine *g_a;
static BzyCoroutine *g_b;

static void co_a(void *arg)
{
	(void)arg;
	for (int i = 0; i < 3; i++)
	{
		g_log[g_n++] = 'a';
		bzy_coroutine_switch(g_b);   /* Yield to B. */
	}

	bzy_coroutine_switch(g_main);    /* Done: back to the scheduler coroutine. */
}

static void co_b(void *arg)
{
	(void)arg;
	for (int i = 0; i < 3; i++)
	{
		g_log[g_n++] = 'b';
		bzy_coroutine_switch(g_a);   /* Yield back to A. */
	}

	bzy_coroutine_switch(g_main);
}

static int g_runs;

/* Returns normally so the looping trampoline parks it for reuse. */
static void co_count(void *arg)
{
	g_runs += (int)(intptr_t)arg;
}

static void test_coroutine_reuse(void)
{
	g_runs = 0;
	g_main = bzy_coroutine_thread_enter();
	BzyCoroutine *c = bzy_coroutine_create(co_count, (void*)(intptr_t)2);
	bzy_coroutine_switch(c);                              /* Runs co_count(2); trampoline returns here. */
	ASSERT_INT(g_runs, 2);

	bzy_coroutine_rearm(c, co_count, (void*)(intptr_t)5);
	bzy_coroutine_switch(c);                              /* Reuses the SAME stack; runs co_count(5). */
	ASSERT_INT(g_runs, 7);

	bzy_coroutine_delete(c);
}

static void test_pingpong(void)
{
	bzy_coroutine_main_init();
	g_main = bzy_coroutine_self();
	g_a = bzy_coroutine_create(co_a, NULL);
	g_b = bzy_coroutine_create(co_b, NULL);
	bzy_coroutine_switch(g_a);       /* Start A; it ping-pongs with B, then returns here. */
	g_log[g_n] = '\0';
	ASSERT_STR(g_log, "ababab");
	ASSERT(bzy_coroutine_self() == g_main);
	bzy_coroutine_delete(g_a);
	bzy_coroutine_delete(g_b);
}

int main(void)
{
	RUN(test_pingpong);
	RUN(test_coroutine_reuse);
	SUMMARY();
	return 0;
}
