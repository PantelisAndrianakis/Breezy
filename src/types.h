#ifndef TYPES_H
#define TYPES_H
#include "ast.h"

#define MAX_CLASSES 128
#define MAX_FUNCS   128

typedef struct
{
	char name[64];
	TypeRef type;
	int offset;
} FieldInfo;
typedef struct
{
	char name[64];
	int vtable_slot;
	char asm_label[160];
	char owner_class[64];
	Func *ast;
	TypeRef ret_type;
	int param_count;
	TypeRef param_types[8];
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
} ClassInfo;
typedef struct
{
	char name[64];
	char asm_label[160];
	Func *ast;
	TypeRef ret_type;
	int param_count;
	TypeRef param_types[8];
} FuncInfo;
typedef struct
{
	ClassInfo classes[MAX_CLASSES];
	int class_count;
	FuncInfo  funcs[MAX_FUNCS];
	int func_count;
} TypeTable;

void       types_init(TypeTable *tt);
void       types_register_unit_names(TypeTable *tt, Unit *u);
void       types_register_unit_members(TypeTable *tt, Unit *u);
ClassInfo *types_find_class(TypeTable *tt, const char *name);
FuncInfo  *types_find_func(TypeTable *tt, const char *name);
MethodInfo*types_find_method(ClassInfo *c, const char *name);
FieldInfo *types_find_field(ClassInfo *c, const char *name);
#endif
