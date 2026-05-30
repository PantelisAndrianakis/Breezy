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
} Codegen;

void cg_init(Codegen *cg, FILE *out);
void cg_emit(Codegen *cg, const char *fmt, ...);
int  cg_label(Codegen *cg);
void cg_program(Codegen *cg, TypeTable *tt, Unit **units, int unit_count);
#endif
