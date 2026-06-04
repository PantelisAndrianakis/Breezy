#include "breezy.h"
#include <stdio.h>

/* Scalar-to-string helpers for string concatenation (`value + string`). Each
   returns an owned (+1) Breezy string and formats identically to the matching
   bzy_print_* helper, so `print(x)` and `"" + x` always agree. */

void *bzy_str_from_i64(int64_t v)
{
	char buf[32];
	int n = snprintf(buf, sizeof(buf), "%lld", (long long)v);
	return bzy_str_new(buf, (int64_t)n);
}

void *bzy_str_from_u64(uint64_t v)
{
	char buf[32];
	int n = snprintf(buf, sizeof(buf), "%llu", (unsigned long long)v);
	return bzy_str_new(buf, (int64_t)n);
}

void *bzy_str_from_bool(int64_t v)
{
	return v ? bzy_str_new("true", 4) : bzy_str_new("false", 5);
}

void *bzy_str_from_f64(double v)
{
	char buf[32];
	int n = snprintf(buf, sizeof(buf), "%.17g", v);
	return bzy_str_new(buf, (int64_t)n);
}
