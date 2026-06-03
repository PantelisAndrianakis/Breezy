#include "breezy.h"

extern void bzy_user_main(void);

int main(void)
{
	bzy_sched_init();
	bzy_spawn(bzy_user_main);
	bzy_sched_run();
	return 0;
}
