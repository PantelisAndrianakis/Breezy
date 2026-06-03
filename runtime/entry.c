#include "breezy.h"
#include <stdlib.h>

extern void bzy_user_main(void);

int main(void)
{
	bzy_sched_init();

	/* Default to one worker per logical core; BZY_WORKERS overrides (1 = the
	   deterministic cooperative single-thread scheduler). */
	const char *env = getenv("BZY_WORKERS");
	bzy_sched_set_workers(env ? atoi(env) : 0);

	bzy_spawn(bzy_user_main);
	bzy_sched_run();

	/* A breeze that died from an uncaught exception is survived (the scheduler keeps
	   running other breezes), but the process still reports failure. */
	return bzy_uncaught_count() ? 1 : 0;
}
