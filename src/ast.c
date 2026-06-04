#include "ast.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#define MAX_NODES 200000
static void *g_nodes[MAX_NODES];
static int   g_node_count = 0;

static void *track(void *p)
{
	if (!p)
	{
		fprintf(stderr, "Ast: out of memory.\n");
		exit(1);
	}
	if (g_node_count >= MAX_NODES)
	{
		fprintf(stderr, "Ast: too many nodes.\n");
		exit(1);
	}
	g_nodes[g_node_count++] = p;
	return p;
}

void ast_free_all(void)
{
	for (int i=0; i<g_node_count; i++)
	{
		free(g_nodes[i]);
	}
	g_node_count = 0;
}

Expr *expr_new(ExprKind kind, int line)
{
	Expr *e = track(calloc(1,sizeof(Expr)));
	e->kind=kind;
	e->line=line;
	return e;
}
TypeRef *typeref_box(TypeRef t)
{
	TypeRef *p = track(calloc(1,sizeof(TypeRef)));
	*p = t;
	return p;
}
Stmt *stmt_new(StmtKind kind, int line)
{
	Stmt *s = track(calloc(1,sizeof(Stmt)));
	s->kind=kind;
	s->line=line;
	return s;
}

Block *block_new(void)
{
	Block *b = track(calloc(1,sizeof(Block)));
	b->cap = 8;
	b->stmts = track(calloc(b->cap, sizeof(Stmt*)));
	return b;
}
void block_push(Block *b, Stmt *s)
{
	if (b->count >= b->cap)
	{
		int nc = b->cap*2;
		Stmt **n = track(calloc(nc,sizeof(Stmt*)));
		memcpy(n, b->stmts, b->count*sizeof(Stmt*));
		b->stmts = n;
		b->cap = nc;
	}
	b->stmts[b->count++] = s;
}
Func      *func_new(void)
{
	return track(calloc(1,sizeof(Func)));
}
ClassDecl *class_new(void)
{
	return track(calloc(1,sizeof(ClassDecl)));
}
InterfaceDecl *interface_new(void)
{
	return track(calloc(1,sizeof(InterfaceDecl)));
}
EnumDecl *enum_new(void)
{
	return track(calloc(1,sizeof(EnumDecl)));
}
Unit      *unit_new(void)
{
	return track(calloc(1,sizeof(Unit)));
}

TypeRef typeref_deepcopy(const TypeRef *t)
{
	TypeRef r=*t;   /* Copies scalars + class_name + targ_count by value. */
	r.elem  = t->elem  ? typeref_box(typeref_deepcopy(t->elem))  : NULL;
	r.elem2 = t->elem2 ? typeref_box(typeref_deepcopy(t->elem2)) : NULL;
	for (int i=0; i<t->targ_count; i++)
	{
		r.targs[i]=typeref_box(typeref_deepcopy(t->targs[i]));
	}
	return r;
}

Expr *expr_clone(const Expr *e)
{
	if (!e)
	{
		return NULL;
	}

	Expr *n=expr_new(e->kind,e->line);
	*n=*e;                                   /* Shallow copy scalars + arrays. */
	n->type=typeref_deepcopy(&e->type);
	n->lhs=expr_clone(e->lhs);
	n->rhs=expr_clone(e->rhs);
	for (int i=0; i<e->arg_count; i++)
	{
		n->args[i]=expr_clone(e->args[i]);
	}
	return n;
}

Stmt *stmt_clone(const Stmt *s)
{
	if (!s)
	{
		return NULL;
	}

	Stmt *n=stmt_new(s->kind,s->line);
	*n=*s;
	n->decl_type=typeref_deepcopy(&s->decl_type);
	n->fe_val_type=typeref_deepcopy(&s->fe_val_type);
	n->decl_init=expr_clone(s->decl_init);
	n->for_init=stmt_clone(s->for_init);
	n->for_post=stmt_clone(s->for_post);
	n->target=expr_clone(s->target);
	n->value=expr_clone(s->value);
	n->cond=expr_clone(s->cond);
	n->ret_val=expr_clone(s->ret_val);
	n->expr=expr_clone(s->expr);
	n->then_blk=block_clone(s->then_blk);
	n->else_blk=block_clone(s->else_blk);
	return n;
}

Block *block_clone(const Block *b)
{
	if (!b)
	{
		return NULL;
	}

	Block *n=block_new();
	for (int i=0; i<b->count; i++)
	{
		block_push(n,stmt_clone(b->stmts[i]));
	}
	return n;
}

Func *func_clone(const Func *f)
{
	if (!f)
	{
		return NULL;
	}

	Func *n=func_new();
	*n=*f;
	n->ret_type=typeref_deepcopy(&f->ret_type);
	for (int i=0; i<f->param_count; i++)
	{
		n->params[i].type=typeref_deepcopy(&f->params[i].type);
	}
	n->body=block_clone(f->body);
	return n;
}

ClassDecl *classdecl_clone(const ClassDecl *c)
{
	ClassDecl *n=class_new();
	*n=*c;
	for (int i=0; i<c->field_count; i++)
	{
		n->fields[i].type=typeref_deepcopy(&c->fields[i].type);
	}
	for (int i=0; i<c->method_count; i++)
	{
		n->methods[i]=func_clone(c->methods[i]);
	}
	n->ctor=func_clone(c->ctor);
	return n;
}
