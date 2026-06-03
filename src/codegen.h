#ifndef CODEGEN_H
#define CODEGEN_H
#include <stdio.h>
#include "ast.h"
#include "types.h"

typedef struct
{
	FILE *out;
	int label_count;
	int sp_save;       /* The rbp offset holding the saved rsp across a runtime call. */
	int val_save;      /* The rbp offset that preserves an object value across a call. */
	int argtmp_base;   /* The rbp offset of ARGTMP[0]; ARGTMP[i] is argtmp_base + i*8. */
	int assign_save;   /* The rbp offset that holds a new field-store value across receiver evaluation. */
	int fp_save;       /* The rbp offset for spilling xmm0 (FP return / FP field store). */
	int cur_break_label;     /* Enclosing loop's end label (-1 if not in a loop). */
	int cur_continue_label;  /* Enclosing loop's continue target (-1 if not in a loop). */
	int exception_fn_count;  /* Number of per-function exception records emitted so far. */
	int exception_try_count; /* File-unique try-region label counter (__exceptiontry<k>_*). */
	int cur_try_count;       /* Try-regions in the function currently being emitted. */
	int cur_try_k[64];       /* Their label indices. */
	int cur_try_c[64];       /* Their clause indices (distinct landing pads per try). */
	char cur_try_vt[64][64]; /* Their catch-type class names (for __vtable_<name>). */
	FuncInfo *breeze_thunks[64]; /* spawn-with-args targets needing a __breeze_ thunk (deduped). */
	int breeze_thunk_count;
	struct
	{
		int is_float;             /* 1 for a 32-bit float constant, 0 for 64-bit double. */
		unsigned long long bits;  /* IEEE-754 bit pattern (low 32 bits used when is_float). */
	} fpk[256];        /* Floating-point literal pool, emitted in .data as __fpk<id>. */
	int fpk_count;
	struct
	{
		char bytes[256];          /* Decoded literal bytes (no embedded NUL). */
		int  len;
	} strk[256];       /* String literal pool, emitted in .data as __str<id>. */
	int strk_count;
} Codegen;

void cg_init(Codegen *cg, FILE *out);
void cg_emit(Codegen *cg, const char *fmt, ...);
int  cg_label(Codegen *cg);
void cg_program(Codegen *cg, TypeTable *tt, Unit **units, int unit_count);
#endif
