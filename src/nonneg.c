#include "nonneg.h"
#include "lexer.h"
#include "grow.h"
#include <stddef.h>

typedef struct
{
	int *off;   /* slot offsets of locals currently known >= 0; grown dynamically. */
	int count;
	int cap;
} NonNeg;

static int nn_has(NonNeg *s, int off)
{
	for (int i = 0; i < s->count; i++)
	{
		if (s->off[i] == off)
		{
			return 1;
		}
	}

	return 0;
}

static void nn_add(NonNeg *s, int off)
{
	if (off > 0 && !nn_has(s, off))
	{
		s->off = grow_ensure(s->off, s->count, &s->cap, sizeof(*s->off));
		s->off[s->count++] = off;
	}
}

static void nn_remove(NonNeg *s, int off)
{
	for (int i = 0; i < s->count; i++)
	{
		if (s->off[i] == off)
		{
			s->off[i] = s->off[--s->count];
			return;
		}
	}
}

/* If `cond` is `v REL C` (or `C REL v`) that guarantees v >= 0, return v's offset,
   else -1. v > C needs C >= -1; v >= C needs C >= 0. */
static int nn_guard_var(Expr *cond)
{
	if (!cond || cond->kind != EX_BINARY)
	{
		return -1;
	}

	Expr *var = NULL;
	Expr *lit = NULL;
	int op = cond->op;
	if (cond->lhs->kind==EX_IDENT && cond->rhs->kind==EX_INT)
	{
		var = cond->lhs;
		lit = cond->rhs;
	}
	else if (cond->rhs->kind==EX_IDENT && cond->lhs->kind==EX_INT)
	{
		var = cond->rhs;
		lit = cond->lhs;
		/* Flip the operator so it reads var REL lit. */
		if (op==TOKEN_LT) op = TOKEN_GT;
		else if (op==TOKEN_GT) op = TOKEN_LT;
		else if (op==TOKEN_LTE) op = TOKEN_GTE;
		else if (op==TOKEN_GTE) op = TOKEN_LTE;
	}
	else
	{
		return -1;
	}

	if (!ty_is_int(var->type.kind) || var->anno_int <= 0)
	{
		return -1;
	}

	long long c = lit->int_val;
	if ((op==TOKEN_GT && c >= -1) || (op==TOKEN_GTE && c >= 0))
	{
		return var->anno_int;
	}

	return -1;
}

static void nn_mark_expr(Expr *e, NonNeg *s)
{
	if (!e)
	{
		return;
	}

	if (e->kind==EX_BINARY && (e->op==TOKEN_SLASH || e->op==TOKEN_PERCENT)
		&& e->lhs->kind==EX_IDENT && nn_has(s, e->lhs->anno_int))
	{
		e->anno_nonneg = 1;
	}

	nn_mark_expr(e->lhs, s);
	nn_mark_expr(e->rhs, s);
	for (int i = 0; i < e->arg_count; i++)
	{
		nn_mark_expr(e->args[i], s);
	}
}

static void nn_block(Block *b, NonNeg *s);

static void nn_stmt(Stmt *st, NonNeg *s)
{
	if (!st)
	{
		return;
	}

	switch (st->kind)
	{
	case ST_WHILE:
	case ST_IF:
	{
		nn_mark_expr(st->cond, s);
		int g = nn_guard_var(st->cond);
		int added = 0;
		if (g >= 0 && !nn_has(s, g))
		{
			nn_add(s, g);
			added = 1;
		}

		nn_block(st->then_blk, s);   /* guard holds in the then/while body. */
		if (added)
		{
			nn_remove(s, g);
		}

		nn_block(st->else_blk, s);   /* guard does NOT hold in else. */
		break;
	}
	case ST_ASSIGN:
		nn_mark_expr(st->value, s);
		if (st->target->kind==EX_IDENT)
		{
			nn_remove(s, st->target->anno_int);   /* Conservative: any write clears the fact. */
		}

		break;
	case ST_VARDECL:
		nn_mark_expr(st->decl_init, s);
		nn_remove(s, st->decl_offset);
		break;
	case ST_FOR:
		nn_stmt(st->for_init, s);
		nn_mark_expr(st->cond, s);
		nn_block(st->then_blk, s);
		nn_stmt(st->for_post, s);
		break;
	default:
		nn_mark_expr(st->cond, s);
		nn_mark_expr(st->value, s);
		nn_mark_expr(st->expr, s);
		nn_mark_expr(st->ret_val, s);
		nn_block(st->then_blk, s);
		nn_block(st->else_blk, s);
		break;
	}
}

static void nn_block(Block *b, NonNeg *s)
{
	if (!b)
	{
		return;
	}

	for (int i = 0; i < b->count; i++)
	{
		nn_stmt(b->stmts[i], s);
	}
}

void nonneg_annotate(Func *f)
{
	if (f->is_extern || !f->body)
	{
		return;
	}

	NonNeg s;
	s.off = NULL;
	s.count = 0;
	s.cap = 0;
	nn_block(f->body, &s);
}
