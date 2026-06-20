#include "symtable.h"
#include "grow.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

void sym_init(SymTable *st)
{
	st->syms=NULL;
	st->count=0;
	st->cap=0;
	st->next_offset=0;
}

Symbol *sym_add(SymTable *st, const char *name, TypeRef type)
{
	st->syms=grow_ensure(st->syms,st->count,&st->cap,sizeof(Symbol));
	Symbol *s=&st->syms[st->count++];
	strncpy(s->name,name,sizeof(s->name)-1);
	s->name[sizeof(s->name)-1]='\0';
	s->type=type;
	st->next_offset+=ty_is_simd(type.kind)?ty_simd_bytes(type.kind):8;   /* A packed SIMD value needs a 16- or 32-byte slot. */
	s->offset=st->next_offset;
	return s;
}

Symbol *sym_find(SymTable *st, const char *name)
{
	for (int i=st->count-1; i>=0; i--)
	{
		if (strcmp(st->syms[i].name,name)==0)
		{
			return &st->syms[i];
		}
	}

	return NULL;
}

int sym_frame_size(SymTable *st)
{
	int s=st->next_offset;
	if (s%16!=0)
	{
		s=(s/16+1)*16;
	}

	return s;
}
