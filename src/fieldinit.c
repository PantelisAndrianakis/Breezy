#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "fieldinit.h"

/* Find a class by name across all units; parse order and file layout are
   irrelevant. Template units cleared by generics_expand hold NULL slots. */
static ClassDecl *find_class(Unit **units, int total, const char *name)
{
	for (int i=0; i<total; i++)
	{
		for (int k=0; k<units[i]->class_count; k++)
		{
			ClassDecl *c=units[i]->klasses[k];
			if (c && strcmp(c->name,name)==0)
			{
				return c;
			}
		}
	}

	return NULL;
}

/* Build one `this.<field> = <clone of init>` assignment. The initializer is
   deep-copied because an ancestor's initializer is embedded into EVERY
   descendant's constructor, and AST nodes are single-owner. */
static Stmt *make_init_assign(const Field *f)
{
	int line=f->init->line;
	Expr *recv=expr_new(EX_THIS,line);
	Expr *fld=expr_new(EX_FIELD,line);
	strcpy(fld->name,f->name);
	fld->lhs=recv;
	Stmt *s=stmt_new(ST_ASSIGN,line);
	s->target=fld;
	s->value=expr_clone(f->init);
	return s;
}

static void lower_class(Unit **units, int total, ClassDecl *c)
{
	if (c->is_static)
	{
		return;   /* Every field is static; the __static_init path owns those. */
	}

	/* Gather the inheritance chain, self first; chain[depth-1] is the root. */
	ClassDecl *chain[64];
	int depth=0;
	ClassDecl *cur=c;
	while (cur)
	{
		if (depth>=64)
		{
			fprintf(stderr,"Class %s: inheritance chain too deep.\n",c->name);
			exit(1);
		}

		chain[depth++]=cur;
		cur = cur->has_parent ? find_class(units,total,cur->parent_name) : NULL;
	}

	/* Skip classes whose whole chain has no instance-field initializers. */
	int any=0;
	for (int i=0; i<depth && !any; i++)
	{
		for (int k=0; k<chain[i]->field_count; k++)
		{
			if (!chain[i]->fields[k].is_static && !chain[i]->is_static && chain[i]->fields[k].init)
			{
				any=1;
				break;
			}
		}
	}

	if (!any)
	{
		return;
	}

	/* Only this class's own constructor runs at `new`, so it must exist. */
	if (!c->ctor)
	{
		Func *f=func_new();
		f->ret_type.kind=TY_VOID;
		strcpy(f->name,c->name);
		f->body=block_new();
		c->ctor=f;
	}

	/* Rebuild the body: initializer assignments (ancestor-first, declaration
	   order), then the original constructor statements. */
	Block *nb=block_new();
	for (int i=depth-1; i>=0; i--)
	{
		ClassDecl *a=chain[i];
		if (a->is_static)
		{
			continue;
		}

		for (int k=0; k<a->field_count; k++)
		{
			Field *fl=&a->fields[k];
			if (fl->is_static || !fl->init)
			{
				continue;
			}

			block_push(nb,make_init_assign(fl));
		}
	}

	for (int i=0; i<c->ctor->body->count; i++)
	{
		block_push(nb,c->ctor->body->stmts[i]);
	}

	c->ctor->body=nb;
}

void fieldinit_expand(Unit **units, int total)
{
	for (int i=0; i<total; i++)
	{
		for (int k=0; k<units[i]->class_count; k++)
		{
			if (units[i]->klasses[k])
			{
				lower_class(units,total,units[i]->klasses[k]);
			}
		}
	}

	/* Clear instance-field inits only after EVERY class is lowered: a
	   subclass in another unit reads its ancestor's init pointers above. */
	for (int i=0; i<total; i++)
	{
		for (int k=0; k<units[i]->class_count; k++)
		{
			ClassDecl *c=units[i]->klasses[k];
			if (!c || c->is_static)
			{
				continue;
			}

			for (int f=0; f<c->field_count; f++)
			{
				if (!c->fields[f].is_static)
				{
					c->fields[f].init=NULL;
				}
			}
		}
	}
}
