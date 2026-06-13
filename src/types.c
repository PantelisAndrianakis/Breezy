#include "types.h"
#include "overload.h"
#include "grow.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

/* Append one parameter type at index idx to a dynamic param_types array. */
static void add_param_type(TypeRef **arr, int *cap, int idx, TypeRef t)
{
	*arr = grow_ensure(*arr, idx, cap, sizeof(TypeRef));
	(*arr)[idx] = t;
}

void types_init(TypeTable *tt)
{
	tt->classes = NULL;
	tt->class_count = 0;
	tt->classes_cap = 0;
	tt->funcs = NULL;
	tt->func_count = 0;
	tt->funcs_cap = 0;
	tt->interfaces = NULL;
	tt->interface_count = 0;
	tt->interfaces_cap = 0;
	tt->iface_slots = 0;
	tt->shared_container_count = 0;
}

/* Append a freshly-allocated, zeroed entry to a stable-object table: the pointer
   array grows (may realloc), but the returned object never moves, so pointers
   held elsewhere (e.g. ClassInfo.parent) stay valid. */
static ClassInfo *tt_add_class(TypeTable *tt)
{
	tt->classes = grow_ensure(tt->classes, tt->class_count, &tt->classes_cap, sizeof(ClassInfo *));
	ClassInfo *c = calloc(1, sizeof(ClassInfo));
	tt->classes[tt->class_count++] = c;
	return c;
}

static FuncInfo *tt_add_func(TypeTable *tt)
{
	tt->funcs = grow_ensure(tt->funcs, tt->func_count, &tt->funcs_cap, sizeof(FuncInfo *));
	FuncInfo *f = calloc(1, sizeof(FuncInfo));
	tt->funcs[tt->func_count++] = f;
	return f;
}

static InterfaceInfo *tt_add_interface(TypeTable *tt)
{
	tt->interfaces = grow_ensure(tt->interfaces, tt->interface_count, &tt->interfaces_cap, sizeof(InterfaceInfo *));
	InterfaceInfo *it = calloc(1, sizeof(InterfaceInfo));
	tt->interfaces[tt->interface_count++] = it;
	return it;
}

void types_register_builtins(TypeTable *tt)
{
	ClassInfo *c = tt_add_class(tt);
	memset(c, 0, sizeof(*c));
	c->fields = grow_reserve(c->fields, 1, &c->fields_cap, sizeof(FieldInfo));
	strcpy(c->name, "Exception");
	c->parent = NULL;
	strcpy(c->fields[0].name, "message");
	c->fields[0].type.kind = TY_STRING;
	c->fields[0].offset = 24;
	c->field_count = 1;
	c->object_size = 32;
	c->vtable_size = 0;
	c->method_count = 0;

	ClassInfo *o = tt_add_class(tt);
	memset(o, 0, sizeof(*o));
	o->fields = grow_reserve(o->fields, 1, &o->fields_cap, sizeof(FieldInfo));
	strcpy(o->name, "IndexOutOfBounds");
	o->parent = c;                       /* `c` is the Exception entry above. */
	strcpy(o->fields[0].name, "message");
	o->fields[0].type.kind = TY_STRING;
	o->fields[0].offset = 24;
	o->field_count = 1;
	o->object_size = 32;
	o->vtable_size = 0;
	o->method_count = 0;

	ClassInfo *io = tt_add_class(tt);
	memset(io, 0, sizeof(*io));
	io->fields = grow_reserve(io->fields, 1, &io->fields_cap, sizeof(FieldInfo));
	strcpy(io->name, "IOException");
	io->parent = c;                      /* Subclass of Exception. */
	strcpy(io->fields[0].name, "message");
	io->fields[0].type.kind = TY_STRING;
	io->fields[0].offset = 24;
	io->field_count = 1;
	io->object_size = 32;
	io->vtable_size = 0;
	io->method_count = 0;

	ClassInfo *nf = tt_add_class(tt);
	memset(nf, 0, sizeof(*nf));
	nf->fields = grow_reserve(nf->fields, 1, &nf->fields_cap, sizeof(FieldInfo));
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
		if (strcmp(tt->classes[i]->name,name)==0)
		{
			return tt->classes[i];
		}
	}

	return NULL;
}

/* ---- Maybe-shared closure -------------------------------------------------
   Decides which types may cross a core boundary. Classes in the set allocate
   with the SHARED gcinfo literal (atomic refcounts from birth); builtin
   container types in the set make codegen emit a SHARED-bit test at their op
   sites (types_typeref_maybe_shared). Share points are channel handoffs, spawn
   arguments, and static-field stores; the closure drags along everything a
   shared value of a seeded type can reach. Conservative and type-based: a
   type in the set may never actually cross, but its objects only pay the
   atomic refcount / a predicted-not-taken bit test, never the lock. */

static int typeref_same(TypeRef *a, TypeRef *b)
{
	if (!a || !b)
	{
		return a==b;
	}

	if (a->kind!=b->kind || strcmp(a->class_name,b->class_name)!=0)
	{
		return 0;
	}

	return typeref_same(a->elem,b->elem) && typeref_same(a->elem2,b->elem2);
}

static int g_shared_changed;   /* Fixpoint dirty flag for the current computation. */

static void mark_type_shared(TypeTable *tt, TypeRef *t);

static void mark_class_shared(TypeTable *tt, const char *name)
{
	ClassInfo *c=types_find_class(tt,name);
	if (c && !c->is_shared)
	{
		c->is_shared=1;
		g_shared_changed=1;
	}
}

static void add_shared_container(TypeTable *tt, TypeRef *t)
{
	for (int i=0; i<tt->shared_container_count; i++)
	{
		if (typeref_same(&tt->shared_containers[i],t))
		{
			return;
		}
	}

	tt->shared_containers=grow_ensure(tt->shared_containers,tt->shared_container_count,&tt->shared_container_cap,sizeof(*tt->shared_containers));
	tt->shared_containers[tt->shared_container_count++]=*t;   /* Struct copy; elem pointers stay owned by the AST. */
	g_shared_changed=1;
}

/* Mark one type as maybe-crossing-cores and drag everything a shared value of
   that type can reach: container element/key/value types and builtin-template
   args (List<T> stays TY_GENERIC after monomorphization). Shared classes drag
   their field types in the fixpoint below. */
static void mark_type_shared(TypeTable *tt, TypeRef *t)
{
	if (!t)
	{
		return;
	}

	switch (t->kind)
	{
	case TY_OBJECT:
		mark_class_shared(tt,t->class_name);
		break;
	case TY_ARRAY:
	case TY_MAP:
	case TY_GENERIC:
	case TY_ENTRY:
		add_shared_container(tt,t);
		mark_type_shared(tt,t->elem);
		mark_type_shared(tt,t->elem2);
		break;
	case TY_STRING:
	case TY_CHANNEL:
		break;   /* Strings are immutable leaves (runtime promotion covers refcounts); channels are shared at alloc already. */
	default:
		break;   /* Value types never carry a SHARED bit. */
	}
}

/* Seed pass over a TypeRef found in a signature: every channel element type is
   a share seed, whatever its kind. */
static void seed_shared_from_typeref(TypeTable *tt, TypeRef *t)
{
	if (!t)
	{
		return;
	}

	if (t->kind==TY_CHANNEL && t->elem)
	{
		mark_type_shared(tt,t->elem);
	}

	seed_shared_from_typeref(tt,t->elem);
	seed_shared_from_typeref(tt,t->elem2);
}

/* Seed pass over statements: a spawned function's parameters cross to another
   core as spawn args. Same nesting walk as codegen's stmt_has_try. */
static void seed_shared_from_spawns(TypeTable *tt, Block *b);

static void seed_shared_from_spawn_stmt(TypeTable *tt, Stmt *s)
{
	if (!s)
	{
		return;
	}

	if (s->kind==ST_SPAWN && s->expr)
	{
		FuncInfo *fi=types_find_func(tt,s->expr->name);
		if (fi)
		{
			for (int pi=0; pi<fi->param_count; pi++)
			{
				mark_type_shared(tt,&fi->param_types[pi]);
			}
		}
	}

	seed_shared_from_spawn_stmt(tt,s->for_init);
	seed_shared_from_spawn_stmt(tt,s->for_post);
	seed_shared_from_spawns(tt,s->then_blk);
	seed_shared_from_spawns(tt,s->else_blk);
}

static void seed_shared_from_spawns(TypeTable *tt, Block *b)
{
	if (!b)
	{
		return;
	}

	for (int i=0; i<b->count; i++)
	{
		seed_shared_from_spawn_stmt(tt,b->stmts[i]);
	}
}

void types_compute_shared_set(TypeTable *tt, Unit **units, int unit_count)
{
	g_shared_changed=0;

	/* Seeds: channel element types anywhere in a usable signature (class fields,
	   method/function/ctor parameters and returns - a channel only reaches
	   another breeze through one of these); static field types (reachable from
	   every breeze with no handoff point); spawned functions' parameter types. */
	for (int ci=0; ci<tt->class_count; ci++)
	{
		ClassInfo *c=tt->classes[ci];
		for (int fi=0; fi<c->field_count; fi++)
		{
			seed_shared_from_typeref(tt,&c->fields[fi].type);
			if (c->fields[fi].is_static)
			{
				mark_type_shared(tt,&c->fields[fi].type);
			}
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

		for (int ci=0; ci<c->ctor_count; ci++)
		{
			for (int pi=0; pi<c->ctors[ci].param_count; pi++)
			{
				seed_shared_from_typeref(tt,&c->ctors[ci].param_types[pi]);
			}
		}
	}

	for (int fi=0; fi<tt->func_count; fi++)
	{
		FuncInfo *f=tt->funcs[fi];
		seed_shared_from_typeref(tt,&f->ret_type);
		for (int pi=0; pi<f->param_count; pi++)
		{
			seed_shared_from_typeref(tt,&f->param_types[pi]);
		}
	}

	for (int ui=0; ui<unit_count; ui++)
	{
		Unit *u=units[ui];
		for (int k=0; k<u->func_count; k++)
		{
			seed_shared_from_spawns(tt,u->funcs[k]->body);
		}

		for (int ci=0; ci<u->class_count; ci++)
		{
			ClassDecl *d=u->klasses[ci];
			for (int k=0; k<d->method_count; k++)
			{
				seed_shared_from_spawns(tt,d->methods[k]->body);
			}

			for (int ci=0; ci<d->ctor_count; ci++)
			{
				seed_shared_from_spawns(tt,d->ctors[ci]->body);
			}
		}
	}

	/* Fixpoint: a shared class drags ALL its managed field types (object fields
	   as before, container fields newly - a shared object's container field
	   crosses with it), and a shared container drags its element types (inside
	   mark_type_shared). Iterate to closure. */
	do
	{
		g_shared_changed=0;
		for (int ci=0; ci<tt->class_count; ci++)
		{
			ClassInfo *c=tt->classes[ci];
			if (!c->is_shared)
			{
				continue;
			}

			for (int fi=0; fi<c->field_count; fi++)
			{
				mark_type_shared(tt,&c->fields[fi].type);
			}
		}
	}
	while (g_shared_changed);
}

int types_typeref_maybe_shared(TypeTable *tt, TypeRef *t)
{
	if (!t)
	{
		return 0;
	}

	if (t->kind==TY_OBJECT)
	{
		ClassInfo *c=types_find_class(tt,t->class_name);
		return c && c->is_shared;
	}

	for (int i=0; i<tt->shared_container_count; i++)
	{
		if (typeref_same(&tt->shared_containers[i],t))
		{
			return 1;
		}
	}

	return 0;
}

FuncInfo *types_find_func(TypeTable *tt, const char *name)
{
	for (int i=0; i<tt->func_count; i++)
	{
		if (strcmp(tt->funcs[i]->name,name)==0)
		{
			return tt->funcs[i];
		}
	}

	return NULL;
}

int types_func_overload_count(TypeTable *tt, const char *name)
{
	int n=0;
	for (int i=0; i<tt->func_count; i++)
	{
		if (strcmp(tt->funcs[i]->name,name)==0)
		{
			n++;
		}
	}

	return n;
}

FuncInfo *types_find_func_idx(TypeTable *tt, const char *name, int idx)
{
	int n=0;
	for (int i=0; i<tt->func_count; i++)
	{
		if (strcmp(tt->funcs[i]->name,name)==0)
		{
			if (n==idx)
			{
				return tt->funcs[i];
			}

			n++;
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

int types_method_overload_count(ClassInfo *c, const char *name)
{
	int n=0;
	for (int i=0; i<c->method_count; i++)
	{
		if (strcmp(c->methods[i].name,name)==0)
		{
			n++;
		}
	}

	return n;
}

MethodInfo *types_find_method_idx(ClassInfo *c, const char *name, int idx)
{
	int n=0;
	for (int i=0; i<c->method_count; i++)
	{
		if (strcmp(c->methods[i].name,name)==0)
		{
			if (n==idx)
			{
				return &c->methods[i];
			}

			n++;
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
	for (int ci=0; ci<u->class_count; ci++)
	{
		if (types_find_class(tt,u->klasses[ci]->name))
		{
			fprintf(stderr,"Class '%s' is already defined.\n",u->klasses[ci]->name);
			exit(1);
		}

		ClassInfo *c=tt_add_class(tt);
		memset(c,0,sizeof(*c));
		strcpy(c->name,u->klasses[ci]->name);
	}

	for (int i=0; i<u->func_count; i++)
	{
		FuncInfo *fi=tt_add_func(tt);
		memset(fi,0,sizeof(*fi));
		strcpy(fi->name,u->funcs[i]->name);
	}
}

InterfaceInfo *types_find_interface(TypeTable *tt, const char *name)
{
	for (int i=0; i<tt->interface_count; i++)
	{
		if (strcmp(tt->interfaces[i]->name,name)==0)
		{
			return tt->interfaces[i];
		}
	}

	return NULL;
}

int types_is_interface(TypeTable *tt, const char *name)
{
	return types_find_interface(tt,name) != NULL;
}

/* Grow all five InterfaceInfo parallel arrays in lockstep under one cap. Done
   manually (not five grow_ensure calls) because a single shared cap would
   short-circuit calls 2..5 after the first grew it, leaving those undersized. */
static void iface_reserve(InterfaceInfo *itf)
{
	if (itf->method_count < itf->method_cap)
	{
		return;
	}

	int nc = itf->method_cap ? itf->method_cap * 2 : 4;
	itf->methods      = realloc(itf->methods,      (size_t)nc * sizeof(*itf->methods));
	itf->ret_types    = realloc(itf->ret_types,    (size_t)nc * sizeof(*itf->ret_types));
	itf->param_types  = realloc(itf->param_types,  (size_t)nc * sizeof(*itf->param_types));
	itf->param_counts = realloc(itf->param_counts, (size_t)nc * sizeof(*itf->param_counts));
	itf->vslot        = realloc(itf->vslot,        (size_t)nc * sizeof(*itf->vslot));
	itf->method_cap = nc;
}

/* Append one interface method (name / return type / param count); allocates
   param_types[k] to exactly pcount entries (filled by the caller) and returns
   the index k. The caller assigns vslot[k]. */
static int iface_add_method(InterfaceInfo *itf, const char *name, TypeRef ret, int pcount)
{
	iface_reserve(itf);
	int k = itf->method_count++;
	strcpy(itf->methods[k], name);
	itf->ret_types[k] = ret;
	itf->param_counts[k] = pcount;
	itf->param_types[k] = pcount ? calloc((size_t)pcount, sizeof(TypeRef)) : NULL;
	return k;
}

/* Reserve global vtable slots 0 and 1 for the synthesized record methods, before
   any user interface claims a slot. Every vtable then has slot 0 = hashCode and
   slot 1 = equals (zero/gap for non-records), so the runtime can hardcode those
   offsets for key_kind==3 keys. Must run once, before types_register_interfaces. */
void types_reserve_hashable(TypeTable *tt)
{
	InterfaceInfo *itf=tt_add_interface(tt);
	memset(itf,0,sizeof(*itf));
	strcpy(itf->name,"__Hashable");
	int hc=iface_add_method(itf,"hashCode",(TypeRef){.kind=TY_INT},0);
	itf->vslot[hc]=tt->iface_slots++;   /* Global slot 0. */
	int eq=iface_add_method(itf,"equals",(TypeRef){.kind=TY_BOOL},1);   /* param type set per record. */
	itf->vslot[eq]=tt->iface_slots++;   /* Global slot 1. */
}

/* Register each interface, assigning every method a global vtable slot in [0..K).
   Must run for all units before any class members (so K = tt->iface_slots is final
   before vtable-slot assignment shifts class methods above it). */
void types_register_interfaces(TypeTable *tt, Unit *u)
{
	for (int i=0; i<u->interface_count; i++)
	{
		InterfaceDecl *d=u->interfaces[i];
		InterfaceInfo *itf=tt_add_interface(tt);
		memset(itf,0,sizeof(*itf));
		strcpy(itf->name,d->name);
		for (int k=0; k<d->method_count; k++)
		{
			Func *m=d->methods[k];
			int idx=iface_add_method(itf,m->name,m->ret_type,m->param_count);
			for (int p=0; p<m->param_count; p++)
			{
				itf->param_types[idx][p]=m->params[p].type;
			}

			itf->vslot[idx]=tt->iface_slots++;   /* Global reserved slot. */
		}
	}
}

/* The reserved global vtable slot for method `name` if `c` implements an interface
   that declares it; -1 otherwise. */
static int iface_slot_for(TypeTable *tt, ClassInfo *c, const char *name)
{
	for (int ii=0; ii<c->implements_count; ii++)
	{
		InterfaceInfo *itf=types_find_interface(tt,c->implements[ii]);
		for (int k=0; itf && k<itf->method_count; k++)
		{
			if (strcmp(itf->methods[k],name)==0)
			{
				return itf->vslot[k];
			}
		}
	}

	return -1;
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

static void link_unit_class(TypeTable *tt, ClassDecl *d);

/* The first same-name FuncInfo not yet bound to an AST. Overloaded functions
   share a name, so binding by first-match would collapse them; binding to an
   unfilled slot distributes each declaration to a distinct FuncInfo. */
static FuncInfo *find_unfilled_func(TypeTable *tt, const char *name)
{
	for (int i=0; i<tt->func_count; i++)
	{
		if (strcmp(tt->funcs[i]->name,name)==0 && tt->funcs[i]->ast==NULL)
		{
			return tt->funcs[i];
		}
	}

	return NULL;
}

static void register_unit_funcs(TypeTable *tt, Unit *u)
{
	for (int i=0; i<u->func_count; i++)
	{
		Func *f=u->funcs[i];
		FuncInfo *fi=find_unfilled_func(tt,f->name);
		fi->ast=f;
		fi->ret_type=f->ret_type;
		fi->param_count=f->param_count;
		for (int k=0; k<f->param_count; k++)
		{
			add_param_type(&fi->param_types,&fi->param_types_cap,k,f->params[k].type);
		}

		if (f->is_extern)
		{
			/* Raw C symbol, no bzy_ prefix. The `$` escapes NASM reserved words
			   (e.g. `abs`); NASM strips it, so the linker sees the bare symbol. */
			snprintf(fi->asm_label,sizeof(fi->asm_label),"$%s",f->name);
			fi->is_extern=1;
			fi->is_blocking=f->is_blocking;
			fi->is_variadic=f->is_variadic;
		}
		else
		{
			snprintf(fi->asm_label,sizeof(fi->asm_label),"bzy_%s",f->name);
		}
	}
}

/* Register every unit's members with classes processed PARENT-FIRST, whatever
   the declaration or file order: a subclass copies its parent's fields,
   methods, vtable size, and object size, so registering it against a name-only
   parent truncates its vtable and misnumbers its slots (the call sites later
   compute slots from the complete view - a guaranteed crash). File order is
   filesystem-dependent (ext4 returns hash order, NTFS alphabetical), so it can
   never be load-bearing. Builtins and other ClassInfos with no ClassDecl are
   already complete and count as done; a stalled pass means an inheritance
   cycle. */
void types_register_all_members(TypeTable *tt, Unit **units, int unit_count)
{
	for (int i=0; i<unit_count; i++)
	{
		register_unit_funcs(tt,units[i]);
	}

	/* Free functions: validate overload sets and mangle on demand. A lone name
	   keeps its legacy label (bzy_<name> / $<name>); a name with multiple
	   declarations gains a type-signature suffix. extern functions and main may
	   not be overloaded. Each name is processed once, at its first occurrence. */
	for (int i=0; i<tt->func_count; i++)
	{
		const char *nm=tt->funcs[i]->name;
		int seen_earlier=0;
		for (int j=0; j<i; j++)
		{
			if (strcmp(tt->funcs[j]->name,nm)==0)
			{
				seen_earlier=1;
				break;
			}
		}

		if (seen_earlier)
		{
			continue;
		}

		int cnt=types_func_overload_count(tt,nm);
		if (cnt<=1)
		{
			continue;   /* Lone function: keep its legacy label. */
		}

		if (strcmp(nm,"main")==0)
		{
			fprintf(stderr,"main cannot be overloaded.\n");
			exit(1);
		}

		OverloadCand cand[16];
		int nc=0;
		for (int oi=0; oi<cnt && oi<16; oi++)
		{
			FuncInfo *fi=types_find_func_idx(tt,nm,oi);
			if (fi->is_extern)
			{
				fprintf(stderr,"extern function '%s' cannot be overloaded.\n",nm);
				exit(1);
			}

			char sa[160];
			overload_encode_types(sa,sizeof(sa),fi->param_types,fi->param_count);
			for (int oj=0; oj<oi; oj++)
			{
				FuncInfo *fj=types_find_func_idx(tt,nm,oj);
				char sb[160];
				overload_encode_types(sb,sizeof(sb),fj->param_types,fj->param_count);
				if (strcmp(sa,sb)==0)
				{
					fprintf(stderr,"Duplicate function signature for '%s'.\n",nm);
					exit(1);
				}
			}

			cand[nc].param_types=fi->param_types;
			cand[nc].param_count=fi->param_count;
			cand[nc].min_args=fi->ast ? overload_min_args(fi->ast) : fi->param_count;
			nc++;
		}

		if (overload_set_is_ambiguous(cand,nc))
		{
			fprintf(stderr,"Ambiguous function overload set '%s'.\n",nm);
			exit(1);
		}

		for (int oi=0; oi<cnt; oi++)
		{
			FuncInfo *fi=types_find_func_idx(tt,nm,oi);
			char sig[140];
			overload_encode_types(sig,sizeof(sig),fi->param_types,fi->param_count);
			snprintf(fi->asm_label,sizeof(fi->asm_label),"bzy_%s__%s",nm,sig);
		}
	}

	/* Everything starts done (builtins, enum classes); user-declared classes pend. */
	for (int ci=0; ci<tt->class_count; ci++)
	{
		tt->classes[ci]->members_done=1;
	}

	int pending=0;
	for (int i=0; i<unit_count; i++)
	{
		for (int ci=0; ci<units[i]->class_count; ci++)
		{
			ClassInfo *c=types_find_class(tt,units[i]->klasses[ci]->name);
			if (c)
			{
				c->members_done=0;
				pending++;
			}
		}
	}

	while (pending>0)
	{
		int progressed=0;
		for (int i=0; i<unit_count; i++)
		{
			for (int ci=0; ci<units[i]->class_count; ci++)
			{
				ClassDecl *d=units[i]->klasses[ci];
				ClassInfo *c=types_find_class(tt,d->name);
				if (!c || c->members_done)
				{
					continue;
				}

				if (d->has_parent)
				{
					ClassInfo *p=types_find_class(tt,d->parent_name);
					if (p && !p->members_done)
					{
						continue;   /* Parent still pending: defer to a later pass. */
					}
				}

				link_unit_class(tt,d);
				c->members_done=1;
				pending--;
				progressed=1;
			}
		}

		if (!progressed)
		{
			for (int i=0; i<unit_count; i++)
			{
				for (int ci=0; ci<units[i]->class_count; ci++)
				{
					ClassInfo *c=types_find_class(tt,units[i]->klasses[ci]->name);
					if (c && !c->members_done)
					{
						fprintf(stderr,"Inheritance cycle involving class %s.\n",c->name);
						exit(1);
					}
				}
			}

			break;   /* Unreachable: no progress implies a pending class above. */
		}
	}
}

static void link_unit_class(TypeTable *tt, ClassDecl *d)
{
	ClassInfo *c=types_find_class(tt,d->name);
	c->is_static=d->is_static;
	c->is_record=d->is_record;
	if (d->is_record && d->has_parent)
	{
		fprintf(stderr,"A record may not extend another type: %s.\n",d->name);
		exit(1);
	}
	if (d->is_static)
	{
		if (d->ctor)
		{
			fprintf(stderr,"Static class %s may not declare a constructor.\n",d->name);
			exit(1);
		}
		if (d->has_parent)
		{
			fprintf(stderr,"Static class %s may not extend a class.\n",d->name);
			exit(1);
		}
		if (d->implements_count)
		{
			fprintf(stderr,"Static class %s may not implement interfaces.\n",d->name);
			exit(1);
		}
		if (d->type_param_count)
		{
			fprintf(stderr,"Static class %s may not be generic.\n",d->name);
			exit(1);
		}
	}

	link_parent(tt,c,d);
	if (c->parent && c->parent->is_record)
	{
		fprintf(stderr,"Cannot extend record %s; records are final.\n",c->parent->name);
		exit(1);
	}

	c->implements=grow_reserve(c->implements,d->implements_count,&c->implements_cap,sizeof(*c->implements));
	c->implements_count=d->implements_count;
	for (int i=0; i<d->implements_count; i++)
	{
		strcpy(c->implements[i],d->implements[i]);
	}

	/* Pre-size the per-class arrays once (inherited entries + this class's own,
	   + 2 methods for a record's synthesized hashCode/equals). The registration
	   loops below then never realloc, so the MethodInfo* pointers they hold for
	   override detection stay valid. */
	int pfields=(c->parent?c->parent->field_count:0)+d->field_count;
	int pmethods=(c->parent?c->parent->method_count:0)+d->method_count+2;
	c->fields=grow_reserve(c->fields,pfields,&c->fields_cap,sizeof(FieldInfo));
	c->methods=grow_reserve(c->methods,pmethods,&c->methods_cap,sizeof(MethodInfo));
	if (d->ctor_count>0)
	{
		c->ctors=grow_reserve(c->ctors,d->ctor_count,&c->ctors_cap,sizeof(MethodInfo));
	}

	if (c->parent)
	{
		c->field_count=c->parent->field_count;
		memcpy(c->fields,c->parent->fields,sizeof(FieldInfo)*c->field_count);
		c->method_count=c->parent->method_count;
		memcpy(c->methods,c->parent->methods,sizeof(MethodInfo)*c->method_count);
		/* The memcpy aliased each inherited method's param_types pointer onto the
		   parent's buffer; give every inherited method its own copy so a later
		   override or grow never corrupts the parent. */
		for (int mi=0; mi<c->method_count; mi++)
		{
			MethodInfo *src=&c->parent->methods[mi];
			MethodInfo *dst=&c->methods[mi];
			dst->param_types=NULL;
			dst->param_types_cap=0;
			for (int p=0; p<src->param_count; p++)
			{
				add_param_type(&dst->param_types,&dst->param_types_cap,p,src->param_types[p]);
			}
		}
		c->vtable_size=c->parent->vtable_size;
	}
	else
	{
		c->vtable_size=tt->iface_slots;   /* Reserve [0..K) for interface methods; own methods follow. */
	}

	/* Instance fields take sequential 8-byte slots from the parent's object_size;
	   static fields are global slots (offset -1) and excluded from the layout. */
	int next_off = c->parent ? c->parent->object_size : 24;
	for (int i=0; i<d->field_count; i++)
	{
		FieldInfo *fi=&c->fields[c->field_count++];
		memset(fi,0,sizeof(*fi));
		strcpy(fi->name,d->fields[i].name);
		fi->type=d->fields[i].type;
		fi->is_static = d->fields[i].is_static || d->is_static;
		if (fi->is_static)
		{
			fi->offset=-1;
		}
		else
		{
			fi->offset=next_off;
			next_off+=8;
		}
	}

	c->object_size = next_off;
	for (int i=0; i<d->method_count; i++)
	{
		Func *m=d->methods[i];
		int mstatic = m->is_static || d->is_static;

		/* Encode this method's parameter signature for override/overload matching. */
		TypeRef mpts[8];
		for (int k=0; k<m->param_count && k<8; k++)
		{
			mpts[k]=m->params[k].type;
		}

		char msig[160];
		overload_encode_types(msig,sizeof(msig),mpts,m->param_count);

		/* An inherited method with the SAME name AND signature is overridden
		   (reuse its vtable slot). A same name+signature entry already owned by
		   this class is a true duplicate. A different signature is a new overload
		   (fresh slot). Static methods never override. */
		MethodInfo *existing=NULL;
		for (int q=0; q<c->method_count; q++)
		{
			if (strcmp(c->methods[q].name,m->name)!=0)
			{
				continue;
			}

			char qsig[160];
			overload_encode_types(qsig,sizeof(qsig),c->methods[q].param_types,c->methods[q].param_count);
			if (strcmp(qsig,msig)!=0)
			{
				continue;
			}

			if (strcmp(c->methods[q].owner_class,c->name)==0)
			{
				fprintf(stderr,"Class %s: duplicate method '%s' with the same signature.\n",c->name,m->name);
				exit(1);
			}

			if (!mstatic)
			{
				existing=&c->methods[q];   /* Override of an inherited method. */
			}

			break;
		}

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
			if (mstatic)
			{
				mi->vtable_slot=-1;   /* No virtual dispatch. */
			}
			else
			{
				int islot=iface_slot_for(tt,c,m->name);   /* Interface method -> its reserved global slot. */
				mi->vtable_slot=(islot>=0) ? islot : c->vtable_size++;
			}
		}

		mi->is_static=mstatic;
		strcpy(mi->owner_class,c->name);
		mi->ast=m;
		mi->ret_type=m->ret_type;
		mi->param_count=m->param_count;
		for (int k=0; k<m->param_count; k++)
		{
			add_param_type(&mi->param_types,&mi->param_types_cap,k,m->params[k].type);
		}
	}

	/* Assign method asm labels on demand: a name with >1 overload in this class
	   gets a type-signature suffix; a lone name keeps the legacy label. Inherited
	   (not-overridden) entries keep the parent's label and slot. */
	for (int i=0; i<c->method_count; i++)
	{
		MethodInfo *mi=&c->methods[i];
		if (strcmp(mi->owner_class,c->name)!=0)
		{
			continue;
		}

		if (types_method_overload_count(c,mi->name)>1)
		{
			char sig[140];
			overload_encode_types(sig,sizeof(sig),mi->param_types,mi->param_count);
			if (mi->is_static)
			{
				snprintf(mi->asm_label,sizeof(mi->asm_label),"__static_%s__%s__%s",c->name,mi->name,sig);
			}
			else
			{
				snprintf(mi->asm_label,sizeof(mi->asm_label),"%s__%s__%s",c->name,mi->name,sig);
			}
		}
		else if (mi->is_static)
		{
			snprintf(mi->asm_label,sizeof(mi->asm_label),"__static_%s__%s",c->name,mi->name);
		}
		else
		{
			snprintf(mi->asm_label,sizeof(mi->asm_label),"%s__%s",c->name,mi->name);
		}
	}

	/* Reject ambiguous method overload sets at declaration time. Check each
	   own-declared name once, at its first occurrence. */
	for (int i=0; i<c->method_count; i++)
	{
		if (strcmp(c->methods[i].owner_class,c->name)!=0)
		{
			continue;
		}

		const char *nm=c->methods[i].name;
		int seen_earlier=0;
		for (int j=0; j<i; j++)
		{
			if (strcmp(c->methods[j].name,nm)==0)
			{
				seen_earlier=1;
				break;
			}
		}

		if (seen_earlier || types_method_overload_count(c,nm)<=1)
		{
			continue;
		}

		OverloadCand cand[16];
		int nc=0;
		int oc=types_method_overload_count(c,nm);
		for (int oi=0; oi<oc && nc<16; oi++)
		{
			MethodInfo *mi=types_find_method_idx(c,nm,oi);
			cand[nc].param_types=mi->param_types;
			cand[nc].param_count=mi->param_count;
			cand[nc].min_args=mi->ast ? overload_min_args(mi->ast) : mi->param_count;
			nc++;
		}

		if (overload_set_is_ambiguous(cand,nc))
		{
			fprintf(stderr,"Class %s: ambiguous method overload set '%s'.\n",c->name,nm);
			exit(1);
		}
	}

	if (c->is_record)
	{
		/* Synthesize hashCode (slot 0) and equals (slot 1) over __Hashable's reserved
		   slots. ast=NULL: codegen emits the bodies directly (cg_emit_record_methods),
		   but the MethodInfo makes them resolvable/callable and vtable-placed. */
		InterfaceInfo *h=types_find_interface(tt,"__Hashable");
		MethodInfo *hc=&c->methods[c->method_count++];
		memset(hc,0,sizeof(*hc));
		strcpy(hc->name,"hashCode");
		hc->vtable_slot=h->vslot[0];
		strcpy(hc->owner_class,c->name);
		snprintf(hc->asm_label,sizeof(hc->asm_label),"__rec_hashCode_%s",c->name);
		hc->ast=NULL;
		hc->ret_type=(TypeRef){.kind=TY_INT};
		hc->param_count=0;
		MethodInfo *eq=&c->methods[c->method_count++];
		memset(eq,0,sizeof(*eq));
		strcpy(eq->name,"equals");
		eq->vtable_slot=h->vslot[1];
		strcpy(eq->owner_class,c->name);
		snprintf(eq->asm_label,sizeof(eq->asm_label),"__rec_equals_%s",c->name);
		eq->ast=NULL;
		eq->ret_type=(TypeRef){.kind=TY_BOOL};
		eq->param_count=1;
		add_param_type(&eq->param_types,&eq->param_types_cap,0,(TypeRef){.kind=TY_OBJECT});
		strcpy(eq->param_types[0].class_name,c->name);
	}

	c->ctor_count=d->ctor_count;
	c->has_ctor=(d->ctor_count>0);
	for (int ci=0; ci<d->ctor_count; ci++)
	{
		Func *cf=d->ctors[ci];
		MethodInfo *mi=&c->ctors[ci];
		memset(mi,0,sizeof(*mi));
		strcpy(mi->name,c->name);
		mi->vtable_slot=-1;              /* Constructors are never virtual. */
		strcpy(mi->owner_class,c->name);
		mi->ast=cf;
		mi->ret_type=(TypeRef){.kind=TY_VOID};
		mi->param_count=cf->param_count;
		for (int k=0; k<cf->param_count; k++)
		{
			add_param_type(&mi->param_types,&mi->param_types_cap,k,cf->params[k].type);
		}
	}

	/* Reject identical-signature duplicate constructors. */
	for (int a=0; a<c->ctor_count; a++)
	{
		char sa[160];
		overload_encode_types(sa,sizeof(sa),c->ctors[a].param_types,c->ctors[a].param_count);
		for (int b=a+1; b<c->ctor_count; b++)
		{
			char sb[160];
			overload_encode_types(sb,sizeof(sb),c->ctors[b].param_types,c->ctors[b].param_count);
			if (strcmp(sa,sb)==0)
			{
				fprintf(stderr,"Class %s: duplicate constructor signature.\n",c->name);
				exit(1);
			}
		}
	}

	/* Reject sets that are ambiguous by construction (definition-time). */
	if (c->ctor_count>1)
	{
		OverloadCand cc[8];
		for (int ci=0; ci<c->ctor_count; ci++)
		{
			cc[ci].param_types=c->ctors[ci].param_types;
			cc[ci].param_count=c->ctors[ci].param_count;
			cc[ci].min_args=overload_min_args(c->ctors[ci].ast);
		}
		if (overload_set_is_ambiguous(cc,c->ctor_count))
		{
			fprintf(stderr,"Class %s: ambiguous constructor overload set.\n",c->name);
			exit(1);
		}
	}

	/* Mangle on demand: a lone constructor keeps the legacy label. */
	for (int ci=0; ci<c->ctor_count; ci++)
	{
		if (c->ctor_count==1)
		{
			snprintf(c->ctors[ci].asm_label,sizeof(c->ctors[ci].asm_label),"__ctor_%s",c->name);
		}
		else
		{
			char sig[140];
			overload_encode_types(sig,sizeof(sig),c->ctors[ci].param_types,c->ctors[ci].param_count);
			snprintf(c->ctors[ci].asm_label,sizeof(c->ctors[ci].asm_label),"__ctor_%s__%s",c->name,sig);
		}
	}

	/* Verify every implemented interface is fully + correctly satisfied. */
	for (int ii=0; ii<c->implements_count; ii++)
	{
		InterfaceInfo *itf=types_find_interface(tt,c->implements[ii]);
		if (!itf)
		{
			fprintf(stderr,"Class %s: unknown interface '%s'.\n",c->name,c->implements[ii]);
			exit(1);
		}

		for (int k=0; k<itf->method_count; k++)
		{
			MethodInfo *mi=types_find_method(c,itf->methods[k]);
			int ok = mi && mi->ret_type.kind==itf->ret_types[k].kind
					 && mi->param_count==itf->param_counts[k];
			for (int p=0; ok && p<mi->param_count; p++)
			{
				if (mi->param_types[p].kind != itf->param_types[k][p].kind)
				{
					ok=0;
				}
			}

			if (!ok)
			{
				fprintf(stderr,"Class %s does not satisfy interface %s: method '%s' missing or signature mismatch.\n",
						c->name,itf->name,itf->methods[k]);
				exit(1);
			}
		}
	}
}
