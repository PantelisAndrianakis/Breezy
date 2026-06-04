#include "breezy.h"

/* Linux placeholder for the IOCP inflight counter the scheduler's deadlock gate
   consults. There is no completion port on Linux (sockets use epoll, 8-4), so
   report zero. The offload pool is real (offload.c) and provides its own
   bzy_offload_inflight. */
int64_t bzy_iocp_inflight(void)
{
	return 0;
}
