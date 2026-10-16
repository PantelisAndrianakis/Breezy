#include "breezy.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <windows.h>

/**
 * Full definition of the op declared opaque in breezy.h.
 * OVERLAPPED MUST be first so CONTAINING_RECORD(ov, struct IocpOp, ov) recovers the op in the completion thread.
 * The op lives on the issuing breeze's coroutine stack (stable while parked); the completion thread fills bytes/err, then wakes the breeze and never touches the op again.
 */
struct IocpOp
{
	OVERLAPPED ov;
	void   *breeze;
	SRWLOCK lock; // park_unlock hand-off: completion can't wake until breeze is off-CPU.
	DWORD   bytes;
	int     err;
};
_Static_assert(sizeof(struct IocpOp) <= BZY_IOCP_OP_SIZE, "BZY_IOCP_OP_SIZE too small.");

static HANDLE g_port; // The completion port.
static HANDLE *g_threads;
static int     g_nthreads;
static volatile LONG g_inflight; // Breezes parked on a network op.
static int     g_started; // Guarded by g_start_lock.
static SRWLOCK g_start_lock = SRWLOCK_INIT;
static int     g_wsa_started;
static int     g_skip; // 1 when sockets complete inline without a packet (FILE_SKIP_COMPLETION_PORT_ON_SUCCESS).

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
			break; // Shutdown sentinel (posted by bzy_iocp_shutdown).
		}

		if (!ov)
		{
			continue; // Spurious / timeout (we use INFINITE, so unlikely).
		}

		struct IocpOp *op = CONTAINING_RECORD(ov, struct IocpOp, ov);
		op->bytes = bytes;
		op->err = ok ? 0 : (int)GetLastError(); // Failed op: the error rode the completion.

		// Wait until the breeze is fully parked (it holds op->lock; the scheduler releases it after the parking switch -- bzy_iocp_park uses park_unlock).
		AcquireSRWLockExclusive(&op->lock);
		void *breeze = op->breeze;
		ReleaseSRWLockExclusive(&op->lock);

		bzy_sched_wake_external(breeze);
		InterlockedDecrement(&g_inflight);
	}

	return 0;
}

/**
 * Inline completion is only reliable when every TCP and UDP provider hands out real kernel handles.
 * A layered provider can report success inline and still queue a packet, which would wake a breeze whose op is gone.
 */
static int ifs_handles_only(void)
{
	INT protos[3] = { IPPROTO_TCP, IPPROTO_UDP, 0 };
	DWORD len = 0;
	WSAEnumProtocolsW(protos, NULL, &len); // Fails with WSAENOBUFS and sets the size needed.
	if (len == 0)
	{
		return 0;
	}

	WSAPROTOCOL_INFOW *info = malloc(len);
	int n = WSAEnumProtocolsW(protos, info, &len);
	int ok = n > 0;
	for (int i = 0; i < n; i++)
	{
		if (!(info[i].dwServiceFlags1 & XP1_IFS_HANDLES))
		{
			ok = 0;
			break;
		}
	}

	free(info);
	return ok;
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
			g_skip = ifs_handles_only();
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
	CreateIoCompletionPort((HANDLE)handle, g_port, 0, 0); // Key unused: we recover the op from ov.
	if (g_skip && !SetFileCompletionNotificationModes((HANDLE)handle, FILE_SKIP_COMPLETION_PORT_ON_SUCCESS | FILE_SKIP_SET_EVENT_ON_HANDLE))
	{
		// Every issue site trusts inline completion once skip mode is on; a handle left in the other mode would queue a packet for an op that no longer exists.
		fprintf(stderr, "Could not switch a socket to inline completions.\n");
		abort();
	}
}

void bzy_iocp_op_reset(IocpOp *op)
{
	struct IocpOp *o = (struct IocpOp*)op;
	memset(&o->ov, 0, sizeof(o->ov));
	InitializeSRWLock(&o->lock);
	o->breeze = bzy_sched_current();
	o->bytes = 0;
	o->err = 0;
	// Hold the handshake lock BEFORE the caller posts the overlapped op, not after (in bzy_iocp_park).
	// IOCP queues a completion even on immediate success, and a completion thread blocks on this lock to wait for the park; taking it first guarantees it cannot wake a not-yet-parked breeze and resume the coroutine on two threads.
	// (The Linux reactor had the mirror-image bug.)
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
	// o->lock is already held (taken in bzy_iocp_op_reset, before the overlapped post); the scheduler releases it after the parking switch.
	bzy_sched_park_unlock(&o->lock);
	// Resumed: the completion thread filled bytes/err and woke us.
}

int bzy_iocp_done_now(IocpOp *op, int ok, unsigned long bytes)
{
	if (!ok || !g_skip)
	{
		return 0; // Pending, or packets are queued even on inline success: park for the completion.
	}

	struct IocpOp *o = (struct IocpOp*)op;
	o->bytes = (DWORD)bytes;
	o->err = 0;
	ReleaseSRWLockExclusive(&o->lock); // Taken in bzy_iocp_op_reset; no completion will ever wait on it.
	return 1;
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
			PostQueuedCompletionStatus(g_port, 0, 0xFFFFFFFFull, NULL); // Sentinel per thread.
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

/**
 * The scheduler's network poll hooks.
 * Completions are still drained by the completion thread above, so workers have nothing to poll.
 */
int bzy_netpoll(int64_t timeout_ms)
{
	(void)timeout_ms;
	return 0;
}

void bzy_netpoll_break(void)
{
}

int64_t bzy_netpoll_pending(void)
{
	return 0;
}
