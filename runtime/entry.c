#include "breezy.h"
#include <stdlib.h>

extern void bzy_user_main(void);

/* AVX guard hook: a weak definition defaulting to NULL. A program that uses a
   256-bit SIMD type emits a strong definition of __bzy_avx_check pointing at the
   AVX-support check (which pulls in the cpu.c TU); that strong symbol overrides
   this weak NULL. Every other program keeps the NULL and pulls in nothing. A weak
   definition (rather than a weak reference) links cleanly on both ELF and PE. */
void (*__bzy_avx_check)(void) __attribute__((weak)) = 0;

int main(int argc, char **argv)
{
	if (__bzy_avx_check)
	{
		__bzy_avx_check();      /* Aborts with a diagnostic if the CPU lacks AVX. */
	}

	bzy_set_args(argc, argv);   /* Make argv available to System.args(). */
	bzy_sched_init();

	/* Default to one worker per logical core; BZY_WORKERS overrides (1 = the
	   deterministic cooperative single-thread scheduler). */
	const char *env = getenv("BZY_WORKERS");
	bzy_sched_set_workers(env ? atoi(env) : 0);

	bzy_spawn(bzy_user_main);
	bzy_sched_run();

	bzy_offload_shutdown();   /* Join the offload pool (no-op if it was never used; portable). */
#ifdef _WIN32
	bzy_iocp_shutdown();      /* Join the IOCP completion thread (Windows sockets). */
#else
	bzy_reactor_shutdown();   /* Join the epoll reactor thread (Linux sockets). */
#endif

	/* A breeze that died from an uncaught exception is survived (the scheduler keeps
	   running other breezes), but the process still reports failure. */
	return bzy_uncaught_count() ? 1 : 0;
}
