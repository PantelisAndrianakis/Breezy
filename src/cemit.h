#ifndef CEMIT_H
#define CEMIT_H
#include "ast.h"

/**
 * The C route: pure integer/bool/double code (arrays of those, plain control flow, integer switch, no calls, no objects) translated to C so gcc can optimize it.
 * Every accepted unit is appended to one in-memory translation unit that the driver writes to out_c.c.
 */

/**
 * Translates loop statement s into `void <sym>(char *fp)`, where fp is the enclosing function's rbp.
 * Locals cross through their frame slots.
 * Returns 1 and writes the symbol (at most 63 chars) into sym and the frame offsets of the locals the unit touches into offs (*noffs of them, at most cap), or 0 when s uses a construct the C route does not handle or touches more than cap locals; a rejected unit leaves no text. outermost is 1 when s sits in no enclosing loop, so locals declared inside it are not written back.
 */
int cemit_region(const Func *f, const Stmt *s, int outermost, char *sym, int *offs, int cap, int *noffs);

/**
 * Translates free function f, linked under the symbol `label`.
 * Only functions that can never throw are accepted (every array index proven in bounds).
 */
int cemit_func(const Func *f, const char *label);

/* The accumulated C source, or NULL when no unit was emitted. */
const char *cemit_source(void);

/* Discards every accumulated unit. */
void cemit_reset(void);

#endif
