/* Smoke test for the coroutine backend: two coroutines hand control back and
   forth, appending to a log, proving create/switch/self work and stacks are
   independent. Built per-host (coroutine_posix.c on Linux, coroutine_win.c on
   Windows). */
#include "test_framework.h"
#include "coroutine.h"

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
	SUMMARY();
	return 0;
}
