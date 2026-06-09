#ifndef BZY_POLLSTATE_H
#define BZY_POLLSTATE_H
#include <stdint.h>
#include "platform.h"

/* Direction indices into PollDesc.g[]. */
#define BZY_POLL_READ  0
#define BZY_POLL_WRITE 1

/* Sentinel for "an edge arrived but no breeze was waiting" - sticky until a
   syscall returns EAGAIN and the owner resets it. Distinct from 0 and from any
   real Waiter pointer (it is an address inside the runtime, never an allocated
   Waiter). */
extern char bzy_pd_ready_sentinel;
#define PD_READY ((void*)&bzy_pd_ready_sentinel)

/* A single park. Lives on the parked breeze's coroutine stack (stable while
   parked, exactly like the old ReactorOp). */
typedef struct
{
	void      *breeze;   /* bzy_sched_current() of the parked breeze. */
	int        result;   /* Set by the waker: 1 = ready, 0 = timed out, -1 = error. */
	bzy_mutex  lock;     /* Park handshake: held by the breeze across the park switch. */
} Waiter;

/* Per-fd readiness, embedded in the socket handle. g[dir] is 0 / PD_READY / Waiter*. */
typedef struct
{
	void      *g[2];          /* __atomic-accessed read/write readiness words. */
	int        registered;    /* fd ADDed to epoll yet? Guarded by reg_lock. */
	bzy_mutex  reg_lock;      /* One-time ADD + close teardown. */
} PollDesc;

void  bzy_poll_init(PollDesc *pd);          /* Zero state + init reg_lock. */

/* Clear a stale ready bit before retrying the syscall. CAS PD_READY -> 0; leaves a
   Waiter* untouched (only the owner calls this, and only when not parked, so the
   word is 0 or PD_READY). */
void  bzy_poll_reset(PollDesc *pd, int dir);

/* Try to arm `w` as the waiter on dir. Returns 1 if it must park (armed), or 0 if
   an edge was already pending (consumed; do NOT park - retry the syscall). */
int   bzy_poll_arm(PollDesc *pd, int dir, Waiter *w);

/* Reactor edge: mark dir ready and return the Waiter to wake (or NULL if none).
   The caller sets the Waiter's result before the handshake. */
Waiter *bzy_poll_unblock_ready(PollDesc *pd, int dir);

/* Timer fire: try to grab THIS waiter (CAS w -> 0). Returns 1 if grabbed (caller
   wakes it as timed-out), 0 if a readiness edge grabbed it first. */
int   bzy_poll_unblock_timer(PollDesc *pd, int dir, Waiter *w);

/* Linux epoll glue (reactor_epoll.c). Declared here so socket.c/udp.c need only
   include this header. On non-Linux these are unused. */
void    bzy_reactor_ensure(void);
int     bzy_poll_wait(PollDesc *pd, int fd, int dir, int64_t timeout_ms);
void    bzy_reactor_deregister(int fd);

#endif
