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
		fputs("This program uses 256-bit SIMD (f64x4/f32x8) but the CPU does not support AVX.\n", stderr);
		exit(1);
	}
}

/* As above, but for the 256-bit integer type (i32x8), whose packed ops need AVX2. */
void bzy_require_avx2(void)
{
	__builtin_cpu_init();
	if (!__builtin_cpu_supports("avx2"))
	{
		fputs("This program uses 256-bit integer SIMD (i32x8) but the CPU does not support AVX2.\n", stderr);
		exit(1);
	}
}
