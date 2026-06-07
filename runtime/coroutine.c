/* Userspace coroutine backend: a hand-written, register-only context switch over
   pooled stacks. Replaces the Windows-fiber and POSIX-ucontext backends behind the
   coroutine.h seam. No syscall on the hot path - vs CreateFiber/DeleteFiber (~6.8 us
   measured) and swapcontext's hidden sigprocmask. A finished coroutine's stack is
   recycled by re-initializing it from the top (bzy_coroutine_create), so reuse is
   correct-by-construction - no OS fiber lifecycle, no trampoline-return tricks.

   The switch saves the ABI's callee-saved registers, swaps rsp, and returns. A new
   coroutine's stack is laid out so the switch's `ret` lands in bzy_co_start, which
   calls bzy_co_run -> fn(arg). On Win64 it also saves xmm6..15 and updates the TEB
   StackBase/StackLimit (gs:[8]/gs:[0x10]) so __chkstk and large frames work on the
   coroutine stack. */
#include "coroutine.h"
#include "platform.h"
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#if defined(_WIN32)
#include <intrin.h>
#endif

#define BZY_CO_STACK (256 * 1024)   /* Per-coroutine stack (reserved; committed on touch). Reused
                                        via the pool, so its size costs footprint, not spawn speed. */

struct BzyCoroutine
{
	void          *sp;        /* Saved stack pointer while suspended. */
	void          *stack_hi;  /* Top (high) of the stack region -> TEB StackBase on Win64. */
	void          *stack_lo;  /* Bottom (low) of the stack region -> TEB StackLimit on Win64. */
	void          *alloc;     /* malloc'd stack block (NULL for a promoted OS thread). */
	BzyCoroutineFn fn;
	void          *arg;
	int            is_thread; /* 1 = promoted OS thread; never recycle or free it. */
	BzyCoroutine  *pool_next; /* Free-list link while pooled. */
};

static __thread BzyCoroutine *t_current;
static __thread BzyCoroutine  t_main;

/* The context switch and the new-coroutine entry stub, in their own asm. The entry
   stub is reached via the switch's `ret`; it is entered 16-aligned, so its `call`
   re-establishes the ABI's (rsp % 16 == 8) before bzy_co_run runs. */
extern void bzy_ctx_switch(void **save_sp, void *to_sp, void *to_hi, void *to_lo);
extern void bzy_co_start(void);
void        bzy_co_run(void);   /* Referenced from bzy_co_start. */

#if defined(_WIN32)
__asm__(
	".text\n"
	".globl bzy_ctx_switch\n"
	"bzy_ctx_switch:\n"            /* rcx=save_sp, rdx=to_sp, r8=to_hi, r9=to_lo */
	"  push %rbp\n  push %rbx\n  push %rdi\n  push %rsi\n"
	"  push %r12\n  push %r13\n  push %r14\n  push %r15\n"
	"  sub  $168, %rsp\n"          /* 160 xmm + 8 pad; keeps rsp 16-aligned for movaps */
	"  movaps %xmm6,0(%rsp)\n  movaps %xmm7,16(%rsp)\n  movaps %xmm8,32(%rsp)\n  movaps %xmm9,48(%rsp)\n"
	"  movaps %xmm10,64(%rsp)\n movaps %xmm11,80(%rsp)\n movaps %xmm12,96(%rsp)\n movaps %xmm13,112(%rsp)\n"
	"  movaps %xmm14,128(%rsp)\n movaps %xmm15,144(%rsp)\n"
	"  movq %rsp, (%rcx)\n"
	"  movq %rdx, %rsp\n"
	"  movq %r8, %gs:0x08\n"       /* TEB StackBase  */
	"  movq %r9, %gs:0x10\n"       /* TEB StackLimit */
	"  movaps 0(%rsp),%xmm6\n  movaps 16(%rsp),%xmm7\n  movaps 32(%rsp),%xmm8\n  movaps 48(%rsp),%xmm9\n"
	"  movaps 64(%rsp),%xmm10\n movaps 80(%rsp),%xmm11\n movaps 96(%rsp),%xmm12\n movaps 112(%rsp),%xmm13\n"
	"  movaps 128(%rsp),%xmm14\n movaps 144(%rsp),%xmm15\n"
	"  add  $168, %rsp\n"
	"  pop %r15\n  pop %r14\n  pop %r13\n  pop %r12\n"
	"  pop %rsi\n  pop %rdi\n  pop %rbx\n  pop %rbp\n"
	"  ret\n"
	".globl bzy_co_start\n"
	"bzy_co_start:\n"
	"  call bzy_co_run\n"
	"  hlt\n"
);
#define BZY_CO_FRAME 232   /* 160 xmm + 8 pad + 64 (8 callee regs) */
#else
__asm__(
	".text\n"
	".globl bzy_ctx_switch\n"
	".type bzy_ctx_switch,@function\n"
	"bzy_ctx_switch:\n"           /* rdi=save_sp, rsi=to_sp (rdx,rcx unused) */
	"  push %rbp\n  push %rbx\n  push %r12\n  push %r13\n  push %r14\n  push %r15\n"
	"  mov  %rsp, (%rdi)\n"
	"  mov  %rsi, %rsp\n"
	"  pop  %r15\n  pop %r14\n  pop %r13\n  pop %r12\n  pop %rbx\n  pop %rbp\n"
	"  ret\n"
	".globl bzy_co_start\n"
	".type bzy_co_start,@function\n"
	"bzy_co_start:\n"
	"  call bzy_co_run\n"
	"  hlt\n"
);
#define BZY_CO_FRAME 48    /* 6 callee regs */
#endif

void bzy_co_run(void)
{
	BzyCoroutine *c = t_current;
	c->fn(c->arg);
	/* A breeze body switches to the scheduler before returning, so control never
	   reaches here. If it ever does, it is a runtime bug. */
	abort();
}

/* Lay out (or relay out) the stack so the next switch-to runs bzy_co_start. The
   return-address slot sits at (16-aligned top - 8) so the slot is %16==8: the
   switch's `ret` then enters bzy_co_start with rsp %16==0, and its `call` restores
   the %16==8 the C ABI expects. */
static void co_setup(BzyCoroutine *c)
{
	uintptr_t top = ((uintptr_t)c->stack_hi) & ~(uintptr_t)15;
	uintptr_t ret = top - 8;
	*(void**)ret = (void*)bzy_co_start;
	uintptr_t sp = ret - BZY_CO_FRAME;
	memset((void*)sp, 0, (size_t)(ret - sp));   /* Zero the saved-register slots. */
	c->sp = (void*)sp;
}

/* Free list of finished coroutines (with their stacks) for reuse. A coroutine is
   created on the spawning worker and may be deleted on another (work-stealing),
   so the pool is shared under a lock. */
static BzyCoroutine *g_pool;
static bzy_mutex     g_pool_lock = BZY_MUTEX_INIT;

static BzyCoroutine *pool_pop(void)
{
	bzy_mutex_lock(&g_pool_lock);
	BzyCoroutine *c = g_pool;
	if (c)
	{
		g_pool = c->pool_next;
	}

	bzy_mutex_unlock(&g_pool_lock);
	return c;
}

static void pool_push(BzyCoroutine *c)
{
	bzy_mutex_lock(&g_pool_lock);
	c->pool_next = g_pool;
	g_pool = c;
	bzy_mutex_unlock(&g_pool_lock);
}

void bzy_coroutine_main_init(void)
{
	bzy_coroutine_thread_enter();
}

BzyCoroutine *bzy_coroutine_thread_enter(void)
{
	if (!t_current)
	{
		t_main.is_thread = 1;
#if defined(_WIN32)
		t_main.stack_hi = (void*)__readgsqword(0x08);   /* This OS thread's real stack bounds. */
		t_main.stack_lo = (void*)__readgsqword(0x10);
#endif
		t_current = &t_main;
	}

	return t_current;
}

BzyCoroutine *bzy_coroutine_create(BzyCoroutineFn fn, void *arg)
{
	BzyCoroutine *c = pool_pop();
	if (!c)
	{
		c = calloc(1, sizeof(*c));
		c->alloc = malloc(BZY_CO_STACK);
		c->stack_lo = c->alloc;
		c->stack_hi = (char*)c->alloc + BZY_CO_STACK;
	}

	c->fn = fn;
	c->arg = arg;
	c->is_thread = 0;
	co_setup(c);                /* Fresh entry frame at the top of the (possibly reused) stack. */
	return c;
}

void bzy_coroutine_switch(BzyCoroutine *to)
{
	BzyCoroutine *from = t_current;
	if (from == to)
	{
		return;
	}

	t_current = to;
	bzy_ctx_switch(&from->sp, to->sp, to->stack_hi, to->stack_lo);
}

BzyCoroutine *bzy_coroutine_self(void)
{
	return t_current;
}

void bzy_coroutine_delete(BzyCoroutine *c)
{
	if (!c || c->is_thread)
	{
		return;                 /* Never recycle/free a promoted OS thread. */
	}

	pool_push(c);               /* Recycle the stack instead of freeing it. */
}
