#include "breezy.h"

#ifdef _WIN32
#include <windows.h>

int64_t bzy_clock_millis(void)
{
	FILETIME ft;
	GetSystemTimeAsFileTime(&ft);   /* 100-ns ticks since 1601-01-01 (UTC). */
	unsigned long long t = ((unsigned long long)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
	t -= 116444736000000000ULL;     /* Ticks between 1601 and the Unix epoch. */
	return (int64_t)(t / 10000ULL); /* 100-ns ticks -> milliseconds. */
}

int64_t bzy_clock_nanos(void)
{
	static LARGE_INTEGER freq;
	if (freq.QuadPart == 0)
	{
		QueryPerformanceFrequency(&freq);
	}

	LARGE_INTEGER c;
	QueryPerformanceCounter(&c);
	long long secs = c.QuadPart / freq.QuadPart;          /* Split to avoid *1e9 overflow. */
	long long rem  = c.QuadPart % freq.QuadPart;
	return (int64_t)(secs * 1000000000LL + (rem * 1000000000LL) / freq.QuadPart);
}

#else
#include <time.h>

int64_t bzy_clock_millis(void)
{
	return (int64_t)time(NULL) * 1000;   /* Coarse fallback; Part 8 refines for Linux. */
}

int64_t bzy_clock_nanos(void)
{
	return (int64_t)clock() * (1000000000LL / CLOCKS_PER_SEC);
}
#endif
