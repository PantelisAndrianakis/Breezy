#ifndef BCE_H
#define BCE_H

#include "ast.h"

/* Bounds-check elimination. Interval-analyse the body and flag each EX_INDEX
   whose index is provably within [0, length) so codegen can drop the runtime
   length compare + bzy_oob call. Sound by construction: any value it cannot
   bound is treated as the full range, leaving the check in place. Run after the
   resolver has typed and slotted the body (and after constprop). */
void bce_annotate(Func *f);

#endif
