#ifndef BREEZY_H
#define BREEZY_H
#include <stdint.h>

/* Object header: [vtable @0][refcount @8][gcinfo @16][fields @24..].
   The type descriptor sits at vtable - 8 and holds:
   [num_obj_fields @0][offset0 @8][offset1 @16]....
   gcinfo packs the cycle-collector state: bits 0-1 color, bit 2 buffered,
   bits 8+ the cyclic refcount (crc) scratch used during trial deletion. */

void   *bzy_alloc(int64_t size);   /* Allocate, zero, set refcount to 1, bump live count; caller sets the vtable. */
void    bzy_retain(void *obj);     /* Increment the refcount (NULL-safe). */
void    bzy_release(void *obj);    /* Decrement the refcount; free acyclic garbage, buffer cycle candidates (NULL-safe). */
int64_t bzy_live_count(void);      /* Number of objects currently alive (for tests and leak checks). */

void    bzy_collect_cycles(void);  /* Run trial deletion over the buffered cycle-root candidates. */
int64_t bzy_roots_buffered(void);  /* Number of candidate roots pending collection (for tests). */

void    bzy_print_i64(int64_t v);  /* Print a signed integer as %lld followed by a newline. */
void    bzy_print_u64(uint64_t v); /* Print an unsigned integer as %llu followed by a newline. */
void    bzy_print_bool(int64_t v); /* Print "true" or "false" followed by a newline. */
void    bzy_print_f64(double v);   /* Print a double with %.17g followed by a newline. */

#endif
