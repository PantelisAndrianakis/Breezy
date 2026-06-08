#include "promote.h"

#define MAX_CAND 256
#define MAX_LOOP 256
#define NREGS    4      /* r12..r15: the ABI-symmetric callee-saved integer registers. */

/* A promotion candidate: a 64-bit integer local, its loop-weighted use count, and
   its live interval [lo,hi] in statement ticks. The interval is a conservative
   superset of the local's true live range (first textual occurrence .. last),
   widened to span any enclosing loop so a loop-carried value never shares a
   register with another local live in the same loop. */
typedef struct
{
	int  off;
	long weight;
	int  lo;
	int  hi;
} Cand;

typedef struct
{
	int lo, hi;   /* A loop's tick span (entry statement .. last body tick), inclusive. */
} Span;

typedef struct
{
	Cand cand[MAX_CAND];
	int  ncand;
	Span loop[MAX_LOOP];
	int  nloop;
	int  tick;
} Ctx;

/* Eligible kinds: 64-bit integers and 32-bit signed int. A promoted local lives
   only in its register, so the register must always hold a value the read path
   (a bare `mov rax, reg`) can use directly. For TY_INT that means a sign-extended
   64-bit value, which codegen guarantees by sign-extending (`movsxd reg, eax`)
   into the register on every store (cg_store_local_off) and by NOT running the
   in-place register-arithmetic path on a 32-bit target (its 64-bit ops would not
   wrap at 32 bits). Narrower ints (short/byte/bool) and unsigned int stay on the
   stack for now (their extension width differs). Floats are excluded (callee-saved
   XMM is Win64-only; promotion stays ABI-symmetric). Objects/strings/etc. are
   excluded so the GC release walk's [rbp-off] slots stay populated. */
static int is_promotable_kind(TypeKind k)
{
	return k == TY_LONG || k == TY_ULONG || k == TY_INT;
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

static void scan_expr(Expr *e, int depth, Ctx *c);
static void scan_block(Block *b, int depth, Ctx *c);

/* Record one loop-weighted use of the local at slot offset `off` at the current
   tick. Each loop level multiplies the weight by ten, so loop-carried locals
   dominate selection; the tick widens the candidate's live interval. */
static void bump(int off, int depth, Ctx *c)
{
	long w = 1;
	for (int i = 0; i < depth; i++)
	{
		w *= 10;
	}

	for (int i = 0; i < c->ncand; i++)
	{
		if (c->cand[i].off == off)
		{
			c->cand[i].weight += w;
			if (c->tick < c->cand[i].lo)
			{
				c->cand[i].lo = c->tick;
			}

			if (c->tick > c->cand[i].hi)
			{
				c->cand[i].hi = c->tick;
			}

			return;
		}
	}

	if (c->ncand < MAX_CAND)
	{
		c->cand[c->ncand].off = off;
		c->cand[c->ncand].weight = w;
		c->cand[c->ncand].lo = c->tick;
		c->cand[c->ncand].hi = c->tick;
		c->ncand++;
	}
}

static void scan_expr(Expr *e, int depth, Ctx *c)
{
	if (!e)
	{
		return;
	}

	/* A local read: anno_int is its rbp slot offset (>0); static fields use -1. */
	if (e->kind == EX_IDENT && is_promotable_kind(e->type.kind) && e->anno_int > 0)
	{
		bump(e->anno_int, depth, c);
	}

	scan_expr(e->lhs, depth, c);
	scan_expr(e->rhs, depth, c);
	for (int i = 0; i < e->arg_count; i++)
	{
		scan_expr(e->args[i], depth, c);
	}
}

static void scan_stmt(Stmt *s, int depth, Ctx *c)
{
	if (!s)
	{
		return;
	}

	/* Every statement gets a monotonically increasing tick; this statement's
	   direct expressions are attributed to it, while nested blocks advance the
	   tick further so a loop's span covers all of its body. */
	c->tick++;
	int my = c->tick;
	int inner = depth;
	int is_loop = (s->kind == ST_WHILE || s->kind == ST_FOR || s->kind == ST_FOREACH);
	if (is_loop)
	{
		inner = depth + 1;
	}

	scan_expr(s->cond, inner, c);          /* while/for run every iteration; if runs once. */
	scan_expr(s->decl_init, depth, c);     /* ST_VARDECL initializer reads. */
	scan_expr(s->value, depth, c);         /* ST_ASSIGN rhs reads. */
	scan_expr(s->target, depth, c);        /* ST_ASSIGN lvalue (a write counts as a use). */
	scan_expr(s->ret_val, depth, c);       /* ST_RETURN reads. */
	scan_expr(s->expr, depth, c);          /* ST_EXPR / ST_THROW reads. */

	/* A scalar var-decl is itself a candidate even if only its register-home
	   writes/reads appear elsewhere; count the declaration as one use. */
	if (s->kind == ST_VARDECL && is_promotable_kind(s->decl_type.kind) && s->decl_offset > 0)
	{
		bump(s->decl_offset, depth, c);
	}

	/* The foreach cursor is an internal 64-bit index loaded, compared and
	   incremented every iteration; promoting it out of its stack slot removes two
	   memory touches per element. Weighted at the loop body's depth. */
	if (s->kind == ST_FOREACH && s->fe_index_offset > 0)
	{
		bump(s->fe_index_offset, inner, c);
	}

	scan_stmt(s->for_init, depth, c);
	scan_stmt(s->for_post, inner, c);
	scan_block(s->then_blk, inner, c);
	scan_block(s->else_blk, depth, c);

	if (is_loop && c->nloop < MAX_LOOP)
	{
		c->loop[c->nloop].lo = my;
		c->loop[c->nloop].hi = c->tick;   /* Last tick emitted while scanning the body. */
		c->nloop++;
	}
}

static void scan_block(Block *b, int depth, Ctx *c)
{
	if (!b)
	{
		return;
	}

	for (int i = 0; i < b->count; i++)
	{
		scan_stmt(b->stmts[i], depth, c);
	}
}

/* Two live intervals overlap (cannot share a register) when neither ends before
   the other begins. */
static int overlaps(int alo, int ahi, int blo, int bhi)
{
	return alo <= bhi && blo <= ahi;
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

	static Ctx c;   /* Large; one function at a time, so a single static instance is fine. */
	c.ncand = 0;
	c.nloop = 0;
	c.tick = 0;
	scan_block(f->body, 0, &c);

	/* Parameters (and `this`) are seeded into their register by the prologue, so
	   they are live from function entry. Mark every candidate in the parameter
	   slot region live-from-entry (lo = 0); over-marking a non-parameter local is
	   safe (it only forgoes some sharing), whereas missing a real parameter would
	   let an early local clobber the seeded value. The region covers `this` at
	   slot 8 plus param_count slots, with margin for either ABI's `this` layout. */
	int param_region_end = 16 + f->param_count * 8;
	for (int i = 0; i < c.ncand; i++)
	{
		if (c.cand[i].off <= param_region_end)
		{
			c.cand[i].lo = 0;
		}
	}

	/* Widen each interval to span any loop it touches, to a fixpoint (nested loops
	   chain outward). After this, any two locals simultaneously live - including a
	   loop-carried value and another local in the same loop - have overlapping
	   intervals and so will never share a register. */
	int changed = 1;
	while (changed)
	{
		changed = 0;
		for (int i = 0; i < c.ncand; i++)
		{
			for (int j = 0; j < c.nloop; j++)
			{
				if (!overlaps(c.cand[i].lo, c.cand[i].hi, c.loop[j].lo, c.loop[j].hi))
				{
					continue;
				}

				if (c.loop[j].lo < c.cand[i].lo)
				{
					c.cand[i].lo = c.loop[j].lo;
					changed = 1;
				}

				if (c.loop[j].hi > c.cand[i].hi)
				{
					c.cand[i].hi = c.loop[j].hi;
					changed = 1;
				}
			}
		}
	}

	/* Greedy interval colouring, highest weight first: each local takes the lowest
	   register held by no overlapping already-assigned local. When all four are
	   taken by overlapping locals, this one stays on the stack. Selection-sort the
	   processing order by weight (ncand is small; clarity over speed). */
	int order[MAX_CAND];
	for (int i = 0; i < c.ncand; i++)
	{
		order[i] = i;
	}

	for (int a = 0; a < c.ncand; a++)
	{
		int best = a;
		for (int b = a + 1; b < c.ncand; b++)
		{
			if (c.cand[order[b]].weight > c.cand[order[best]].weight)
			{
				best = b;
			}
		}

		int tmp = order[a];
		order[a] = order[best];
		order[best] = tmp;
	}

	/* Live interval of each already-assigned local, parallel to f->promo_*. */
	int asg_lo[PROMO_MAX], asg_hi[PROMO_MAX];

	for (int oi = 0; oi < c.ncand; oi++)
	{
		Cand *cd = &c.cand[order[oi]];
		int chosen = -1;
		for (int r = 0; r < NREGS && chosen < 0; r++)
		{
			int free = 1;
			for (int k = 0; k < f->promo_count; k++)
			{
				if (f->promo_reg[k] == r && overlaps(cd->lo, cd->hi, asg_lo[k], asg_hi[k]))
				{
					free = 0;
					break;
				}
			}

			if (free)
			{
				chosen = r;
			}
		}

		if (chosen < 0 || f->promo_count >= PROMO_MAX)
		{
			continue;   /* Out of registers for this interval: keep it on the stack. */
		}

		f->promo_off[f->promo_count] = cd->off;
		f->promo_reg[f->promo_count] = chosen;
		asg_lo[f->promo_count] = cd->lo;
		asg_hi[f->promo_count] = cd->hi;
		f->promo_count++;
	}
}
