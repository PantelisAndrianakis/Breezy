#ifndef IREMIT_H
#define IREMIT_H

#include "codegen.h"
#include "ir.h"

/* Emit `label:` as a complete function body from IR, ABI-identical to
   cg_emit_func for a call-free leaf: standard rbp frame, integer args spilled to
   their slots, result in rax. Naive - every vreg lives in its own frame slot.
   Only valid for the integer scalar functions ir_eligible accepts. */
void ir_emit_func(Codegen *cg, IRFunc *f, const char *label);

#endif
