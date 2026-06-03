#include "breezy.h"
#include <stdint.h>
#include <math.h>

/* xoshiro256** seeded (lazily, from the clock) via splitmix64. A single global
   generator. */
static uint64_t g_s[4];
static int g_seeded;
static int g_has_spare;
static double g_spare;

static uint64_t rotl(uint64_t x, int k)
{
	return (x << k) | (x >> (64 - k));
}

static uint64_t splitmix64(uint64_t *x)
{
	uint64_t z = (*x += 0x9e3779b97f4a7c15ULL);
	z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
	z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
	return z ^ (z >> 31);
}

static void rnd_seed(void)
{
	uint64_t sm = (uint64_t)bzy_clock_nanos() ^ 0x0123456789abcdefULL;
	for (int i = 0; i < 4; i++)
	{
		g_s[i] = splitmix64(&sm);
	}

	g_seeded = 1;
}

static uint64_t next_u64(void)
{
	if (!g_seeded)
	{
		rnd_seed();
	}

	uint64_t result = rotl(g_s[1] * 5, 7) * 9;
	uint64_t t = g_s[1] << 17;
	g_s[2] ^= g_s[0];
	g_s[3] ^= g_s[1];
	g_s[1] ^= g_s[2];
	g_s[0] ^= g_s[3];
	g_s[2] ^= t;
	g_s[3] = rotl(g_s[3], 45);
	return result;
}

/* Unbiased-enough bounded [0, bound) via Lemire multiply-shift. */
static uint64_t bounded(uint64_t bound)
{
	return (uint64_t)(((__uint128_t)next_u64() * bound) >> 64);
}

int64_t bzy_rnd_bool(void)
{
	return (int64_t)(next_u64() >> 63);
}

int64_t bzy_rnd_int(void)
{
	return (int64_t)(int32_t)(uint32_t)(next_u64() >> 32);
}

int64_t bzy_rnd_long(void)
{
	return (int64_t)next_u64();
}

float bzy_rnd_float(void)
{
	return (float)((next_u64() >> 40) * (1.0f / 16777216.0f));   /* 24 bits / 2^24. */
}

double bzy_rnd_double(void)
{
	return (double)((next_u64() >> 11) * (1.0 / 9007199254740992.0));   /* 53 bits / 2^53. */
}

double bzy_rnd_gaussian(void)
{
	if (g_has_spare)
	{
		g_has_spare = 0;
		return g_spare;
	}

	double u, v, s;
	do
	{
		u = 2.0 * bzy_rnd_double() - 1.0;
		v = 2.0 * bzy_rnd_double() - 1.0;
		s = u * u + v * v;
	}
	while (s >= 1.0 || s == 0.0);

	double mul = sqrt(-2.0 * log(s) / s);
	g_spare = v * mul;
	g_has_spare = 1;
	return u * mul;
}

int64_t bzy_rnd_get_i(int64_t bound)
{
	return bound <= 0 ? 0 : (int64_t)bounded((uint64_t)bound);
}

int64_t bzy_rnd_get_ii(int64_t origin, int64_t bound)
{
	if (origin >= bound)
	{
		return origin;
	}

	return origin + (int64_t)bounded((uint64_t)(bound - origin) + 1);   /* inclusive */
}

int64_t bzy_rnd_get_l(int64_t bound)
{
	return bound <= 0 ? 0 : (int64_t)bounded((uint64_t)bound);
}

int64_t bzy_rnd_get_ll(int64_t origin, int64_t bound)
{
	if (origin >= bound)
	{
		return origin;
	}

	return origin + (int64_t)bounded((uint64_t)(bound - origin) + 1);
}

float bzy_rnd_get_f(float bound)
{
	return bound <= 0.0f ? 0.0f : bzy_rnd_float() * bound;
}

float bzy_rnd_get_ff(float origin, float bound)
{
	return origin >= bound ? origin : origin + bzy_rnd_float() * (bound - origin);
}

double bzy_rnd_get_d(double bound)
{
	return bound <= 0.0 ? 0.0 : bzy_rnd_double() * bound;
}

double bzy_rnd_get_dd(double origin, double bound)
{
	return origin >= bound ? origin : origin + bzy_rnd_double() * (bound - origin);
}

void bzy_rnd_bytes(void *arr)
{
	int64_t n = bzy_array_len(arr);
	int64_t *slots = (int64_t*)((char*)arr + 32);
	for (int64_t i = 0; i < n; i++)
	{
		slots[i] = (int64_t)(next_u64() & 0xFF);
	}
}
