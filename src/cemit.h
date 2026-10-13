#ifndef CEMIT_H
#define CEMIT_H
#include "ast.h"

/* The C route: pure integer/bool/double code (arrays of those, plain control
   flow, integer switch, no calls, no objects) translated to C so gcc can
   optimize it. Every accepted unit is appended to one in-memory translation
   unit that the driver writes to out_c.c. */

/* Translates loop statement s into `void <sym>(char *fp)`, where fp is the
   enclosing function's rbp. Locals cross through their frame slots. Returns 1
   and writes the symbol (at most 63 chars) into sym, or 0 when s uses a
   construct the C route does not handle; a rejected unit leaves no text. */
int cemit_region(const Stmt *s, char *sym);

/* Translates free function f, linked under the symbol `label`. Only functions
   that can never throw are accepted (every array index proven in bounds). */
int cemit_func(const Func *f, const char *label);

/* The accumulated C source, or NULL when no unit was emitted. */
const char *cemit_source(void);

/* Discards every accumulated unit. */
void cemit_reset(void);

#endif
