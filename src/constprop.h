#ifndef CONSTPROP_H
#define CONSTPROP_H

#include "ast.h"

/* Constant propagation for single-assignment literal scalars. A 64-bit integer
   local assigned exactly once, at function scope, from an integer literal and
   never reassigned has each later read rewritten to that literal. The local then
   needs neither a per-use stack reload nor a promotion register, freeing that
   register for hotter values (e.g. a loop bound stops pinning a register across
   intervening loops). Run after the resolver assigns slot offsets and before
   promote_annotate, which then no longer sees the local as a candidate. */
void constprop_annotate(Func *f);

#endif
