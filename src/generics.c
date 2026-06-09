#include "generics.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_INSTANCES 256

/* The built-in template names (Box/List/... from Parts 4e/4f) are handled by
   inline specialization, not lowering. A user generic may not shadow them. This
   mirrors parser.c's is_generic_template; keep the two in sync. */
static int is_builtin_template(const char *name)
{
	return strcmp(name,"Box")==0 || strcmp(name,"List")==0 || strcmp(name,"Stack")==0
		   || strcmp(name,"Queue")==0 || strcmp(name,"Deque")==0
		   || strcmp(name,"ArrayDeque")==0 || strcmp(name,"Set")==0;
}

/* ---- template + instance registries (file-static; one expand() per run) ---- */
static ClassDecl *g_templates[64];
static int        g_template_count;
static char       g_instances[MAX_INSTANCES][128];
static int        g_instance_count;
static Unit     **g_units;
static int       *g_total;
static int        g_max;
static int        g_changed;

static ClassDecl *find_template(const char *name)
{
	for (int i=0; i<g_template_count; i++)
	{
		if (strcmp(g_templates[i]->name,name)==0)
		{
			return g_templates[i];
		}
	}

	return NULL;
}

static int instance_exists(const char *mangled)
{
	for (int i=0; i<g_instance_count; i++)
	{
		if (strcmp(g_instances[i],mangled)==0)
		{
			return 1;
		}
	}

	return 0;
}

/* Find any class declaration (template or already-synthesized) by name. */
static ClassDecl *find_classdecl(const char *name)
{
	for (int i=0; i<*g_total; i++)
	{
		for (int ci=0; ci<g_units[i]->class_count; ci++)
		{
			if (strcmp(g_units[i]->klasses[ci]->name,name)==0)
			{
				return g_units[i]->klasses[ci];
			}
		}
	}

	return NULL;
}

/* Append the mangled spelling of an (already-lowered) type to buf. */
static void mangle_type(const TypeRef *t, char *buf)
{
	switch (t->kind)
	{
	case TY_BOOL:
		strcat(buf,"bool");
		break;
	case TY_BYTE:
		strcat(buf,"byte");
		break;
	case TY_SHORT:
		strcat(buf,"short");
		break;
	case TY_INT:
		strcat(buf,"int");
		break;
	case TY_LONG:
		strcat(buf,"long");
		break;
	case TY_UBYTE:
		strcat(buf,"ubyte");
		break;
	case TY_USHORT:
		strcat(buf,"ushort");
		break;
	case TY_UINT:
		strcat(buf,"uint");
		break;
	case TY_ULONG:
		strcat(buf,"ulong");
		break;
	case TY_FLOAT:
		strcat(buf,"f32");
		break;
	case TY_DOUBLE:
		strcat(buf,"f64");
		break;
	case TY_STRING:
		strcat(buf,"string");
		break;
	case TY_OBJECT:
		strcat(buf,t->class_name);
		break;
	case TY_ARRAY:
		mangle_type(t->elem,buf);
		strcat(buf,"_arr");
		break;
	case TY_MAP:
		strcat(buf,"map_");
		mangle_type(t->elem,buf);
		strcat(buf,"_");
		mangle_type(t->elem2,buf);
		break;
	case TY_GENERIC:   /* A built-in template arg, e.g. List<int>. */
		strcat(buf,t->class_name);
		strcat(buf,"$");
		mangle_type(t->elem,buf);
		break;
	default:
		fprintf(stderr,"Generics: cannot mangle type kind %d as a type argument.\n",t->kind);
		exit(1);
	}
}

static void mangle_application(const char *tmpl, struct TypeRef *const targs[], int n, char *out)
{
	strcpy(out,tmpl);
	for (int i=0; i<n; i++)
	{
		strcat(out,"$");
		mangle_type(targs[i],out);
	}
}

/* ---- substitution: replace type parameters with concrete args in a clone ---- */
static void subst_block(Block *b, const ClassDecl *tpl, struct TypeRef *const args[]);

static void subst_typeref(TypeRef *t, const ClassDecl *tpl, struct TypeRef *const args[])
{
	if (t->kind==TY_OBJECT)
	{
		for (int i=0; i<tpl->type_param_count; i++)
		{
			if (strcmp(t->class_name,tpl->type_params[i])==0)
			{
				*t=typeref_deepcopy(args[i]);   /* T -> concrete (may be a generic app). */
				return;
			}
		}

		return;
	}

	if (t->elem)
	{
		subst_typeref(t->elem,tpl,args);
	}

	if (t->elem2)
	{
		subst_typeref(t->elem2,tpl,args);
	}

	for (int i=0; i<t->targ_count; i++)
	{
		subst_typeref(t->targs[i],tpl,args);
	}
}

static void subst_expr(Expr *e, const ClassDecl *tpl, struct TypeRef *const args[])
{
	if (!e)
	{
		return;
	}

	subst_typeref(&e->type,tpl,args);
	/* `new T(...)`: if the constructed name is a type parameter, substitute the
	   arg's object class name (v1 requires the arg to be an object type). */
	if (e->kind==EX_NEW)
	{
		for (int i=0; i<tpl->type_param_count; i++)
		{
			if (strcmp(e->name,tpl->type_params[i])==0)
			{
				if (args[i]->kind!=TY_OBJECT)
				{
					fprintf(stderr,"line %d: Cannot 'new' type parameter '%s' bound to a non-object type.\n",
							e->line,e->name);
					exit(1);
				}

				strcpy(e->name,args[i]->class_name);
				break;
			}
		}
	}

	subst_expr(e->lhs,tpl,args);
	subst_expr(e->rhs,tpl,args);
	for (int i=0; i<e->arg_count; i++)
	{
		subst_expr(e->args[i],tpl,args);
	}
}

static void subst_stmt(Stmt *s, const ClassDecl *tpl, struct TypeRef *const args[])
{
	if (!s)
	{
		return;
	}

	subst_typeref(&s->decl_type,tpl,args);
	subst_typeref(&s->fe_val_type,tpl,args);
	subst_expr(s->decl_init,tpl,args);
	subst_stmt(s->for_init,tpl,args);
	subst_stmt(s->for_post,tpl,args);
	subst_expr(s->target,tpl,args);
	subst_expr(s->value,tpl,args);
	subst_expr(s->cond,tpl,args);
	subst_expr(s->ret_val,tpl,args);
	subst_expr(s->expr,tpl,args);
	subst_block(s->then_blk,tpl,args);
	subst_block(s->else_blk,tpl,args);
}

static void subst_block(Block *b, const ClassDecl *tpl, struct TypeRef *const args[])
{
	if (!b)
	{
		return;
	}

	for (int i=0; i<b->count; i++)
	{
		subst_stmt(b->stmts[i],tpl,args);
	}
}

static void subst_func(Func *f, const ClassDecl *tpl, struct TypeRef *const args[])
{
	subst_typeref(&f->ret_type,tpl,args);
	for (int i=0; i<f->param_count; i++)
	{
		subst_typeref(&f->params[i].type,tpl,args);
	}

	subst_block(f->body,tpl,args);
}

/* Verify an interface bound: arg must be an object whose class implements it. */
static void check_bound(const ClassDecl *tpl, int pi, const TypeRef *arg, int line)
{
	const char *bound=tpl->type_param_bounds[pi];
	if (bound[0]=='\0')
	{
		return;   /* Unbounded. */
	}

	if (arg->kind!=TY_OBJECT)
	{
		fprintf(stderr,"line %d: Type argument for '%s' must implement '%s' (got a non-object type).\n",
				line,tpl->type_params[pi],bound);
		exit(1);
	}

	ClassDecl *ac=find_classdecl(arg->class_name);
	int ok=0;
	if (ac)
	{
		for (int i=0; i<ac->implements_count; i++)
		{
			if (strcmp(ac->implements[i],bound)==0)
			{
				ok=1;
				break;
			}
		}
	}

	if (!ok)
	{
		fprintf(stderr,"line %d: Type argument '%s' does not implement bound '%s'.\n",
				line,arg->class_name,bound);
		exit(1);
	}
}

/* Ensure an instance exists; return its mangled name (into out). */
static void ensure_instance(const char *tmpl, struct TypeRef *const targs[], int n, int line, char *out)
{
	mangle_application(tmpl,targs,n,out);
	if (instance_exists(out))
	{
		return;
	}

	ClassDecl *t=find_template(tmpl);
	if (!t)
	{
		fprintf(stderr,"line %d: Unknown generic class '%s'.\n",line,tmpl);
		exit(1);
	}

	if (n!=t->type_param_count)
	{
		fprintf(stderr,"line %d: '%s' expects %d type argument(s), got %d.\n",
				line,tmpl,t->type_param_count,n);
		exit(1);
	}

	for (int i=0; i<n; i++)
	{
		check_bound(t,i,targs[i],line);
	}

	if (g_instance_count>=MAX_INSTANCES || *g_total>=g_max)
	{
		fprintf(stderr,"Generics: too many instantiations (possible infinitely recursive generic).\n");
		exit(1);
	}

	/* Clone + substitute. */
	ClassDecl *c=classdecl_clone(t);
	strcpy(c->name,out);
	c->type_param_count=0;
	for (int i=0; i<c->field_count; i++)
	{
		subst_typeref(&c->fields[i].type,t,targs);
	}
	for (int i=0; i<c->method_count; i++)
	{
		subst_func(c->methods[i],t,targs);
	}
	if (c->ctor)
	{
		subst_func(c->ctor,t,targs);
	}

	Unit *u=unit_new();
	unit_add_class(u,c);
	strcpy(g_instances[g_instance_count++],out);
	g_units[(*g_total)++]=u;
	g_changed=1;   /* New unit must be walked for further applications. */
}

/* ---- whole-program walk: rewrite every user-generic application in place ---- */
static void rewrite_typeref(TypeRef *t)
{
	if (t->kind==TY_GENERIC)
	{
		if (is_builtin_template(t->class_name))
		{
			if (t->elem)
			{
				rewrite_typeref(t->elem);   /* e.g. List<Pair<int,int>>. */
			}

			return;
		}

		/* User generic application: lower args first, then this node. */
		for (int i=0; i<t->targ_count; i++)
		{
			rewrite_typeref(t->targs[i]);
		}

		char mangled[128];
		ensure_instance(t->class_name,t->targs,t->targ_count,0,mangled);
		t->kind=TY_OBJECT;
		strcpy(t->class_name,mangled);
		t->elem=NULL;
		t->elem2=NULL;
		t->targ_count=0;
		return;
	}

	if (t->elem)
	{
		rewrite_typeref(t->elem);
	}

	if (t->elem2)
	{
		rewrite_typeref(t->elem2);
	}

	for (int i=0; i<t->targ_count; i++)
	{
		rewrite_typeref(t->targs[i]);
	}
}

static void rewrite_block(Block *b);

static void rewrite_expr(Expr *e)
{
	if (!e)
	{
		return;
	}

	if (e->kind==EX_NEWGEN && e->type.kind==TY_GENERIC && !is_builtin_template(e->type.class_name))
	{
		for (int i=0; i<e->type.targ_count; i++)
		{
			rewrite_typeref(e->type.targs[i]);
		}

		char mangled[128];
		ensure_instance(e->type.class_name,e->type.targs,e->type.targ_count,e->line,mangled);
		e->kind=EX_NEW;
		strcpy(e->name,mangled);
		e->type.kind=TY_OBJECT;
		strcpy(e->type.class_name,mangled);
		e->type.elem=NULL;
		e->type.elem2=NULL;
		e->type.targ_count=0;
	}
	else
	{
		rewrite_typeref(&e->type);
	}

	rewrite_expr(e->lhs);
	rewrite_expr(e->rhs);
	for (int i=0; i<e->arg_count; i++)
	{
		rewrite_expr(e->args[i]);
	}
}

static void rewrite_stmt(Stmt *s)
{
	if (!s)
	{
		return;
	}

	rewrite_typeref(&s->decl_type);
	rewrite_typeref(&s->fe_val_type);
	rewrite_expr(s->decl_init);
	rewrite_stmt(s->for_init);
	rewrite_stmt(s->for_post);
	rewrite_expr(s->target);
	rewrite_expr(s->value);
	rewrite_expr(s->cond);
	rewrite_expr(s->ret_val);
	rewrite_expr(s->expr);
	rewrite_block(s->then_blk);
	rewrite_block(s->else_blk);
}

static void rewrite_block(Block *b)
{
	if (!b)
	{
		return;
	}

	for (int i=0; i<b->count; i++)
	{
		rewrite_stmt(b->stmts[i]);
	}
}

static void rewrite_func(Func *f)
{
	if (!f)
	{
		return;
	}

	rewrite_typeref(&f->ret_type);
	for (int i=0; i<f->param_count; i++)
	{
		rewrite_typeref(&f->params[i].type);
	}

	rewrite_block(f->body);
}

static void rewrite_unit(Unit *u)
{
	for (int ci=0; ci<u->class_count; ci++)
	{
		ClassDecl *c=u->klasses[ci];
		if (c->type_param_count!=0)   /* Skip templates; walk concrete classes. */
		{
			continue;
		}
		for (int i=0; i<c->field_count; i++)
		{
			rewrite_typeref(&c->fields[i].type);
		}
		for (int i=0; i<c->method_count; i++)
		{
			rewrite_func(c->methods[i]);
		}

		rewrite_func(c->ctor);
	}

	for (int i=0; i<u->interface_count; i++)
	{
		for (int m=0; m<u->interfaces[i]->method_count; m++)
		{
			rewrite_func(u->interfaces[i]->methods[m]);
		}
	}

	for (int i=0; i<u->func_count; i++)
	{
		rewrite_func(u->funcs[i]);
	}
}

void generics_expand(Unit **units, int *total, int max)
{
	g_units=units;
	g_total=total;
	g_max=max;
	g_template_count=0;
	g_instance_count=0;

	for (int i=0; i<*total; i++)
	{
		for (int ci=0; ci<units[i]->class_count; ci++)
		{
			ClassDecl *k=units[i]->klasses[ci];
			if (k->type_param_count>0)
			{
				if (is_builtin_template(k->name))
				{
					fprintf(stderr,"A user generic class may not shadow built-in template '%s'.\n",k->name);
					exit(1);
				}

				g_templates[g_template_count++]=k;
			}
		}
	}

	/* Fixpoint: rewrite all units (including instances appended mid-walk) until
	   no new instantiation appears. ensure_instance sets g_changed and appends. */
	do
	{
		g_changed=0;
		int n=*total;   /* Snapshot; newly appended units are picked up next round. */
		for (int i=0; i<n; i++)
		{
			rewrite_unit(units[i]);
		}
	}
	while (g_changed);

	/* Exclude templates from registration/codegen by removing them from their units. */
	for (int i=0; i<g_template_count; i++)
	{
		for (int u=0; u<*total; u++)
		{
			Unit *un=units[u];
			for (int ci=0; ci<un->class_count; ci++)
			{
				if (un->klasses[ci]==g_templates[i])
				{
					for (int j=ci+1; j<un->class_count; j++)
					{
						un->klasses[j-1]=un->klasses[j];
					}
					un->class_count--;
					ci--;   /* Re-check the slot now holding the shifted element. */
				}
			}
		}
	}
}
