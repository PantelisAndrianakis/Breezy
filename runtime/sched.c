#include "breezy.h"
#include "coroutine.h"
#include <stdlib.h>

typedef struct Breeze
{
	BzyCoroutine *coroutine;
	void (*entry)(void);
	int done;
	struct Breeze *next;
} Breeze;

static Breeze *g_head, *g_tail;   /* Ready FIFO. */
static Breeze *g_running;         /* The breeze currently on the CPU (NULL = scheduler). */
static BzyCoroutine *g_sched;     /* The scheduler coroutine (the program's main fiber). */

static void enqueue(Breeze *b)
{
	b->next = NULL;
	if (g_tail)
	{
		g_tail->next = b;
	}
	else
	{
		g_head = b;
	}

	g_tail = b;
}

static Breeze *dequeue(void)
{
	Breeze *b = g_head;
	if (b)
	{
		g_head = b->next;
		if (!g_head)
		{
			g_tail = NULL;
		}
	}

	return b;
}

static void breeze_run(void *p)
{
	Breeze *b = (Breeze*)p;
	b->entry();
	b->done = 1;
	bzy_coroutine_switch(g_sched);   /* Back to the scheduler; this fiber is never resumed again. */
}

void bzy_sched_init(void)
{
	bzy_coroutine_main_init();
	g_sched = bzy_coroutine_self();
}

void bzy_spawn(void (*entry)(void))
{
	Breeze *b = calloc(1, sizeof(*b));
	b->entry = entry;
	b->coroutine = bzy_coroutine_create(breeze_run, b);
	enqueue(b);
}

void bzy_yield(void)
{
	Breeze *b = g_running;
	if (!b)
	{
		return;                 /* Called from the scheduler itself: nothing to yield. */
	}

	enqueue(b);                 /* Re-queue at the tail (round-robin). */
	bzy_coroutine_switch(g_sched);
}

void bzy_sched_run(void)
{
	Breeze *b;
	while ((b = dequeue()) != NULL)
	{
		g_running = b;
		bzy_coroutine_switch(b->coroutine);
		g_running = NULL;
		if (b->done)
		{
			bzy_coroutine_delete(b->coroutine);
			free(b);
		}
	}
}
