#ifndef SYMTABLE_H
#define SYMTABLE_H
#include "ast.h"

typedef struct
{
	char name[64];
	TypeRef type;
	int offset;
} Symbol;
typedef struct
{
	Symbol *syms;     /* Grown via grow_ensure; count live, cap allocated. */
	int count;
	int cap;
	int next_offset;
} SymTable;

void    sym_init(SymTable *st);
Symbol *sym_add(SymTable *st, const char *name, TypeRef type);
Symbol *sym_find(SymTable *st, const char *name);
int     sym_frame_size(SymTable *st);
#endif
