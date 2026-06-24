#ifndef CODEGEN_H
#define CODEGEN_H
#include <stdio.h>
#include "ast.h"
#include "types.h"
#include "ir.h"
#include "regalloc.h"

typedef enum { TARGET_WINDOWS, TARGET_LINUX } Target;


typedef struct
{
	FILE *out;
	char last_line[256];   /* The previous emitted line, for cg_emit's spill-reload peephole. */
	int label_count;
	Target target;     /* Emission target ABI/format (Win64 vs System V AMD64). */
	int sp_save;       /* The rbp offset holding the saved rsp across a runtime call. */
	int val_save;      /* The rbp offset that preserves an object value across a call. */
	int argtmp_base;   /* The rbp offset of ARGTMP[0]; ARGTMP[i] is argtmp_base + i*8. */
	int assign_save;   /* The rbp offset that holds a new field-store value across receiver evaluation. */
	int fp_save;       /* The rbp offset for spilling xmm0 (FP return / FP field store). */
	int rbx_save;      /* The rbp offset holding the caller's rbx. rbx is callee-saved in both ABIs;
	                      codegen uses it as scratch, so every function must save/restore it or it
	                      corrupts a caller (e.g. the C runtime) that kept a value in rbx across the call. */
	int callee_save[4];  /* rbp offsets holding the caller's r12..r15 when this frame promotes locals. */
	Func *cur_func;      /* The Func currently being emitted, or NULL for synthesized/hand-rolled frames. */
	int temp_base;       /* rbp offset of temp slot 0; slot i is [rbp - (temp_base + i*8)] (fixed-rsp plan). */
	int outarg_base;     /* == frame; outgoing args/shadow sit at the bottom, addressed rsp-relative. */
	int cur_temp_depth;  /* Emission-time count of live temp slots; reset to 0 per function. */
	int cur_temp_cap;    /* This function's max_temp_depth; cg_temp_push traps on overflow. */
	int scratch_base;    /* rbp offset of the scratch arena's empty top; a block at cursor c reaches [rbp-(scratch_base+c)]. */
	int cur_scratch;     /* Emission-time arena high-water cursor in bytes; reset to 0 per function. */
	int cur_scratch_cap; /* This function's arena size (max_scratch_bytes, 16-aligned); cg_scratch_alloc traps on overflow. */
	int cur_break_label;     /* Enclosing loop's end label (-1 if not in a loop). */
	int cur_continue_label;  /* Enclosing loop's continue target (-1 if not in a loop). */
	int hoist_n;             /* Loop-invariant register cache: number of locals cached in r8..r11 for the current innermost loop. */
	int hoist_off[4];        /* Their slot offsets; an EX_IDENT read of one of these uses its register instead of a memory reload. */
	int hoist_reg[4];        /* Their register index 0..3 -> r8..r11. */
	int hoist_depth;         /* Nesting counter: > 0 means an outer loop owns the hoist registers; inner loops skip begin/end to keep outer state. */
	int   sr_n;              /* Strength-reduced array accesses: arr[INV + iv] whose element-0 address (arr + INV*stride + 32) is pinned in a register. */
	Expr *sr_node[4];        /* The EX_INDEX node each entry covers (matched by identity at its codegen site). */
	int   sr_reg[4];         /* Their register index 0..3 -> r8..r11 (shared pool with the value cache above). */
	int   sr_stride[4];      /* Element stride in bytes, so the access is [reg + iv*stride]. */
	const char *sr_ivreg;    /* The induction variable's register for the current loop (coefficient-1 index term). */
	int   cur_counter_off;   /* Slot offset of the current loop's promoted, non-negative, unit-step int counter (0 if none): its `i=i+1` skips the re-extension, since a 32-bit add zero-extends and the value never goes negative. */
	int   defer_n;           /* Promoted int accumulators in the current loop whose per-iteration sign-extension is deferred to the loop exit (every use is `acc = acc +/- EXPR`, lowered to a 32-bit add). */
	int   defer_off[8];      /* Their slot offsets; re-extended once at the loop end label. */
	int   lpromo_n;          /* Loop-scoped float promotion: hot doubles homed in xmm6..xmm11 for the current innermost call-free loop (loaded before the entry guard, stored back after the end label). */
	int   lpromo_off[6];     /* Their slot offsets; entry i lives in CG_LPROMO_REGS[i]. */
	int   lpromo_save_base;  /* rbp offset of the dedicated frame area preserving the caller's xmm6.. around an armed loop on Win64 (callee-saved there; SysV leaves them volatile). Register i saves at [rbp - (lpromo_save_base + i*16)]. */
	int   low32_ok;          /* One-hop hint set by a parent before evaluating an int operand whose result it consumes only at 32 bits (the low-32-pure ops + - * & | ^ <<, and stores into an int slot, which reload sign-extended). When set, an int binary skips its trailing movsxd: the 32-bit op already left the low 32 bits correct, and the high bits are unobserved. cg_expr captures and clears it on entry, so it never reaches a grandchild. */
	int   unrolling;         /* Depth of active fully-unrolled fixed-trip loops (0 = none): induction variables are compile-time constants per copy. */
	int   unroll_iv_off;     /* The INNERMOST unrolled loop's induction variable slot. */
	long long unroll_iv_val; /* Its constant value for the copy currently being emitted. */
	int   uc_n;              /* Copy-constant environment: locals whose value is a compile-time constant for the current unrolled copy - the induction variables plus any local assigned a foldable expression of them (cb = u * 8). */
	int   *uc_off;           /* Their slot offsets; grown dynamically. */
	long long *uc_val;       /* Their constant values. */
	int   uc_cap;
	Stmt *cur_accum_stmt;    /* P5: the accumulation stmt to lower to sb appends, or NULL. */
	int cur_accum_sb_off;    /* P5: frame offset of the active lowering StringBuilder. */
	int exception_fn_count;  /* Number of per-function exception records emitted so far. */
	const char *cur_file;    /* Source path of the unit whose functions are being emitted (for stack-trace records; NULL for synthesized frames). */
	int line_lbl_seq;        /* Monotonic file-unique counter for ..@line<n> stack-trace markers. */
	int *cur_line;           /* Stack-trace markers in the current function: interleaved [label-id, source-line] pairs. Grown dynamically; reset per function. */
	int cur_line_count;      /* Pair count. */
	int cur_line_cap;
	int cur_line_last;       /* Last source line emitted (deduplicates consecutive markers). */
	int exception_try_count; /* File-unique try-region label counter (__exceptiontry<k>_*). */
	int cur_try_count;       /* Try-regions in the function currently being emitted. */
	int cur_try_cap;
	int *cur_try_k;          /* Their label indices; grown dynamically. */
	int *cur_try_c;          /* Their clause indices (distinct landing pads per try). */
	char (*cur_try_vt)[64];  /* Their catch-type class names (for __vtable_<name>). */
	const Stmt **region_stmt; /* IR loop regions: the loop statements the pre-scan recorded. Grown dynamically. */
	IRFunc     **region_irf;  /* Their lowered IR. */
	IRAlloc    **region_alloc;/* Their register allocations (also sized the shared frame area). */
	int         region_count; /* Regions recorded for the current function. */
	int         region_cap;
	int         region_base;                 /* rbp offset of the shared region spill/callee-save area. */
	FuncInfo **breeze_thunks; /* spawn-with-args targets needing a __breeze_ thunk (deduped). Grown. */
	int breeze_thunk_count;
	int breeze_thunk_cap;
	FuncInfo **blocking_thunks; /* `extern blocking` targets needing a __blocking_ thunk (deduped). */
	int blocking_thunk_count;
	int blocking_thunk_cap;
	struct fpk_entry
	{
		int is_float;             /* 1 for a 32-bit float constant, 0 for 64-bit double. */
		unsigned long long bits;  /* IEEE-754 bit pattern (low 32 bits used when is_float). */
	} *fpk;            /* Floating-point literal pool, emitted in .data as __fpk<id>. Grown. */
	int fpk_count;
	int fpk_cap;
	struct strk_entry
	{
		char *bytes;              /* Decoded literal bytes (no embedded NUL); allocated to len. */
		int  len;
	} *strk;           /* String literal pool, emitted in .data as __str<id>. Grown. */
	int strk_count;
	int strk_cap;
	int call_variadic;   /* FFI: 1 while lowering a call to a variadic extern (set per call site). */
} Codegen;

void cg_init(Codegen *cg, FILE *out);
void cg_emit(Codegen *cg, const char *fmt, ...);
int  cg_label(Codegen *cg);
void cg_program(Codegen *cg, TypeTable *tt, Unit **units, int unit_count);

/* Emit one per-function exception record (PC range, frame size, name, object-local
   offsets, try table) into .data, leaving .text active. Shared with the IR emitter
   so an IR function that can throw a bounds error is unwindable. */
void cg_emit_exception_record(Codegen *cg, const char *label, int frame, Func *f);
#endif
