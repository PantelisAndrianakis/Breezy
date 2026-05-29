#ifndef CODEGEN_H
#define CODEGEN_H
#include <stdio.h>
#include "ast.h"
#include "types.h"

typedef struct
{
	FILE *out;
	int label_count;
} Codegen;

void cg_init(Codegen *cg, FILE *out);
void cg_emit(Codegen *cg, const char *fmt, ...);
int  cg_label(Codegen *cg);
void cg_program(Codegen *cg, TypeTable *tt, Unit **units, int unit_count);
#endif
