#ifndef NONNEG_H
#define NONNEG_H

#include "ast.h"

/* Flag each integer / and % node whose dividend (lhs) is provably non-negative,
   using enclosing while/if guards, so codegen can drop the signed sign-bias.
   Run after the resolver has typed and slotted the body. */
void nonneg_annotate(Func *f);

#endif
