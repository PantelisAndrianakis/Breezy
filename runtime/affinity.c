/* System.cpuCount / System.affinity: a thin, honest wrapper over the OS thread
   affinity calls. Pins the CURRENT OS (worker) thread, not the breeze — a breeze
   migrates across workers, so deterministic pinning needs a dedicated worker.
   Lives in its own translation unit so a program that calls neither links none
   of this (the no-bloat invariant). */
#ifndef _WIN32
#define _GNU_SOURCE
#endif

#include "breezy.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <sched.h>
#include <unistd.h>
#endif

int64_t bzy_sys_cpu_count(void)
{
#ifdef _WIN32
	SYSTEM_INFO si;
	GetSystemInfo(&si);
	return (int64_t)si.dwNumberOfProcessors;
#else
	long n = sysconf(_SC_NPROCESSORS_ONLN);
	return n > 0 ? (int64_t)n : 1;
#endif
}

int64_t bzy_sys_affinity(int64_t mask)
{
	if (mask == 0)
	{
		return 0;   /* Empty mask: nothing to pin to. */
	}

#ifdef _WIN32
	DWORD_PTR prev = SetThreadAffinityMask(GetCurrentThread(), (DWORD_PTR)mask);
	return prev != 0 ? 1 : 0;
#else
	cpu_set_t set;
	CPU_ZERO(&set);
	for (int i = 0; i < 64; i++)
	{
		if (mask & ((int64_t)1 << i))
		{
			CPU_SET(i, &set);
		}
	}
	return sched_setaffinity(0, sizeof(set), &set) == 0 ? 1 : 0;
#endif
}
