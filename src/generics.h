#ifndef GENERICS_H
#define GENERICS_H
#include "ast.h"

/* Lower every user-generic application in the program to an ordinary class.
   Clones each (template, type-arg-tuple) into a synthesized ClassDecl, appends
   it as a new Unit to units[*total] (growing *total, never exceeding max),
   rewrites all application references to plain object refs + EX_NEW, and clears
   each template Unit's klass so it is not registered/emitted. After this pass no
   TY_GENERIC (user) nodes and no generic-class templates remain. */
void generics_expand(Unit **units, int *total, int max);

#endif
