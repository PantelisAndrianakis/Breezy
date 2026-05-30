#ifndef OWNERSHIP_H
#define OWNERSHIP_H
#include "ast.h"

/* Record the stack offset of every object-typed local (each ST_VARDECL whose
   type is an object) into f->obj_local_offsets. Must run after resolve has
   assigned each local its decl_offset. */
void ownership_annotate(Func *f);

#endif
