#include "irlower.h"
#include "lexer.h"   /* TokenType values for operators. */
#include <string.h>  /* strcmp for the arr.length field name. */

/* A scalar integer kind the v1 IR backend can hold in a register: any integer,
   bool, or void (for void-returning functions). Floats and managed kinds are out
   of scope (the naive emitter is integer-only); arrays are handled separately. */
static int elig_type(TypeKind k)
{
	return ty_is_int(k) || k == TY_BOOL || k == TY_VOID;
}

/* An element kind the array path supports: integer or bool (no float arrays in
   Plan 3 - that is Plan 3b / xmm). */
static int elig_elem_kind(TypeKind k)
{
	return ty_is_int(k) || k == TY_BOOL;
}

/* A reference to an array of supported elements (e.g. an `int[]` parameter). */
static int elig_arrayref(const TypeRef *t)
{
	return t && t->kind == TY_ARRAY && t->elem && elig_elem_kind(t->elem->kind);
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
		return elig_type(e->type.kind);
	case EX_IDENT:
		/* A scalar local/param, or an array reference (its pointer is a register
		   value like any other 64-bit local). */
		return (e->type.kind == TY_ARRAY) ? elig_arrayref(&e->type) : elig_type(e->type.kind);
	case EX_INDEX:
		/* arr[i]: only when BCE proved it in range, so no runtime check / bzy_oob
		   is needed (the IR function stays call-free; bounds checking is a later
		   step). The element must be an integer/bool. */
		return e->anno_index_safe
			   && elig_arrayref(&e->lhs->type) && elig_elem_kind(e->type.kind)
			   && elig_expr(e->lhs) && elig_expr(e->rhs);
	case EX_FIELD:
		/* arr.length only (a load of the array header's length field). */
		return e->lhs && e->lhs->type.kind == TY_ARRAY
			   && strcmp(e->name, "length") == 0 && elig_expr(e->lhs);
	case EX_BINARY:
		return elig_type(e->type.kind) && elig_expr(e->lhs) && elig_expr(e->rhs);
	case EX_UNARY:
		return elig_type(e->type.kind) && elig_expr(e->lhs);
	case EX_CAST:
		return elig_type(e->type.kind) && elig_type(e->lhs->type.kind) && elig_expr(e->lhs);
	default:
		/* EX_STR, EX_CALL, EX_METHOD_CALL, EX_NEW*, EX_THIS, EX_NULL, EX_INCDEC. */
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

	if (f->param_count > 4)
	{
		return 0;   /* v1 spills register args only (<= 4 covers Win64 and System V). */
	}

	for (int i = 0; i < f->param_count; i++)
	{
		TypeKind pk = f->params[i].type.kind;
		int ok = (pk == TY_ARRAY) ? elig_arrayref(&f->params[i].type) : elig_type(pk);
		if (!ok)
		{
			return 0;
		}
	}

	return elig_block(f->body);
}

/* Lowering state. `cur` is the block instructions are appended to; it moves as
   control flow forks. break/cont hold the current loop's exit/continue targets.
   `ok` clears if an unsupported construct is reached -> the whole lowering aborts
   and the caller falls back to the emitter. */
typedef struct
{
	IRFunc *f;
	int     cur;
	int     break_blk;
	int     cont_blk;
	int     ok;
} Low;

static int tok_is_cmp(int t)
{
	return t == TOKEN_EQ || t == TOKEN_NEQ || t == TOKEN_LT
		   || t == TOKEN_GT || t == TOKEN_LTE || t == TOKEN_GTE;
}

static IROp tok_arith_op(int t, int *ok)
{
	*ok = 1;
	switch (t)
	{
	case TOKEN_PLUS:    return IR_ADD;
	case TOKEN_MINUS:   return IR_SUB;
	case TOKEN_STAR:    return IR_MUL;
	case TOKEN_SLASH:   return IR_DIV;
	case TOKEN_PERCENT: return IR_MOD;
	case TOKEN_AMP:     return IR_AND;
	case TOKEN_PIPE:    return IR_OR;
	case TOKEN_CARET:   return IR_XOR;
	case TOKEN_SHL:     return IR_SHL;
	case TOKEN_SHR:     return IR_SHR;
	default:
		*ok = 0;
		return IR_ADD;   /* &&, ||, etc. - not in v1. */
	}
}

/* Bytes per array element (a valid x86 index scale). Mirrors codegen.c's
   cg_elem_stride exactly: 8-bit widths and bool pack to one byte; managed
   elements are 8-byte pointers. The address mode must agree with the emitter. */
static int elem_stride(TypeKind k)
{
	if (ty_is_managed(k))
	{
		return 8;
	}

	int bits = (k == TY_BOOL) ? 8 : ty_bits(k);
	if (bits <= 8)
	{
		return 1;
	}

	if (bits <= 16)
	{
		return 2;
	}

	if (bits <= 32)
	{
		return 4;
	}

	return 8;
}

/* Lower an expression, returning the vreg holding its value (IR_NO_REG on bail). */
static IRReg low_expr(Low *L, const Expr *e)
{
	if (!L->ok)
	{
		return IR_NO_REG;
	}

	switch (e->kind)
	{
	case EX_INT:
	case EX_BOOL:
	{
		IRReg r = ir_reg(L->f);
		IRInstr *in = ir_emit(L->f, L->cur, IR_CONST, e->type.kind);
		in->dst = r;
		in->imm = e->int_val;
		in->line = e->line;
		return r;
	}
	case EX_IDENT:
	{
		/* A local/param read: load from its frame slot at -anno_int. */
		IRReg r = ir_reg(L->f);
		IRInstr *in = ir_emit(L->f, L->cur, IR_LOAD, e->type.kind);
		in->dst = r;
		in->is_frame = 1;
		in->disp = e->anno_int;
		in->line = e->line;
		return r;
	}
	case EX_CAST:
	{
		IRReg s = low_expr(L, e->lhs);
		IRReg r = ir_reg(L->f);
		IRInstr *in = ir_emit(L->f, L->cur, IR_CAST, e->lhs->type.kind);
		in->dst = r;
		in->a = s;
		in->to_kind = e->type.kind;
		in->line = e->line;
		return r;
	}
	case EX_INDEX:
	{
		/* a[i] -> load [base + i*stride + 32]. Eligibility guaranteed the access is
		   BCE-safe, so no bounds check is emitted. */
		IRReg base = low_expr(L, e->lhs);
		IRReg idx = low_expr(L, e->rhs);
		IRReg r = ir_reg(L->f);
		IRInstr *in = ir_emit(L->f, L->cur, IR_LOAD, e->type.kind);
		in->dst = r;
		in->a = base;
		in->b = idx;
		in->scale = elem_stride(e->type.kind);
		in->disp = 32;
		in->is_frame = 0;
		in->line = e->line;
		return r;
	}
	case EX_FIELD:
	{
		/* arr.length -> load the 8-byte length at [base + 24]. */
		IRReg base = low_expr(L, e->lhs);
		IRReg r = ir_reg(L->f);
		IRInstr *in = ir_emit(L->f, L->cur, IR_LOAD, TY_LONG);
		in->dst = r;
		in->a = base;
		in->b = IR_NO_REG;
		in->scale = 0;
		in->disp = 24;
		in->is_frame = 0;
		in->line = e->line;
		return r;
	}
	case EX_UNARY:
		if (e->op == TOKEN_MINUS)
		{
			IRReg s = low_expr(L, e->lhs);
			IRReg r = ir_reg(L->f);
			IRInstr *in = ir_emit(L->f, L->cur, IR_NEG, e->type.kind);
			in->dst = r;
			in->a = s;
			in->line = e->line;
			return r;
		}

		L->ok = 0;
		return IR_NO_REG;
	case EX_BINARY:
	{
		if (tok_is_cmp(e->op))
		{
			IRReg la = low_expr(L, e->lhs);
			IRReg rb = low_expr(L, e->rhs);
			IRReg r = ir_reg(L->f);
			IRInstr *in = ir_emit(L->f, L->cur, IR_CMP, e->lhs->type.kind);
			in->dst = r;
			in->a = la;
			in->b = rb;
			in->cmp_op = e->op;
			in->line = e->line;
			return r;
		}

		int arith_ok;
		IROp op = tok_arith_op(e->op, &arith_ok);
		if (!arith_ok)
		{
			L->ok = 0;
			return IR_NO_REG;
		}

		IRReg la = low_expr(L, e->lhs);
		IRReg rb = low_expr(L, e->rhs);
		IRReg r = ir_reg(L->f);
		IRInstr *in = ir_emit(L->f, L->cur, op, e->type.kind);
		in->dst = r;
		in->a = la;
		in->b = rb;
		in->line = e->line;
		return r;
	}
	default:
		L->ok = 0;
		return IR_NO_REG;
	}
}

static int blk_terminated(IRFunc *f, int blk)
{
	IRBlock *b = &f->blocks[blk];
	if (b->count == 0)
	{
		return 0;
	}

	IROp op = b->instrs[b->count - 1].op;
	return op == IR_RET || op == IR_BR || op == IR_BRCOND;
}

static void low_store_local(Low *L, int offset, TypeKind k, IRReg val)
{
	IRInstr *in = ir_emit(L->f, L->cur, IR_STORE, k);
	in->is_frame = 1;
	in->disp = offset;
	in->c = val;
}

static void low_br(Low *L, int blk, int target)
{
	IRInstr *in = ir_emit(L->f, blk, IR_BR, TY_VOID);
	in->blk_true = target;
}

static void low_block(Low *L, const Block *b);

static void low_stmt(Low *L, const Stmt *s)
{
	if (!L->ok)
	{
		return;
	}

	switch (s->kind)
	{
	case ST_VARDECL:
		if (s->decl_init)
		{
			IRReg v = low_expr(L, s->decl_init);
			low_store_local(L, s->decl_offset, s->decl_type.kind, v);
		}

		break;
	case ST_ASSIGN:
		if (s->target->kind == EX_IDENT)
		{
			IRReg v = low_expr(L, s->value);
			low_store_local(L, s->target->anno_int, s->target->type.kind, v);
		}
		else if (s->target->kind == EX_INDEX)
		{
			/* a[i] = v -> store [base + i*stride + 32]. BCE-safe (eligibility). */
			IRReg base = low_expr(L, s->target->lhs);
			IRReg idx = low_expr(L, s->target->rhs);
			IRReg v = low_expr(L, s->value);
			IRInstr *in = ir_emit(L->f, L->cur, IR_STORE, s->target->type.kind);
			in->is_frame = 0;
			in->a = base;
			in->b = idx;
			in->c = v;
			in->scale = elem_stride(s->target->type.kind);
			in->disp = 32;
		}
		else
		{
			L->ok = 0;   /* Field store targets are not in v1. */
		}

		break;
	case ST_RETURN:
	{
		IRReg v = IR_NO_REG;
		if (s->ret_val)
		{
			v = low_expr(L, s->ret_val);
		}

		IRInstr *in = ir_emit(L->f, L->cur, IR_RET, s->ret_val ? s->ret_val->type.kind : TY_VOID);
		in->a = v;
		break;
	}
	case ST_EXPR:
		low_expr(L, s->expr);
		break;
	case ST_BREAK:
		if (L->break_blk < 0)
		{
			L->ok = 0;
			break;
		}

		low_br(L, L->cur, L->break_blk);
		break;
	case ST_CONTINUE:
		if (L->cont_blk < 0)
		{
			L->ok = 0;
			break;
		}

		low_br(L, L->cur, L->cont_blk);
		break;
	case ST_IF:
	{
		IRReg c = low_expr(L, s->cond);
		int then_blk = ir_block_new(L->f);
		int else_blk = s->else_blk ? ir_block_new(L->f) : -1;
		int join_blk = ir_block_new(L->f);
		IRInstr *br = ir_emit(L->f, L->cur, IR_BRCOND, TY_BOOL);
		br->a = c;
		br->blk_true = then_blk;
		br->blk_false = (else_blk >= 0) ? else_blk : join_blk;

		L->cur = then_blk;
		low_block(L, s->then_blk);
		if (!blk_terminated(L->f, L->cur))
		{
			low_br(L, L->cur, join_blk);
		}

		if (else_blk >= 0)
		{
			L->cur = else_blk;
			low_block(L, s->else_blk);
			if (!blk_terminated(L->f, L->cur))
			{
				low_br(L, L->cur, join_blk);
			}
		}

		L->cur = join_blk;
		break;
	}
	case ST_WHILE:
	{
		int head = ir_block_new(L->f);
		int body = ir_block_new(L->f);
		int exit = ir_block_new(L->f);
		low_br(L, L->cur, head);

		L->cur = head;
		IRReg c = low_expr(L, s->cond);
		IRInstr *br = ir_emit(L->f, L->cur, IR_BRCOND, TY_BOOL);
		br->a = c;
		br->blk_true = body;
		br->blk_false = exit;

		int sb = L->break_blk;
		int sc = L->cont_blk;
		L->break_blk = exit;
		L->cont_blk = head;
		L->cur = body;
		low_block(L, s->then_blk);
		if (!blk_terminated(L->f, L->cur))
		{
			low_br(L, L->cur, head);
		}

		L->break_blk = sb;
		L->cont_blk = sc;
		L->cur = exit;
		break;
	}
	case ST_FOR:
	{
		if (s->for_init)
		{
			low_stmt(L, s->for_init);
		}

		int head = ir_block_new(L->f);
		int body = ir_block_new(L->f);
		int post = ir_block_new(L->f);
		int exit = ir_block_new(L->f);
		low_br(L, L->cur, head);

		L->cur = head;
		if (s->cond)
		{
			IRReg c = low_expr(L, s->cond);
			IRInstr *br = ir_emit(L->f, L->cur, IR_BRCOND, TY_BOOL);
			br->a = c;
			br->blk_true = body;
			br->blk_false = exit;
		}
		else
		{
			low_br(L, L->cur, body);
		}

		int sb = L->break_blk;
		int sc = L->cont_blk;
		L->break_blk = exit;
		L->cont_blk = post;
		L->cur = body;
		low_block(L, s->then_blk);
		if (!blk_terminated(L->f, L->cur))
		{
			low_br(L, L->cur, post);
		}

		L->cur = post;
		if (s->for_post)
		{
			low_stmt(L, s->for_post);
		}

		low_br(L, L->cur, head);
		L->break_blk = sb;
		L->cont_blk = sc;
		L->cur = exit;
		break;
	}
	default:
		L->ok = 0;   /* ST_FOREACH, ST_SWITCH, ST_THROW, ST_TRY, ST_SPAWN. */
		break;
	}
}

static void low_block(Low *L, const Block *b)
{
	if (!b)
	{
		return;
	}

	for (int i = 0; i < b->count && L->ok; i++)
	{
		low_stmt(L, b->stmts[i]);
	}
}

IRFunc *ir_lower_func(const Func *f, TypeTable *tt)
{
	(void)tt;
	if (!ir_eligible(f))
	{
		return 0;
	}

	IRFunc *irf = ir_func_new(f);
	int entry = ir_block_new(irf);
	Low L;
	L.f = irf;
	L.cur = entry;
	L.break_blk = -1;
	L.cont_blk = -1;
	L.ok = 1;
	low_block(&L, f->body);
	if (!L.ok)
	{
		ir_func_free(irf);
		return 0;
	}

	/* A void function that ran off the end needs an explicit return. */
	if (!blk_terminated(irf, L.cur))
	{
		IRInstr *in = ir_emit(irf, L.cur, IR_RET, TY_VOID);
		in->a = IR_NO_REG;
	}

	return irf;
}
