#include "breezy.h"
#include <stdlib.h>

extern void bzy_user_main(void);

int main(int argc, char **argv)
{
	bzy_set_args(argc, argv);   /* Make argv available to System.args(). */
	bzy_sched_init();

	/* Default to one worker per logical core; BZY_WORKERS overrides (1 = the
	   deterministic cooperative single-thread scheduler). */
	const char *env = getenv("BZY_WORKERS");
	bzy_sched_set_workers(env ? atoi(env) : 0);

	bzy_spawn(bzy_user_main);
	bzy_sched_run();

#ifdef _WIN32
	bzy_offload_shutdown();   /* Join the offload pool (no-op if it was never used). */
	bzy_iocp_shutdown();      /* Join the IOCP completion thread (no-op if no sockets were used). */
#endif

	/* A breeze that died from an uncaught exception is survived (the scheduler keeps
	   running other breezes), but the process still reports failure. */
	return bzy_uncaught_count() ? 1 : 0;
}
