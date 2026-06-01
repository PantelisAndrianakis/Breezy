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
		fprintf(stderr, "ast: out of memory\n");
		exit(1);
	}
	if (g_node_count >= MAX_NODES)
	{
		fprintf(stderr, "ast: too many nodes\n");
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
Unit      *unit_new(void)
{
	return track(calloc(1,sizeof(Unit)));
}
