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
	return (int64_t)time(NULL) * 1000;   /* Coarse fallback for non-Windows platforms. */
}

int64_t bzy_clock_nanos(void)
{
	return (int64_t)clock() * (1000000000LL / CLOCKS_PER_SEC);
}
#endif

static void put_pad(char *o, int *n, int v, int width)
{
	char t[16];
	int len = snprintf(t, sizeof(t), "%0*d", width, v);
	for (int i = 0; i < len && *n < 250; i++)
	{
		o[(*n)++] = t[i];
	}
}

/* Format millis (epoch ms) in local time using a Java-style pattern. Tokens:
   yyyy yy MM dd HH hh mm ss SSS a; any other character is copied literally. */
static void *format_date(int64_t millis, const char *f, int fl)
{
	time_t secs = (time_t)(millis / 1000);
	int ms = (int)(millis % 1000);
	if (ms < 0)
	{
		ms += 1000;
	}

	struct tm tmv;
	struct tm *lt = localtime(&secs);
	tmv = *lt;                                   /* Copy out of the static buffer immediately. */

	char out[256];
	int n = 0, i = 0;
	while (i < fl && n < 250)
	{
		if (i + 4 <= fl && memcmp(f + i, "yyyy", 4) == 0)
		{
			put_pad(out, &n, tmv.tm_year + 1900, 4);
			i += 4;
		}
		else if (i + 3 <= fl && memcmp(f + i, "SSS", 3) == 0)
		{
			put_pad(out, &n, ms, 3);
			i += 3;
		}
		else if (i + 2 <= fl && memcmp(f + i, "yy", 2) == 0)
		{
			put_pad(out, &n, (tmv.tm_year + 1900) % 100, 2);
			i += 2;
		}
		else if (i + 2 <= fl && memcmp(f + i, "MM", 2) == 0)
		{
			put_pad(out, &n, tmv.tm_mon + 1, 2);
			i += 2;
		}
		else if (i + 2 <= fl && memcmp(f + i, "dd", 2) == 0)
		{
			put_pad(out, &n, tmv.tm_mday, 2);
			i += 2;
		}
		else if (i + 2 <= fl && memcmp(f + i, "HH", 2) == 0)
		{
			put_pad(out, &n, tmv.tm_hour, 2);
			i += 2;
		}
		else if (i + 2 <= fl && memcmp(f + i, "hh", 2) == 0)
		{
			int h = tmv.tm_hour % 12;
			if (h == 0)
			{
				h = 12;
			}

			put_pad(out, &n, h, 2);
			i += 2;
		}
		else if (i + 2 <= fl && memcmp(f + i, "mm", 2) == 0)
		{
			put_pad(out, &n, tmv.tm_min, 2);
			i += 2;
		}
		else if (i + 2 <= fl && memcmp(f + i, "ss", 2) == 0)
		{
			put_pad(out, &n, tmv.tm_sec, 2);
			i += 2;
		}
		else if (f[i] == 'a')
		{
			const char *ap = tmv.tm_hour < 12 ? "AM" : "PM";
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
