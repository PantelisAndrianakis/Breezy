#include "constprop.h"

/* A "write" to a local is: a var-decl initializer, an assignment with the local
   as its lvalue (compound assignments desugar to plain ST_ASSIGN), a ++/-- whose
   operand is the local, or a foreach loop/value variable. Counting every write
   - including the ones hidden inside loops and ++/-- expressions - is what makes
   rematerialization safe: a loop index initialised to 0 is also written by its
   post-step, so it is never mistaken for a single-assignment constant. */

static int expr_writes(Expr *e, int off)
{
	if (!e)
	{
		return 0;
	}

	int n = 0;
	if (e->kind == EX_INCDEC && e->lhs && e->lhs->kind == EX_IDENT && e->lhs->anno_int == off)
	{
		n++;
	}

	n += expr_writes(e->lhs, off);
	n += expr_writes(e->rhs, off);
	for (int i = 0; i < e->arg_count; i++)
	{
		n += expr_writes(e->args[i], off);
	}

	return n;
}

static int block_writes(Block *b, int off);

static int stmt_writes(Stmt *s, int off)
{
	if (!s)
	{
		return 0;
	}

	int n = 0;
	if (s->kind == ST_VARDECL && s->decl_offset == off && s->decl_init)
	{
		n++;
	}

	if (s->kind == ST_ASSIGN && s->target && s->target->kind == EX_IDENT && s->target->anno_int == off)
	{
		n++;
	}

	if (s->kind == ST_FOREACH)
	{
		if (s->decl_offset == off)
		{
			n++;
		}

		if (s->fe_val_offset == off)
		{
			n++;
		}
	}

	n += expr_writes(s->cond, off);
	n += expr_writes(s->decl_init, off);
	n += expr_writes(s->value, off);
	n += expr_writes(s->target, off);
	n += expr_writes(s->ret_val, off);
	n += expr_writes(s->expr, off);
	n += stmt_writes(s->for_init, off);
	n += stmt_writes(s->for_post, off);
	n += block_writes(s->then_blk, off);
	n += block_writes(s->else_blk, off);
	return n;
}

static int block_writes(Block *b, int off)
{
	if (!b)
	{
		return 0;
	}

	int n = 0;
	for (int i = 0; i < b->count; i++)
	{
		n += stmt_writes(b->stmts[i], off);
	}

	return n;
}

/* Rewrite every read of the local at offset `off` to the integer literal `val`.
   An identifier has no children, so converting it in place is complete; the node
   keeps its resolved type so codegen emits the correct width. */
static void rewrite_expr(Expr *e, int off, long long val)
{
	if (!e)
	{
		return;
	}

	if (e->kind == EX_IDENT && e->anno_int == off)
	{
		e->kind = EX_INT;
		e->int_val = val;
		return;
	}

	rewrite_expr(e->lhs, off, val);
	rewrite_expr(e->rhs, off, val);
	for (int i = 0; i < e->arg_count; i++)
	{
		rewrite_expr(e->args[i], off, val);
	}
}

static void rewrite_block(Block *b, int off, long long val);

static void rewrite_stmt(Stmt *s, int off, long long val)
{
	if (!s)
	{
		return;
	}

	rewrite_expr(s->cond, off, val);
	rewrite_expr(s->decl_init, off, val);
	rewrite_expr(s->value, off, val);
	rewrite_expr(s->target, off, val);   /* No write to off survives here (single-write guarantee). */
	rewrite_expr(s->ret_val, off, val);
	rewrite_expr(s->expr, off, val);
	rewrite_stmt(s->for_init, off, val);
	rewrite_stmt(s->for_post, off, val);
	rewrite_block(s->then_blk, off, val);
	rewrite_block(s->else_blk, off, val);
}

static void rewrite_block(Block *b, int off, long long val)
{
	if (!b)
	{
		return;
	}

	for (int i = 0; i < b->count; i++)
	{
		rewrite_stmt(b->stmts[i], off, val);
	}
}

void constprop_annotate(Func *f)
{
	if (f->is_extern || !f->body)
	{
		return;
	}

	Block *b = f->body;
	for (int i = 0; i < b->count; i++)
	{
		Stmt *s = b->stmts[i];

		/* A top-level (function-scope, unconditional) single write of an integer
		   literal to a 64-bit local. Restricting to 64-bit locals keeps the value
		   the full register width, so no slot-width re-extension is skipped; the
		   write must dominate every read, which a top-level statement does. */
		int off = 0;
		long long val = 0;
		int ok = 0;
		if (s->kind == ST_VARDECL
			&& (s->decl_type.kind == TY_LONG || s->decl_type.kind == TY_ULONG)
			&& s->decl_offset > 0 && s->decl_init && s->decl_init->kind == EX_INT)
		{
			off = s->decl_offset;
			val = s->decl_init->int_val;
			ok = 1;
		}
		else if (s->kind == ST_ASSIGN && s->target && s->target->kind == EX_IDENT
				 && s->target->anno_int > 0
				 && (s->target->type.kind == TY_LONG || s->target->type.kind == TY_ULONG)
				 && s->value && s->value->kind == EX_INT)
		{
			off = s->target->anno_int;
			val = s->value->int_val;
			ok = 1;
		}

		if (!ok)
		{
			continue;
		}

		if (block_writes(b, off) != 1)
		{
			continue;   /* Reassigned somewhere: not a constant. */
		}

		/* Rewrite reads only in statements after the write, so a read that
		   precedes the assignment (use-before-init, already a bug) is left alone. */
		for (int j = i + 1; j < b->count; j++)
		{
			rewrite_stmt(b->stmts[j], off, val);
		}
	}
}
