/* CPU feature guard. Linked only when a program actually uses an AVX vector type
   (the compiler emits the call to bzy_require_avx at startup in that case), so a
   program with no 256-bit SIMD pulls none of this in. */
#include "breezy.h"
#include <stdio.h>
#include <stdlib.h>

/* Abort with a clear diagnostic if the running CPU lacks AVX, instead of letting
   the first VEX-encoded instruction fault with an illegal-instruction signal. */
void bzy_require_avx(void)
{
	__builtin_cpu_init();
	if (!__builtin_cpu_supports("avx"))
	{
		fputs("This program uses 256-bit SIMD (f64x4) but the CPU does not support AVX.\n", stderr);
		exit(1);
	}
}
