#ifndef FIELDINIT_H
#define FIELDINIT_H
#include "ast.h"

/* Lower instance-field initializers to constructor-body assignments.
   For each non-static class whose inheritance chain declares initializers
   on instance fields, synthesize a zero-arg constructor when the class has
   none, then prepend `this.field = init` assignments ancestor-first in
   declaration order. Runs after generics_expand (so instantiated generic
   classes are lowered too) and BEFORE the type table is built (types.c
   captures has_ctor and the constructor label from d->ctor). Clears each
   instance field's init afterwards, so resolve's declaration-initializer
   loop only ever sees static fields. */
void fieldinit_expand(Unit **units, int total);

#endif
