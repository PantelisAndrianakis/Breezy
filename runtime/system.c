#include "breezy.h"
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#endif

/* System.shell launches an OS command interpreter: "cmd /c <command>" on Windows
   (CreateProcess), "/bin/sh -c <command>" on POSIX (fork/exec). Either returns
   the launched process id (async) or waits and returns the exit code. The caller
   owns the command string across any park; this only reads it. System.args lives
   in the portable args.c. */

#ifdef _WIN32
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
#else
/* POSIX backend. The wait path forks once and waitpid()s the child for its exit
   code. The async path double-forks so the grandchild is reparented to init and
   never lingers as a zombie; its pid is passed back through a pipe. Everything
   executed between fork and exec is async-signal-safe (no malloc), as required in
   a multithreaded process — the command string is already built by the caller. */
static int64_t shell_run(const char *command, int64_t wait)
{
	if (wait)
	{
		pid_t pid = fork();
		if (pid < 0)
		{
			return 0;
		}

		if (pid == 0)
		{
			execl("/bin/sh", "sh", "-c", command, (char*)NULL);
			_exit(127);                      /* exec failed: shell "command not found" code. */
		}

		int status = 0;
		if (waitpid(pid, &status, 0) < 0)
		{
			return 0;
		}

		if (WIFEXITED(status))
		{
			return (int64_t)WEXITSTATUS(status);
		}

		if (WIFSIGNALED(status))
		{
			return (int64_t)(128 + WTERMSIG(status));   /* Shell convention for signal death. */
		}

		return 0;
	}

	/* Async: double-fork, reporting the grandchild pid back through the pipe. */
	int pfd[2];
	if (pipe(pfd) != 0)
	{
		return 0;
	}

	pid_t mid = fork();
	if (mid < 0)
	{
		close(pfd[0]);
		close(pfd[1]);
		return 0;
	}

	if (mid == 0)
	{
		close(pfd[0]);
		pid_t gc = fork();
		if (gc == 0)
		{
			close(pfd[1]);
			execl("/bin/sh", "sh", "-c", command, (char*)NULL);
			_exit(127);
		}

		pid_t report = (gc > 0) ? gc : 0;
		ssize_t w = write(pfd[1], &report, sizeof(report));
		(void)w;
		close(pfd[1]);
		_exit(0);
	}

	close(pfd[1]);
	pid_t gc = 0;
	ssize_t r = read(pfd[0], &gc, sizeof(gc));
	(void)r;
	close(pfd[0]);
	waitpid(mid, NULL, 0);                    /* Reap the middle child (exits at once). */
	return (int64_t)(gc > 0 ? gc : 0);
}
#endif

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

/* System.getenv(name): the environment value as an owned string, or the null
   string when unset. Mirrors the fromCString NULL->null convention. */
void *bzy_sys_getenv(void *name)
{
	const char *v = getenv(bzy_str_data(name));
	if (!v)
	{
		return NULL;   /* Unset -> Breezy null string. */
	}

	return bzy_str_new(v, (int64_t)strlen(v));
}
