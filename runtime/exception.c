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
} BzyExceptionFunc;

/* Must match the codegen try-region layout (4 qwords). */
typedef struct
{
	int64_t start;
	int64_t end;
	void *catch_vtable;
	int64_t pad;
} BzyExceptionTry;

extern BzyExceptionFunc *__bzy_exception_funcs[];
extern int64_t __bzy_exception_func_count;

extern void *__bzy_vtable_parents[];     /* Flat [child0, parent0, child1, parent1, ...]. */
extern int64_t __bzy_vtable_parent_count;   /* Number of (child, parent) pairs. */

extern char __vtable_IndexOutOfBounds[];   /* Emitted by codegen for the builtin class. */
extern char __vtable_NumberFormatException[];   /* Reused for enum valueOf misses. */

static int64_t g_uncaught = 0;   /* Uncaught exceptions survived by the scheduler (atomic). */

int64_t bzy_uncaught_count(void)   /* Lets the entry point exit non-zero after a survived crash. */
{
	return __atomic_load_n(&g_uncaught, __ATOMIC_RELAXED);
}

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
static void exception_resume(void *exc, int64_t newrbp, int64_t newrsp, int64_t pad)
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

static BzyExceptionFunc *exception_find(int64_t pc)
{
	for (int64_t i = 0; i < __bzy_exception_func_count; i++)
	{
		BzyExceptionFunc *f = __bzy_exception_funcs[i];
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
		BzyExceptionFunc *f = exception_find(pc);
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
			BzyExceptionTry *tr = &((BzyExceptionTry*)f->tryptr)[t];
			if (pc >= tr->start && pc < tr->end && is_a(*(void**)exc, tr->catch_vtable))
			{
				exception_resume(exc, frame, frame - f->frame, tr->pad);   /* Never returns. */
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

	if (bzy_sched_current())
	{
		/* Inside a breeze: survive it. The unwind loop above already released every
		   frame's object locals; the breeze's stack is abandoned and its fiber freed,
		   so we must not longjmp across those NASM frames. The Exception object itself
		   is left unreleased (a small, rare per-crash leak — see the 6a-4 limitations). */
		__atomic_add_fetch(&g_uncaught, 1, __ATOMIC_RELAXED);   /* The process will exit non-zero. */
		bzy_sched_breeze_uncaught();   /* End this breeze; resume the scheduler. Never returns. */
	}

	abort();   /* No breeze context (unit test / pre-scheduler): preserve the original behavior. */
}

/* Array index out of bounds: build an IndexOutOfBounds and unwind from the
   access site (pc/frame passed by codegen). Catchable; uncaught -> trace+abort. */
void bzy_oob(int64_t index, int64_t length, int64_t pc, int64_t frame)
{
	char buf[96];
	int len = snprintf(buf, sizeof(buf), "Array index %lld out of bounds for length %lld.",
					   (long long)index, (long long)length);
	void *msg = bzy_str_new(buf, len);
	void *exc = bzy_alloc(32);                       /* Owned (+1); fields zeroed. */
	*(void**)exc = (void*)__vtable_IndexOutOfBounds;
	*(void**)((char*)exc + 24) = msg;                /* Exception.message. */
	bzy_throw(exc, pc, frame);                       /* Never returns. */
}

/* Thrown by Enum.valueOf(name) when no constant matches the given name. */
void bzy_enum_no_constant(void *name, int64_t pc, int64_t frame)
{
	char buf[160];
	int len;
	if (name)
	{
		len = snprintf(buf, sizeof(buf), "No enum constant: %.*s.",
					   (int)bzy_str_len(name), bzy_str_data(name));
	}
	else
	{
		len = snprintf(buf, sizeof(buf), "No enum constant.");
	}

	void *msg = bzy_str_new(buf, len);
	void *exc = bzy_alloc(32);                       /* Owned (+1); fields zeroed. */
	*(void**)exc = (void*)__vtable_NumberFormatException;
	*(void**)((char*)exc + 24) = msg;                /* Exception.message. */
	bzy_throw(exc, pc, frame);                       /* Never returns. */
}
