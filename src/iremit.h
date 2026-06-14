#ifndef IREMIT_H
#define IREMIT_H

#include "codegen.h"
#include "ir.h"
#include "regalloc.h"

/* Emit `label:` as a complete function body from IR, ABI-identical to
   cg_emit_func for a call-free leaf: standard rbp frame, integer args spilled to
   their slots, result in rax. Naive - every vreg lives in its own frame slot.
   Only valid for the integer scalar functions ir_eligible accepts. */
void ir_emit_func(Codegen *cg, IRFunc *f, const char *label);

/* Emit a lowered loop region inline inside the current emitter function: saves
   the callee-saved registers the allocation uses into the shared region area
   (spill slots at [rbp - spill_base ...], saves right below them), loads
   register-allocated frame locals from their home slots, emits the blocks,
   stores the locals back and restores. Emits no labels other than .L block
   labels, no section directives, no ret. The caller owns `f` and `a`. */
void ir_emit_region(Codegen *cg, IRFunc *f, IRAlloc *a, int spill_base, const Stmt *region);

#endif
