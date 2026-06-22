#ifndef OVERLOAD_H
#define OVERLOAD_H
#include "ast.h"
#include <stddef.h>

/* Type-based overloading: signature encoding + best-match selection. Pure
   functions over TypeRef/Func — no dependency on the type table or resolver, so
   the logic is unit-testable in isolation and mirrors assignable()'s conversion
   rules (with class-aware ranking added for selection only). */

/* Append the canonical, injective signature suffix for a parameter-type list to
   `out` (an empty list yields "v"). Distinct lists never collide, so the suffix
   safely disambiguates overloaded symbol names. */
void overload_encode_types(char *out, size_t cap, const TypeRef *types, int count);

/* Match rank of one argument type against one parameter type:
     0  exact     (same kind; objects same class; array/map/generic exact element)
     1  widening  (int -> wider int, same signedness; int -> double)
     2  permissive (object -> different-class object; null -> any managed ref)
    -1  no match
   Mirrors the conversions assignable() permits, plus class-aware ranking. */
int overload_rank_arg(const TypeRef *param, const TypeRef *arg);

/* One overload candidate. min_args = count of leading params without a default;
   a call of n args is admissible when min_args <= n <= param_count. */
typedef struct
{
	const TypeRef *param_types;
	int param_count;
	int min_args;
	int is_variadic;   /* FFI variadic extern: accept argc > param_count (extra args unchecked). */
} OverloadCand;

#define OVL_NONE  (-1)   /* No viable candidate. */
#define OVL_AMBIG (-2)   /* Two or more equally-good candidates. */

/* Pick the best candidate for args[0..argc). Returns its index, or OVL_NONE /
   OVL_AMBIG. "Best" = viable, and no worse on every argument plus strictly
   better on at least one than every other viable candidate. */
int overload_select(const OverloadCand *cands, int ncand,
					const TypeRef *args, int argc);

/* Min admissible arg count for a Func (params before the first defaulted one). */
int overload_min_args(const Func *f);

/* 1 if the set contains two candidates that tie for some realizable arity —
   used to reject ambiguous overload sets at declaration time (spec section A). */
int overload_set_is_ambiguous(const OverloadCand *cands, int ncand);

#endif
