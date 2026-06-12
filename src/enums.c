#include "enums.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static EnumInfo g_enums[32];
static int g_enum_count;

static int is_reserved_method(const char *n)
{
	return strcmp(n,"values")==0 || strcmp(n,"valueOf")==0
		   || strcmp(n,"name")==0 || strcmp(n,"ordinal")==0;
}

/* The base class for an enum: hidden __ordinal/__name fields, then the user
   fields, the user methods, the constructor, and the implements list. */
static ClassDecl *make_base_class(const EnumDecl *e)
{
	ClassDecl *c=class_new();
	strcpy(c->name,e->name);
	c->implements_count=e->implements_count;
	for (int i=0; i<e->implements_count; i++)
	{
		strcpy(c->implements[i],e->implements[i]);
	}

	c->fields[0].type.kind=TY_INT;
	strcpy(c->fields[0].name,"__ordinal");
	c->fields[1].type.kind=TY_STRING;
	strcpy(c->fields[1].name,"__name");
	c->field_count=2;
	for (int i=0; i<e->field_count; i++)
	{
		if (strcmp(e->fields[i].name,"__ordinal")==0 || strcmp(e->fields[i].name,"__name")==0)
		{
			fprintf(stderr,"Enum '%s': field name '%s' is reserved.\n",e->name,e->fields[i].name);
			exit(1);
		}

		c->fields[c->field_count++]=e->fields[i];
	}

	for (int i=0; i<e->method_count; i++)
	{
		if (is_reserved_method(e->methods[i]->name))
		{
			fprintf(stderr,"Enum '%s': method '%s' is reserved (compiler-provided).\n",
					e->name,e->methods[i]->name);
			exit(1);
		}

		c->methods[c->method_count++]=func_clone(e->methods[i]);
	}

	if (e->ctor)
	{
		c->ctors[c->ctor_count++]=func_clone(e->ctor);
		c->ctor=c->ctors[0];
	}
	return c;
}

/* A per-constant subclass: extends the base, overrides the listed methods, and
   carries a copy of the enum constructor so `new Enum$CONST(args)` initializes
   the inherited fields the same way the base does. */
static ClassDecl *make_constant_subclass(const EnumDecl *e, const EnumConstant *k, const char *mangled)
{
	ClassDecl *c=class_new();
	strcpy(c->name,mangled);
	c->has_parent=1;
	strcpy(c->parent_name,e->name);
	for (int i=0; i<k->override_count; i++)
	{
		if (is_reserved_method(k->overrides[i]->name))
		{
			fprintf(stderr,"Enum '%s': method '%s' is reserved (compiler-provided).\n",
					e->name,k->overrides[i]->name);
			exit(1);
		}

		c->methods[c->method_count++]=func_clone(k->overrides[i]);
	}

	if (e->ctor)
	{
		c->ctors[c->ctor_count++]=func_clone(e->ctor);
		c->ctor=c->ctors[0];
	}
	return c;
}

static void lower_one(const EnumDecl *e, Unit **units, int *total, int max)
{
	if (g_enum_count>=32)
	{
		fprintf(stderr,"Too many enums.\n");
		exit(1);
	}

	int ctor_argc=e->ctor ? e->ctor->param_count : 0;
	EnumInfo *info=&g_enums[g_enum_count++];
	memset(info,0,sizeof(*info));
	strcpy(info->name,e->name);
	info->implements_count=e->implements_count;
	for (int i=0; i<e->implements_count; i++)
	{
		strcpy(info->implements[i],e->implements[i]);
	}

	info->constant_count=e->constant_count;

	/* Base class first (a parent must register before its subclasses). */
	if (*total>=max)
	{
		fprintf(stderr,"Too many units (enum lowering).\n");
		exit(1);
	}
	Unit *bu=unit_new();
	unit_add_class(bu, make_base_class(e));
	units[(*total)++]=bu;

	for (int i=0; i<e->constant_count; i++)
	{
		const EnumConstant *k=&e->constants[i];
		for (int j=0; j<i; j++)
		{
			if (strcmp(info->const_name[j],k->name)==0)
			{
				fprintf(stderr,"Enum '%s': duplicate constant '%s'.\n",e->name,k->name);
				exit(1);
			}
		}

		if (k->arg_count!=ctor_argc)
		{
			fprintf(stderr,"Enum '%s': constant '%s' passes %d argument(s), constructor takes %d.\n",
					e->name,k->name,k->arg_count,ctor_argc);
			exit(1);
		}

		strcpy(info->const_name[i],k->name);
		info->const_argc[i]=k->arg_count;
		for (int a=0; a<k->arg_count; a++)
		{
			info->const_args[i][a]=k->args[a];
		}

		if (k->override_count>0)
		{
			char mangled[128];
			snprintf(mangled,sizeof(mangled),"%s$%s",e->name,k->name);
			strcpy(info->const_class[i],mangled);
			if (*total>=max)
			{
				fprintf(stderr,"Too many units (enum lowering).\n");
				exit(1);
			}
			Unit *su=unit_new();
			unit_add_class(su, make_constant_subclass(e,k,mangled));
			units[(*total)++]=su;
		}
		else
		{
			strcpy(info->const_class[i],e->name);
		}
	}
}

void enums_expand(Unit **units, int *total, int max)
{
	g_enum_count=0;
	int n=*total;
	for (int i=0; i<n; i++)
	{
		for (int j=0; j<units[i]->enum_count; j++)
		{
			lower_one(units[i]->enums[j],units,total,max);
		}

		units[i]->enum_count=0;   /* Consumed: downstream sees only the synthesized classes. */
	}
}

static const EnumInfo *find_enum(const char *name)
{
	for (int i=0; i<g_enum_count; i++)
	{
		if (strcmp(g_enums[i].name,name)==0)
		{
			return &g_enums[i];
		}
	}

	return NULL;
}

int enum_is(const char *name)
{
	return find_enum(name)!=NULL;
}

int enum_ordinal(const char *en, const char *c)
{
	const EnumInfo *e=find_enum(en);
	if (!e)
	{
		return -1;
	}

	for (int i=0; i<e->constant_count; i++)
	{
		if (strcmp(e->const_name[i],c)==0)
		{
			return i;
		}
	}

	return -1;
}

int enum_count_of(const char *en)
{
	const EnumInfo *e=find_enum(en);
	return e ? e->constant_count : 0;
}

const char *enum_const_name(const char *en, int idx)
{
	const EnumInfo *e=find_enum(en);
	if (!e || idx<0 || idx>=e->constant_count)
	{
		return NULL;
	}

	return e->const_name[idx];
}

int enum_total(void)
{
	return g_enum_count;
}

const EnumInfo *enum_at(int i)
{
	return (i>=0 && i<g_enum_count) ? &g_enums[i] : NULL;
}
