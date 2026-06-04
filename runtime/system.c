#include "breezy.h"
#include <stdlib.h>
#include <string.h>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

/* Command-line arguments, captured by the C entry point (entry.c) before the
   scheduler starts. g_argv[0] is the program path; user args follow. */
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

/* Build "cmd /c <command>", launch it, and either return the pid (async) or wait
   and return the exit code. The caller owns the command string across any park;
   this only reads it. */
static int64_t shell_run(const char *command, int64_t wait)
{
	size_t n = strlen(command) + 8;          /* "cmd /c " + NUL. */
	char *line = malloc(n);
	strcpy(line, "cmd /c ");
	strcat(line, command);

	STARTUPINFOA si;
	PROCESS_INFORMATION pi;
	memset(&si, 0, sizeof(si));
	si.cb = sizeof(si);
	memset(&pi, 0, sizeof(pi));

	BOOL ok = CreateProcessA(NULL, line, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi);
	free(line);
	if (!ok)
	{
		return 0;                            /* Launch failed (VB.NET Shell returns 0). */
	}

	int64_t ret;
	if (wait)
	{
		WaitForSingleObject(pi.hProcess, INFINITE);
		DWORD code = 0;
		GetExitCodeProcess(pi.hProcess, &code);
		ret = (int64_t)code;                 /* Wait: return the exit code. */
	}
	else
	{
		ret = (int64_t)pi.dwProcessId;       /* Async: return the pid. */
	}

	CloseHandle(pi.hProcess);
	CloseHandle(pi.hThread);
	return ret;
}

typedef struct
{
	const char *cmd;
	int64_t wait;
	int64_t result;
} ShellCtx;

static void shell_offload(void *p)
{
	ShellCtx *c = (ShellCtx*)p;
	c->result = shell_run(c->cmd, c->wait);   /* Runs on an offload worker. */
}

int64_t bzy_system_shell(void *command, int64_t wait)
{
	const char *cmd = bzy_str_data(command);

	/* The wait path blocks on the child process; offload it so the breeze parks
	   instead of stalling its core (mirrors the 6b-1 file-I/O wrappers). The async
	   launch returns immediately, and outside a breeze there is nothing to park. */
	if (wait && bzy_sched_current())
	{
		ShellCtx c;
		c.cmd = cmd;
		c.wait = wait;
		c.result = 0;
		bzy_offload_run(shell_offload, &c);
		return c.result;
	}

	return shell_run(cmd, wait);
}
