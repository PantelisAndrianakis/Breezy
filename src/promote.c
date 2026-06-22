#include "promote.h"
#include "grow.h"
#include <stdlib.h>
#include <string.h>

#define NREGS    4      /* r12..r15: the ABI-symmetric callee-saved integer registers. */
#define NFREGS   4      /* xmm2..xmm5: caller-saved on BOTH Win64 and SysV, never used as codegen scratch. */

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
	int  olo;   /* True live interval before loop-widening (used for the float call-free test). */
	int  ohi;
} Cand;

typedef struct
{
	int lo, hi;   /* A loop's tick span (entry statement .. last body tick), inclusive. */
} Span;

typedef struct
{
	Cand *cand;             /* Integer promotion candidates; grown dynamically. */
	int  ncand;
	int  cand_cap;
	Cand *fcand;            /* Double-local candidates for XMM promotion (parallel machinery). */
	int  nfcand;
	int  fcand_cap;
	int  *ctick;            /* Statement ticks whose direct expressions emit a call (xmm2..5 clobber). */
	int  nctick;
	int  ctick_cap;
	Span *loop;
	int  nloop;
	int  loop_cap;
	int  tick;
} Ctx;

/* Conservative "does evaluating this expression emit a call?" predicate. Any call,
   constructor/allocation, or string-typed sub-expression (string ops lower to
   runtime calls) taints the enclosing statement's tick: a promoted double lives in
   a caller-saved XMM register, so it must never be live across such a call. The
   array-index fast path emits no call (its out-of-bounds slow path aborts, so a
   clobber there is moot), so pure numeric kernels stay eligible. */
static int expr_has_call(Expr *e)
{
	if (!e)
	{
		return 0;
	}

	/* Simd.* intrinsics lower entirely inline (no call instruction) and touch only
	   xmm0/xmm1 and integer scratch - never the xmm2..5 promotion homes. So they do
	   not poison the caller-saved-XMM call-free test; recurse into their args (which
	   may still contain a real call) without counting the intrinsic itself. */
	if (e->kind == EX_CALL && strncmp(e->name, "Simd.", 5) == 0)
	{
		for (int i = 0; i < e->arg_count; i++)
		{
			if (expr_has_call(e->args[i]))
			{
				return 1;
			}
		}

		return 0;
	}

	switch (e->kind)
	{
	case EX_CALL:
	case EX_METHOD_CALL:
	case EX_NEW:
	case EX_NEWARRAY:
	case EX_NEWMAP:
	case EX_NEWGEN:
	case EX_NEWCHANNEL:
		return 1;
	default:
		break;
	}

	if (e->type.kind == TY_STRING)
	{
		return 1;   /* Concatenation / formatting / coercion to string all call the runtime. */
	}

	if (expr_has_call(e->lhs) || expr_has_call(e->rhs))
	{
		return 1;
	}

	for (int i = 0; i < e->arg_count; i++)
	{
		if (expr_has_call(e->args[i]))
		{
			return 1;
		}
	}

	return 0;
}

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
static void bump_into(Cand **arr, int *n, int *cap, int off, int depth, int tick)
{
	long w = 1;
	for (int i = 0; i < depth; i++)
	{
		w *= 10;
	}

	for (int i = 0; i < *n; i++)
	{
		if ((*arr)[i].off == off)
		{
			(*arr)[i].weight += w;
			if (tick < (*arr)[i].lo)
			{
				(*arr)[i].lo = tick;
			}

			if (tick > (*arr)[i].hi)
			{
				(*arr)[i].hi = tick;
			}

			return;
		}
	}

	*arr = grow_ensure(*arr, *n, cap, sizeof(**arr));
	(*arr)[*n].off = off;
	(*arr)[*n].weight = w;
	(*arr)[*n].lo = tick;
	(*arr)[*n].hi = tick;
	(*n)++;
}

static void bump(int off, int depth, Ctx *c)
{
	bump_into(&c->cand, &c->ncand, &c->cand_cap, off, depth, c->tick);
}

/* Float (double) promotion candidate: same loop-weighted interval machinery as the
   integer path, kept in a parallel array. */
static void fbump(int off, int depth, Ctx *c)
{
	bump_into(&c->fcand, &c->nfcand, &c->fcand_cap, off, depth, c->tick);
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

	/* A double or any packed SIMD local read is a candidate for XMM (xmm2..5)
	   promotion: all live in a full caller-saved xmm over a call-free interval. */
	if (e->kind == EX_IDENT && (e->type.kind == TY_DOUBLE || ty_is_simd(e->type.kind)) && e->anno_int > 0)
	{
		fbump(e->anno_int, depth, c);
	}

	/* A lambda captures enclosing locals by value at its construction site. Those
	   reads live inside the lambda body (not this AST), so count each capture as a
	   use HERE: the captured local must stay live - and, if promoted, register-
	   resident with the right value - through the closure build, never reassigned
	   out from under a later-constructed capturing closure. */
	if (e->kind == EX_LAMBDA && e->lam)
	{
		for (int i = 0; i < e->lam->cap_count; i++)
		{
			int off = e->lam->caps[i].src_offset;
			TypeKind ck = e->lam->caps[i].type.kind;
			if (off > 0 && is_promotable_kind(ck))
			{
				bump(off, depth, c);
			}
			else if (off > 0 && ck == TY_DOUBLE)
			{
				fbump(off, depth, c);
			}
		}
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

	/* Record this tick if any of its direct expressions emits a call: a double held
	   in a caller-saved XMM register must not be live across it. */
	if (expr_has_call(s->cond) || expr_has_call(s->decl_init) || expr_has_call(s->value)
			|| expr_has_call(s->target) || expr_has_call(s->ret_val) || expr_has_call(s->expr))
	{
		c->ctick = grow_ensure(c->ctick, c->nctick, &c->ctick_cap, sizeof(*c->ctick));
		c->ctick[c->nctick++] = my;
	}

	/* A scalar var-decl is itself a candidate even if only its register-home
	   writes/reads appear elsewhere; count the declaration as one use. */
	if (s->kind == ST_VARDECL && is_promotable_kind(s->decl_type.kind) && s->decl_offset > 0)
	{
		bump(s->decl_offset, depth, c);
	}

	if (s->kind == ST_VARDECL && (s->decl_type.kind == TY_DOUBLE || ty_is_simd(s->decl_type.kind)) && s->decl_offset > 0)
	{
		fbump(s->decl_offset, depth, c);
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

	if (is_loop)
	{
		c->loop = grow_ensure(c->loop, c->nloop, &c->loop_cap, sizeof(*c->loop));
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
	f->fpromo_count = 0;
	if (f->is_extern || !f->body)
	{
		return;
	}

	if (has_catch_block(f->body))
	{
		return;   /* Exception-unwind hazard: a throw landing in a catch bypasses the
		             normal ret-based register restore, so never promote here. */
	}

	static Ctx c;   /* One function at a time: a single static instance whose grown
	                   buffers are reused (counts reset; caps/pointers persist). */
	c.ncand = 0;
	c.nfcand = 0;
	c.nctick = 0;
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
	int *order = malloc((size_t)(c.ncand > 0 ? c.ncand : 1) * sizeof(int));
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

	/* --- Float (double) promotion into xmm2..xmm5. --------------------------------
	   xmm2..5 are caller-saved on both ABIs and never used as codegen scratch, so a
	   promoted double needs no prologue save/restore - PROVIDED it is never live
	   across a call (a callee may clobber the register). We therefore promote only
	   double locals whose widened live interval contains no call tick, and never a
	   parameter (no prologue seeds the incoming xmm arg into our register). */
	/* Snapshot each double's TRUE live interval before widening: the call-free test
	   below uses it, not the widened span. A value dead before a call is safe in a
	   caller-saved register even when loop-widening later stretches its interval
	   across that call (the register just holds a dead value there). */
	for (int i = 0; i < c.nfcand; i++)
	{
		c.fcand[i].olo = c.fcand[i].lo;
		c.fcand[i].ohi = c.fcand[i].hi;
	}

	/* Widen each double interval over the loops it touches, same fixpoint as ints. */
	int fchanged = 1;
	while (fchanged)
	{
		fchanged = 0;
		for (int i = 0; i < c.nfcand; i++)
		{
			for (int j = 0; j < c.nloop; j++)
			{
				if (!overlaps(c.fcand[i].lo, c.fcand[i].hi, c.loop[j].lo, c.loop[j].hi))
				{
					continue;
				}

				if (c.loop[j].lo < c.fcand[i].lo)
				{
					c.fcand[i].lo = c.loop[j].lo;
					fchanged = 1;
				}

				if (c.loop[j].hi > c.fcand[i].hi)
				{
					c.fcand[i].hi = c.loop[j].hi;
					fchanged = 1;
				}
			}
		}
	}

	/* Eligibility: a non-parameter double whose interval crosses no call tick. */
	int *forder = malloc((size_t)(c.nfcand > 0 ? c.nfcand : 1) * sizeof(int));
	int nford = 0;
	for (int i = 0; i < c.nfcand; i++)
	{
		if (c.fcand[i].off <= param_region_end)
		{
			continue;   /* Parameter slot: not seeded into an xmm2..5 home. */
		}

		int crosses_call = 0;
		for (int t = 0; t < c.nctick; t++)
		{
			if (c.fcand[i].olo <= c.ctick[t] && c.ctick[t] <= c.fcand[i].ohi)
			{
				crosses_call = 1;
				break;
			}
		}

		if (!crosses_call)
		{
			forder[nford++] = i;
		}
	}

	/* Highest weight first (selection sort; nford is small). */
	for (int a = 0; a < nford; a++)
	{
		int best = a;
		for (int b = a + 1; b < nford; b++)
		{
			if (c.fcand[forder[b]].weight > c.fcand[forder[best]].weight)
			{
				best = b;
			}
		}

		int tmp = forder[a];
		forder[a] = forder[best];
		forder[best] = tmp;
	}

	int fasg_lo[PROMO_MAX], fasg_hi[PROMO_MAX];
	for (int oi = 0; oi < nford; oi++)
	{
		Cand *cd = &c.fcand[forder[oi]];
		int chosen = -1;
		for (int r = 0; r < NFREGS && chosen < 0; r++)
		{
			int free = 1;
			for (int k = 0; k < f->fpromo_count; k++)
			{
				if (f->fpromo_reg[k] == r && overlaps(cd->lo, cd->hi, fasg_lo[k], fasg_hi[k]))
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

		if (chosen < 0 || f->fpromo_count >= PROMO_MAX)
		{
			continue;
		}

		f->fpromo_off[f->fpromo_count] = cd->off;
		f->fpromo_reg[f->fpromo_count] = chosen;
		fasg_lo[f->fpromo_count] = cd->lo;
		fasg_hi[f->fpromo_count] = cd->hi;
		f->fpromo_count++;
	}
}
