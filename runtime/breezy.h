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
int64_t bzy_str_eq(void *a, void *b);                /* 1 if byte-equal (NULL-safe, identity fast path). */
int64_t bzy_str_contains(void *s, void *needle);     /* 1 if needle occurs in s (empty needle -> 1). */
int64_t bzy_str_starts_with(void *s, void *pre);     /* 1 if s begins with pre. */
int64_t bzy_str_ends_with(void *s, void *suf);       /* 1 if s ends with suf. */
int64_t bzy_str_index_of(void *s, void *needle);     /* First byte index of needle, or -1 (empty -> 0). */
void   *bzy_str_substring(void *s, int64_t start, int64_t end); /* Clamped to [0,len]; owned. */
void   *bzy_str_replace(void *s, void *from, void *to);         /* All literal occurrences; owned. */
void   *bzy_str_trim(void *s);                                  /* Strip ASCII whitespace ends; owned. */
void   *bzy_str_to_upper(void *s);                              /* ASCII upper; owned. */
void   *bzy_str_to_lower(void *s);                              /* ASCII lower; owned. */
int64_t bzy_str_equals_ignore_case(void *a, void *b);  /* ASCII case-insensitive equality. */
int64_t bzy_str_is_empty(void *s);                     /* 1 if length 0. */
int64_t bzy_str_char_at(void *s, int64_t i);           /* Byte at i as int, or -1 out of range. */
int64_t bzy_str_last_index_of(void *s, void *needle);  /* Last byte index of needle, or -1 (empty -> len). */
void   *bzy_str_repeat(void *s, int64_t n);            /* s repeated n times (n<=0 -> ""); owned. */
void   *bzy_str_split(void *s, void *sep);             /* Split on sep -> owned string[] (empty sep -> [s]). */
void    bzy_print_str(void *s);                      /* Write the bytes + '\n'. */

void   *bzy_sb_new(void);                            /* Owned (+1) empty StringBuilder. */
void    bzy_sb_append(void *sb, void *s);            /* Append a string's bytes. */
void    bzy_sb_append_cstr(void *sb, const char *bytes, int64_t len);
void   *bzy_sb_to_string(void *sb);                  /* Owned (+1) immutable snapshot. */

void   *bzy_array_new(int64_t n, int64_t elem_is_managed); /* Owned (+1) zeroed array. */
int64_t bzy_array_len(void *a);
void    bzy_oob(int64_t index, int64_t length, int64_t pc, int64_t frame); /* Throw IndexOutOfBounds (no return). */
void    bzy_oob_abort(int64_t index, int64_t length);                      /* Print + abort (no return). */

void   *bzy_map_new(int64_t key_kind, int64_t val_is_managed); /* Owned (+1). */
void    bzy_map_put(void *m, int64_t key, int64_t val);
int64_t bzy_map_get(void *m, int64_t key);   /* 0/NULL if absent; retains a managed value. */
int64_t bzy_map_has(void *m, int64_t key);   /* 1 / 0 */
void    bzy_map_remove(void *m, int64_t key);
int64_t bzy_map_len(void *m);
int64_t bzy_map_iter(void *m, int64_t from);   /* Next full slot index >= from, or -1. */
int64_t bzy_map_key_at(void *m, int64_t slot); /* Key at slot (borrowed; no retain). */

void   *bzy_vec_new(int64_t elem_kind);          /* Owned (+1). 0 int,1 float,2 double,3 string,4 object. */
int64_t bzy_vec_len(void *v);
void    bzy_vec_push_back(void *v, int64_t val); /* Retains a managed element. */
void    bzy_vec_push_front(void *v, int64_t val);
int64_t bzy_vec_pop_back(void *v);               /* Transfers out (owned); aborts if empty. */
int64_t bzy_vec_pop_front(void *v);
int64_t bzy_vec_get(void *v, int64_t i);         /* Bounds-checked; retains a managed value (owned). */
void    bzy_vec_set(void *v, int64_t i, int64_t val);  /* Bounds-checked; retain new / release old. */
int64_t bzy_vec_peek_back(void *v);              /* Retains (owned); aborts if empty. */
int64_t bzy_vec_peek_front(void *v);
void    bzy_vec_remove_at(void *v, int64_t i);   /* Bounds-checked; releases the removed managed element. */
int64_t bzy_vec_index_of(void *v, int64_t needle); /* First index equal per elem_kind, or -1. */
int64_t bzy_vec_contains(void *v, int64_t needle); /* 1 / 0 */

int64_t bzy_clock_millis(void);   /* Wall-clock milliseconds since the Unix epoch. */
int64_t bzy_clock_nanos(void);    /* High-resolution monotonic counter, in nanoseconds. */
void   *bzy_clock_date(int64_t millis);                /* "yyyy-MM-dd HH:mm:ss", local time; owned. */
void   *bzy_clock_date_fmt(int64_t millis, void *fmt); /* Java-style pattern, local time; owned. */

int64_t bzy_rnd_bool(void);                            /* 0 or 1. */
int64_t bzy_rnd_int(void);                             /* Full 32-bit signed range. */
int64_t bzy_rnd_long(void);                            /* Full 64-bit range. */
float   bzy_rnd_float(void);                            /* [0, 1). */
double  bzy_rnd_double(void);                           /* [0, 1). */
double  bzy_rnd_gaussian(void);                         /* mean 0, stddev 1. */
int64_t bzy_rnd_get_i(int64_t bound);                   /* [0, bound). */
int64_t bzy_rnd_get_ii(int64_t origin, int64_t bound);  /* [origin, bound] inclusive. */
int64_t bzy_rnd_get_l(int64_t bound);
int64_t bzy_rnd_get_ll(int64_t origin, int64_t bound);
float   bzy_rnd_get_f(float bound);                     /* [0, bound). */
float   bzy_rnd_get_ff(float origin, float bound);      /* [origin, bound). */
double  bzy_rnd_get_d(double bound);
double  bzy_rnd_get_dd(double origin, double bound);
void    bzy_rnd_bytes(void *arr);                       /* Fill each byte[] slot with [0,255]. */

void    bzy_throw(void *exc, int64_t pc, int64_t frame); /* Unwind the rbp chain; never returns. */

int64_t bzy_regex_matches(void *pat, void *text);            /* Full match -> 1/0. */
int64_t bzy_regex_test(void *pat, void *text);              /* Search -> 1/0. */
void   *bzy_regex_find(void *pat, void *text);              /* Leftmost match substring (owned; "" if none). */
void   *bzy_regex_replace(void *pat, void *text, void *repl); /* All matches replaced (owned). */

#endif
