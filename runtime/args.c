#include "breezy.h"
#include <string.h>

/* Command-line arguments, captured by the C entry point (entry.c) before the
   scheduler starts. g_argv[0] is the program path; user args follow. Portable
   (no OS dependency) — built on both Windows and Linux. */
static int    g_argc;
static char **g_argv;

void bzy_set_args(int argc, char **argv)
{
	g_argc = argc;
	g_argv = argv;
}

/* System.args() -> string[] of the user arguments (argv[1..], excluding the
   program path). Returns an owned (+1) managed-string array. */
void *bzy_sys_args(void)
{
	int n = (g_argc > 1) ? (g_argc - 1) : 0;
	void *arr = bzy_array_new(n, 1);                 /* Managed string elements. */
	void **elems = (void**)((char*)arr + 32);
	for (int i = 0; i < n; i++)
	{
		const char *a = g_argv[i + 1];
		elems[i] = bzy_str_new(a, (int64_t)strlen(a));
	}

	return arr;
}
