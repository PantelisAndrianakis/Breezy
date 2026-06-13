#ifndef TYPES_H
#define TYPES_H
#include "ast.h"

typedef struct
{
	char name[64];
	TypeRef type;
	int offset;       /* Instance field byte offset; -1 for a static field (global slot). */
	int is_static;    /* Static field: one shared global slot, not in the object. */
} FieldInfo;
typedef struct
{
	char name[64];
	int vtable_slot;  /* -1 for a static method (no virtual dispatch). */
	char asm_label[160];
	char owner_class[64];
	Func *ast;
	TypeRef ret_type;
	int param_count;
	TypeRef *param_types;          /* Grown to param_count; param_types_cap allocated. */
	int param_types_cap;
	int is_static;    /* Static method: no `this`, called via the class name. */
} MethodInfo;
typedef struct ClassInfo
{
	char name[64];
	struct ClassInfo *parent;
	FieldInfo *fields;             /* Pre-sized in register_class_members; field_count live. */
	int field_count;
	int fields_cap;
	MethodInfo *methods;
	int method_count;
	int methods_cap;
	int vtable_size;
	int object_size;
	int has_ctor;                  /* 1 if the class declares >= 1 constructor. */
	MethodInfo *ctors;             /* Constructor overload set (ast/param_types/asm_label per entry). */
	int ctor_count;
	int ctors_cap;
	int is_shared;                 /* 1 if instances may cross a core boundary -> atomic refcounts (6a-3). */
	char (*implements)[64];        /* Interface names this class implements. */
	int implements_count;
	int implements_cap;
	int is_static;                 /* `static class`: not instantiable; all members static. */
	int is_record;                 /* `record`: final class with synthesized hashCode/equals. */
	int members_done;              /* Registration fixpoint: fields/methods/vtable complete (parents first). */
} ClassInfo;
typedef struct
{
	char     name[64];
	char   (*methods)[64];   /* [method] name, declaration order. */
	TypeRef  *ret_types;     /* [method] return type. */
	TypeRef **param_types;   /* [method] -> array of exactly param_counts[m] param types. */
	int      *param_counts;  /* [method] param count. */
	int      *vslot;         /* [method] global vtable slot. */
	int       method_count;
	int       method_cap;    /* One cap for all five parallel arrays (grown in lockstep). */
} InterfaceInfo;
typedef struct
{
	char name[64];
	char asm_label[160];
	Func *ast;
	TypeRef ret_type;
	int param_count;
	TypeRef *param_types;          /* Grown to param_count; param_types_cap allocated. */
	int param_types_cap;
	int is_extern;            /* FFI: asm_label is the raw C symbol; no body emitted. */
	int is_blocking;          /* FFI: dispatch via the offload pool (parks the breeze). */
	int is_variadic;          /* FFI: trailing `...` — a variadic C function. */
} FuncInfo;
typedef struct
{
	ClassInfo **classes;           /* Stable heap objects: the pointer array grows, the objects never move. */
	int class_count;
	int classes_cap;
	FuncInfo  **funcs;
	int func_count;
	int funcs_cap;
	InterfaceInfo **interfaces;
	int interface_count;
	int interfaces_cap;
	int iface_slots;               /* K: total interface methods = reserved vtable slots [0..K). */
	TypeRef *shared_containers;        /* Builtin container types (array/map/List/...) that may cross cores. */
	int     shared_container_count;
	int     shared_container_cap;
} TypeTable;

void       types_init(TypeTable *tt);
void       types_register_builtins(TypeTable *tt);
void       types_reserve_hashable(TypeTable *tt);              /* Before interfaces: reserves slots 0/1 for record hashCode/equals. */
void       types_register_interfaces(TypeTable *tt, Unit *u);   /* Before members: reserves slots [0..K). */
void       types_register_unit_names(TypeTable *tt, Unit *u);
void       types_register_all_members(TypeTable *tt, Unit **units, int unit_count);   /* Classes parent-first, whatever the file/declaration order. */
InterfaceInfo *types_find_interface(TypeTable *tt, const char *name);
int        types_is_interface(TypeTable *tt, const char *name);
void       types_compute_shared_set(TypeTable *tt, Unit **units, int unit_count);   /* Conservative-static: mark cross-core-reachable types shared (channels, spawn params, statics). */
int        types_typeref_maybe_shared(TypeTable *tt, TypeRef *t);   /* 1 if a value of this static type may be SHARED at runtime (codegen gating). */
ClassInfo *types_find_class(TypeTable *tt, const char *name);
FuncInfo  *types_find_func(TypeTable *tt, const char *name);
int        types_func_overload_count(TypeTable *tt, const char *name);     /* Same-name free functions. */
FuncInfo  *types_find_func_idx(TypeTable *tt, const char *name, int idx);   /* The idx-th same-name overload, declaration order. */
MethodInfo*types_find_method(ClassInfo *c, const char *name);
int        types_method_overload_count(ClassInfo *c, const char *name);   /* Same-name entries (inherited + own). */
MethodInfo*types_find_method_idx(ClassInfo *c, const char *name, int idx); /* The idx-th same-name overload, declaration order. */
FieldInfo *types_find_field(ClassInfo *c, const char *name);
#endif
