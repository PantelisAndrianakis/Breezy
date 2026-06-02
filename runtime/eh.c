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

/* Must match the codegen try-region layout (4 qwords). */
typedef struct
{
	int64_t start;
	int64_t end;
	void *catch_vtable;
	int64_t pad;
} BzyEHTry;

extern BzyEHFunc *__bzy_eh_funcs[];
extern int64_t __bzy_eh_func_count;

extern void *__bzy_vtable_parents[];     /* Flat [child0, parent0, child1, parent1, ...]. */
extern int64_t __bzy_vtable_parent_count;   /* Number of (child, parent) pairs. */

static void *parent_vtable(void *vt)
{
	for (int64_t i = 0; i < __bzy_vtable_parent_count; i++)
	{
		if (__bzy_vtable_parents[i*2] == vt)
		{
			return __bzy_vtable_parents[i*2 + 1];
		}
	}

	return NULL;
}

/* True if the object's class is the catch class or a subclass of it. */
static int is_a(void *obj_vt, void *catch_vt)
{
	for (void *v = obj_vt; v; v = parent_vtable(v))
	{
		if (v == catch_vt)
		{
			return 1;
		}
	}

	return 0;
}

/* Restore the handler frame and jump to its landing pad with the exception in
   rax. Uses only volatile registers (r8-r11) for the operands so setting rsp/rbp
   never invalidates an operand still to be read. Never returns. */
__attribute__((noreturn))
static void eh_resume(void *exc, int64_t newrbp, int64_t newrsp, int64_t pad)
{
	register int64_t r_rsp __asm__("r8")  = newrsp;
	register int64_t r_rbp __asm__("r9")  = newrbp;
	register void   *r_exc __asm__("r10") = exc;
	register int64_t r_pad __asm__("r11") = pad;
	__asm__ volatile(
		"movq %0, %%rsp\n\t"
		"movq %1, %%rbp\n\t"
		"movq %2, %%rax\n\t"
		"jmp  *%3\n\t"
		:
		: "r"(r_rsp), "r"(r_rbp), "r"(r_exc), "r"(r_pad)
		: "rax"
	);
	__builtin_unreachable();
}

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

		/* If this frame has a try-region covering the PC whose catch type matches
		   the thrown object's vtable exactly, resume into its landing pad without
		   releasing this frame's locals (the handler frame keeps executing). */
		for (int64_t t = 0; t < f->ntry; t++)
		{
			BzyEHTry *tr = &((BzyEHTry*)f->tryptr)[t];
			if (pc >= tr->start && pc < tr->end && is_a(*(void**)exc, tr->catch_vtable))
			{
				eh_resume(exc, frame, frame - f->frame, tr->pad);   /* Never returns. */
			}
		}

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
