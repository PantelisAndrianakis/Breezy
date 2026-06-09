#ifndef IRLOWER_H
#define IRLOWER_H

#include "ast.h"
#include "ir.h"
#include "types.h"   /* TypeTable. */

/* 1 if `f` is a call-free scalar-numeric function the v1 IR backend can compile:
   only i8..i64/f32/f64/bool scalars; arithmetic, compares, if/else, while, for,
   return; no calls, arrays, strings, exceptions, managed-object refcounting,
   coroutines, channels, foreach, switch, throw. Arrays arrive in a later plan. */
int ir_eligible(const Func *f);

/* Lower an eligible function to IR, or NULL if a construct it cannot yet handle
   is reached (the caller then falls back to the emitter). Caller owns the result
   and must ir_func_free it. */
IRFunc *ir_lower_func(const Func *f, TypeTable *tt);

#endif
