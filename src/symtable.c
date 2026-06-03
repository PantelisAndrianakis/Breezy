#include "symtable.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

void sym_init(SymTable *st)
{
	st->count=0;
	st->next_offset=0;
}

Symbol *sym_add(SymTable *st, const char *name, TypeRef type)
{
	if (st->count>=MAX_SYMS)
	{
		fprintf(stderr,"Too many locals.\n");
		exit(1);
	}

	Symbol *s=&st->syms[st->count++];
	strncpy(s->name,name,sizeof(s->name)-1);
	s->name[sizeof(s->name)-1]='\0';
	s->type=type;
	st->next_offset+=8;
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
