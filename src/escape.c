#include "escape.h"
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

#define MAX_ESC 256
static int g_esc[MAX_ESC];
static int g_esc_n;

static void esc_add(int off)
{
	for (int i=0; i<g_esc_n; i++)
	{
		if (g_esc[i]==off)
		{
			return;
		}
	}

	if (g_esc_n<MAX_ESC)
	{
		g_esc[g_esc_n++]=off;
	}
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
		for (int i=0; i<e->arg_count; i++)
		{
			mark_captured(e->args[i]);
			walk_expr(e->args[i]);
		}
		break;
	case EX_METHOD_CALL:
		mark_captured(e->lhs);     /* The receiver is passed as this. */
		walk_expr(e->lhs);
		for (int i=0; i<e->arg_count; i++)
		{
			mark_captured(e->args[i]);
			walk_expr(e->args[i]);
		}
		break;
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
}

static void mark_block_stack(TypeTable *tt, Block *b, Func *f, int base)
{
	for (int i=0; i<b->count; i++)
	{
		mark_stmt_stack(tt, b->stmts[i], f, base);
	}
}

void escape_annotate(TypeTable *tt, Func *f)
{
	g_esc_n = 0;
	f->stack_alloc_bytes = 0;

	/* The stack-object region sits below the 64-byte ARC scratch area, which
	   itself sits below the locals; this base must match cg_emit_func. */
	int locals = f->frame_size < 16 ? 16 : f->frame_size;
	int base = locals + 64;

	scan_block_escapes(f->body);
	mark_block_stack(tt, f->body, f, base);
}
