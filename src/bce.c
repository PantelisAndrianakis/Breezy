#include "bce.h"
#include "lexer.h"
#include <stddef.h>

/* Bounds-check elimination by forward interval analysis.

   For every integer local we carry a conservative inclusive interval [lo, hi];
   for every array local we carry an interval for its *length*. At each EX_INDEX
   `arr[idx]` we ask: is idx provably >= 0 and < the array's minimum length? If
   so the runtime check is redundant and we set anno_index_safe.

   Soundness is the whole game. Anything we cannot bound becomes the full range
   (`known == 0`), which never satisfies the elimination test, so a missed fact
   only costs a check we keep - never one we wrongly drop. Loops widen every
   variable they assign to the full range before the body is analysed, except a
   recognised counted-for induction variable, which gets its exact iteration
   range. Forward assignments inside the body then re-tighten derived values
   (e.g. `cbase = u * 8`) on each iteration, which is where the real indices come
   from. Arithmetic is done in 64-bit with overflow guarded to the operand's
   declared width, so a result that could wrap on the hardware degrades to the
   full range rather than a false-tight one. */

#define BCE_MAX 256
#define I32_MIN (-2147483648LL)
#define I32_MAX 2147483647LL

typedef struct
{
	int known;        /* 0 = full range (unbounded); 1 = [lo, hi] holds. */
	long long lo, hi;
} Iv;

static const Iv IV_TOP = { 0, 0, 0 };

static Iv iv_const(long long v)
{
	Iv r;
	r.known = 1;
	r.lo = v;
	r.hi = v;
	return r;
}

/* Restrict an interval to a declared integer width: if it might not fit, the
   hardware would wrap, so the only sound interval left is the full range. */
static Iv iv_clamp(Iv x, TypeKind k)
{
	if (!x.known)
	{
		return x;
	}

	if (k == TY_INT)
	{
		if (x.lo < I32_MIN || x.hi > I32_MAX)
		{
			return IV_TOP;
		}

		return x;
	}

	if (k == TY_LONG)
	{
		return x;   /* Computed in 64-bit with overflow already guarded. */
	}

	return IV_TOP;  /* Unsigned / non-integer index math: do not reason about it. */
}

static Iv iv_add(Iv a, Iv b)
{
	long long lo, hi;
	if (!a.known || !b.known
		|| __builtin_add_overflow(a.lo, b.lo, &lo)
		|| __builtin_add_overflow(a.hi, b.hi, &hi))
	{
		return IV_TOP;
	}

	Iv r = { 1, lo, hi };
	return r;
}

static Iv iv_sub(Iv a, Iv b)
{
	long long lo, hi;
	if (!a.known || !b.known
		|| __builtin_sub_overflow(a.lo, b.hi, &lo)
		|| __builtin_sub_overflow(a.hi, b.lo, &hi))
	{
		return IV_TOP;
	}

	Iv r = { 1, lo, hi };
	return r;
}

static Iv iv_mul(Iv a, Iv b)
{
	if (!a.known || !b.known)
	{
		return IV_TOP;
	}

	long long c[4];
	if (__builtin_mul_overflow(a.lo, b.lo, &c[0])
		|| __builtin_mul_overflow(a.lo, b.hi, &c[1])
		|| __builtin_mul_overflow(a.hi, b.lo, &c[2])
		|| __builtin_mul_overflow(a.hi, b.hi, &c[3]))
	{
		return IV_TOP;
	}

	long long lo = c[0], hi = c[0];
	for (int i = 1; i < 4; i++)
	{
		if (c[i] < lo)
		{
			lo = c[i];
		}

		if (c[i] > hi)
		{
			hi = c[i];
		}
	}

	Iv r = { 1, lo, hi };
	return r;
}

/* ---- environment: scalar-int intervals + array-length intervals ---- */

typedef struct
{
	int n;
	int off[BCE_MAX];
	Iv  iv[BCE_MAX];
	int an;
	int aoff[BCE_MAX];
	Iv  alen[BCE_MAX];
} Env;

static Iv env_get(Env *e, int off)
{
	for (int i = 0; i < e->n; i++)
	{
		if (e->off[i] == off)
		{
			return e->iv[i];
		}
	}

	return IV_TOP;
}

static void env_set(Env *e, int off, Iv v)
{
	if (off == 0)
	{
		return;
	}

	for (int i = 0; i < e->n; i++)
	{
		if (e->off[i] == off)
		{
			e->iv[i] = v;
			return;
		}
	}

	if (e->n < BCE_MAX)
	{
		e->off[e->n] = off;
		e->iv[e->n] = v;
		e->n++;
	}
}

static Iv alen_get(Env *e, int off)
{
	for (int i = 0; i < e->an; i++)
	{
		if (e->aoff[i] == off)
		{
			return e->alen[i];
		}
	}

	return IV_TOP;
}

static void alen_set(Env *e, int off, Iv v)
{
	if (off == 0)
	{
		return;
	}

	for (int i = 0; i < e->an; i++)
	{
		if (e->aoff[i] == off)
		{
			e->alen[i] = v;
			return;
		}
	}

	if (e->an < BCE_MAX)
	{
		e->aoff[e->an] = off;
		e->alen[e->an] = v;
		e->an++;
	}
}

/* ---- interval of an expression under the current environment ---- */

static Iv iv_expr(Expr *e, Env *env)
{
	if (!e)
	{
		return IV_TOP;
	}

	switch (e->kind)
	{
	case EX_INT:
		return iv_const(e->int_val);
	case EX_IDENT:
		if (ty_is_signed(e->type.kind) && e->anno_int != 0)
		{
			return env_get(env, e->anno_int);
		}

		return IV_TOP;
	case EX_CAST:
		return iv_clamp(iv_expr(e->lhs, env), e->type.kind);
	case EX_BINARY:
	{
		Iv a = iv_expr(e->lhs, env);
		Iv b = iv_expr(e->rhs, env);
		Iv r = IV_TOP;
		long long c = (e->rhs->kind == EX_INT) ? e->rhs->int_val : 0;
		int rhs_const = (e->rhs->kind == EX_INT);
		switch (e->op)
		{
		case TOKEN_PLUS:
			r = iv_add(a, b);
			break;
		case TOKEN_MINUS:
			r = iv_sub(a, b);
			break;
		case TOKEN_STAR:
			r = iv_mul(a, b);
			break;
		case TOKEN_SHL:
			if (rhs_const && c >= 0 && c < 31)
			{
				r = iv_mul(a, iv_const(1LL << c));
			}

			break;
		case TOKEN_SLASH:
			if (rhs_const && c > 0 && a.known && a.lo >= 0)
			{
				Iv t = { 1, a.lo / c, a.hi / c };
				r = t;
			}

			break;
		case TOKEN_PERCENT:
			if (rhs_const && c > 0 && a.known && a.lo >= 0)
			{
				long long hi = (a.hi < c - 1) ? a.hi : c - 1;
				Iv t = { 1, 0, hi };
				r = t;
			}

			break;
		case TOKEN_AMP:
			if (rhs_const && c >= 0)
			{
				Iv t = { 1, 0, c };
				r = t;
			}

			break;
		default:
			break;
		}

		return iv_clamp(r, e->type.kind);
	}
	default:
		return IV_TOP;
	}
}

/* ---- modified-set: every local a region writes (recursively) ---- */

typedef struct
{
	int off[BCE_MAX];
	int n;
} OffSet;

static void off_add(OffSet *s, int off)
{
	if (off == 0)
	{
		return;
	}

	for (int i = 0; i < s->n; i++)
	{
		if (s->off[i] == off)
		{
			return;
		}
	}

	if (s->n < BCE_MAX)
	{
		s->off[s->n++] = off;
	}
}

static int off_has(OffSet *s, int off)
{
	for (int i = 0; i < s->n; i++)
	{
		if (s->off[i] == off)
		{
			return 1;
		}
	}

	return 0;
}

static void mod_expr(Expr *e, OffSet *s)
{
	if (!e)
	{
		return;
	}

	if (e->kind == EX_INCDEC && e->lhs && e->lhs->kind == EX_IDENT)
	{
		off_add(s, e->lhs->anno_int);
	}

	mod_expr(e->lhs, s);
	mod_expr(e->rhs, s);
	for (int i = 0; i < e->arg_count; i++)
	{
		mod_expr(e->args[i], s);
	}
}

static void mod_block(Block *b, OffSet *s);

static void mod_stmt(Stmt *st, OffSet *s)
{
	if (!st)
	{
		return;
	}

	if (st->kind == ST_VARDECL)
	{
		off_add(s, st->decl_offset);
	}

	if (st->kind == ST_ASSIGN && st->target && st->target->kind == EX_IDENT)
	{
		off_add(s, st->target->anno_int);
	}

	if (st->kind == ST_FOREACH)
	{
		off_add(s, st->decl_offset);
		off_add(s, st->fe_val_offset);
	}

	mod_expr(st->cond, s);
	mod_expr(st->decl_init, s);
	mod_expr(st->value, s);
	mod_expr(st->target, s);
	mod_expr(st->ret_val, s);
	mod_expr(st->expr, s);
	mod_stmt(st->for_init, s);
	mod_stmt(st->for_post, s);
	mod_block(st->then_blk, s);
	mod_block(st->else_blk, s);
}

static void mod_block(Block *b, OffSet *s)
{
	if (!b)
	{
		return;
	}

	for (int i = 0; i < b->count; i++)
	{
		mod_stmt(b->stmts[i], s);
	}
}

/* True if any identifier read inside `e` refers to a slot in `s`. Used to reject
   a loop bound that the loop itself rewrites. */
static int expr_reads_any(Expr *e, OffSet *s)
{
	if (!e)
	{
		return 0;
	}

	if (e->kind == EX_IDENT && off_has(s, e->anno_int))
	{
		return 1;
	}

	if (expr_reads_any(e->lhs, s) || expr_reads_any(e->rhs, s))
	{
		return 1;
	}

	for (int i = 0; i < e->arg_count; i++)
	{
		if (expr_reads_any(e->args[i], s))
		{
			return 1;
		}
	}

	return 0;
}

/* ---- annotate indexes under a (read-only) environment ---- */

static void mark_indexes(Expr *e, Env *env)
{
	if (!e)
	{
		return;
	}

	mark_indexes(e->lhs, env);
	mark_indexes(e->rhs, env);
	for (int i = 0; i < e->arg_count; i++)
	{
		mark_indexes(e->args[i], env);
	}

	if (e->kind == EX_INDEX && e->lhs->kind == EX_IDENT)
	{
		Iv len = alen_get(env, e->lhs->anno_int);
		if (len.known)
		{
			Iv idx = iv_expr(e->rhs, env);
			if (idx.known && idx.lo >= 0 && idx.hi < len.lo)
			{
				e->anno_index_safe = 1;
			}
		}
	}
}

static void bce_block(Block *b, Env *env);

/* Record an array-length fact for `off` from the value it is assigned, or clear
   it (alias to an array of unknown length) so we never assume a stale length. */
static void note_array_assign(Env *env, int off, Expr *val)
{
	if (val && val->kind == EX_NEWARRAY)
	{
		alen_set(env, off, iv_expr(val->lhs, env));
	}
	else
	{
		alen_set(env, off, IV_TOP);
	}
}

/* Analyse a loop. `body`/`post` are widened to the full range up front; if the
   loop is a recognised increasing counted-for, its induction variable instead
   gets its exact iteration range, which is what makes derived indices provable. */
static void bce_loop(Stmt *st, Env *env)
{
	OffSet mod = { .n = 0 };
	mod_block(st->then_blk, &mod);
	mod_stmt(st->for_post, &mod);

	/* Recognise: for (int i = LO; i < HI; i = i + C>0)  (or i++). */
	int ind = 0;
	Iv ind_iv = IV_TOP;
	if (st->kind == ST_FOR && st->for_init && st->cond && st->for_post)
	{
		int io = 0;
		Expr *lo = NULL;
		if (st->for_init->kind == ST_VARDECL)
		{
			io = st->for_init->decl_offset;
			lo = st->for_init->decl_init;
		}
		else if (st->for_init->kind == ST_ASSIGN && st->for_init->target
				 && st->for_init->target->kind == EX_IDENT)
		{
			io = st->for_init->target->anno_int;
			lo = st->for_init->value;
		}

		Expr *cond = st->cond;
		int cmp_ok = (cond->kind == EX_BINARY
					  && (cond->op == TOKEN_LT || cond->op == TOKEN_LTE)
					  && cond->lhs->kind == EX_IDENT && cond->lhs->anno_int == io
					  && io != 0);

		/* Positive step: `i = i + C` (C>0) or `i++`. */
		int step_ok = 0;
		Stmt *post = st->for_post;
		if (post->kind == ST_ASSIGN && post->target && post->target->kind == EX_IDENT
			&& post->target->anno_int == io && post->value
			&& post->value->kind == EX_BINARY && post->value->op == TOKEN_PLUS
			&& post->value->lhs->kind == EX_IDENT && post->value->lhs->anno_int == io
			&& post->value->rhs->kind == EX_INT && post->value->rhs->int_val > 0)
		{
			step_ok = 1;
		}
		else if (post->kind == ST_EXPR && post->expr && post->expr->kind == EX_INCDEC
				 && post->expr->op == TOKEN_PLUSPLUS
				 && post->expr->lhs && post->expr->lhs->kind == EX_IDENT
				 && post->expr->lhs->anno_int == io)
		{
			step_ok = 1;
		}

		if (cmp_ok && step_ok && lo)
		{
			Iv lo_iv = iv_expr(lo, env);          /* Evaluated in the pre-loop env. */
			Iv hi_iv = iv_expr(cond->rhs, env);
			int hi_stable = !expr_reads_any(cond->rhs, &mod);
			if (lo_iv.known && hi_iv.known && hi_stable)
			{
				long long top = (cond->op == TOKEN_LT) ? hi_iv.hi - 1 : hi_iv.hi;
				if (top >= lo_iv.lo)
				{
					Iv t = { 1, lo_iv.lo, top };
					ind = io;
					ind_iv = iv_clamp(t, TY_INT);
				}
			}
		}
	}

	Env child = *env;
	for (int i = 0; i < mod.n; i++)
	{
		env_set(&child, mod.off[i], IV_TOP);   /* Loop-carried values: unknown in the body. */
	}

	if (ind != 0)
	{
		env_set(&child, ind, ind_iv);
	}

	mark_indexes(st->cond, &child);
	bce_block(st->then_blk, &child);
	if (st->for_post)
	{
		mark_indexes(st->for_post->value, &child);
		mark_indexes(st->for_post->expr, &child);
	}

	/* After the loop, the parent must not trust any value the loop rewrote. */
	for (int i = 0; i < mod.n; i++)
	{
		env_set(env, mod.off[i], IV_TOP);
		alen_set(env, mod.off[i], IV_TOP);
	}
}

static void bce_stmt(Stmt *st, Env *env)
{
	if (!st)
	{
		return;
	}

	switch (st->kind)
	{
	case ST_VARDECL:
		mark_indexes(st->decl_init, env);
		if (st->decl_type.kind == TY_ARRAY)
		{
			note_array_assign(env, st->decl_offset, st->decl_init);
		}
		else if (ty_is_signed(st->decl_type.kind))
		{
			env_set(env, st->decl_offset,
					st->decl_init ? iv_expr(st->decl_init, env) : IV_TOP);
		}

		break;
	case ST_ASSIGN:
		mark_indexes(st->value, env);
		mark_indexes(st->target, env);
		if (st->target && st->target->kind == EX_IDENT)
		{
			if (st->target->type.kind == TY_ARRAY)
			{
				note_array_assign(env, st->target->anno_int, st->value);
			}
			else if (ty_is_signed(st->target->type.kind))
			{
				env_set(env, st->target->anno_int, iv_expr(st->value, env));
			}
		}

		break;
	case ST_FOR:
	case ST_WHILE:
	case ST_FOREACH:
		bce_loop(st, env);
		break;
	case ST_IF:
	{
		mark_indexes(st->cond, env);
		/* Branches may each assign; widen their union, then descend so inner
		   loops still get tight local facts. */
		OffSet mod = { .n = 0 };
		mod_block(st->then_blk, &mod);
		mod_block(st->else_blk, &mod);
		Env c1 = *env;
		Env c2 = *env;
		bce_block(st->then_blk, &c1);
		bce_block(st->else_blk, &c2);
		for (int i = 0; i < mod.n; i++)
		{
			env_set(env, mod.off[i], IV_TOP);
			alen_set(env, mod.off[i], IV_TOP);
		}

		break;
	}
	case ST_SWITCH:
	case ST_CASE:
	case ST_DEFAULT:
	case ST_TRY:
	case ST_CATCH:
	{
		mark_indexes(st->cond, env);
		mark_indexes(st->value, env);
		mark_indexes(st->expr, env);
		OffSet mod = { .n = 0 };
		mod_block(st->then_blk, &mod);
		mod_block(st->else_blk, &mod);
		Env child = *env;
		bce_block(st->then_blk, &child);
		Env child2 = *env;
		bce_block(st->else_blk, &child2);
		for (int i = 0; i < mod.n; i++)
		{
			env_set(env, mod.off[i], IV_TOP);
			alen_set(env, mod.off[i], IV_TOP);
		}

		break;
	}
	default:
		mark_indexes(st->cond, env);
		mark_indexes(st->value, env);
		mark_indexes(st->expr, env);
		mark_indexes(st->ret_val, env);
		mark_indexes(st->decl_init, env);
		break;
	}
}

static void bce_block(Block *b, Env *env)
{
	if (!b)
	{
		return;
	}

	for (int i = 0; i < b->count; i++)
	{
		bce_stmt(b->stmts[i], env);
	}
}

void bce_annotate(Func *f)
{
	if (f->is_extern || !f->body)
	{
		return;
	}

	Env env;
	env.n = 0;
	env.an = 0;
	bce_block(f->body, &env);
}
