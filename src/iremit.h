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

/* Whether cg_scan_regions should keep this region on the IR path despite the xmm
   hot-spill heuristic. True iff the region is a vectorizable reduction whose emit
   will succeed: the vectorizer reads invariant scalars from their home slots, so
   high xmm pressure (the matrix's 16 coefficients spilling) does not block it -
   only acc's xmm register and the array bases / loop index need to be resident,
   which this checks exactly as ir_emit_region's reduction path does. Other
   vectorizable shapes are low-pressure and never hot-spill, so they need no
   exemption and this returns 0 for them. */
int ir_region_vectorizable(const Func *f, const Stmt *s, IRAlloc *a);

#endif
