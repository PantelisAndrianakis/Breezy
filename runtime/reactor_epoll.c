#include "breezy.h"
#include "platform.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/epoll.h>
#include <sys/timerfd.h>
#include <sys/eventfd.h>

/* The Linux readiness reactor: the epoll counterpart to iocp.c. A breeze registers
   a fd + interest (read/write, one-shot) and parks; the reactor thread's epoll_wait
   wakes it when the fd is ready (or its timeout fires), and the breeze then performs
   the non-blocking syscall itself. Mirrors the iocp park/wake handshake: the op
   holds a mutex across the parking switch so the reactor cannot wake the breeze
   until it is fully off the CPU. */

/* Per-wait op: a stack node on the parked breeze's coroutine stack (stable while
   parked). 8-byte aligned, so the low pointer bit is free to tag the timer fd. */
typedef struct
{
	int       fd;        /* The socket fd the breeze waits on. */
	int       tfd;       /* timerfd for a bounded wait, or -1. */
	void     *breeze;
	int       ready;     /* Out: 1 = fd ready, 0 = timed out. */
	int       done;      /* Reactor-side dedup (fd + timer in one batch). */
	bzy_mutex lock;      /* Park handshake (scheduler releases after the switch). */
} ReactorOp;

#define REACTOR_MAX_THREADS 8      /* Cap on parallel reactor threads. */

static int        g_ep = -1;       /* The epoll instance. */
static int        g_evfd = -1;     /* eventfd: shutdown wakeup. */
static bzy_thread  g_threads[REACTOR_MAX_THREADS];   /* Pool draining one shared epoll fd. */
static int        g_nthreads;      /* How many of g_threads are live. */
static int        g_started;
static int        g_inflight;      /* Breezes parked on the reactor (via __atomic). */
static bzy_mutex  g_start_lock = BZY_MUTEX_INIT;

static void *reactor_loop(void *unused)
{
	(void)unused;
	struct epoll_event evs[64];
	for (;;)
	{
		int n = epoll_wait(g_ep, evs, 64, -1);
		if (n < 0)
		{
			if (errno == EINTR)
			{
				continue;
			}

			break;
		}

		for (int i = 0; i < n; i++)
		{
			void *p = evs[i].data.ptr;
			if (!p)
			{
				return NULL;   /* Shutdown eventfd (registered with data.ptr == NULL). */
			}

			int is_timer = (int)((uintptr_t)p & 1u);
			ReactorOp *op = (ReactorOp*)((uintptr_t)p & ~(uintptr_t)1u);
			if (op->done)
			{
				continue;      /* The fd and its timer both fired this batch: handle once. */
			}

			op->done = 1;
			op->ready = is_timer ? 0 : 1;
			epoll_ctl(g_ep, EPOLL_CTL_DEL, op->fd, NULL);
			if (op->tfd >= 0)
			{
				epoll_ctl(g_ep, EPOLL_CTL_DEL, op->tfd, NULL);
				close(op->tfd);
				op->tfd = -1;
			}

			/* Handshake: wait until the breeze is fully parked (it holds op->lock;
			   the scheduler releases it after the parking switch). */
			bzy_mutex_lock(&op->lock);
			void *breeze = op->breeze;
			bzy_mutex_unlock(&op->lock);

			bzy_sched_wake_external(breeze);
			__atomic_sub_fetch(&g_inflight, 1, __ATOMIC_SEQ_CST);
		}
	}

	return NULL;
}

void bzy_reactor_ensure(void)
{
	bzy_mutex_lock(&g_start_lock);
	if (!g_started)
	{
		g_ep = epoll_create1(0);
		g_evfd = eventfd(0, 0);
		struct epoll_event ev;
		memset(&ev, 0, sizeof(ev));
		ev.events = EPOLLIN;
		ev.data.ptr = NULL;   /* The shutdown sentinel. */
		epoll_ctl(g_ep, EPOLL_CTL_ADD, g_evfd, &ev);

		/* Drain epoll on a pool of threads, not one: a single reactor thread
		   serialises every wakeup, so per-round-trip latency grows linearly with
		   concurrent connections. EPOLLONESHOT delivers each op's event to exactly
		   one thread (and per-op state lives on the parked breeze's stack), so the
		   threads need no coordination beyond the already-atomic inflight counter
		   and the locked injection queue. Size to the cores, capped. */
		int nt = (int)sysconf(_SC_NPROCESSORS_ONLN);
		if (nt < 1)
		{
			nt = 1;
		}
		if (nt > REACTOR_MAX_THREADS)
		{
			nt = REACTOR_MAX_THREADS;
		}

		g_nthreads = nt;
		for (int i = 0; i < g_nthreads; i++)
		{
			bzy_thread_start(&g_threads[i], reactor_loop, NULL);
		}

		g_started = 1;
	}

	bzy_mutex_unlock(&g_start_lock);
}

/* Park the breeze until `fd` is ready for read (or write), or `timeout_ms` elapses.
   timeout_ms < 0 = infinite; == 0 = return 0 now (non-parking, for the try-* ops).
   Returns 1 = ready, 0 = timed out, -1 = error. */
int bzy_reactor_wait(int fd, int want_write, int64_t timeout_ms)
{
	if (timeout_ms == 0)
	{
		return 0;   /* Caller's try-path treats this as "would block". */
	}

	bzy_reactor_ensure();

	ReactorOp op;
	op.fd = fd;
	op.tfd = -1;
	op.breeze = bzy_sched_current();
	op.ready = 0;
	op.done = 0;
	bzy_mutex_init(&op.lock);
	/* Hold op.lock BEFORE the fd is registered, not after. The reactor's wake path
	   blocks on this lock until the scheduler releases it post-park, so taking it
	   first guarantees the reactor can never observe the event and wake this breeze
	   before it has finished parking. Registering first left a window in which a
	   reactor thread could grab the lock and wake a not-yet-parked breeze, resuming
	   the same coroutine on two workers (stack corruption). Rare with one reactor
	   thread, frequent with several. */
	bzy_mutex_lock(&op.lock);

	struct epoll_event ev;
	memset(&ev, 0, sizeof(ev));
	ev.events = (want_write ? EPOLLOUT : EPOLLIN) | EPOLLONESHOT;
	ev.data.ptr = &op;
	if (epoll_ctl(g_ep, EPOLL_CTL_ADD, fd, &ev) != 0)
	{
		bzy_mutex_unlock(&op.lock);
		return -1;
	}

	if (timeout_ms > 0)
	{
		op.tfd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK);
		struct itimerspec its;
		memset(&its, 0, sizeof(its));
		its.it_value.tv_sec = (time_t)(timeout_ms / 1000);
		its.it_value.tv_nsec = (long)((timeout_ms % 1000) * 1000000L);
		if (its.it_value.tv_sec == 0 && its.it_value.tv_nsec == 0)
		{
			its.it_value.tv_nsec = 1;   /* A 0 itimerspec disarms; arm minimally. */
		}

		timerfd_settime(op.tfd, 0, &its, NULL);
		struct epoll_event tev;
		memset(&tev, 0, sizeof(tev));
		tev.events = EPOLLIN | EPOLLONESHOT;
		tev.data.ptr = (void*)((uintptr_t)&op | 1u);   /* Low-bit tag = the timer. */
		epoll_ctl(g_ep, EPOLL_CTL_ADD, op.tfd, &tev);
	}

	__atomic_add_fetch(&g_inflight, 1, __ATOMIC_SEQ_CST);
	bzy_sched_park_unlock(&op.lock);     /* op.lock (already held) is released after the switch. */
	/* Resumed: the reactor set op.ready and cleaned up the fd/timer registrations. */
	return op.ready;
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
		uint64_t one = 1;
		ssize_t w = write(g_evfd, &one, sizeof(one));   /* Wake the reactors to exit. */
		(void)w;
		/* The sentinel is level-triggered and never read, so it stays signalled and
		   every reactor thread's epoll_wait returns it; join them all. */
		for (int i = 0; i < g_nthreads; i++)
		{
			bzy_thread_join(g_threads[i]);
		}

		g_nthreads = 0;
		close(g_evfd);
		close(g_ep);
		g_evfd = -1;
		g_ep = -1;
		g_started = 0;
	}

	bzy_mutex_unlock(&g_start_lock);
}
