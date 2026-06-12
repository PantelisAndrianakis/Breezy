#include "grow.h"
#include <stdlib.h>
#include <stdio.h>

void *grow_ensure(void *arr, int count, int *cap, size_t elem_size)
{
	if (count < *cap)
	{
		return arr;
	}

	int ncap = (*cap < 4) ? 4 : (*cap * 2);
	void *n = realloc(arr, (size_t)ncap * elem_size);
	if (!n)
	{
		fprintf(stderr, "Out of memory growing array.\n");
		exit(1);
	}

	*cap = ncap;
	return n;
}
