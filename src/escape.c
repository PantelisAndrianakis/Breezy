#include "escape.h"
#include "grow.h"
#include <string.h>

/* The escape pass is two sweeps over a resolved function body. The first sweep
   records the stack offsets of object locals that escape the frame; the second
   marks every `new` flowing only into a non-escaping local for stack allocation
   and assigns it a frame slot. Conservative: when in doubt, an object escapes
   and stays on the heap with full ARC.

   A local escapes only when a reference to its object is captured: passed as a
   call argument, used as a method receiver (passed as this), stored into a
   field, aliased into another local, or returned. Reading or writing a field of
   a local (c.v) does NOT escape c. */

static int *g_esc=NULL;
static int  g_esc_n=0;
static int  g_esc_cap=0;

/* The type table for the body currently being scanned, so a call position can
   look up the callee's summary. NULL => no interprocedural knowledge, so every
   call position captures (the original conservative behaviour). */
static TypeTable *g_cur_tt=NULL;

/* The unique body a call site invokes, or NULL when the callee is not provably
   unique (a closure/function-value call, an unknown/builtin/extern/variadic
   target, an interface or overridable method). Only a non-NULL result lets a
   capture be withheld, so every NULL path stays conservative and sound. */
static Func *resolve_callee(TypeTable *tt, Expr *e)
{
	if (!tt)
	{
		return NULL;
	}

	if (e->kind==EX_CALL)
	{
		if (e->anno_indirect)
		{
			return NULL;   /* Call through a function value: target unknown here. */
		}

		FuncInfo *fi=types_find_func_idx(tt,e->name,e->anno_overload);
		if (!fi || fi->is_extern || fi->is_variadic || !fi->ast)
		{
			return NULL;   /* Builtin (namespaced name), extern, variadic, or no body. */
		}

		return fi->ast;
	}

	if (e->kind==EX_METHOD_CALL)
	{
		ClassInfo *c=types_find_class(tt,e->anno_str);
		if (!c)
		{
			return NULL;   /* Interface-typed or unknown receiver. */
		}

		MethodInfo *m=types_find_method_idx(c,e->name,e->anno_overload);
		if (!m || !m->ast)
		{
			return NULL;
		}

		if (m->vtable_slot<0)
		{
			return m->ast;   /* Static method: one implementation, no dispatch. */
		}

		if (types_method_is_monomorphic(tt,e->anno_str,e->name,e->anno_overload))
		{
			return m->ast;   /* No descendant override: one implementation. */
		}

		return NULL;   /* Polymorphic: the runtime body is not known here. */
	}

	return NULL;
}

/* Does argument index `i` escape the callee body? Positions past bit 63 cannot be
   represented, so they are treated as escaping (the >64-param summary fallback). */
static int callee_arg_escapes(Func *cal, int i)
{
	if (i>=64)
	{
		return 1;
	}

	return ((cal->esc.param_escapes>>i)&1ull) ? 1 : 0;
}

static void esc_add(int off)
{
	for (int i=0; i<g_esc_n; i++)
	{
		if (g_esc[i]==off)
		{
			return;
		}
	}

	g_esc=grow_ensure(g_esc,g_esc_n,&g_esc_cap,sizeof(*g_esc));
	g_esc[g_esc_n++]=off;
}

static int esc_has(int off)
{
	for (int i=0; i<g_esc_n; i++)
	{
		if (g_esc[i]==off)
		{
			return 1;
		}
	}

	return 0;
}

/* The value of e is captured (retained, stored, or returned). If e is a bare
   object local, that local escapes. A fresh value (new/call) or a field read
   does not make any local escape. */
static void mark_captured(Expr *e)
{
	if (!e)
	{
		return;
	}

	if (e->kind==EX_IDENT && e->type.kind==TY_OBJECT)
	{
		esc_add(e->anno_int);
	}
}

/* Walk an expression for capturing sub-positions: call arguments and method
   receivers. Field/binary/unary operands are traversed but are not, by
   themselves, capturing positions for the locals they read. */
static void walk_expr(Expr *e)
{
	if (!e)
	{
		return;
	}

	switch (e->kind)
	{
	case EX_CALL:
	{
		/* Capture each argument only if the resolved callee lets that parameter
		   escape; an unresolved callee (NULL) captures all, as before. */
		Func *cal=resolve_callee(g_cur_tt,e);
		for (int i=0; i<e->arg_count; i++)
		{
			if (!cal || callee_arg_escapes(cal,i))
			{
				mark_captured(e->args[i]);
			}

			walk_expr(e->args[i]);
		}

		break;
	}
	case EX_METHOD_CALL:
	{
		Func *cal=resolve_callee(g_cur_tt,e);
		if (!cal || cal->esc.this_escapes)
		{
			mark_captured(e->lhs);     /* The receiver is passed as this. */
		}

		walk_expr(e->lhs);
		for (int i=0; i<e->arg_count; i++)
		{
			if (!cal || callee_arg_escapes(cal,i))
			{
				mark_captured(e->args[i]);
			}

			walk_expr(e->args[i]);
		}

		break;
	}
	case EX_FIELD:
		walk_expr(e->lhs);
		break;
	case EX_BINARY:
		walk_expr(e->lhs);
		walk_expr(e->rhs);
		break;
	case EX_UNARY:
		walk_expr(e->lhs);
		break;
	case EX_LAMBDA:
		/* A closure captures enclosing locals by value into a heap environment that
		   may outlive the frame (or form a reference cycle with the captured object,
		   reclaimed only by the cycle collector). An object captured this way must be
		   heap-allocated and reference-counted, never a frame-local stack object. */
		if (e->lam)
		{
			for (int i=0; i<e->lam->cap_count; i++)
			{
				if (e->lam->caps[i].type.kind==TY_OBJECT)
				{
					esc_add(e->lam->caps[i].src_offset);
				}
			}
		}
		break;
	default:
		break;
	}
}

static void scan_block_escapes(Block *b);

static void scan_stmt_escapes(Stmt *s)
{
	switch (s->kind)
	{
	case ST_VARDECL:
		/* Initializing a local from a bare local read aliases it. */
		if (s->decl_type.kind==TY_OBJECT && s->decl_init && s->decl_init->kind==EX_IDENT)
		{
			mark_captured(s->decl_init);
		}

		walk_expr(s->decl_init);
		break;
	case ST_ASSIGN:
		/* Storing an object into a field, or aliasing a local into another
		   local, captures the source value. */
		if (s->target->kind==EX_FIELD)
		{
			mark_captured(s->value);
			walk_expr(s->target->lhs);
		}

		/* Storing a local into an array element captures it: the array may
		   outlive the local and holds the reference with ARC. */
		if (s->target->kind==EX_INDEX)
		{
			mark_captured(s->value);
			walk_expr(s->target->lhs);
			walk_expr(s->target->rhs);
		}

		if (s->target->kind==EX_IDENT && s->value->kind==EX_IDENT)
		{
			mark_captured(s->value);
		}

		walk_expr(s->value);
		break;
	case ST_RETURN:
		mark_captured(s->ret_val);
		walk_expr(s->ret_val);
		break;
	case ST_IF:
		walk_expr(s->cond);
		scan_block_escapes(s->then_blk);
		if (s->else_blk)
		{
			scan_block_escapes(s->else_blk);
		}
		break;
	case ST_WHILE:
		walk_expr(s->cond);
		scan_block_escapes(s->then_blk);
		break;
	case ST_EXPR:
		walk_expr(s->expr);
		break;
	case ST_FOREACH:
		walk_expr(s->expr);
		scan_block_escapes(s->then_blk);
		break;
	case ST_BREAK:
	case ST_CONTINUE:
		break;
	case ST_FOR:
		scan_stmt_escapes(s->for_init);
		walk_expr(s->cond);
		scan_stmt_escapes(s->for_post);
		scan_block_escapes(s->then_blk);
		break;
	case ST_SWITCH:
		walk_expr(s->cond);
		scan_block_escapes(s->then_blk);
		break;
	case ST_SELECT:
		for (int i = 0; i < s->sel_arm_count; i++)
		{
			walk_expr(s->sel_arms[i].chan);
			walk_expr(s->sel_arms[i].send_val);
			scan_block_escapes(s->sel_arms[i].body);
		}

		if (s->else_blk)
		{
			scan_block_escapes(s->else_blk);
		}

		break;
	case ST_CASE:
	case ST_DEFAULT:
		break;
	case ST_THROW:
		mark_captured(s->expr);
		walk_expr(s->expr);
		break;
	case ST_TRY:
		scan_block_escapes(s->then_blk);
		scan_block_escapes(s->else_blk);
		break;
	case ST_CATCH:
		scan_block_escapes(s->then_blk);
		break;
	case ST_SPAWN:
		walk_expr(s->expr);
		break;
	}
}

static void scan_block_escapes(Block *b)
{
	for (int i=0; i<b->count; i++)
	{
		scan_stmt_escapes(b->stmts[i]);
	}
}

/* The object local an assignment or var-decl writes to, or -1 if the statement
   does not write a plain object local. */
static int target_local_offset(Stmt *s)
{
	if (s->kind==ST_VARDECL && s->decl_type.kind==TY_OBJECT)
	{
		return s->decl_offset;
	}

	if (s->kind==ST_ASSIGN && s->target->kind==EX_IDENT && s->target->type.kind==TY_OBJECT)
	{
		return s->target->anno_int;
	}

	return -1;
}

/* The EX_NEW directly produced into a local by this statement, or NULL. */
static Expr *stmt_new_rhs(Stmt *s)
{
	if (s->kind==ST_VARDECL && s->decl_init && s->decl_init->kind==EX_NEW)
	{
		return s->decl_init;
	}

	if (s->kind==ST_ASSIGN && s->value && s->value->kind==EX_NEW)
	{
		return s->value;
	}

	return NULL;
}

static void mark_block_stack(TypeTable *tt, Block *b, Func *f, int base);

static void mark_stmt_stack(TypeTable *tt, Stmt *s, Func *f, int base)
{
	Expr *n = stmt_new_rhs(s);
	if (n)
	{
		int off = target_local_offset(s);
		if (off >= 0 && !esc_has(off))
		{
			ClassInfo *c = types_find_class(tt, n->name);
			f->stack_alloc_bytes += c->object_size;
			n->anno_stack = 1;
			n->anno_stack_off = base + f->stack_alloc_bytes;
		}
	}

	if (s->kind==ST_IF)
	{
		mark_block_stack(tt, s->then_blk, f, base);
		if (s->else_blk)
		{
			mark_block_stack(tt, s->else_blk, f, base);
		}
	}

	if (s->kind==ST_WHILE)
	{
		mark_block_stack(tt, s->then_blk, f, base);
	}

	if (s->kind==ST_FOR)
	{
		mark_block_stack(tt, s->then_blk, f, base);
	}

	if (s->kind==ST_FOREACH)
	{
		mark_block_stack(tt, s->then_blk, f, base);
	}

	if (s->kind==ST_SWITCH)
	{
		mark_block_stack(tt, s->then_blk, f, base);
	}
}

static void mark_block_stack(TypeTable *tt, Block *b, Func *f, int base)
{
	for (int i=0; i<b->count; i++)
	{
		mark_stmt_stack(tt, b->stmts[i], f, base);
	}
}

/* Read the conservative escape set into the function's summary: a parameter (or
   `this`) escapes when its frame slot is captured somewhere in the body. With no
   callee knowledge every call position captures, so this is the seed that the
   call-graph fixpoint later refines. Only object parameters can hold a reference
   that escapes; scalar args never reach mark_captured, so their bits stay clear. */
static void summarize(Func *f)
{
	f->esc.this_escapes = (f->this_offset >= 0 && esc_has(f->this_offset)) ? 1 : 0;
	f->esc.param_escapes = 0;
	if (f->param_count > 64)
	{
		f->esc.param_escapes = ~0ull;   /* Beyond bit 63 there is no room to be precise: treat the tail as escaping. */
	}
	else
	{
		for (int i = 0; i < f->param_count; i++)
		{
			if (f->params[i].type.kind == TY_OBJECT && esc_has(f->params[i].offset))
			{
				f->esc.param_escapes |= (1ull << i);
			}
		}
	}

	f->esc.solved = 1;
}

void escape_solve(TypeTable *tt, Func **funcs, int count)
{
	g_cur_tt = tt;

	/* Optimistic start: every summary empty, so the first pass assumes callees
	   leak nothing and grows escapes only as evidence appears. The transfer is
	   monotone (a wider callee summary can only widen its callers), so this climbs
	   to the least fixpoint and never oscillates. */
	for (int i = 0; i < count; i++)
	{
		if (funcs[i])
		{
			funcs[i]->esc.this_escapes = 0;
			funcs[i]->esc.param_escapes = 0;
			funcs[i]->esc.solved = 0;
		}
	}

	/* Iterate to stability. Bound the loop at the total representable bits as a
	   backstop; real convergence is two or three passes. */
	long guard = (long)count * 65 + 16;
	int changed = 1;
	while (changed && guard-- > 0)
	{
		changed = 0;
		for (int i = 0; i < count; i++)
		{
			Func *f = funcs[i];
			if (!f || !f->body)
			{
				continue;
			}

			unsigned char old_this = f->esc.this_escapes;
			unsigned long long old_params = f->esc.param_escapes;
			g_esc_n = 0;
			scan_block_escapes(f->body);
			summarize(f);
			if (f->esc.this_escapes != old_this || f->esc.param_escapes != old_params)
			{
				changed = 1;
			}
		}
	}
}

void escape_annotate(TypeTable *tt, Func *f)
{
	g_cur_tt = tt;   /* So this body's call sites consult the (now fixed) summaries. */
	g_esc_n = 0;
	f->stack_alloc_bytes = 0;

	/* The stack-object region sits below the ARC scratch area, which itself sits
	   below the locals; this base MUST match cg_emit_func's scratch size. The
	   scratch slots run sp_save..fp_save (locals+8..locals+64) plus the rbx_save
	   slot at locals+72, so the deepest scratch byte is at locals+72 and stack
	   objects must start there -- otherwise the first object overwrites the saved
	   rbx and corrupts any -O2 caller that kept a value in rbx across the call. */
	int locals = f->frame_size < 16 ? 16 : f->frame_size;
	int base = locals + 72;

	scan_block_escapes(f->body);
	summarize(f);
	mark_block_stack(tt, f->body, f, base);
}
