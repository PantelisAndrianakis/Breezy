#include "enums.h"
#include "grow.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static EnumInfo *g_enums;
static int g_enum_count;
static int g_enum_cap;

static int is_reserved_method(const char *n)
{
	return strcmp(n,"values")==0 || strcmp(n,"valueOf")==0
		   || strcmp(n,"name")==0 || strcmp(n,"ordinal")==0;
}

/* Forward a generic enum's type parameters onto a synthesized class so the
   normal class monomorphization handles them. */
static void copy_type_params(ClassDecl *c, const EnumDecl *e)
{
	for (int i=0; i<e->type_param_count; i++)
	{
		c->type_params       = grow_ensure(c->type_params,       c->type_param_count, &c->type_param_cap,        sizeof(*c->type_params));
		c->type_param_bounds = grow_ensure(c->type_param_bounds, c->type_param_count, &c->type_param_bounds_cap, sizeof(*c->type_param_bounds));
		strcpy(c->type_params[c->type_param_count],e->type_params[i]);
		c->type_param_bounds[c->type_param_count][0]='\0';
		c->type_param_count++;
	}
}

/* The base class for an enum: hidden __ordinal/__name fields, then the user
   fields, the user methods, the constructor, and the implements list. */
static ClassDecl *make_base_class(const EnumDecl *e)
{
	ClassDecl *c=class_new();
	strcpy(c->name,e->name);
	copy_type_params(c,e);
	c->implements=grow_reserve(c->implements,e->implements_count,&c->implements_cap,sizeof(*c->implements));
	c->implements_count=e->implements_count;
	for (int i=0; i<e->implements_count; i++)
	{
		strcpy(c->implements[i],e->implements[i]);
	}

	Field *fo=class_add_field(c);
	fo->type.kind=TY_INT;
	strcpy(fo->name,"__ordinal");
	Field *fnm=class_add_field(c);
	fnm->type.kind=TY_STRING;
	strcpy(fnm->name,"__name");
	for (int i=0; i<e->field_count; i++)
	{
		if (strcmp(e->fields[i].name,"__ordinal")==0 || strcmp(e->fields[i].name,"__name")==0)
		{
			fprintf(stderr,"Enum '%s': field name '%s' is reserved.\n",e->name,e->fields[i].name);
			exit(1);
		}

		Field *ff=class_add_field(c);
		*ff=e->fields[i];
	}

	for (int i=0; i<e->method_count; i++)
	{
		if (is_reserved_method(e->methods[i]->name))
		{
			fprintf(stderr,"Enum '%s': method '%s' is reserved (compiler-provided).\n",
					e->name,e->methods[i]->name);
			exit(1);
		}

		class_add_method(c,func_clone(e->methods[i]));
	}

	if (e->ctor)
	{
		class_add_ctor(c,func_clone(e->ctor));
		c->ctor=c->ctors[0];
	}
	return c;
}

/* One `this.<field> = <value>` assignment in a synthesized constructor body. */
static Stmt *variant_assign(const char *field, Expr *value)
{
	Expr *recv=expr_new(EX_THIS,0);
	Expr *fld=expr_new(EX_FIELD,0);
	strcpy(fld->name,field);
	fld->lhs=recv;
	Stmt *s=stmt_new(ST_ASSIGN,0);
	s->target=fld;
	s->value=value;
	return s;
}

/* The constructor synthesized for a payload variant: parameters are the variant's
   declared fields, and the body stamps the hidden __ordinal/__name and stores
   each field, so `new Enum$Const(args)` builds a fully-formed instance. */
static Func *make_variant_ctor(const EnumConstant *k, int ord, const char *mangled)
{
	Func *f=func_new();
	f->ret_type.kind=TY_VOID;
	strcpy(f->name,mangled);
	for (int i=0; i<k->payload_count; i++)
	{
		Param *pm=func_add_param(f);
		pm->type=k->payload[i].type;
		strcpy(pm->name,k->payload[i].name);
	}

	f->body=block_new();

	Expr *ordv=expr_new(EX_INT,0);
	ordv->int_val=ord;
	ordv->type.kind=TY_INT;
	block_push(f->body,variant_assign("__ordinal",ordv));

	Expr *namev=expr_new(EX_STR,0);
	strcpy(namev->str_val,k->name);
	namev->type.kind=TY_STRING;
	block_push(f->body,variant_assign("__name",namev));

	for (int i=0; i<k->payload_count; i++)
	{
		Expr *v=expr_new(EX_IDENT,0);
		strcpy(v->name,k->payload[i].name);
		block_push(f->body,variant_assign(k->payload[i].name,v));
	}

	return f;
}

/* A per-constant subclass: extends the base, overrides the listed methods, and
   carries a constructor. For an ordinary constant that is a copy of the enum
   constructor (so `new Enum$CONST(args)` initializes the inherited fields the
   same way the base does); for a payload variant it is the synthesized
   field-storing constructor and the variant's own fields are added here. */
static ClassDecl *make_constant_subclass(const EnumDecl *e, const EnumConstant *k, int ord, const char *mangled)
{
	ClassDecl *c=class_new();
	strcpy(c->name,mangled);
	c->has_parent=1;
	strcpy(c->parent_name,e->name);
	/* A generic enum's variant subclass is itself generic and extends the generic
	   base: Enum$Variant<T> extends Enum<T>. Forward the parameters and the parent
	   type arguments so monomorphization produces Enum$Variant$int extends
	   Enum$int. */
	copy_type_params(c,e);
	if (e->type_param_count>0)
	{
		int pcap=0;
		for (int i=0; i<e->type_param_count; i++)
		{
			TypeRef *tr=calloc(1,sizeof(TypeRef));
			tr->kind=TY_OBJECT;
			strcpy(tr->class_name,e->type_params[i]);
			c->parent_targs=grow_ensure(c->parent_targs,c->parent_targ_count,&pcap,sizeof(*c->parent_targs));
			c->parent_targs[c->parent_targ_count++]=tr;
		}
	}

	for (int i=0; i<k->override_count; i++)
	{
		if (is_reserved_method(k->overrides[i]->name))
		{
			fprintf(stderr,"Enum '%s': method '%s' is reserved (compiler-provided).\n",
					e->name,k->overrides[i]->name);
			exit(1);
		}

		class_add_method(c,func_clone(k->overrides[i]));
	}

	if (k->payload_count>0)
	{
		for (int i=0; i<k->payload_count; i++)
		{
			Field *ff=class_add_field(c);
			*ff=k->payload[i];
		}

		class_add_ctor(c,make_variant_ctor(k,ord,mangled));
		c->ctor=c->ctors[0];
	}
	else if (e->ctor)
	{
		class_add_ctor(c,func_clone(e->ctor));
		c->ctor=c->ctors[0];
	}

	return c;
}

static void lower_one(const EnumDecl *e, Unit ***units, int *total, int *cap)
{
	int ctor_argc=e->ctor ? e->ctor->param_count : 0;
	g_enums=grow_ensure(g_enums,g_enum_count,&g_enum_cap,sizeof(*g_enums));
	EnumInfo *info=&g_enums[g_enum_count++];
	memset(info,0,sizeof(*info));
	strcpy(info->name,e->name);
	info->type_param_count=e->type_param_count;
	info->implements=grow_reserve(info->implements,e->implements_count,&info->implements_cap,sizeof(*info->implements));
	info->implements_count=e->implements_count;
	for (int i=0; i<e->implements_count; i++)
	{
		strcpy(info->implements[i],e->implements[i]);
	}

	info->constant_count=e->constant_count;
	{
		int cc=e->constant_count;   /* Per-constant parallel arrays, sized exactly. */
		info->constant_cap=cc;
		info->const_name  = cc ? calloc((size_t)cc, sizeof(*info->const_name))  : NULL;
		info->const_class = cc ? calloc((size_t)cc, sizeof(*info->const_class)) : NULL;
		info->const_args  = cc ? calloc((size_t)cc, sizeof(*info->const_args))  : NULL;
		info->const_argc  = cc ? calloc((size_t)cc, sizeof(*info->const_argc))  : NULL;
		info->const_payload = cc ? calloc((size_t)cc, sizeof(*info->const_payload)) : NULL;
	}

	/* Base class first (a parent must register before its subclasses). */
	*units = grow_ensure(*units, *total, cap, sizeof(**units));
	Unit *bu=unit_new();
	unit_add_class(bu, make_base_class(e));
	(*units)[(*total)++]=bu;

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

		/* A generic enum's constants are constructed per call (no per-instantiation
		   singleton exists), so they must carry a payload for now. A zero-field
		   variant (e.g. Option's None) needs construction-site type inference,
		   which is a later addition. */
		if (e->type_param_count>0 && k->payload_count==0)
		{
			fprintf(stderr,"Enum '%s': a generic enum's constant '%s' must carry a payload (zero-field variants not supported yet).\n",
					e->name,k->name);
			exit(1);
		}

		strcpy(info->const_name[i],k->name);

		/* Payload variant: a constructible sum-type case. It always becomes a
		   subclass `Enum$Const` (with its own fields + synthesized constructor)
		   and is built per call, not stamped as a startup singleton, so it
		   carries no singleton args. */
		if (k->payload_count>0)
		{
			info->const_payload[i]=1;
			info->const_argc[i]=0;
			char mangled[128];
			snprintf(mangled,sizeof(mangled),"%s$%s",e->name,k->name);
			strcpy(info->const_class[i],mangled);
			*units = grow_ensure(*units, *total, cap, sizeof(**units));
			Unit *su=unit_new();
			unit_add_class(su, make_constant_subclass(e,k,i,mangled));
			(*units)[(*total)++]=su;
			continue;
		}

		if (k->arg_count!=ctor_argc)
		{
			fprintf(stderr,"Enum '%s': constant '%s' passes %d argument(s), constructor takes %d.\n",
					e->name,k->name,k->arg_count,ctor_argc);
			exit(1);
		}

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
			*units = grow_ensure(*units, *total, cap, sizeof(**units));
			Unit *su=unit_new();
			unit_add_class(su, make_constant_subclass(e,k,i,mangled));
			(*units)[(*total)++]=su;
		}
		else
		{
			strcpy(info->const_class[i],e->name);
		}
	}
}

void enums_expand(Unit ***units, int *total, int *cap)
{
	g_enum_count=0;
	int n=*total;
	for (int i=0; i<n; i++)
	{
		for (int j=0; j<(*units)[i]->enum_count; j++)
		{
			lower_one((*units)[i]->enums[j],units,total,cap);
		}

		(*units)[i]->enum_count=0;   /* Consumed: downstream sees only the synthesized classes. */
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

void enum_register_instance(const char *inst, const char *base, const char *suffix)
{
	const EnumInfo *b=find_enum(base);
	if (!b || find_enum(inst))
	{
		return;
	}

	/* Capture the base's array pointers + count before growing g_enums (the grow
	   may realloc, invalidating `b`; the captured arrays live outside it). */
	int cc=b->constant_count;
	char (*bname)[64]=b->const_name;
	char (*bclass)[64]=b->const_class;
	int *bpay=b->const_payload;

	g_enums=grow_ensure(g_enums,g_enum_count,&g_enum_cap,sizeof(*g_enums));
	EnumInfo *info=&g_enums[g_enum_count++];
	memset(info,0,sizeof(*info));
	strcpy(info->name,inst);
	info->constant_count=cc;
	info->constant_cap=cc;
	info->const_name    = cc ? calloc((size_t)cc,sizeof(*info->const_name))    : NULL;
	info->const_class   = cc ? calloc((size_t)cc,sizeof(*info->const_class))   : NULL;
	info->const_args    = cc ? calloc((size_t)cc,sizeof(*info->const_args))    : NULL;
	info->const_argc    = cc ? calloc((size_t)cc,sizeof(*info->const_argc))    : NULL;
	info->const_payload = cc ? calloc((size_t)cc,sizeof(*info->const_payload)) : NULL;
	for (int i=0; i<cc; i++)
	{
		strcpy(info->const_name[i],bname[i]);
		info->const_payload[i]=bpay[i];
		if (bpay[i])
		{
			snprintf(info->const_class[i],64,"%s%s",bclass[i],suffix);   /* Wrap$Of -> Wrap$Of$int. */
		}
		else
		{
			strcpy(info->const_class[i],inst);
		}
	}
}

int enum_is_generic(const char *en)
{
	const EnumInfo *e=find_enum(en);
	return e ? (e->type_param_count>0) : 0;
}

int enum_const_is_payload(const char *en, const char *c)
{
	const EnumInfo *e=find_enum(en);
	if (!e || !e->const_payload)
	{
		return 0;
	}

	for (int i=0; i<e->constant_count; i++)
	{
		if (strcmp(e->const_name[i],c)==0)
		{
			return e->const_payload[i];
		}
	}

	return 0;
}

const char *enum_variant_class(const char *en, const char *c)
{
	const EnumInfo *e=find_enum(en);
	if (!e || !e->const_payload)
	{
		return NULL;
	}

	for (int i=0; i<e->constant_count; i++)
	{
		if (strcmp(e->const_name[i],c)==0)
		{
			return e->const_payload[i] ? e->const_class[i] : NULL;
		}
	}

	return NULL;
}

int enum_total(void)
{
	return g_enum_count;
}

const EnumInfo *enum_at(int i)
{
	return (i>=0 && i<g_enum_count) ? &g_enums[i] : NULL;
}
