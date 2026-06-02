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

void   *bzy_str_new(const char *bytes, int64_t len); /* Owned (+1) immutable string. */
int64_t bzy_str_len(void *s);                        /* Byte length (excludes the NUL). */
const char *bzy_str_data(void *s);                   /* Pointer to the inline NUL-terminated bytes. */
void   *bzy_str_concat(void *a, void *b);            /* Owned (+1) a followed by b. */
void    bzy_print_str(void *s);                      /* Write the bytes + '\n'. */

void   *bzy_sb_new(void);                            /* Owned (+1) empty StringBuilder. */
void    bzy_sb_append(void *sb, void *s);            /* Append a string's bytes. */
void    bzy_sb_append_cstr(void *sb, const char *bytes, int64_t len);
void   *bzy_sb_to_string(void *sb);                  /* Owned (+1) immutable snapshot. */

void   *bzy_array_new(int64_t n, int64_t elem_is_managed); /* Owned (+1) zeroed array. */
int64_t bzy_array_len(void *a);
void    bzy_oob(int64_t index, int64_t length);     /* Print + abort (no return). */

void   *bzy_map_new(int64_t key_kind, int64_t val_is_managed); /* Owned (+1). */
void    bzy_map_put(void *m, int64_t key, int64_t val);
int64_t bzy_map_get(void *m, int64_t key);   /* 0/NULL if absent; retains a managed value. */
int64_t bzy_map_has(void *m, int64_t key);   /* 1 / 0 */
void    bzy_map_remove(void *m, int64_t key);
int64_t bzy_map_len(void *m);

#endif
