#ifndef ESCAPE_H
#define ESCAPE_H
#include "ast.h"
#include "types.h"

/* Mark non-escaping `new` sites for stack allocation and reserve frame space.
   Run after resolve and ownership have assigned offsets and types. For each
   qualifying EX_NEW it sets anno_stack=1 and anno_stack_off, and accumulates
   f->stack_alloc_bytes. */
void escape_annotate(TypeTable *tt, Func *f);

#endif
