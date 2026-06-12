#ifndef TYPES_H
#define TYPES_H
#include "ast.h"

#define MAX_CLASSES 128
#define MAX_FUNCS   128

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
	TypeRef param_types[8];
	int is_static;    /* Static method: no `this`, called via the class name. */
} MethodInfo;
typedef struct ClassInfo
{
	char name[64];
	struct ClassInfo *parent;
	FieldInfo fields[64];
	int field_count;
	MethodInfo methods[64];
	int method_count;
	int vtable_size;
	int object_size;
	int has_ctor;                  /* 1 if the class declares >= 1 constructor. */
	MethodInfo ctors[8];           /* Constructor overload set (ast/param_types/asm_label per entry). */
	int ctor_count;
	int is_shared;                 /* 1 if instances may cross a core boundary -> atomic refcounts (6a-3). */
	char implements[8][64];        /* Interface names this class implements. */
	int implements_count;
	int is_static;                 /* `static class`: not instantiable; all members static. */
	int is_record;                 /* `record`: final class with synthesized hashCode/equals. */
	int members_done;              /* Registration fixpoint: fields/methods/vtable complete (parents first). */
} ClassInfo;
typedef struct
{
	char    name[64];
	char    methods[16][64];       /* Method names, declaration order. */
	TypeRef ret_types[16];
	TypeRef param_types[16][8];
	int     param_counts[16];
	int     vslot[16];             /* The global vtable slot of each interface method. */
	int     method_count;
} InterfaceInfo;
typedef struct
{
	char name[64];
	char asm_label[160];
	Func *ast;
	TypeRef ret_type;
	int param_count;
	TypeRef param_types[8];
	int is_extern;            /* FFI: asm_label is the raw C symbol; no body emitted. */
	int is_blocking;          /* FFI: dispatch via the offload pool (parks the breeze). */
} FuncInfo;
typedef struct
{
	ClassInfo classes[MAX_CLASSES];
	int class_count;
	FuncInfo  funcs[MAX_FUNCS];
	int func_count;
	InterfaceInfo interfaces[64];
	int interface_count;
	int iface_slots;               /* K: total interface methods = reserved vtable slots [0..K). */
	TypeRef shared_containers[256];   /* Builtin container types (array/map/List/...) that may cross cores. */
	int     shared_container_count;
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
MethodInfo*types_find_method(ClassInfo *c, const char *name);
FieldInfo *types_find_field(ClassInfo *c, const char *name);
#endif
