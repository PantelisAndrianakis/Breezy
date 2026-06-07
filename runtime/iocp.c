#include "breezy.h"
#include <stdlib.h>
#include <string.h>
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <windows.h>

/* Full definition of the op declared opaque in breezy.h. OVERLAPPED MUST be first
   so CONTAINING_RECORD(ov, struct IocpOp, ov) recovers the op in the completion
   thread. The op lives on the issuing breeze's coroutine stack (stable while
   parked); the completion thread fills bytes/err, then wakes the breeze and never
   touches the op again. */
struct IocpOp
{
	OVERLAPPED ov;
	void   *breeze;
	SRWLOCK lock;       /* park_unlock hand-off: completion can't wake until breeze is off-CPU. */
	DWORD   bytes;
	int     err;
};
_Static_assert(sizeof(struct IocpOp) <= BZY_IOCP_OP_SIZE, "BZY_IOCP_OP_SIZE too small.");

static HANDLE g_port;                 /* The completion port. */
static HANDLE *g_threads;
static int     g_nthreads;
static volatile LONG g_inflight;      /* Breezes parked on a network op. */
static int     g_started;             /* Guarded by g_start_lock. */
static SRWLOCK g_start_lock = SRWLOCK_INIT;
static int     g_wsa_started;

/* Test hook: lets the unit test post a fake completion to the real port. */
void *bzy_iocp_test_port(void)
{
	return (void*)g_port;
}

static DWORD WINAPI completion_thread(void *unused)
{
	(void)unused;
	for (;;)
	{
		DWORD bytes = 0;
		ULONG_PTR key = 0;
		OVERLAPPED *ov = NULL;
		BOOL ok = GetQueuedCompletionStatus(g_port, &bytes, &key, &ov, INFINITE);

		if (!ov && key == 0xFFFFFFFFull)
		{
			break;                    /* Shutdown sentinel (posted by bzy_iocp_shutdown). */
		}

		if (!ov)
		{
			continue;                 /* Spurious / timeout (we use INFINITE, so unlikely). */
		}

		struct IocpOp *op = CONTAINING_RECORD(ov, struct IocpOp, ov);
		op->bytes = bytes;
		op->err = ok ? 0 : (int)GetLastError();   /* Failed op: the error rode the completion. */

		/* Wait until the breeze is fully parked (it holds op->lock; the scheduler
		   releases it after the parking switch -- bzy_iocp_park uses park_unlock). */
		AcquireSRWLockExclusive(&op->lock);
		void *breeze = op->breeze;
		ReleaseSRWLockExclusive(&op->lock);

		bzy_sched_wake_external(breeze);
		InterlockedDecrement(&g_inflight);
	}

	return 0;
}

void bzy_iocp_ensure(void)
{
	AcquireSRWLockExclusive(&g_start_lock);
	if (!g_started)
	{
		if (!g_wsa_started)
		{
			WSADATA wsa;
			WSAStartup(MAKEWORD(2, 2), &wsa);
			g_wsa_started = 1;
		}

		g_port = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 0);

		const char *env = getenv("BZY_IOCP_THREADS");
		int n = env ? atoi(env) : 1;
		if (n < 1)
		{
			n = 1;
		}

		if (n > 64)
		{
			n = 64;
		}

		g_threads = calloc((size_t)n, sizeof(HANDLE));
		for (int i = 0; i < n; i++)
		{
			g_threads[i] = CreateThread(NULL, 0, completion_thread, NULL, 0, NULL);
		}

		g_nthreads = n;
		g_started = 1;
	}

	ReleaseSRWLockExclusive(&g_start_lock);
}

void bzy_iocp_associate(void *handle)
{
	CreateIoCompletionPort((HANDLE)handle, g_port, 0, 0);   /* Key unused: we recover the op from ov. */
}

void bzy_iocp_op_reset(IocpOp *op)
{
	struct IocpOp *o = (struct IocpOp*)op;
	memset(&o->ov, 0, sizeof(o->ov));
	InitializeSRWLock(&o->lock);
	o->breeze = bzy_sched_current();
	o->bytes = 0;
	o->err = 0;
	/* Hold the handshake lock BEFORE the caller posts the overlapped op, not after
	   (in bzy_iocp_park). IOCP queues a completion even on immediate success, and a
	   completion thread blocks on this lock to wait for the park; taking it first
	   guarantees it cannot wake a not-yet-parked breeze and resume the coroutine on
	   two threads. (The Linux reactor had the mirror-image bug.) */
	AcquireSRWLockExclusive(&o->lock);
}

void *bzy_iocp_op_overlapped(IocpOp *op)
{
	return &((struct IocpOp*)op)->ov;
}

void bzy_iocp_park(IocpOp *op)
{
	struct IocpOp *o = (struct IocpOp*)op;
	InterlockedIncrement(&g_inflight);
	/* o->lock is already held (taken in bzy_iocp_op_reset, before the overlapped
	   post); the scheduler releases it after the parking switch. */
	bzy_sched_park_unlock(&o->lock);
	/* Resumed: the completion thread filled bytes/err and woke us. */
}

unsigned long bzy_iocp_op_bytes(IocpOp *op)
{
	return ((struct IocpOp*)op)->bytes;
}

int bzy_iocp_op_err(IocpOp *op)
{
	return ((struct IocpOp*)op)->err;
}

int64_t bzy_iocp_inflight(void)
{
	return (int64_t)g_inflight;
}

void bzy_iocp_shutdown(void)
{
	AcquireSRWLockExclusive(&g_start_lock);
	if (g_started)
	{
		for (int i = 0; i < g_nthreads; i++)
		{
			PostQueuedCompletionStatus(g_port, 0, 0xFFFFFFFFull, NULL);   /* Sentinel per thread. */
		}

		WaitForMultipleObjects((DWORD)g_nthreads, g_threads, TRUE, INFINITE);
		for (int i = 0; i < g_nthreads; i++)
		{
			CloseHandle(g_threads[i]);
		}

		free(g_threads);
		g_threads = NULL;
		CloseHandle(g_port);
		g_port = NULL;
		g_nthreads = 0;
		g_started = 0;
	}

	ReleaseSRWLockExclusive(&g_start_lock);
}
