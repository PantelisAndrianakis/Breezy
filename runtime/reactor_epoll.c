#include "breezy.h"
#include "platform.h"
#include "pollstate.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <limits.h>
#include <unistd.h>
#include <sys/epoll.h>
#include <sys/timerfd.h>
#include <sys/eventfd.h>

/**
 * Linux readiness reactor (edge-triggered, register-once).
 * Each fd is ADDed to the shared epoll instance exactly once (EPOLLIN|EPOLLOUT|EPOLLET, data.ptr = its PollDesc) on the breeze's first park; never re-armed, never DELeted per op (the kernel drops it on close).
 * Edges are dispatched into the PollDesc, whose sticky readiness guarantees no wakeup is lost across the reset->syscall->wait window.
 * The scheduler's workers drain the one epoll fd themselves through bzy_netpoll; EPOLLET + the PollDesc atomics make concurrent drain safe (each edge resolves to at most one Waiter per direction).
 * An eventfd (data.ptr NULL, level-triggered) breaks a worker out of a blocking poll.
 */

/* epoll data.ptr low-bit tag: 0 = a PollDesc (socket edge), 1 = a TimerReg. */
#define TAG_TIMER 1u

/* Per-timed-wait registration, on the waiting breeze's stack next to its Waiter. */
typedef struct
{
	PollDesc *pd;
	int       dir;
	Waiter   *w;
	int       tfd;
} TimerReg;

static int        g_ep = -1;
static int        g_evfd = -1;
static int        g_started;
static int        g_inflight; // Parked breezes (deadlock gate and poll gate).
static bzy_mutex  g_start_lock = BZY_MUTEX_INIT;

/* Wake `w`'s breeze after the park handshake: take w->lock (blocks until the breeze released it post-switch) then make it runnable. w->result must already be set. */
static void wake_waiter(Waiter *w)
{
	bzy_mutex_lock(&w->lock);
	bzy_mutex_unlock(&w->lock);
	bzy_sched_ready(w->breeze);
	__atomic_sub_fetch(&g_inflight, 1, __ATOMIC_SEQ_CST);
}

/**
 * Resolve a batch of epoll events to parked Waiters and wake them.
 * An entry with a NULL data.ptr is a break on the eventfd; only a blocking poll (`blocking` set) drains it.
 * A non-blocking poll that drained it would steal the wakeup from the blocked poller, which the kernel then puts back to sleep with the break already spent.
 * Returns the number of breezes woken.
 */
static int reactor_dispatch(struct epoll_event *evs, int n, int blocking)
{
	int woken = 0;
	for (int i = 0; i < n; i++)
	{
		void *p = evs[i].data.ptr;
		if (!p)
		{
			if (blocking)
			{
				uint64_t v;
				ssize_t rd = read(g_evfd, &v, sizeof(v)); // Drain it so the next blocking poll can sleep again.
				(void)rd;
			}

			continue;
		}

		if ((uintptr_t)p & TAG_TIMER)
		{
			TimerReg *tr = (TimerReg*)((uintptr_t)p & ~(uintptr_t)TAG_TIMER);
			if (bzy_poll_unblock_timer(tr->pd, tr->dir, tr->w))
			{
				tr->w->result = 0; // Timed out.
				wake_waiter(tr->w);
				woken++;
			}

			continue;
		}

		PollDesc *pd = (PollDesc*)p;
		uint32_t ev = evs[i].events;
		// A half-closed/errored fd reports HUP/ERR; treat as both directions ready so the parked side wakes and the syscall surfaces the real errno/EOF.
		if (ev & (EPOLLIN | EPOLLHUP | EPOLLERR))
		{
			Waiter *w = bzy_poll_unblock_ready(pd, BZY_POLL_READ);
			if (w)
			{
				w->result = 1;
				wake_waiter(w);
				woken++;
			}
		}

		if (ev & (EPOLLOUT | EPOLLHUP | EPOLLERR))
		{
			Waiter *w = bzy_poll_unblock_ready(pd, BZY_POLL_WRITE);
			if (w)
			{
				w->result = 1;
				wake_waiter(w);
				woken++;
			}
		}
	}

	return woken;
}

int bzy_netpoll(int64_t timeout_ms)
{
	if (!__atomic_load_n(&g_started, __ATOMIC_ACQUIRE))
	{
		return 0;
	}

	struct epoll_event evs[64];
	int to = timeout_ms < 0 ? -1 : (timeout_ms > INT_MAX ? INT_MAX : (int)timeout_ms);
	int n = epoll_wait(g_ep, evs, 64, to);
	if (n <= 0)
	{
		return 0; // Timed out, or interrupted (EINTR): the caller loops back anyway.
	}

	return reactor_dispatch(evs, n, timeout_ms != 0);
}

void bzy_netpoll_break(void)
{
	if (__atomic_load_n(&g_started, __ATOMIC_ACQUIRE))
	{
		uint64_t one = 1;
		ssize_t wr = write(g_evfd, &one, sizeof(one));
		(void)wr;
	}
}

int64_t bzy_netpoll_pending(void)
{
	return (int64_t)__atomic_load_n(&g_inflight, __ATOMIC_SEQ_CST);
}

void bzy_reactor_ensure(void)
{
	bzy_mutex_lock(&g_start_lock);
	if (!g_started)
	{
		g_ep = epoll_create1(EPOLL_CLOEXEC);
		g_evfd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
		struct epoll_event ev;
		memset(&ev, 0, sizeof(ev));
		ev.events = EPOLLIN; // Level-triggered: every poll sees a pending break until one drains it.
		ev.data.ptr = NULL;
		epoll_ctl(g_ep, EPOLL_CTL_ADD, g_evfd, &ev);
		__atomic_store_n(&g_started, 1, __ATOMIC_RELEASE);
	}

	bzy_mutex_unlock(&g_start_lock);
}

/* Register fd in epoll once (EPOLLET, both directions). Returns 0 ok, -1 on error. */
static int ensure_registered(PollDesc *pd, int fd)
{
	if (__atomic_load_n(&pd->registered, __ATOMIC_ACQUIRE))
	{
		return 0;
	}

	int rc = 0;
	bzy_mutex_lock(&pd->reg_lock);
	if (!pd->registered)
	{
		struct epoll_event ev;
		memset(&ev, 0, sizeof(ev));
		ev.events = EPOLLIN | EPOLLOUT | EPOLLET;
		ev.data.ptr = pd;
		if (epoll_ctl(g_ep, EPOLL_CTL_ADD, fd, &ev) == 0)
		{
			__atomic_store_n(&pd->registered, 1, __ATOMIC_RELEASE);
		}
		else
		{
			rc = -1;
		}
	}

	bzy_mutex_unlock(&pd->reg_lock);
	return rc;
}

/**
 * Park the breeze until `fd` is ready in `dir` (BZY_POLL_READ/WRITE), or timeout. timeout_ms < 0 = infinite; == 0 = "would block" (non-parking try-path).
 * Returns 1 ready, 0 timed out, -1 error.
 */
int bzy_poll_wait(PollDesc *pd, int fd, int dir, int64_t timeout_ms)
{
	if (timeout_ms == 0)
	{
		return 0;
	}

	bzy_reactor_ensure();
	if (ensure_registered(pd, fd) != 0)
	{
		return -1;
	}

	Waiter w;
	w.breeze = bzy_sched_current();
	w.result = 0;
	bzy_mutex_init(&w.lock);
	bzy_mutex_lock(&w.lock); // Held across the park; the waking worker blocks on it.

	if (!bzy_poll_arm(pd, dir, &w))
	{
		bzy_mutex_unlock(&w.lock); // A pending edge was consumed - do not park.
		return 1;
	}

	TimerReg tr;
	tr.tfd = -1;
	if (timeout_ms > 0)
	{
		tr.pd = pd;
		tr.dir = dir;
		tr.w = &w;
		tr.tfd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK);
		struct itimerspec its;
		memset(&its, 0, sizeof(its));
		its.it_value.tv_sec = (time_t)(timeout_ms / 1000);
		its.it_value.tv_nsec = (long)((timeout_ms % 1000) * 1000000L);
		if (its.it_value.tv_sec == 0 && its.it_value.tv_nsec == 0)
		{
			its.it_value.tv_nsec = 1; // A 0 itimerspec disarms; arm minimally.
		}

		timerfd_settime(tr.tfd, 0, &its, NULL);
		struct epoll_event tev;
		memset(&tev, 0, sizeof(tev));
		tev.events = EPOLLIN;
		tev.data.ptr = (void*)((uintptr_t)&tr | TAG_TIMER);
		epoll_ctl(g_ep, EPOLL_CTL_ADD, tr.tfd, &tev);
	}

	if (__atomic_add_fetch(&g_inflight, 1, __ATOMIC_SEQ_CST) == 1)
	{
		bzy_sched_io_parked(); // The first parked I/O: make sure some worker will poll for it.
	}

	bzy_sched_park_unlock(&w.lock); // Released by the scheduler after the switch.

	// Resumed: a polling worker set w.result; the fd stays registered.
	// Tear down the per-wait timer registration (if any).
	if (tr.tfd >= 0)
	{
		epoll_ctl(g_ep, EPOLL_CTL_DEL, tr.tfd, NULL);
		close(tr.tfd);
	}

	return w.result;
}

/* Remove an fd from epoll (used by connect's throwaway stack PollDesc hand-off). */
void bzy_reactor_deregister(int fd)
{
	if (g_ep >= 0)
	{
		epoll_ctl(g_ep, EPOLL_CTL_DEL, fd, NULL);
	}
}

/* The scheduler's deadlock gate consults this (Windows: iocp.c; Linux: here). */
int64_t bzy_iocp_inflight(void)
{
	return (int64_t)__atomic_load_n(&g_inflight, __ATOMIC_SEQ_CST);
}

void bzy_reactor_shutdown(void)
{
	bzy_mutex_lock(&g_start_lock);
	if (g_started)
	{
		close(g_evfd);
		close(g_ep);
		g_evfd = -1;
		g_ep = -1;
		__atomic_store_n(&g_started, 0, __ATOMIC_RELEASE);
	}

	bzy_mutex_unlock(&g_start_lock);
}
