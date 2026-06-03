#include "breezy.h"
#include "coroutine.h"
#include <stdlib.h>
#include <stdio.h>

typedef struct Breeze
{
	BzyCoroutine *coroutine;
	void (*entry)(void);        /* Zero-arg spawn (6a-1). */
	void (*thunk)(void*);       /* Arg'd spawn: codegen-emitted per-target thunk. */
	void *arg;                  /* Arg block for the thunk. */
	int done;
	struct Breeze *next;
} Breeze;

static Breeze *g_head, *g_tail;   /* Ready FIFO. */
static Breeze *g_running;         /* The breeze currently on the CPU (NULL = scheduler). */
static BzyCoroutine *g_sched;     /* The scheduler coroutine (the program's main fiber). */
static int64_t g_live;            /* Breezes created but not yet finished (for deadlock detection). */

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
	if (b->thunk)
	{
		b->thunk(b->arg);            /* Arg'd spawn: the thunk loads args, calls the target, frees the block. */
	}
	else
	{
		b->entry();
	}

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
	g_live++;
	enqueue(b);
}

void bzy_spawn_args(void (*thunk)(void*), void *arg)
{
	Breeze *b = calloc(1, sizeof(*b));
	b->thunk = thunk;
	b->arg = arg;
	b->coroutine = bzy_coroutine_create(breeze_run, b);
	g_live++;
	enqueue(b);
}

void *bzy_sched_current(void)      /* Opaque handle to the running breeze (for waiter lists). */
{
	return g_running;
}

void bzy_sched_park(void)          /* Suspend the running breeze; it stays off the ready queue. */
{
	bzy_coroutine_switch(g_sched);  /* A wake() must re-enqueue it, else it is lost. */
}

void bzy_sched_wake(void *breeze)  /* Make a parked breeze ready again. */
{
	enqueue((Breeze*)breeze);
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
	for (;;)
	{
		Breeze *b = dequeue();
		if (!b)
		{
			if (g_live > 0)
			{
				fprintf(stderr, "deadlock: all breezes blocked\n");   /* Parked with no one to wake them. */
				abort();
			}

			break;
		}

		g_running = b;
		bzy_coroutine_switch(b->coroutine);   /* A parked breeze switches back here without re-enqueueing. */
		g_running = NULL;
		if (b->done)
		{
			bzy_coroutine_delete(b->coroutine);
			free(b);
			g_live--;
		}
	}
}
