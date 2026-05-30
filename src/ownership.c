#include "ownership.h"

static void walk_block(Func *f, Block *b);

static void walk_stmt(Func *f, Stmt *s)
{
	switch (s->kind)
	{
	case ST_VARDECL:
		/* Record object-typed locals; their slots need release at scope exit. */
		if (s->decl_type.kind == TY_OBJECT && f->obj_local_count < 64)
		{
			f->obj_local_offsets[f->obj_local_count++] = s->decl_offset;
		}
		break;
	case ST_IF:
		walk_block(f, s->then_blk);
		if (s->else_blk)
		{
			walk_block(f, s->else_blk);
		}
		break;
	case ST_WHILE:
		walk_block(f, s->then_blk);
		break;
	default:
		break;
	}
}

static void walk_block(Func *f, Block *b)
{
	for (int i = 0; i < b->count; i++)
	{
		walk_stmt(f, b->stmts[i]);
	}
}

void ownership_annotate(Func *f)
{
	f->obj_local_count = 0;
	walk_block(f, f->body);
}
