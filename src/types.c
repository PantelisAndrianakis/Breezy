#include "types.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

void types_init(TypeTable *tt)
{
	tt->class_count = 0;
	tt->func_count = 0;
}

void types_register_builtins(TypeTable *tt)
{
	ClassInfo *c = &tt->classes[tt->class_count++];
	memset(c, 0, sizeof(*c));
	strcpy(c->name, "Exception");
	c->parent = NULL;
	strcpy(c->fields[0].name, "message");
	c->fields[0].type.kind = TY_STRING;
	c->fields[0].offset = 24;
	c->field_count = 1;
	c->object_size = 32;
	c->vtable_size = 0;
	c->method_count = 0;

	ClassInfo *o = &tt->classes[tt->class_count++];
	memset(o, 0, sizeof(*o));
	strcpy(o->name, "IndexOutOfBounds");
	o->parent = c;                       /* `c` is the Exception entry above. */
	strcpy(o->fields[0].name, "message");
	o->fields[0].type.kind = TY_STRING;
	o->fields[0].offset = 24;
	o->field_count = 1;
	o->object_size = 32;
	o->vtable_size = 0;
	o->method_count = 0;

	ClassInfo *io = &tt->classes[tt->class_count++];
	memset(io, 0, sizeof(*io));
	strcpy(io->name, "IOException");
	io->parent = c;                      /* Subclass of Exception. */
	strcpy(io->fields[0].name, "message");
	io->fields[0].type.kind = TY_STRING;
	io->fields[0].offset = 24;
	io->field_count = 1;
	io->object_size = 32;
	io->vtable_size = 0;
	io->method_count = 0;

	ClassInfo *nf = &tt->classes[tt->class_count++];
	memset(nf, 0, sizeof(*nf));
	strcpy(nf->name, "NumberFormatException");
	nf->parent = c;                      /* Subclass of Exception. */
	strcpy(nf->fields[0].name, "message");
	nf->fields[0].type.kind = TY_STRING;
	nf->fields[0].offset = 24;
	nf->field_count = 1;
	nf->object_size = 32;
	nf->vtable_size = 0;
	nf->method_count = 0;
}

ClassInfo *types_find_class(TypeTable *tt, const char *name)
{
	for (int i=0; i<tt->class_count; i++)
	{
		if (strcmp(tt->classes[i].name,name)==0)
		{
			return &tt->classes[i];
		}
	}

	return NULL;
}

/* Seed step: any channel<T> with an object element type T marks T's class shared.
   Recurses through container TypeRefs (array/map) so a channel nested inside, e.g.,
   a parameter type is still found. (Sending a *collection* of confined objects
   without the element type itself being a channel element is a known conservative
   gap; builtin managed types string/array/map crossing a channel are likewise not
   marked here, since they carry no ClassInfo.) */
static void seed_shared_from_typeref(TypeTable *tt, TypeRef *t)
{
	if (!t)
	{
		return;
	}

	if (t->kind==TY_CHANNEL && t->elem && t->elem->kind==TY_OBJECT)
	{
		ClassInfo *c=types_find_class(tt,t->elem->class_name);
		if (c)
		{
			c->is_shared=1;
		}
	}

	seed_shared_from_typeref(tt,t->elem);
	seed_shared_from_typeref(tt,t->elem2);
}

void types_compute_shared_set(TypeTable *tt)
{
	/* Seed from every channel<T> that appears in a signature the runtime can use
	   to move a channel between breezes: class fields, method/function/ctor
	   parameters, and return types. A channel only reaches another breeze through
	   one of these, so this is sufficient to catch every cross-core object. */
	for (int ci=0; ci<tt->class_count; ci++)
	{
		ClassInfo *c=&tt->classes[ci];
		for (int fi=0; fi<c->field_count; fi++)
		{
			seed_shared_from_typeref(tt,&c->fields[fi].type);
		}

		for (int mi=0; mi<c->method_count; mi++)
		{
			MethodInfo *m=&c->methods[mi];
			seed_shared_from_typeref(tt,&m->ret_type);
			for (int pi=0; pi<m->param_count; pi++)
			{
				seed_shared_from_typeref(tt,&m->param_types[pi]);
			}
		}

		for (int pi=0; pi<c->ctor_param_count; pi++)
		{
			seed_shared_from_typeref(tt,&c->ctor_param_types[pi]);
		}
	}

	for (int fi=0; fi<tt->func_count; fi++)
	{
		FuncInfo *f=&tt->funcs[fi];
		seed_shared_from_typeref(tt,&f->ret_type);
		for (int pi=0; pi<f->param_count; pi++)
		{
			seed_shared_from_typeref(tt,&f->param_types[pi]);
		}
	}

	/* Fixpoint: a shared object reachable across cores drags its object-typed
	   fields along, so they must be shared too. Iterate until nothing changes. */
	int changed=1;
	while (changed)
	{
		changed=0;
		for (int ci=0; ci<tt->class_count; ci++)
		{
			ClassInfo *c=&tt->classes[ci];
			if (!c->is_shared)
			{
				continue;
			}

			for (int fi=0; fi<c->field_count; fi++)
			{
				FieldInfo *f=&c->fields[fi];
				if (f->type.kind==TY_OBJECT)
				{
					ClassInfo *fc=types_find_class(tt,f->type.class_name);
					if (fc && !fc->is_shared)
					{
						fc->is_shared=1;
						changed=1;
					}
				}
			}
		}
	}
}

FuncInfo *types_find_func(TypeTable *tt, const char *name)
{
	for (int i=0; i<tt->func_count; i++)
	{
		if (strcmp(tt->funcs[i].name,name)==0)
		{
			return &tt->funcs[i];
		}
	}

	return NULL;
}

MethodInfo *types_find_method(ClassInfo *c, const char *name)
{
	for (int i=0; i<c->method_count; i++)
	{
		if (strcmp(c->methods[i].name,name)==0)
		{
			return &c->methods[i];
		}
	}

	return NULL;
}

FieldInfo *types_find_field(ClassInfo *c, const char *name)
{
	for (int i=0; i<c->field_count; i++)
	{
		if (strcmp(c->fields[i].name,name)==0)
		{
			return &c->fields[i];
		}
	}

	return NULL;
}

void types_register_unit_names(TypeTable *tt, Unit *u)
{
	if (u->klass)
	{
		if (tt->class_count>=MAX_CLASSES)
		{
			fprintf(stderr,"Too many classes.\n");
			exit(1);
		}

		ClassInfo *c=&tt->classes[tt->class_count++];
		memset(c,0,sizeof(*c));
		strcpy(c->name,u->klass->name);
	}

	for (int i=0; i<u->func_count; i++)
	{
		if (tt->func_count>=MAX_FUNCS)
		{
			fprintf(stderr,"Too many funcs.\n");
			exit(1);
		}

		FuncInfo *fi=&tt->funcs[tt->func_count++];
		memset(fi,0,sizeof(*fi));
		strcpy(fi->name,u->funcs[i]->name);
	}
}

static void link_parent(TypeTable *tt, ClassInfo *c, ClassDecl *d)
{
	if (!d->has_parent)
	{
		c->parent=NULL;
		return;
	}

	c->parent=types_find_class(tt,d->parent_name);
	if (!c->parent)
	{
		fprintf(stderr,"Class %s: unknown parent '%s'\n",c->name,d->parent_name);
		exit(1);
	}
}

void types_register_unit_members(TypeTable *tt, Unit *u)
{
	for (int i=0; i<u->func_count; i++)
	{
		Func *f=u->funcs[i];
		FuncInfo *fi=types_find_func(tt,f->name);
		fi->ast=f;
		fi->ret_type=f->ret_type;
		fi->param_count=f->param_count;
		for (int k=0; k<f->param_count; k++)
		{
			fi->param_types[k]=f->params[k].type;
		}

		if (f->is_extern)
		{
			snprintf(fi->asm_label,sizeof(fi->asm_label),"%s",f->name);   /* Raw C symbol, no bzy_ prefix. */
			fi->is_extern=1;
		}
		else
		{
			snprintf(fi->asm_label,sizeof(fi->asm_label),"bzy_%s",f->name);
		}
	}

	if (!u->klass)
	{
		return;
	}

	ClassDecl *d=u->klass;
	ClassInfo *c=types_find_class(tt,d->name);
	link_parent(tt,c,d);
	if (c->parent)
	{
		c->field_count=c->parent->field_count;
		memcpy(c->fields,c->parent->fields,sizeof(FieldInfo)*c->field_count);
		c->method_count=c->parent->method_count;
		memcpy(c->methods,c->parent->methods,sizeof(MethodInfo)*c->method_count);
		c->vtable_size=c->parent->vtable_size;
	}

	for (int i=0; i<d->field_count; i++)
	{
		FieldInfo *fi=&c->fields[c->field_count];
		strcpy(fi->name,d->fields[i].name);
		fi->type=d->fields[i].type;
		fi->offset=24 + c->field_count*8;
		c->field_count++;
	}

	c->object_size = 24 + c->field_count*8;
	for (int i=0; i<d->method_count; i++)
	{
		Func *m=d->methods[i];
		MethodInfo *existing=types_find_method(c,m->name);
		MethodInfo *mi;
		if (existing)
		{
			mi=existing;
		}
		else
		{
			mi=&c->methods[c->method_count++];
			memset(mi,0,sizeof(*mi));
			strcpy(mi->name,m->name);
			mi->vtable_slot=c->vtable_size++;
		}

		strcpy(mi->owner_class,c->name);
		snprintf(mi->asm_label,sizeof(mi->asm_label),"%s__%s",c->name,m->name);
		mi->ast=m;
		mi->ret_type=m->ret_type;
		mi->param_count=m->param_count;
		for (int k=0; k<m->param_count; k++)
		{
			mi->param_types[k]=m->params[k].type;
		}
	}

	if (d->ctor)
	{
		c->has_ctor=1;
		c->ctor_ast=d->ctor;
		c->ctor_param_count=d->ctor->param_count;
		for (int k=0; k<d->ctor->param_count; k++)
		{
			c->ctor_param_types[k]=d->ctor->params[k].type;
		}

		snprintf(c->ctor_asm_label,sizeof(c->ctor_asm_label),"__ctor_%s",c->name);
	}
}
