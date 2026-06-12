#include "ast.h"
#include "grow.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

static void **g_nodes = NULL;
static int    g_node_count = 0;
static int    g_node_cap = 0;

static void *track(void *p)
{
	if (!p)
	{
		fprintf(stderr, "Ast: out of memory.\n");
		exit(1);
	}
	g_nodes = grow_ensure(g_nodes, g_node_count, &g_node_cap, sizeof(*g_nodes));
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
void unit_add_class(Unit *u, ClassDecl *c)
{
	if (u->class_count >= u->class_cap)
	{
		int nc = u->class_cap ? u->class_cap*2 : 4;
		ClassDecl **n = track(calloc(nc, sizeof(ClassDecl*)));
		memcpy(n, u->klasses, u->class_count*sizeof(ClassDecl*));
		u->klasses = n;
		u->class_cap = nc;
	}
	u->klasses[u->class_count++] = c;
}

TypeRef typeref_deepcopy(const TypeRef *t)
{
	TypeRef r=*t;   /* Shallow: scalars + class_name + the targs pointer (replaced below). */
	r.elem  = t->elem  ? typeref_box(typeref_deepcopy(t->elem))  : NULL;
	r.elem2 = t->elem2 ? typeref_box(typeref_deepcopy(t->elem2)) : NULL;
	r.targs=NULL;   /* Own buffer: do not alias t's (the copy is mutated independently). */
	r.targ_cap=0;
	if (t->targ_count>0)
	{
		r.targs=grow_reserve(r.targs,t->targ_count,&r.targ_cap,sizeof(*r.targs));
		for (int i=0; i<t->targ_count; i++)
		{
			r.targs[i]=typeref_box(typeref_deepcopy(t->targs[i]));
		}
	}
	return r;
}

Param *func_add_param(Func *f)
{
	f->params=grow_ensure(f->params,f->param_count,&f->param_cap,sizeof(Param));
	Param *p=&f->params[f->param_count++];
	memset(p,0,sizeof(*p));
	return p;
}

void expr_add_arg(Expr *e, Expr *a)
{
	e->args=grow_ensure(e->args,e->arg_count,&e->arg_cap,sizeof(Expr *));
	e->args[e->arg_count++]=a;
}

Expr *expr_clone(const Expr *e)
{
	if (!e)
	{
		return NULL;
	}

	Expr *n=expr_new(e->kind,e->line);
	*n=*e;                                   /* Shallow copy scalars; args pointer aliases e until reset. */
	n->args=NULL;                            /* Own buffer: rebuild rather than alias e->args. */
	n->arg_count=0;
	n->arg_cap=0;
	n->type=typeref_deepcopy(&e->type);
	n->lhs=expr_clone(e->lhs);
	n->rhs=expr_clone(e->rhs);
	for (int i=0; i<e->arg_count; i++)
	{
		expr_add_arg(n,expr_clone(e->args[i]));
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
	n->params=NULL;                          /* Own buffer: rebuild rather than alias f->params. */
	n->param_count=0;
	n->param_cap=0;
	n->ret_type=typeref_deepcopy(&f->ret_type);
	for (int i=0; i<f->param_count; i++)
	{
		Param *p=func_add_param(n);
		*p=f->params[i];                     /* Name + scalars. */
		p->type=typeref_deepcopy(&f->params[i].type);
		p->def=expr_clone(f->params[i].def);
	}
	n->body=block_clone(f->body);
	return n;
}

Field *class_add_field(ClassDecl *c)
{
	c->fields=grow_ensure(c->fields,c->field_count,&c->fields_cap,sizeof(Field));
	Field *f=&c->fields[c->field_count++];
	memset(f,0,sizeof(*f));
	return f;
}

void class_add_method(ClassDecl *c, Func *m)
{
	c->methods=grow_ensure(c->methods,c->method_count,&c->methods_cap,sizeof(Func *));
	c->methods[c->method_count++]=m;
}

void class_add_ctor(ClassDecl *c, Func *f)
{
	c->ctors=grow_ensure(c->ctors,c->ctor_count,&c->ctors_cap,sizeof(Func *));
	c->ctors[c->ctor_count++]=f;
}

ClassDecl *classdecl_clone(const ClassDecl *c)
{
	ClassDecl *n=class_new();
	*n=*c;                                   /* Aliases fields/methods/ctors pointers until reset below. */
	n->fields=NULL;
	n->field_count=0;
	n->fields_cap=0;
	n->methods=NULL;
	n->method_count=0;
	n->methods_cap=0;
	n->ctors=NULL;
	n->ctor_count=0;
	n->ctors_cap=0;
	for (int i=0; i<c->field_count; i++)
	{
		Field *fl=class_add_field(n);
		*fl=c->fields[i];
		fl->type=typeref_deepcopy(&c->fields[i].type);
	}
	for (int i=0; i<c->method_count; i++)
	{
		class_add_method(n,func_clone(c->methods[i]));
	}
	for (int i=0; i<c->ctor_count; i++)
	{
		class_add_ctor(n,func_clone(c->ctors[i]));
	}
	n->ctor = n->ctor_count ? n->ctors[0] : NULL;
	return n;
}
