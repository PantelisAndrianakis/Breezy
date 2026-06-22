#include "pollstate.h"

char bzy_pd_ready_sentinel;   /* Address-only sentinel; its value is never read. */

void bzy_poll_init(PollDesc *pd)
{
	__atomic_store_n(&pd->g[0], (void*)0, __ATOMIC_RELAXED);
	__atomic_store_n(&pd->g[1], (void*)0, __ATOMIC_RELAXED);
	pd->registered = 0;
	bzy_mutex_init(&pd->reg_lock);
}

void bzy_poll_reset(PollDesc *pd, int dir)
{
	void *expected = PD_READY;
	/* Clear only a sticky-ready word; never disturb a Waiter* (none can be present:
	   the owner is running, not parked). */
	__atomic_compare_exchange_n(&pd->g[dir], &expected, (void*)0,
								0, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED);
}

int bzy_poll_arm(PollDesc *pd, int dir, Waiter *w)
{
	void *expected = (void*)0;
	if (__atomic_compare_exchange_n(&pd->g[dir], &expected, (void*)w,
									0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
	{
		return 1;   /* Armed: must park. */
	}

	/* CAS failed: the only other reachable value is PD_READY (a pending edge).
	   Consume it and tell the caller not to park. */
	__atomic_store_n(&pd->g[dir], (void*)0, __ATOMIC_RELEASE);
	return 0;
}

Waiter *bzy_poll_unblock_ready(PollDesc *pd, int dir)
{
	void *g = __atomic_exchange_n(&pd->g[dir], PD_READY, __ATOMIC_ACQ_REL);
	if (g != (void*)0 && g != PD_READY)
	{
		return (Waiter*)g;   /* A breeze was parked; caller wakes it. */
	}

	return NULL;             /* No waiter; readiness left sticky for the next arm. */
}

int bzy_poll_unblock_timer(PollDesc *pd, int dir, Waiter *w)
{
	void *expected = (void*)w;
	/* Grab the waiter only if it is still parked (not already taken by an edge). */
	return __atomic_compare_exchange_n(&pd->g[dir], &expected, (void*)0,
									   0, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED) ? 1 : 0;
}
