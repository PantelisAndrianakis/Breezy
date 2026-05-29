#ifndef SYMTABLE_H
#define SYMTABLE_H
#include "ast.h"
#define MAX_SYMS 256

typedef struct
{
	char name[64];
	TypeRef type;
	int offset;
} Symbol;
typedef struct
{
	Symbol syms[MAX_SYMS];
	int count;
	int next_offset;
} SymTable;

void    sym_init(SymTable *st);
Symbol *sym_add(SymTable *st, const char *name, TypeRef type);
Symbol *sym_find(SymTable *st, const char *name);
int     sym_frame_size(SymTable *st);
#endif
