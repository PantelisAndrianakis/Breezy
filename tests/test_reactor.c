/* Unit test of the PollDesc state machine (no epoll, no scheduler). Models the
   three actors - owner (arm/reset), reactor (unblock_ready), timer
   (unblock_timer) - asserting no lost wakeup and no double-grab. */
#include "pollstate.h"
#include <stdio.h>
#include <string.h>

static int failures = 0;
#define CHECK(c, msg) do { if (!(c)) { printf("FAIL: %s\n", msg); failures++; } } while (0)

/* 1. Arm-then-edge: a parked waiter is returned exactly once by unblock_ready. */
static void test_arm_then_edge(void)
{
	PollDesc pd; bzy_poll_init(&pd);
	Waiter w; memset(&w, 0, sizeof(w)); w.breeze = (void*)0x1; bzy_mutex_init(&w.lock);
	int must_park = bzy_poll_arm(&pd, BZY_POLL_READ, &w);
	CHECK(must_park == 1, "arm on idle returns must-park");
	Waiter *got = bzy_poll_unblock_ready(&pd, BZY_POLL_READ);
	CHECK(got == &w, "edge returns the armed waiter");
	Waiter *again = bzy_poll_unblock_ready(&pd, BZY_POLL_READ);
	CHECK(again == NULL, "second edge returns no waiter (already ready)");
}

/* 2. Edge-before-arm: sticky readiness means arm consumes it and does NOT park. */
static void test_edge_then_arm(void)
{
	PollDesc pd; bzy_poll_init(&pd);
	Waiter *got = bzy_poll_unblock_ready(&pd, BZY_POLL_READ);
	CHECK(got == NULL, "edge with no waiter returns NULL");
	Waiter w; memset(&w, 0, sizeof(w)); w.breeze = (void*)0x1; bzy_mutex_init(&w.lock);
	int must_park = bzy_poll_arm(&pd, BZY_POLL_READ, &w);
	CHECK(must_park == 0, "arm after sticky edge consumes it, no park");
}

/* 3. reset clears a sticky edge. */
static void test_reset(void)
{
	PollDesc pd; bzy_poll_init(&pd);
	(void)bzy_poll_unblock_ready(&pd, BZY_POLL_READ);   /* set PD_READY */
	bzy_poll_reset(&pd, BZY_POLL_READ);
	Waiter w; memset(&w, 0, sizeof(w)); w.breeze = (void*)0x1; bzy_mutex_init(&w.lock);
	int must_park = bzy_poll_arm(&pd, BZY_POLL_READ, &w);
	CHECK(must_park == 1, "after reset, arm must park (edge was cleared)");
}

/* 4. Timer vs edge: exactly one of them grabs the waiter. */
static void test_timer_vs_edge(void)
{
	PollDesc pd; bzy_poll_init(&pd);
	Waiter w; memset(&w, 0, sizeof(w)); w.breeze = (void*)0x1; bzy_mutex_init(&w.lock);
	(void)bzy_poll_arm(&pd, BZY_POLL_READ, &w);
	Waiter *e = bzy_poll_unblock_ready(&pd, BZY_POLL_READ);   /* edge grabs it */
	int t = bzy_poll_unblock_timer(&pd, BZY_POLL_READ, &w);   /* timer loses */
	CHECK(e == &w, "edge grabbed the waiter");
	CHECK(t == 0, "timer did not double-grab");
}

/* 5. Independent directions: a read edge does not disturb a write waiter. */
static void test_directions_independent(void)
{
	PollDesc pd; bzy_poll_init(&pd);
	Waiter wr; memset(&wr, 0, sizeof(wr)); wr.breeze = (void*)0x1; bzy_mutex_init(&wr.lock);
	Waiter ww; memset(&ww, 0, sizeof(ww)); ww.breeze = (void*)0x2; bzy_mutex_init(&ww.lock);
	(void)bzy_poll_arm(&pd, BZY_POLL_READ, &wr);
	(void)bzy_poll_arm(&pd, BZY_POLL_WRITE, &ww);
	Waiter *r = bzy_poll_unblock_ready(&pd, BZY_POLL_READ);
	CHECK(r == &wr, "read edge wakes the read waiter");
	Waiter *w2 = bzy_poll_unblock_ready(&pd, BZY_POLL_WRITE);
	CHECK(w2 == &ww, "write waiter still parked, woken by its own edge");
}

int main(void)
{
	test_arm_then_edge();
	test_edge_then_arm();
	test_reset();
	test_timer_vs_edge();
	test_directions_independent();
	if (failures == 0) { printf("test_reactor: all passed\n"); return 0; }
	printf("test_reactor: %d FAILED\n", failures);
	return 1;
}
