#include "breezy.h"
#include <stdio.h>
#include <stdlib.h>

/* Scalar print helpers, dispatched by the compiler on the argument's static type:
   signed integers print as %lld, unsigned as %llu, bools as true/false. */

void bzy_print_i64(int64_t v)
{
	printf("%lld\n", (long long)v);
}

void bzy_print_u64(uint64_t v)
{
	printf("%llu\n", (unsigned long long)v);
}

void bzy_print_bool(int64_t v)
{
	printf("%s\n", v ? "true" : "false");
}

void bzy_print_f64(double v)
{
	printf("%.17g\n", v);
}

/* input(): read one line from standard input and return it as an owned string,
   with the trailing newline (and a preceding CR, for CRLF endings) removed. At
   end of input with nothing read, returns an empty string. Blocking by design -
   it is for interactive console programs. */
void *bzy_input_line(void)
{
	fflush(stdout);   /* Flush any prompt printed first so it is visible before we block. */

	size_t cap = 128;
	size_t len = 0;
	char *buf = malloc(cap);
	if (!buf)
	{
		return bzy_str_new(NULL, 0);
	}

	int c;
	while ((c = getchar()) != EOF && c != '\n')
	{
		if (len + 1 >= cap)
		{
			cap *= 2;
			char *grown = realloc(buf, cap);
			if (!grown)
			{
				free(buf);
				return bzy_str_new(NULL, 0);
			}

			buf = grown;
		}

		buf[len++] = (char)c;
	}

	if (len > 0 && buf[len - 1] == '\r')   /* Strip the CR of a CRLF line ending. */
	{
		len--;
	}

	void *s = bzy_str_new(buf, (int64_t)len);
	free(buf);
	return s;
}
