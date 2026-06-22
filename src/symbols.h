#ifndef BREEZY_SYMBOLS_H
#define BREEZY_SYMBOLS_H

#include "ast.h"

/* Emit the LSP symbol index for the resolved program as one JSON object on stdout:
   {"symbols":[{file,line,col,endCol,name,kind,type}, ...]}. One entry per named
   occurrence (identifier / field / call / method-call / new) carrying its 1-based
   span and its resolved type -- the data the language server answers hover from.
   Run after resolve_program (the `--symbols` front-end mode), like --check. */
void symbols_emit(Unit **units, int unit_count);

#endif
