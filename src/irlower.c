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
		/* arr[i]: a BCE-proved access skips the check; otherwise a runtime bounds
		   check + bzy_oob is emitted (the function carries an exception record). The
		   element must be an integer/bool. */
		return elig_arrayref(&e->lhs->type) && elig_elem_kind(e->type.kind)
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

/* Region mode (Plan 4): eligibility for a loop subtree emitted inline inside an
   emitter function. Regions additionally reject ST_RETURN (a region cannot run
   the function epilogue) and managed assignment targets (reassigning an array
   local would skip refcounting; element stores remain fine). */
static int elig_region_mode = 0;

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
		if (elig_region_mode && ty_is_managed(s->target->type.kind))
		{
			return 0;
		}

		return elig_expr(s->target) && elig_expr(s->value);
	case ST_IF:
		return elig_expr(s->cond) && elig_block(s->then_blk) && elig_block(s->else_blk);
	case ST_WHILE:
		return elig_expr(s->cond) && elig_block(s->then_blk);
	case ST_FOR:
		return elig_stmt(s->for_init) && elig_expr(s->cond)
			   && elig_stmt(s->for_post) && elig_block(s->then_blk);
	case ST_RETURN:
		return !elig_region_mode && elig_expr(s->ret_val);
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

	if (f->obj_local_count > 0)
	{
		/* Owned managed locals are released on unwind at frame offsets the emitter
		   assigns; the IR allocates its own layout, so a thrown bounds error would
		   release garbage. IR-eligible functions never `new`, so this is normally 0;
		   bail to the emitter if any exist. */
		return 0;
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
	int     depth;   /* Current loop-nesting depth, stamped on new blocks for spill weighting. */
	/* Unrolled-loop induction constants: reads of slot cc_off[i] lower to
	   IR_CONST cc_val[i] instead of a frame load. Stack-shaped (nesting). */
	int       cc_n;
	int       cc_off[8];
	long long cc_val[8];
} Low;

/* A fresh block stamped with loop-nesting depth d. */
static int low_block_at(Low *L, int d)
{
	int b = ir_block_new(L->f);
	L->f->blocks[b].depth = d;
	return b;
}

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

/* Structural equality for the side-effect-free expression subset regions
   lower (idents, literals, casts, unary/binary arithmetic, indexing,
   .length). Used to recognise arr[X] = arr[X] op V so X lowers once. */
static int expr_eq(const Expr *a, const Expr *b)
{
	if (!a || !b)
	{
		return a == b;
	}

	if (a->kind != b->kind || a->type.kind != b->type.kind)
	{
		return 0;
	}

	switch (a->kind)
	{
	case EX_INT:
	case EX_BOOL:
		return a->int_val == b->int_val;
	case EX_IDENT:
		return a->anno_int == b->anno_int;
	case EX_CAST:
		return expr_eq(a->lhs, b->lhs);
	case EX_UNARY:
		return a->op == b->op && expr_eq(a->lhs, b->lhs);
	case EX_BINARY:
		return a->op == b->op && expr_eq(a->lhs, b->lhs) && expr_eq(a->rhs, b->rhs);
	case EX_INDEX:
		return expr_eq(a->lhs, b->lhs) && expr_eq(a->rhs, b->rhs);
	case EX_FIELD:
		return strcmp(a->name, b->name) == 0 && expr_eq(a->lhs, b->lhs);
	default:
		return 0;
	}
}

static IRReg low_expr(Low *L, const Expr *e);

/* The compile-time value of e, if it is an integer literal or an unrolled
   loop's induction constant. */
static int low_const(Low *L, const Expr *e, long long *out)
{
	if (e->kind == EX_INT)
	{
		*out = e->int_val;
		return 1;
	}

	if (e->kind == EX_IDENT)
	{
		for (int i = L->cc_n - 1; i >= 0; i--)
		{
			if (L->cc_off[i] == e->anno_int)
			{
				*out = L->cc_val[i];
				return 1;
			}
		}
	}

	return 0;
}

/* Lower an array-access index, folding a constant addend into the byte
   displacement when the access is BCE-proved safe: arr[x + K] -> index x,
   *disp += K*scale. A checked access must compare the full index against the
   length, so it never folds. K may be a literal or an unrolled induction
   constant, on either side of the +, or the rhs of a -. */
static IRReg low_index(Low *L, const Expr *ix, int safe, int scale, long long *disp)
{
	long long k;
	if (safe && ix->kind == EX_BINARY)
	{
		if (ix->op == TOKEN_PLUS && low_const(L, ix->rhs, &k))
		{
			*disp += k * scale;
			return low_expr(L, ix->lhs);
		}

		if (ix->op == TOKEN_PLUS && low_const(L, ix->lhs, &k))
		{
			*disp += k * scale;
			return low_expr(L, ix->rhs);
		}

		if (ix->op == TOKEN_MINUS && low_const(L, ix->rhs, &k))
		{
			*disp -= k * scale;
			return low_expr(L, ix->lhs);
		}
	}

	return low_expr(L, ix);
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
		/* An unrolled loop's induction variable is a compile-time constant for
		   the current copy: lower the read as that constant. */
		for (int i = L->cc_n - 1; i >= 0; i--)
		{
			if (L->cc_off[i] == e->anno_int)
			{
				IRReg cr = ir_reg(L->f);
				IRInstr *kc = ir_emit(L->f, L->cur, IR_CONST, e->type.kind);
				kc->dst = cr;
				kc->imm = L->cc_val[i];
				kc->line = e->line;
				return cr;
			}
		}

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
		/* a[i] -> load [base + i*stride + 32]; a safe constant addend in the
		   index folds into the displacement. */
		IRReg base = low_expr(L, e->lhs);
		int scale = elem_stride(e->type.kind);
		long long disp = 32;
		IRReg idx = low_index(L, e->rhs, e->anno_index_safe, scale, &disp);
		IRReg r = ir_reg(L->f);
		IRInstr *in = ir_emit(L->f, L->cur, IR_LOAD, e->type.kind);
		in->dst = r;
		in->a = base;
		in->b = idx;
		in->scale = scale;
		in->disp = disp;
		in->is_frame = 0;
		in->checked = !e->anno_index_safe;
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

		if (e->op == TOKEN_TILDE)
		{
			/* ~x == x ^ -1: one xor with a folded immediate, no new IR op. A clean
			   sign-extended narrow int stays clean (all high bits flip together with
			   bit 31), so no re-extension is needed. */
			IRReg s = low_expr(L, e->lhs);
			IRReg m = ir_reg(L->f);
			IRInstr *kc = ir_emit(L->f, L->cur, IR_CONST, e->type.kind);
			kc->dst = m;
			kc->imm = -1;
			kc->line = e->line;
			IRReg r = ir_reg(L->f);
			IRInstr *in = ir_emit(L->f, L->cur, IR_XOR, e->type.kind);
			in->dst = r;
			in->a = s;
			in->b = m;
			in->line = e->line;
			return r;
		}

		L->ok = 0;
		return IR_NO_REG;
	case EX_BINARY:
	{
		/* (int)bytearr[i] & 255 is an unsigned byte load: the masked value is
		   exactly the zero-extension the hardware movzx produces, so the cast
		   and the mask fold away. The canonical byte-parsing idiom. */
		if (e->op == TOKEN_AMP && e->rhs->kind == EX_INT && e->rhs->int_val == 255
			&& e->lhs->kind == EX_CAST && ty_is_int(e->lhs->type.kind)
			&& e->lhs->lhs->kind == EX_INDEX
			&& e->lhs->lhs->type.kind == TY_BYTE)
		{
			const Expr *ix = e->lhs->lhs;
			IRReg base = low_expr(L, ix->lhs);
			long long disp = 32;
			IRReg idx = low_index(L, ix->rhs, ix->anno_index_safe, 1, &disp);
			IRReg r = ir_reg(L->f);
			IRInstr *in = ir_emit(L->f, L->cur, IR_LOAD, TY_UBYTE);
			in->dst = r;
			in->a = base;
			in->b = idx;
			in->scale = 1;
			in->disp = disp;
			in->is_frame = 0;
			in->checked = !ix->anno_index_safe;
			in->line = e->line;
			return r;
		}

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

/* Defined below ir_lower_func; used by the constant-trip unroller here. */
static int const_int(const Expr *e, long long *out);
static long long const_trip_count(const Stmt *s);

/* True if the subtree contains a break/continue that would target THIS loop
   (does not descend into nested loops, which own their own targets). */
static int block_has_breakcont(const Block *b);

static int stmt_has_breakcont(const Stmt *s)
{
	if (!s)
	{
		return 0;
	}

	if (s->kind == ST_BREAK || s->kind == ST_CONTINUE)
	{
		return 1;
	}

	if (s->kind == ST_FOR || s->kind == ST_WHILE || s->kind == ST_FOREACH)
	{
		return 0;
	}

	return block_has_breakcont(s->then_blk) || block_has_breakcont(s->else_blk);
}

static int block_has_breakcont(const Block *b)
{
	if (!b)
	{
		return 0;
	}

	for (int i = 0; i < b->count; i++)
	{
		if (stmt_has_breakcont(b->stmts[i]))
		{
			return 1;
		}
	}

	return 0;
}

/* True if the subtree assigns local slot `off` (the unroll guard: a body that
   rewrites its own counter cannot be constant-folded). */
static int block_writes_off(const Block *b, int off);

static int stmt_writes_off(const Stmt *s, int off)
{
	if (!s)
	{
		return 0;
	}

	if (s->kind == ST_VARDECL && s->decl_offset == off)
	{
		return 1;
	}

	if (s->kind == ST_ASSIGN && s->target && s->target->kind == EX_IDENT
		&& s->target->anno_int == off)
	{
		return 1;
	}

	if (s->kind == ST_FOREACH && (s->decl_offset == off || s->fe_val_offset == off))
	{
		return 1;
	}

	if (s->kind == ST_EXPR && s->expr && s->expr->kind == EX_INCDEC
		&& s->expr->lhs && s->expr->lhs->kind == EX_IDENT
		&& s->expr->lhs->anno_int == off)
	{
		return 1;
	}

	return stmt_writes_off(s->for_init, off) || stmt_writes_off(s->for_post, off)
		   || block_writes_off(s->then_blk, off) || block_writes_off(s->else_blk, off);
}

static int block_writes_off(const Block *b, int off)
{
	if (!b)
	{
		return 0;
	}

	for (int i = 0; i < b->count; i++)
	{
		if (stmt_writes_off(b->stmts[i], off))
		{
			return 1;
		}
	}

	return 0;
}

/* Statement count of a block subtree, the unroll code-growth guard. */
static int block_stmt_count(const Block *b)
{
	if (!b)
	{
		return 0;
	}

	int n = 0;
	for (int i = 0; i < b->count; i++)
	{
		const Stmt *s = b->stmts[i];
		n += 1 + block_stmt_count(s->then_blk) + block_stmt_count(s->else_blk);
	}

	return n;
}

/* An expression that is safe and cheap to evaluate UNCONDITIONALLY: pure
   arithmetic over locals, literals, and BCE-proved loads. Excludes division
   and remainder (can fault) and checked loads (can throw); capped in size so
   a select never computes more speculative work than the branch it removes. */
static int sel_expr_ok(const Expr *e, int *budget)
{
	if (!e || --(*budget) < 0)
	{
		return 0;
	}

	switch (e->kind)
	{
	case EX_INT:
	case EX_BOOL:
		return 1;
	case EX_IDENT:
		return ty_is_int(e->type.kind) && e->anno_int > 0;
	case EX_CAST:
		return ty_is_int(e->type.kind) && ty_is_int(e->lhs->type.kind)
			   && sel_expr_ok(e->lhs, budget);
	case EX_INDEX:
		return e->anno_index_safe && ty_is_int(e->type.kind)
			   && e->lhs->kind == EX_IDENT && sel_expr_ok(e->rhs, budget);
	case EX_BINARY:
		if (e->op == TOKEN_SLASH || e->op == TOKEN_PERCENT)
		{
			return 0;
		}

		if (tok_is_cmp(e->op) || e->op == TOKEN_AND || e->op == TOKEN_OR)
		{
			return 0;
		}

		return ty_is_int(e->type.kind)
			   && sel_expr_ok(e->lhs, budget) && sel_expr_ok(e->rhs, budget);
	default:
		return 0;
	}
}

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
			/* arr[X] = arr[X] op V (or V op arr[X], commutative op): lower the
			   base and X once and reuse the vregs for the inner load and the
			   store - the duplicated index expression is the costly part. */
			const Expr *t = s->target;
			const Expr *inner = NULL;
			if (s->value->kind == EX_BINARY)
			{
				if (expr_eq(s->value->lhs, t))
				{
					inner = s->value->lhs;
				}
				else if ((s->value->op == TOKEN_PLUS || s->value->op == TOKEN_STAR
						  || s->value->op == TOKEN_AMP || s->value->op == TOKEN_PIPE
						  || s->value->op == TOKEN_CARET)
						 && expr_eq(s->value->rhs, t))
				{
					inner = s->value->rhs;
				}
			}

			if (inner)
			{
				IRReg base = low_expr(L, t->lhs);
				int scale = elem_stride(t->type.kind);
				long long disp = 32;
				IRReg idx = low_index(L, t->rhs, t->anno_index_safe, scale, &disp);

				IRReg cur = ir_reg(L->f);
				IRInstr *ld = ir_emit(L->f, L->cur, IR_LOAD, t->type.kind);
				ld->dst = cur;
				ld->a = base;
				ld->b = idx;
				ld->scale = scale;
				ld->disp = disp;
				ld->is_frame = 0;
				ld->checked = !t->anno_index_safe;
				ld->line = s->line;

				const Expr *other = (inner == s->value->lhs) ? s->value->rhs : s->value->lhs;
				IRReg ov = low_expr(L, other);
				int aok;
				IROp aop = tok_arith_op(s->value->op, &aok);
				if (!aok)
				{
					L->ok = 0;
					break;
				}

				IRReg nv = ir_reg(L->f);
				IRInstr *bi = ir_emit(L->f, L->cur, aop, s->value->type.kind);
				bi->dst = nv;
				bi->a = (inner == s->value->lhs) ? cur : ov;
				bi->b = (inner == s->value->lhs) ? ov : cur;
				bi->line = s->line;

				IRInstr *st = ir_emit(L->f, L->cur, IR_STORE, t->type.kind);
				st->is_frame = 0;
				st->a = base;
				st->b = idx;
				st->c = nv;
				st->scale = scale;
				st->disp = disp;
				/* The load above already proved (or checked) the same base and
				   index in range; a second check would be pure overhead. */
				st->checked = 0;
				st->line = s->line;
				break;
			}

			/* a[i] = v -> store [base + i*stride + 32]; a safe constant addend
			   in the index folds into the displacement. */
			IRReg base = low_expr(L, s->target->lhs);
			int scale = elem_stride(s->target->type.kind);
			long long disp = 32;
			IRReg idx = low_index(L, s->target->rhs, s->target->anno_index_safe, scale, &disp);
			IRReg v = low_expr(L, s->value);
			IRInstr *in = ir_emit(L->f, L->cur, IR_STORE, s->target->type.kind);
			in->is_frame = 0;
			in->a = base;
			in->b = idx;
			in->c = v;
			in->scale = scale;
			in->disp = disp;
			in->checked = !s->target->anno_index_safe;
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
		/* if (A <cmp> B) { x = EXPR; } with a pure, cheap EXPR lowers to a
		   branchless select: x = cmp ? EXPR : x via cmov. Kills the data-
		   dependent misprediction the max/min/clamp patterns otherwise pay. */
		if (!s->else_blk && s->then_blk && s->then_blk->count == 1
			&& s->cond->kind == EX_BINARY && tok_is_cmp(s->cond->op)
			&& ty_is_int(s->cond->lhs->type.kind))
		{
			const Stmt *as = s->then_blk->stmts[0];
			if (as->kind == ST_ASSIGN && as->target->kind == EX_IDENT
				&& as->target->anno_int > 0 && ty_is_int(as->target->type.kind))
			{
				int budget = 6;
				int ok = sel_expr_ok(s->cond->lhs, &budget)
						 && sel_expr_ok(s->cond->rhs, &budget);
				budget = 6;
				ok = ok && sel_expr_ok(as->value, &budget);
				if (ok)
				{
					IRReg av = low_expr(L, s->cond->lhs);
					IRReg bv = low_expr(L, s->cond->rhs);
					IRReg tv = low_expr(L, as->value);

					/* The current value of the target (the false side). */
					IRReg fv = ir_reg(L->f);
					IRInstr *ld = ir_emit(L->f, L->cur, IR_LOAD, as->target->type.kind);
					ld->dst = fv;
					ld->is_frame = 1;
					ld->disp = as->target->anno_int;
					ld->line = s->line;

					IRReg r = ir_reg(L->f);
					IRInstr *sel = ir_emit(L->f, L->cur, IR_SEL, s->cond->lhs->type.kind);
					sel->dst = r;
					sel->a = av;
					sel->b = bv;
					sel->c = tv;
					sel->d = fv;
					sel->cmp_op = s->cond->op;
					sel->line = s->line;

					low_store_local(L, as->target->anno_int, as->target->type.kind, r);
					break;
				}
			}
		}

		IRReg c = low_expr(L, s->cond);
		int then_blk = low_block_at(L, L->depth);
		int else_blk = s->else_blk ? low_block_at(L, L->depth) : -1;
		int join_blk = low_block_at(L, L->depth);
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
		int head = low_block_at(L, L->depth + 1);   /* Condition runs every iteration: hot. */
		int body = low_block_at(L, L->depth + 1);
		int exit = low_block_at(L, L->depth);
		low_br(L, L->cur, head);

		int sd = L->depth;
		L->depth = sd + 1;
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
		L->depth = sd;
		L->cur = exit;
		break;
	}
	case ST_FOR:
	{
		/* A small constant-trip loop lowers as straight-line copies with the
		   induction variable folded to a constant per copy: no counter, no
		   compare, no branch. The body must not rewrite the counter and must
		   not break/continue; the trip and size caps bound code growth. */
		long long trips = const_trip_count(s);
		if (trips >= 2 && trips <= 16 && L->cc_n < 8
			&& trips * (long long)block_stmt_count(s->then_blk) <= 96
			&& !block_has_breakcont(s->then_blk))
		{
			int iv_off = (s->for_init->kind == ST_VARDECL)
						 ? s->for_init->decl_offset
						 : s->for_init->target->anno_int;
			if (iv_off != 0 && !block_writes_off(s->then_blk, iv_off))
			{
				long long c0 = 0;
				long long c2 = 0;
				const_int(s->for_init->kind == ST_VARDECL
						  ? s->for_init->decl_init : s->for_init->value, &c0);
				const_int(s->for_post->value->rhs, &c2);

				low_stmt(L, s->for_init);   /* Keeps the slot's value truthful. */

				L->cc_off[L->cc_n] = iv_off;
				L->cc_n++;
				for (long long k = 0; k < trips && L->ok; k++)
				{
					L->cc_val[L->cc_n - 1] = c0 + k * c2;
					low_block(L, s->then_blk);
				}

				L->cc_n--;

				/* The counter's final value (the first failing the guard), for
				   any read after the loop - regions keep every local live. */
				IRReg fr = ir_reg(L->f);
				IRInstr *kc = ir_emit(L->f, L->cur, IR_CONST, TY_INT);
				kc->dst = fr;
				kc->imm = c0 + trips * c2;
				kc->line = s->line;
				low_store_local(L, iv_off, TY_INT, fr);
				break;
			}
		}

		if (s->for_init)
		{
			low_stmt(L, s->for_init);
		}

		int head = low_block_at(L, L->depth + 1);   /* Condition + increment run every iteration. */
		int body = low_block_at(L, L->depth + 1);
		int post = low_block_at(L, L->depth + 1);
		int exit = low_block_at(L, L->depth);
		low_br(L, L->cur, head);

		int sd = L->depth;
		L->depth = sd + 1;
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
		L->depth = sd;
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
	L.depth = 0;
	L.cc_n = 0;
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

/* The constant value of e if it is an integer literal, via *out; 0 otherwise. */
static int const_int(const Expr *e, long long *out)
{
	if (e && e->kind == EX_INT)
	{
		*out = e->int_val;
		return 1;
	}

	return 0;
}

/* The trip count of a canonical counted for-loop (`int i = C0; i </<= C1;
   i = i + C2`, all integer literals, C2 > 0), or -1 when not of that shape. */
static long long const_trip_count(const Stmt *s)
{
	if (s->kind != ST_FOR || !s->for_init || !s->cond || !s->for_post)
	{
		return -1;
	}

	long long c0, c1, c2;
	long long ctr;
	if (s->for_init->kind == ST_VARDECL && const_int(s->for_init->decl_init, &c0))
	{
		ctr = s->for_init->decl_offset;
	}
	else if (s->for_init->kind == ST_ASSIGN && s->for_init->target->kind == EX_IDENT
			 && const_int(s->for_init->value, &c0))
	{
		ctr = s->for_init->target->anno_int;
	}
	else
	{
		return -1;
	}

	const Expr *c = s->cond;
	if (c->kind != EX_BINARY || (c->op != TOKEN_LT && c->op != TOKEN_LTE)
		|| c->lhs->kind != EX_IDENT || c->lhs->anno_int != ctr || !const_int(c->rhs, &c1))
	{
		return -1;
	}

	const Stmt *p = s->for_post;
	if (p->kind != ST_ASSIGN || p->target->kind != EX_IDENT || p->target->anno_int != ctr
		|| p->value->kind != EX_BINARY || p->value->op != TOKEN_PLUS
		|| p->value->lhs->kind != EX_IDENT || p->value->lhs->anno_int != ctr
		|| !const_int(p->value->rhs, &c2) || c2 <= 0)
	{
		return -1;
	}

	long long span = c1 - c0 + ((c->op == TOKEN_LTE) ? 1 : 0);
	if (span <= 0)
	{
		return 0;
	}

	return (span + c2 - 1) / c2;
}

int ir_region_eligible(const Stmt *s)
{
	if (!s || (s->kind != ST_FOR && s->kind != ST_WHILE) || s->accum_sb_offset)
	{
		return 0;
	}

	/* Small fixed-trip loops belong to the emitter: its unroller handles them,
	   and the region's once-per-entry save/load/store-back overhead would
	   dominate a body that runs only a handful of times (codec's 8x8 stage
	   loops enter hundreds of thousands of times). */
	long long trips = const_trip_count(s);
	if (trips >= 0 && trips <= 8)
	{
		return 0;
	}

	elig_region_mode = 1;
	int ok = elig_stmt(s);
	elig_region_mode = 0;
	return ok;
}

IRFunc *ir_lower_region(const Func *f, const Stmt *s)
{
	if (!ir_region_eligible(s))
	{
		return 0;
	}

	IRFunc *irf = ir_func_new(f);
	irf->is_region = 1;
	int entry = ir_block_new(irf);
	Low L;
	L.f = irf;
	L.cur = entry;
	L.break_blk = -1;
	L.cont_blk = -1;
	L.ok = 1;
	L.depth = 0;
	L.cc_n = 0;
	low_stmt(&L, s);
	if (!L.ok)
	{
		ir_func_free(irf);
		return 0;
	}

	/* Terminate into a final empty block (created last, so laid out last): the
	   region has no IR_RET, and emission falls off this block into the region
	   epilogue the codegen appends. */
	int done = low_block_at(&L, 0);
	if (!blk_terminated(irf, L.cur))
	{
		low_br(&L, L.cur, done);
	}

	L.cur = done;
	return irf;
}
