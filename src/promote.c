#include "promote.h"

#define MAX_CAND 256

typedef struct
{
	int  off;
	long weight;
} Cand;

/* Eligible kinds: 64-bit integers only. Narrower ints (int/short/byte/bool) are
   excluded because the slot load path sign/zero-extends them on every read
   (cg_load_scalar); a bare register read would skip that and leak dirty high
   bits into 64-bit arithmetic. Floats are excluded too (callee-saved XMM is
   Win64-only; integer promotion stays ABI-symmetric). Objects/strings/etc. are
   excluded so the GC release walk's [rbp-off] slots stay populated. */
static int is_promotable_kind(TypeKind k)
{
	return k == TY_LONG || k == TY_ULONG;
}

static int has_catch_block(Block *b);

/* True if any statement reachable from `s` installs a catch handler. A try-region
   (ST_TRY) or its clauses (ST_CATCH) are the trigger; every nested block is
   reachable through then_blk/else_blk (switch cases are ST_CASE statements inside
   then_blk, each carrying its own body in then_blk; a try keeps its body in
   then_blk and its catch clauses in else_blk). */
static int stmt_has_catch(Stmt *s)
{
	if (!s)
	{
		return 0;
	}

	if (s->kind == ST_TRY || s->kind == ST_CATCH)
	{
		return 1;
	}

	if (has_catch_block(s->then_blk))
	{
		return 1;
	}

	if (has_catch_block(s->else_blk))
	{
		return 1;
	}

	return 0;
}

static int has_catch_block(Block *b)
{
	if (!b)
	{
		return 0;
	}

	for (int i = 0; i < b->count; i++)
	{
		if (stmt_has_catch(b->stmts[i]))
		{
			return 1;
		}
	}

	return 0;
}

static void scan_expr(Expr *e, int depth, Cand *cs, int *n);
static void scan_block(Block *b, int depth, Cand *cs, int *n);

/* Record one loop-weighted use of the local at slot offset `off`. Each loop
   level multiplies the weight by ten, so loop-carried locals dominate selection. */
static void bump(int off, int depth, Cand *cs, int *n)
{
	long w = 1;
	for (int i = 0; i < depth; i++)
	{
		w *= 10;
	}

	for (int i = 0; i < *n; i++)
	{
		if (cs[i].off == off)
		{
			cs[i].weight += w;
			return;
		}
	}

	if (*n < MAX_CAND)
	{
		cs[*n].off = off;
		cs[*n].weight = w;
		(*n)++;
	}
}

static void scan_expr(Expr *e, int depth, Cand *cs, int *n)
{
	if (!e)
	{
		return;
	}

	/* A local read: anno_int is its rbp slot offset (>0); static fields use -1. */
	if (e->kind == EX_IDENT && is_promotable_kind(e->type.kind) && e->anno_int > 0)
	{
		bump(e->anno_int, depth, cs, n);
	}

	scan_expr(e->lhs, depth, cs, n);
	scan_expr(e->rhs, depth, cs, n);
	for (int i = 0; i < e->arg_count; i++)
	{
		scan_expr(e->args[i], depth, cs, n);
	}
}

static void scan_stmt(Stmt *s, int depth, Cand *cs, int *n)
{
	if (!s)
	{
		return;
	}

	/* A loop's condition, post-clause and body run once per iteration. */
	int inner = depth;
	if (s->kind == ST_WHILE || s->kind == ST_FOR || s->kind == ST_FOREACH)
	{
		inner = depth + 1;
	}

	scan_expr(s->cond, inner, cs, n);          /* while/for run every iteration; if runs once. */
	scan_expr(s->decl_init, depth, cs, n);     /* ST_VARDECL initializer reads. */
	scan_expr(s->value, depth, cs, n);         /* ST_ASSIGN rhs reads. */
	scan_expr(s->target, depth, cs, n);        /* ST_ASSIGN lvalue (a write counts as a use). */
	scan_expr(s->ret_val, depth, cs, n);       /* ST_RETURN reads. */
	scan_expr(s->expr, depth, cs, n);          /* ST_EXPR / ST_THROW reads. */
	scan_stmt(s->for_init, depth, cs, n);
	scan_stmt(s->for_post, inner, cs, n);
	scan_block(s->then_blk, inner, cs, n);
	scan_block(s->else_blk, depth, cs, n);

	/* A scalar var-decl is itself a candidate even if only its register-home
	   writes/reads appear elsewhere; count the declaration as one use. */
	if (s->kind == ST_VARDECL && is_promotable_kind(s->decl_type.kind) && s->decl_offset > 0)
	{
		bump(s->decl_offset, depth, cs, n);
	}
}

static void scan_block(Block *b, int depth, Cand *cs, int *n)
{
	if (!b)
	{
		return;
	}

	for (int i = 0; i < b->count; i++)
	{
		scan_stmt(b->stmts[i], depth, cs, n);
	}
}

void promote_annotate(Func *f)
{
	f->promo_count = 0;
	if (f->is_extern || !f->body)
	{
		return;
	}

	if (has_catch_block(f->body))
	{
		return;   /* Exception-unwind hazard: a throw landing in a catch bypasses the
		             normal ret-based register restore, so never promote here. */
	}

	Cand cs[MAX_CAND];
	int n = 0;
	scan_block(f->body, 0, cs, &n);

	/* Selection-sort the top four by weight (n is small; clarity over speed). */
	for (int slot = 0; slot < 4 && slot < n; slot++)
	{
		int best = slot;
		for (int i = slot + 1; i < n; i++)
		{
			if (cs[i].weight > cs[best].weight)
			{
				best = i;
			}
		}

		Cand tmp = cs[slot];
		cs[slot] = cs[best];
		cs[best] = tmp;

		f->promo_off[slot] = cs[slot].off;
		f->promo_reg[slot] = slot;   /* slot 0 -> r12, 1 -> r13, 2 -> r14, 3 -> r15. */
		f->promo_count = slot + 1;
	}
}
