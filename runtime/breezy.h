#ifndef BREEZY_H
#define BREEZY_H
#include <stdint.h>

/* Object header: [vtable @0][refcount @8][fields @16..].
   The type descriptor sits at vtable - 8 and holds:
   [num_obj_fields @0][offset0 @8][offset1 @16].... */

void   *bzy_alloc(int64_t size);   /* Allocate, zero, set refcount to 1, bump live count; caller sets the vtable. */
void    bzy_retain(void *obj);     /* Increment the refcount (NULL-safe). */
void    bzy_release(void *obj);    /* Decrement the refcount; at zero release object fields, drop live count, free (NULL-safe). */
int64_t bzy_live_count(void);      /* Number of objects currently alive (for tests and leak checks). */

#endif
