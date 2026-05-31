#include "breezy.h"
#include <stdio.h>

/* Scalar print helpers, dispatched by the compiler on the argument's static type:
   signed integers print as %lld, unsigned as %llu, booleans as true/false. */

void bzy_print_i64(int64_t v)
{
	printf("%lld\n", (long long)v);
}

void bzy_print_u64(uint64_t v)
{
	printf("%llu\n", (unsigned long long)v);
}

void bzy_print_bool(int64_t v)
{
	printf("%s\n", v ? "true" : "false");
}

void bzy_print_f64(double v)
{
	printf("%.17g\n", v);
}
