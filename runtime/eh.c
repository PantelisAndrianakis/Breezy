#include "breezy.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

/* Must match the codegen record layout (8 qwords). */
typedef struct
{
	int64_t start;
	int64_t end;
	int64_t frame;
	const char *name;
	int64_t nobj;
	int64_t *objs;
	int64_t ntry;
	void *tryptr;
} BzyEHFunc;

extern BzyEHFunc *__bzy_eh_funcs[];
extern int64_t __bzy_eh_func_count;

static BzyEHFunc *eh_find(int64_t pc)
{
	for (int64_t i = 0; i < __bzy_eh_func_count; i++)
	{
		BzyEHFunc *f = __bzy_eh_funcs[i];
		if (pc >= f->start && pc < f->end)
		{
			return f;
		}
	}

	return NULL;
}

void bzy_throw(void *exc, int64_t pc, int64_t frame)
{
	const char *trace[256];
	int n = 0;

	/* The thrown object arrives owned (+1), so releasing frame locals below
	   (one of which may be the thrown object) never frees it. */
	for (;;)
	{
		BzyEHFunc *f = eh_find(pc);
		if (!f)
		{
			break;                               /* Off the top -> uncaught. */
		}

		if (n < 256)
		{
			trace[n++] = f->name;
		}

		/* (5e-2: scan f's try-regions for a matching catch and resume here.) */
		for (int64_t j = 0; j < f->nobj; j++)
		{
			bzy_release(*(void**)((char*)(uintptr_t)frame - f->objs[j]));
		}

		pc = *(int64_t*)((char*)(uintptr_t)frame + 8);   /* Return address into the caller. */
		frame = *(int64_t*)((char*)(uintptr_t)frame);    /* Caller's saved rbp. */
		if (frame == 0)
		{
			break;
		}
	}

	void *msg = *(void**)((char*)exc + 24);              /* Exception.message at offset 24. */
	fprintf(stderr, "Uncaught exception");
	if (msg)
	{
		fprintf(stderr, ": %.*s", (int)bzy_str_len(msg), bzy_str_data(msg));
	}

	fprintf(stderr, "\n");
	for (int i = 0; i < n; i++)
	{
		fprintf(stderr, "  at %s\n", trace[i]);
	}

	abort();
}
