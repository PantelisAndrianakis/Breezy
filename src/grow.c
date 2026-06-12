#include "grow.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

/* Grow to `ncap` and zero the newly-added region [old_cap, ncap). The fixed
   arrays these helpers replace lived in BSS and were zero-initialised, and much
   of the compiler relies on that (e.g. a freshly-grown struct slot having NULL
   pointers); realloc leaves new memory uninitialised, so we must zero it. */
static void *grow_to(void *arr, int old_cap, int ncap, int *cap, size_t elem_size)
{
	void *n = realloc(arr, (size_t)ncap * elem_size);
	if (!n)
	{
		fprintf(stderr, "Out of memory growing array.\n");
		exit(1);
	}

	memset((char *)n + (size_t)old_cap * elem_size, 0,
	       (size_t)(ncap - old_cap) * elem_size);
	*cap = ncap;
	return n;
}

void *grow_ensure(void *arr, int count, int *cap, size_t elem_size)
{
	if (count < *cap)
	{
		return arr;
	}

	int ncap = (*cap < 4) ? 4 : (*cap * 2);
	return grow_to(arr, *cap, ncap, cap, elem_size);
}

void *grow_reserve(void *arr, int need, int *cap, size_t elem_size)
{
	if (need <= *cap)
	{
		return arr;
	}

	int ncap = (*cap < 4) ? 4 : *cap;
	while (ncap < need)
	{
		ncap *= 2;
	}

	return grow_to(arr, *cap, ncap, cap, elem_size);
}
