#ifndef ESCAPE_H
#define ESCAPE_H
#include "ast.h"
#include "types.h"

/* Mark non-escaping `new` sites for stack allocation and reserve frame space.
   Run after resolve and ownership have assigned offsets and types, and after
   escape_solve has fixed the interprocedural summaries this consults. For each
   qualifying EX_NEW it sets anno_stack=1 and anno_stack_off, and accumulates
   f->stack_alloc_bytes. */
void escape_annotate(TypeTable *tt, Func *f);

/* Compute the interprocedural escape summary (f->esc) for every body to a
   fixpoint, so escape_annotate can withhold a capture when a uniquely-resolved
   callee proves the receiver/argument does not escape. Optimistic least-fixpoint:
   summaries start empty and grow monotonically until stable. Run once over the
   whole program after resolution, before any escape_annotate. */
void escape_solve(TypeTable *tt, Func **funcs, int count);

#endif
