#include "codegen.h"
#include "symtable.h"
#include <stdarg.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

void cg_init(Codegen *cg, FILE *out)
{
	cg->out=out;
	cg->label_count=0;
}
void cg_emit(Codegen *cg, const char *fmt, ...)
{
	va_list ap;
	va_start(ap,fmt);
	vfprintf(cg->out,fmt,ap);
	va_end(ap);
	fputc('\n',cg->out);
}
int  cg_label(Codegen *cg)
{
	return cg->label_count++;
}

void cg_program(Codegen *cg, TypeTable *tt, Unit **units, int unit_count)
{
	(void)cg;    /* replaced in Task 12 */
	(void)tt;
	(void)units;
	(void)unit_count;
}
