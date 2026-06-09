#include "irlower.h"

/* A scalar numeric kind the v1 IR backend can hold in a register: any integer,
   float/double, bool, or void (for void-returning functions). Arrays and managed
   kinds are out of scope until a later plan. */
static int elig_type(TypeKind k)
{
	return ty_is_int(k) || ty_is_float(k) || k == TY_BOOL || k == TY_VOID;
}

static int elig_expr(const Expr *e)
{
	if (!e)
	{
		return 1;
	}

	switch (e->kind)
	{
	case EX_INT:
	case EX_BOOL:
	case EX_FLOAT:
	case EX_IDENT:
		return elig_type(e->type.kind);
	case EX_BINARY:
		return elig_type(e->type.kind) && elig_expr(e->lhs) && elig_expr(e->rhs);
	case EX_UNARY:
		return elig_type(e->type.kind) && elig_expr(e->lhs);
	case EX_CAST:
		return elig_type(e->type.kind) && elig_type(e->lhs->type.kind) && elig_expr(e->lhs);
	default:
		/* EX_STR, EX_CALL, EX_METHOD_CALL, EX_NEW*, EX_FIELD, EX_INDEX, EX_THIS,
		   EX_NULL, EX_INCDEC: not in v1 (arrays/indexing arrive in a later plan). */
		return 0;
	}
}

static int elig_block(const Block *b);

static int elig_stmt(const Stmt *s)
{
	if (!s)
	{
		return 1;
	}

	switch (s->kind)
	{
	case ST_VARDECL:
		return elig_type(s->decl_type.kind) && elig_expr(s->decl_init);
	case ST_ASSIGN:
		return elig_expr(s->target) && elig_expr(s->value);
	case ST_IF:
		return elig_expr(s->cond) && elig_block(s->then_blk) && elig_block(s->else_blk);
	case ST_WHILE:
		return elig_expr(s->cond) && elig_block(s->then_blk);
	case ST_FOR:
		return elig_stmt(s->for_init) && elig_expr(s->cond)
			   && elig_stmt(s->for_post) && elig_block(s->then_blk);
	case ST_RETURN:
		return elig_expr(s->ret_val);
	case ST_EXPR:
		return elig_expr(s->expr);
	case ST_BREAK:
	case ST_CONTINUE:
		return 1;
	default:
		/* ST_FOREACH, ST_SWITCH/CASE/DEFAULT, ST_THROW, ST_TRY/CATCH, ST_SPAWN. */
		return 0;
	}
}

static int elig_block(const Block *b)
{
	if (!b)
	{
		return 1;
	}

	for (int i = 0; i < b->count; i++)
	{
		if (!elig_stmt(b->stmts[i]))
		{
			return 0;
		}
	}

	return 1;
}

int ir_eligible(const Func *f)
{
	if (!f || !f->body || f->is_extern)
	{
		return 0;
	}

	if (!elig_type(f->ret_type.kind))
	{
		return 0;
	}

	for (int i = 0; i < f->param_count; i++)
	{
		if (!elig_type(f->params[i].type.kind))
		{
			return 0;
		}
	}

	return elig_block(f->body);
}

IRFunc *ir_lower_func(const Func *f, TypeTable *tt)
{
	(void)f;
	(void)tt;
	return 0;   /* Implemented in a later task; NULL means "fall back to the emitter". */
}
