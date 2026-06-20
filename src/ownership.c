#include "ownership.h"

static void walk_block(Func *f, Block *b);

static void walk_stmt(Func *f, Stmt *s)
{
	switch (s->kind)
	{
	case ST_VARDECL:
		/* Record managed (object/string) locals; their slots need release at scope exit. */
		if (ty_is_managed(s->decl_type.kind) && f->obj_local_count < 64)
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
	case ST_FOR:
		walk_stmt(f, s->for_init);
		walk_block(f, s->then_blk);
		break;
	case ST_FOREACH:
		walk_block(f, s->then_blk);
		break;
	case ST_SWITCH:
		walk_block(f, s->then_blk);
		break;
	case ST_SELECT:
		for (int i = 0; i < s->sel_arm_count; i++)
		{
			SelectArm *a = &s->sel_arms[i];
			/* A managed receive bind is an object local: zeroed at the prologue
			   (so an unready arm's untouched slot releases safely) and released at
			   function exit, like a catch variable. */
			if (!a->is_send && a->bind[0] && ty_is_managed(a->bind_type.kind)
					&& f->obj_local_count < 64)
			{
				f->obj_local_offsets[f->obj_local_count++] = a->bind_offset;
			}

			walk_block(f, a->body);
		}

		walk_block(f, s->else_blk);
		break;
	case ST_TRY:
		walk_block(f, s->then_blk);
		walk_block(f, s->else_blk);
		break;
	case ST_CATCH:
		/* The catch variable is an object local: zeroed at the prologue and
		   released at function exit, like any managed local. */
		if (ty_is_managed(s->decl_type.kind) && f->obj_local_count < 64)
		{
			f->obj_local_offsets[f->obj_local_count++] = s->decl_offset;
		}

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
