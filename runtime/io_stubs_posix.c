#include "breezy.h"

/* Linux placeholder for the async-I/O inflight counters the scheduler's deadlock
   gate consults. The real IOCP/offload backends are not built in the Part 8-2
   runtime (no sockets/files yet), so report zero in flight. Replaced when the
   offload pool (8-3) and epoll reactor (8-4) land. */
int64_t bzy_offload_inflight(void)
{
	return 0;
}

int64_t bzy_iocp_inflight(void)
{
	return 0;
}
