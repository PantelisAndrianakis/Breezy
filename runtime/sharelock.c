#include "breezy.h"
#include "platform.h"
#include <stdint.h>

/* Striped locks for shared-container operations. A shared container's op takes
   the stripe its address hashes to; no per-object lock storage, no header layout
   change, and an uncontended acquire is one CAS (SRWLOCK / pthread fast path).
   Critical sections never park a breeze (container ops perform no channel or
   I/O work), so there is no scheduler interaction. 1024 stripes keep accidental
   collisions rare; a collision only serializes two shared-container ops briefly.
   Zero-initialization is a valid initial state for both SRWLOCK (SRWLOCK_INIT
   is {0}) and glibc's pthread_mutex_t (PTHREAD_MUTEX_INITIALIZER is all-zero),
   so the static array needs no init pass. */
#define SHARE_STRIPES 1024

static bzy_mutex g_stripes[SHARE_STRIPES];

static bzy_mutex *stripe_of(void *o)
{
	uint64_t h = (uint64_t)(uintptr_t)o >> 4;   /* Drop allocation-granularity zeros. */
	h *= 0x9E3779B97F4A7C15ull;                 /* Fibonacci hash spreads the stripes. */
	return &g_stripes[h >> (64 - 10)];          /* Top 10 bits -> 0..1023. */
}

void bzy_shared_lock(void *o)
{
	bzy_mutex_lock(stripe_of(o));
}

void bzy_shared_unlock(void *o)
{
	bzy_mutex_unlock(stripe_of(o));
}
