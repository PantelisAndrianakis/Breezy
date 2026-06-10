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

/* IR loop regions (Plan 4): an outermost for/while whose whole subtree lowers is
   emitted from IR inline inside an emitter-compiled function, sharing its frame.
   On top of the whole-function rules, a region rejects ST_RETURN (it cannot run
   the function epilogue) and assignments to managed-typed locals (reassignment
   would skip refcounting; element stores remain fine). */
int ir_region_eligible(const Stmt *s);

/* Lower an eligible region to IR with no IR_RET: the final block is empty and
   emission falls off it into the region epilogue. NULL when ineligible. Caller
   owns the result and must ir_func_free it. */
IRFunc *ir_lower_region(const Func *f, const Stmt *s);

#endif
