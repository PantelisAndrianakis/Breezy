/* Platform sync seam: a thin wrapper over the host's mutex / counting semaphore /
   thread primitives so the scheduler, channel, and timer code is written once.
   The Windows side wraps exactly today's primitives (SRWLOCK + a counting HANDLE
   semaphore), so the Windows build stays behavior-identical; the POSIX side uses
   pthreads + a POSIX semaphore. Consumed by scheduler.c / channel.c (Part 8-2). */
#ifndef BZY_PLATFORM_H
#define BZY_PLATFORM_H
#include <stdint.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
typedef SRWLOCK bzy_mutex;
typedef HANDLE  bzy_sem;
#define BZY_MUTEX_INIT SRWLOCK_INIT
static inline void bzy_mutex_init(bzy_mutex *m)
{
	InitializeSRWLock(m);
}

static inline void bzy_mutex_lock(bzy_mutex *m)
{
	AcquireSRWLockExclusive(m);
}

static inline void bzy_mutex_unlock(bzy_mutex *m)
{
	ReleaseSRWLockExclusive(m);
}

static inline void bzy_sem_init(bzy_sem *s)
{
	*s = CreateSemaphore(NULL, 0, 0x7fffffff, NULL);
}

static inline void bzy_sem_post(bzy_sem *s, int n)
{
	ReleaseSemaphore(*s, (LONG)n, NULL);
}

static inline void bzy_sem_wait(bzy_sem *s)
{
	WaitForSingleObject(*s, INFINITE);
}
/* Returns 1 if signaled, 0 if timed out. */
static inline int  bzy_sem_wait_ms(bzy_sem *s, int64_t ms)
{
	return WaitForSingleObject(*s, (DWORD)ms) == WAIT_OBJECT_0;
}

static inline void bzy_sem_destroy(bzy_sem *s)
{
	CloseHandle(*s);
}

typedef HANDLE bzy_thread;
static inline void bzy_thread_start(bzy_thread *t, void *(*fn)(void*), void *arg)
{
	*t = CreateThread(NULL, 0, (LPTHREAD_START_ROUTINE)(void*)fn, arg, 0, NULL);
}

static inline void bzy_thread_join(bzy_thread t)
{
	WaitForSingleObject(t, INFINITE);
	CloseHandle(t);
}
#else
#include <pthread.h>
#include <semaphore.h>
#include <time.h>
typedef pthread_mutex_t bzy_mutex;
typedef sem_t           bzy_sem;
#define BZY_MUTEX_INIT PTHREAD_MUTEX_INITIALIZER
static inline void bzy_mutex_init(bzy_mutex *m)
{
	pthread_mutex_init(m, NULL);
}

static inline void bzy_mutex_lock(bzy_mutex *m)
{
	pthread_mutex_lock(m);
}

static inline void bzy_mutex_unlock(bzy_mutex *m)
{
	pthread_mutex_unlock(m);
}

static inline void bzy_sem_init(bzy_sem *s)
{
	sem_init(s, 0, 0);
}

static inline void bzy_sem_post(bzy_sem *s, int n)
{
	for (int i = 0; i < n; i++)
	{
		sem_post(s);
	}
}

static inline void bzy_sem_wait(bzy_sem *s)
{
	while (sem_wait(s) != 0) { }
}

static inline int  bzy_sem_wait_ms(bzy_sem *s, int64_t ms)
{
	struct timespec ts;
	clock_gettime(CLOCK_REALTIME, &ts);
	ts.tv_sec  += (time_t)(ms / 1000);
	ts.tv_nsec += (long)((ms % 1000) * 1000000L);
	if (ts.tv_nsec >= 1000000000L)
	{
		ts.tv_sec++;
		ts.tv_nsec -= 1000000000L;
	}

	return sem_timedwait(s, &ts) == 0;
}

static inline void bzy_sem_destroy(bzy_sem *s)
{
	sem_destroy(s);
}

typedef pthread_t bzy_thread;
static inline void bzy_thread_start(bzy_thread *t, void *(*fn)(void*), void *arg)
{
	pthread_create(t, NULL, fn, arg);
}

static inline void bzy_thread_join(bzy_thread t)
{
	pthread_join(t, NULL);
}
#endif

#endif
