#ifndef PROMOTE_H
#define PROMOTE_H

#include "ast.h"

/* Register promotion: choose up to four hot 64-bit integer locals to live in
   r12..r15 for this function's lifetime, recording them in f->promo_*. Run
   after the resolver has assigned slot offsets and ownership/escape have run. */
void promote_annotate(Func *f);

#endif
