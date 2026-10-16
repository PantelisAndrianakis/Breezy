#ifndef IRLOWER_H
#define IRLOWER_H

#include "ast.h"
#include "ir.h"
#include "types.h"   /* TypeTable. */

/**
 * 1 if `f` is a call-free scalar-numeric function the v1 IR backend can compile: only i8..i64/f32/f64/bool scalars; arithmetic, compares, if/else, while, for, return; no calls, arrays, strings, exceptions, managed-object refcounting, coroutines, channels, foreach, switch, throw.
 * Arrays arrive in a later plan.
 */
int ir_eligible(const Func *f);

/**
 * Set the TypeTable for the current codegen run (used to resolve a direct call's callee during IR call lowering).
 * Call once before lowering; safe to leave unset outside codegen (call eligibility is then off).
 */
void ir_set_tt(TypeTable *tt);

/**
 * Lower an eligible function to IR, or NULL if a construct it cannot yet handle is reached (the caller then falls back to the emitter).
 * Caller owns the result and must ir_func_free it.
 */
IRFunc *ir_lower_func(const Func *f, TypeTable *tt);

/**
 * IR loop regions (Plan 4): an outermost for/while whose whole subtree lowers is emitted from IR inline inside an emitter-compiled function, sharing its frame.
 * On top of the whole-function rules, a region rejects ST_RETURN (it cannot run the function epilogue) and assignments to managed-typed locals (reassignment would skip refcounting; element stores remain fine).
 */
int ir_region_eligible(const Stmt *s);

/* Region eligibility for the C route: the same rules plus an integer or bool `switch` with literal case labels, static final arrays and clock reads. */
int ir_region_eligible_c(const Stmt *s);
int ir_c_expr_ok(const Expr *e);   /* C route: 1 when expression e can be emitted as C inside a region. */

/* 1 for Clock.currentTimeNanos() or Clock.currentTimeMillis(): a runtime read that never throws and touches no Breezy state, so generated C may call it. */
int ir_is_clock_read(const Expr *e);

/**
 * 1 for an integer or bool push onto a local List, Stack, Queue or Deque (add, push, enqueue, addLast), 2 for PriorityQueue.add, else 0.
 * Generated C stores the value inline when there is room and calls the runtime otherwise.
 */
int ir_c_push(const Expr *e);
const char *ir_c_call(const Expr *e);   /* C route: the runtime function for a non-throwing integer Map/TreeMap/PriorityQueue call on a local, or NULL. */
FuncInfo *ir_c_callee(const Expr *e);   /* C route: the callee of a direct call to a function compiled whole to C, or NULL. */
const char *ir_c_rnd(const Expr *e);   /* C route: the runtime function for a non-throwing Random builtin call, or NULL. */
int ir_c_foreach(const Stmt *s);   /* C route: a foreach over a local string, integer or double array, or such a List/Stack/Queue/Deque whose body never names it. */

/**
 * Lower an eligible region to IR with no IR_RET: the final block is empty and emission falls off it into the region epilogue.
 * NULL when ineligible.
 * Caller owns the result and must ir_func_free it.
 */
IRFunc *ir_lower_region(const Func *f, const Stmt *s);

/**
 * If `slot` (a frame offset) names a function-level constant local of `f` - one every assignment to which is the same integer literal - store that value in *out and return 1; else 0.
 * Used by the region vectorizer to resolve a loop bound that reads such a local (e.g. `for i < verts` where `verts = 1000000`) to its constant, since the bound is folded to an immediate in the IR but is still an EX_IDENT in the AST the vectorizer analyzes.
 */
int ir_func_const_local(const Func *f, int slot, long long *out);

#endif
