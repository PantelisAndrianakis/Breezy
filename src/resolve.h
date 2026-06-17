#ifndef RESOLVE_H
#define RESOLVE_H
#include "ast.h"
#include "types.h"
void resolve_program(TypeTable *tt, Unit **units, int unit_count);
void resolve_func(TypeTable *tt, Func *f, const char *this_class /* NULL if file-scope */);

/* Lambda body registry (populated during resolution): the codegen driver emits
   each synthesized body function and its closure typeinfo descriptor. */
int         bzy_lambda_count(void);
LambdaInfo *bzy_lambda_at(int i);
#endif
