#include "breezy.h"
#include <stdlib.h>

/* Logger managed object (object_size = 56), three managed children:
   0 vtable | 8 rc | 16 gcinfo | 24 data_ch | 32 done_ch | 40 writer | 48 closed.
   typeinfo = {finalizer=0, num_obj_fields=3, 24, 32, 40}: the generic release frees
   all three children once the refcount hits 0 (after close() joins the breeze). */
#define LG_DATA(o)   (*(void**)((char*)(o) + 24))
#define LG_DONE(o)   (*(void**)((char*)(o) + 32))
#define LG_WRITER(o) (*(void**)((char*)(o) + 40))
#define LG_CLOSED(o) (*(int64_t*)((char*)(o) + 48))

#define LOG_CAPACITY 1024   /* Bounded data channel: a far-behind producer parks here. */

static int64_t g_logger_typeinfo[5] = { 0 /* Finalizer. */, 3, 24, 32, 40 };
static int64_t g_logger_vtable[2];

static void *logger_vtable(void)
{
	g_logger_vtable[0] = (int64_t)&g_logger_typeinfo[0];
	return &g_logger_vtable[1];
}

/* The logger breeze: drain the data channel, writing each line, until the 0 sentinel.
   Then flush+close the writer, signal close(), and drop the breeze's Logger reference. */
static void logger_loop(void *logger)
{
	for (;;)
	{
		int64_t line = bzy_channel_recv(LG_DATA(logger));   /* Owned string, or 0 = stop. */
		if (line == 0)
		{
			break;
		}

		bzy_filewriter_write_line(LG_WRITER(logger), (void*)line);
		bzy_release((void*)line);                            /* Consumed the moved-in string. */
	}

	bzy_filewriter_close(LG_WRITER(logger));                 /* Final flush + close (offloaded; parks). */
	bzy_channel_send(LG_DONE(logger), 1);                    /* Resume the parked close(). */
	bzy_release(logger);                                     /* Drop the breeze's +1. */
}

void *bzy_logger_open(void *path)
{
	void *writer = bzy_filewriter_open(path, 1, 0);   /* Append; default buffer. NULL + io_fail on error. */
	void *logger = bzy_alloc(56);
	*(void**)logger = logger_vtable();
	LG_DATA(logger) = bzy_channel_new(LOG_CAPACITY, 1);   /* Managed string channel; owned (+1). */
	LG_DONE(logger) = bzy_channel_new(1, 0);              /* Unmanaged int signal; owned (+1). */
	LG_WRITER(logger) = writer;                           /* Owned (+1). */
	LG_CLOSED(logger) = 0;

	bzy_retain(logger);                       /* +1 for the breeze, so it outlives a dropped producer ref. */
	bzy_spawn_args(logger_loop, logger);
	return logger;                            /* Owned (+1) for the caller. */
}

void bzy_logger_log(void *logger, void *str)
{
	bzy_channel_send(LG_DATA(logger), (int64_t)str);   /* Moves the string in (ownership transfers). */
}

void bzy_logger_close(void *logger)
{
	if (LG_CLOSED(logger))
	{
		return;   /* Idempotent. */
	}

	LG_CLOSED(logger) = 1;
	bzy_channel_send(LG_DATA(logger), 0);     /* Sentinel: lands behind every queued line (FIFO). */
	bzy_channel_recv(LG_DONE(logger));        /* Park until the breeze drained, flushed, closed, signalled. */
}
