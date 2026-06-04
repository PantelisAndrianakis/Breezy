#include "breezy.h"
#include <stdlib.h>
#include <string.h>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

/* System.shell: Windows-only (CreateProcess). A POSIX fork/exec backend lands
   with the Linux port; System.args lives in the portable args.c. */

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
