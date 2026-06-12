#ifndef GROW_H
#define GROW_H
#include <stddef.h>

/* Ensure `arr` (with `count` live elements of `elem_size`) can hold one more.
   Grows capacity by doubling (from 4) via realloc and writes the new capacity
   back through `cap`. Returns the possibly-moved base pointer. The compiler is
   short-lived and leaks by design, so the returned buffer is never freed. */
void *grow_ensure(void *arr, int count, int *cap, size_t elem_size);

/* Ensure capacity for at least `need` elements (pre-sizing), doubling from 4.
   Use when a known count of appends follows and held element pointers must not
   be invalidated by an intervening realloc. */
void *grow_reserve(void *arr, int need, int *cap, size_t elem_size);

#endif
