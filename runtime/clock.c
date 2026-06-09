#include "breezy.h"
#include <time.h>
#include <stdio.h>
#include <string.h>

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

/* Read the x86-64 invariant TSC. No system call; ~2-4 cycles. */
static inline uint64_t read_tsc(void)
{
	uint32_t lo, hi;
	__asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
	return ((uint64_t)hi << 32) | lo;
}

static volatile uint64_t g_tsc0      = 0;
static volatile uint64_t g_scale_q32 = 0;   /* ns-per-tick in Q32.32 fixed-point. */

static void rdtsc_init(void)
{
	/* Calibrate the TSC -> ns scale against QPC over a ~100 us spin. Called at
	   most once per process (first bzy_clock_nanos call). */
	LARGE_INTEGER f, c0, c1;
	QueryPerformanceFrequency(&f);
	QueryPerformanceCounter(&c0);
	uint64_t t0 = read_tsc();
	int64_t  end = c0.QuadPart + f.QuadPart / 10000;   /* ~100 us. */
	do { QueryPerformanceCounter(&c1); } while (c1.QuadPart < end);
	uint64_t t1 = read_tsc();

	uint64_t ns    = (uint64_t)((c1.QuadPart - c0.QuadPart) * 1000000000LL / f.QuadPart);
	uint64_t scale = (uint64_t)(((unsigned __int128)ns << 32) / (t1 - t0));
	g_tsc0      = t0;     /* Write base before scale. On x86 TSO, stores are not */
	g_scale_q32 = scale;  /* reordered, so any reader that sees scale != 0 also   */
	                      /* sees the matching tsc0.                               */
}

int64_t bzy_clock_nanos(void)
{
	/* Hot path: rdtsc + 128-bit multiply + shift -- ~3-4 ns, nanosecond resolution. */
	if (!g_scale_q32)
		rdtsc_init();
	return (int64_t)(((unsigned __int128)(read_tsc() - g_tsc0) * g_scale_q32) >> 32);
}

#else
#include <time.h>

int64_t bzy_clock_millis(void)
{
	/* time() has 1-second resolution, which collapses every sub-second timer onto
	   one tick: timers scheduled milliseconds apart all come due together and the
	   workers race to run them (broke proj_timer_order). clock_gettime(REALTIME)
	   is the same epoch as time() -- so format_date still works -- but with
	   millisecond resolution. */
	struct timespec ts;
	clock_gettime(CLOCK_REALTIME, &ts);
	return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

int64_t bzy_clock_nanos(void)
{
	/* clock() measures CPU time, not elapsed wall time -- wrong for a monotonic
	   counter (and what System.currentTimeNanos promises). CLOCK_MONOTONIC is the
	   POSIX analogue of Windows' QueryPerformanceCounter. */
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}
#endif

/* Emit v (non-negative) zero-padded to at least `width` digits. Replaces an
   snprintf("%0*d") that dominated the format hot path (format-string parse +
   locale on every numeric field, 12M calls in the time benchmark). */
static void put_pad(char *o, int *n, int v, int width)
{
	char t[16];
	int len = 0;
	if (v == 0)
	{
		t[len++] = '0';
	}
	else
	{
		int x = v;
		while (x > 0)
		{
			t[len++] = (char)('0' + x % 10);
			x /= 10;
		}
	}
	while (len < width)
	{
		t[len++] = '0';
	}

	for (int i = len - 1; i >= 0 && *n < 250; i--)   /* Digits were produced least-significant first. */
	{
		o[(*n)++] = t[i];
	}
}

/* Floor division by a positive divisor (C's / truncates toward zero, which is
   wrong for negative seconds, i.e. pre-1970 timestamps). */
static int64_t floordiv(int64_t a, int64_t b)
{
	int64_t q = a / b;
	if ((a % b != 0) && ((a < 0) != (b < 0)))
	{
		q--;
	}
	return q;
}

/* Civil date (year, 1-based month, day) from a count of days since 1970-01-01.
   Howard Hinnant's branch-free algorithm: avoids a localtime() call per format. */
static void civil_from_days(int64_t z, int *year, int *month, int *day)
{
	z += 719468;                                          /* Shift epoch to 0000-03-01. */
	int64_t era = floordiv(z, 146097);
	unsigned doe = (unsigned)(z - era * 146097);          /* Day of era [0, 146096]. */
	unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;   /* Year of era [0, 399]. */
	int64_t y = (int64_t)yoe + era * 400;
	unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100); /* Day of year [0, 365], Mar-1 = 0. */
	unsigned mp = (5 * doy + 2) / 153;                     /* Month [0, 11], Mar = 0. */
	unsigned d = doy - (153 * mp + 2) / 5 + 1;             /* Day [1, 31]. */
	unsigned m = mp < 10 ? mp + 3 : mp - 9;                /* Month [1, 12], Jan = 1. */
	*year = (int)(y + (m <= 2));
	*month = (int)m;
	*day = (int)d;
}

/* Local UTC offset (seconds) for the given epoch second, cached in 15-minute
   (900 s) buckets. glibc's localtime() takes a global tzset lock and re-derives
   the zone on every call (~890 ns each); calling it once per bucket instead cuts
   that ~24x for the typical sub-minute timestamp stride. The bucket is safe
   against DST: every real-world transition instant falls on a 15-minute UTC
   boundary (the finest zone-offset granularity is 15 min and transitions occur
   on whole local minutes), so a single bucket never straddles a transition.
   Thread-local so concurrent breezes never race on the cache. */
static int64_t local_offset(time_t secs)
{
	static __thread int64_t cache_lo = 0;
	static __thread int64_t cache_hi = 0;
	static __thread int64_t cache_off = 0;
	static __thread int cache_valid = 0;

	int64_t s = (int64_t)secs;
	if (cache_valid && s >= cache_lo && s < cache_hi)
	{
		return cache_off;
	}

	struct tm tmv;
#ifdef _WIN32
	struct tm *lt = localtime(&secs);
	tmv = *lt;
	int64_t off = (int64_t)_mkgmtime(&tmv) - s;   /* Re-interpret the local fields as UTC -> offset. */
#else
	localtime_r(&secs, &tmv);
	int64_t off = (int64_t)timegm(&tmv) - s;
#endif

	int64_t lo = floordiv(s, 900) * 900;
	cache_lo = lo;
	cache_hi = lo + 900;
	cache_off = off;
	cache_valid = 1;
	return off;
}

/* Format millis (epoch ms) in local time using a Java-style pattern. Tokens:
   yyyy yy MM dd HH hh mm ss SSS a; any other character is copied literally. */
static void *format_date(int64_t millis, const char *f, int fl)
{
	int64_t secs = floordiv(millis, 1000);
	int ms = (int)(millis - secs * 1000);        /* Always in [0, 999], even for negative millis. */

	int64_t local = secs + local_offset((time_t)secs);
	int64_t days = floordiv(local, 86400);
	int sod = (int)(local - days * 86400);        /* Second of day [0, 86399]. */

	int year, mon, mday;
	civil_from_days(days, &year, &mon, &mday);
	int hour = sod / 3600;
	int minute = (sod % 3600) / 60;
	int sec = sod % 60;

	char out[256];
	int n = 0, i = 0;
	while (i < fl && n < 250)
	{
		if (i + 4 <= fl && memcmp(f + i, "yyyy", 4) == 0)
		{
			put_pad(out, &n, year, 4);
			i += 4;
		}
		else if (i + 3 <= fl && memcmp(f + i, "SSS", 3) == 0)
		{
			put_pad(out, &n, ms, 3);
			i += 3;
		}
		else if (i + 2 <= fl && memcmp(f + i, "yy", 2) == 0)
		{
			put_pad(out, &n, year % 100, 2);
			i += 2;
		}
		else if (i + 2 <= fl && memcmp(f + i, "MM", 2) == 0)
		{
			put_pad(out, &n, mon, 2);
			i += 2;
		}
		else if (i + 2 <= fl && memcmp(f + i, "dd", 2) == 0)
		{
			put_pad(out, &n, mday, 2);
			i += 2;
		}
		else if (i + 2 <= fl && memcmp(f + i, "HH", 2) == 0)
		{
			put_pad(out, &n, hour, 2);
			i += 2;
		}
		else if (i + 2 <= fl && memcmp(f + i, "hh", 2) == 0)
		{
			int h = hour % 12;
			if (h == 0)
			{
				h = 12;
			}

			put_pad(out, &n, h, 2);
			i += 2;
		}
		else if (i + 2 <= fl && memcmp(f + i, "mm", 2) == 0)
		{
			put_pad(out, &n, minute, 2);
			i += 2;
		}
		else if (i + 2 <= fl && memcmp(f + i, "ss", 2) == 0)
		{
			put_pad(out, &n, sec, 2);
			i += 2;
		}
		else if (f[i] == 'a')
		{
			const char *ap = hour < 12 ? "AM" : "PM";
			out[n++] = ap[0];
			if (n < 250)
			{
				out[n++] = ap[1];
			}

			i++;
		}
		else
		{
			out[n++] = f[i++];
		}
	}

	return bzy_str_new(out, n);
}

void *bzy_clock_date(int64_t millis)
{
	return format_date(millis, "yyyy-MM-dd HH:mm:ss", 19);
}

void *bzy_clock_date_fmt(int64_t millis, void *fmt)
{
	return format_date(millis, bzy_str_data(fmt), (int)bzy_str_len(fmt));
}
