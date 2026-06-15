#include "breezy.h"
#include "platform.h"
#include <stdlib.h>
#include <string.h>
#include <signal.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <conio.h>
#else
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <errno.h>
#include <termios.h>
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

/* ---- System.awaitShutdown(): park until a termination signal ---- */

static volatile sig_atomic_t g_shutdown_flag = 0;
static bzy_sem               g_sig_sem;
static int                   g_handlers_installed = 0;   /* One-shot via atomic exchange. */

/* Async-signal-safe handler: ONLY set the flag and post the semaphore. No Breezy
   code, no allocation, no locks run in signal context. */
static void shutdown_signal_handler(int sig)
{
	(void)sig;
	g_shutdown_flag = 1;
	bzy_sem_post(&g_sig_sem, 1);   /* sem_post / ReleaseSemaphore is async-signal-safe. */
}

#ifdef _WIN32
static BOOL WINAPI console_ctrl_handler(DWORD type)
{
	if (type == CTRL_C_EVENT || type == CTRL_CLOSE_EVENT || type == CTRL_BREAK_EVENT)
	{
		g_shutdown_flag = 1;
		bzy_sem_post(&g_sig_sem, 1);   /* Runs on a normal thread; post is fine. */
		return TRUE;
	}

	return FALSE;
}
#endif

static void install_handlers_once(void)
{
	if (__atomic_exchange_n(&g_handlers_installed, 1, __ATOMIC_ACQ_REL))
	{
		return;   /* Another caller already installed. */
	}

	bzy_sem_init(&g_sig_sem);

#ifdef _WIN32
	SetConsoleCtrlHandler(console_ctrl_handler, TRUE);
	signal(SIGINT, shutdown_signal_handler);    /* So raise(SIGINT) is caught too (programmatic / test). */
	signal(SIGTERM, shutdown_signal_handler);
#else
	struct sigaction sa;
	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = shutdown_signal_handler;
	sa.sa_flags = SA_RESTART;                   /* Do not disturb other syscalls. */
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGTERM, &sa, NULL);
#endif
}

static void sig_wait_offload(void *unused)
{
	(void)unused;
	if (!g_shutdown_flag)
	{
		bzy_sem_wait(&g_sig_sem);   /* Blocks the offload worker until the handler posts. */
	}
}

/* System.awaitShutdown(): park the calling breeze until SIGINT/SIGTERM (Ctrl+C on
   Windows). The handler only flags + posts; we observe it here, in normal context. */
void bzy_await_shutdown(void)
{
	install_handlers_once();
	if (g_shutdown_flag)
	{
		return;   /* Latch: a signal already arrived -> no lost-signal race. */
	}

	if (bzy_sched_current())
	{
		bzy_offload_run(sig_wait_offload, NULL);   /* Park the breeze; offload worker waits. */
	}
	else
	{
		sig_wait_offload(NULL);   /* No breeze to park: block this thread directly. */
	}
}

/* ---- System.sleep(ms): park the breeze for ~ms, offloaded ---- */

static void sleep_impl(int64_t ms)
{
#ifdef _WIN32
	Sleep((DWORD)ms);
#else
	struct timespec ts;
	ts.tv_sec  = (time_t)(ms / 1000);
	ts.tv_nsec = (long)((ms % 1000) * 1000000L);
	while (nanosleep(&ts, &ts) != 0 && errno == EINTR)
	{
		/* Interrupted: nanosleep wrote the remaining time back into ts; resume. */
	}
#endif
}

static void sleep_offload(void *p)
{
	sleep_impl(*(int64_t*)p);   /* Runs on an offload worker so the breeze's core is free. */
}

void bzy_sys_sleep(int64_t ms)
{
	if (ms <= 0)
	{
		return;   /* Non-positive duration: return at once. */
	}

	if (bzy_sched_current())
	{
		bzy_offload_run(sleep_offload, &ms);   /* Park the breeze; worker sleeps. */
	}
	else
	{
		sleep_impl(ms);   /* No breeze to park: sleep this thread directly. */
	}
}

/* ---- System.rawMode(on): char-at-a-time terminal, auto-restored ---- */

static int g_raw_active = 0;
static int g_raw_atexit_done = 0;

#ifdef _WIN32
static HANDLE g_raw_in = NULL;
static DWORD  g_raw_saved_mode = 0;
#else
static struct termios g_raw_saved_termios;
#endif

static void raw_restore(void)
{
	if (!g_raw_active)
	{
		return;
	}

#ifdef _WIN32
	SetConsoleMode(g_raw_in, g_raw_saved_mode);
#else
	tcsetattr(STDIN_FILENO, TCSANOW, &g_raw_saved_termios);
#endif
	g_raw_active = 0;
}

/* Termination-signal handler: restore the terminal, then re-raise with the
   default disposition so the process dies as it normally would. Decoupled from
   awaitShutdown; a program that uses both should call rawMode(false) in its
   drain path (the last-installed handler otherwise wins). */
static void raw_signal_restore(int sig)
{
	raw_restore();
	signal(sig, SIG_DFL);
	raise(sig);
}

void bzy_sys_raw_mode(int64_t on)
{
	if (on)
	{
		if (g_raw_active)
		{
			return;   /* Idempotent. */
		}

#ifdef _WIN32
		g_raw_in = GetStdHandle(STD_INPUT_HANDLE);
		if (g_raw_in == INVALID_HANDLE_VALUE || g_raw_in == NULL)
		{
			return;
		}
		if (!GetConsoleMode(g_raw_in, &g_raw_saved_mode))
		{
			return;   /* Not a console (piped/redirected): no-op. */
		}

		/* Clear line-buffering and echo; keep ENABLE_PROCESSED_INPUT so Ctrl+C
		   still raises (awaitShutdown / raw_signal_restore depend on it). */
		DWORD raw = g_raw_saved_mode & ~(ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT);
		SetConsoleMode(g_raw_in, raw);
#else
		if (!isatty(STDIN_FILENO))
		{
			return;   /* Piped/redirected stdin: no-op. */
		}
		if (tcgetattr(STDIN_FILENO, &g_raw_saved_termios) != 0)
		{
			return;
		}

		struct termios raw = g_raw_saved_termios;
		raw.c_lflag &= ~(ICANON | ECHO);   /* No line buffering, no echo. */
		raw.c_cc[VMIN]  = 0;                /* read() returns immediately... */
		raw.c_cc[VTIME] = 0;               /* ...with 0 bytes when nothing is pending. */
		tcsetattr(STDIN_FILENO, TCSANOW, &raw);
#endif
		g_raw_active = 1;

		if (!g_raw_atexit_done)
		{
			atexit(raw_restore);
			signal(SIGINT,  raw_signal_restore);
			signal(SIGTERM, raw_signal_restore);
			g_raw_atexit_done = 1;
		}
	}
	else
	{
		raw_restore();
	}
}

/* ---- System.pollKey(): non-blocking next input byte, or -1 ---- */

int64_t bzy_sys_poll_key(void)
{
#ifdef _WIN32
	if (_kbhit())
	{
		return (int64_t)(unsigned char)_getch();   /* One byte; multi-byte keys arrive byte-by-byte. */
	}
	return -1;
#else
	unsigned char c;
	ssize_t n = read(STDIN_FILENO, &c, 1);   /* VMIN=0/VTIME=0 from raw mode -> non-blocking. */
	if (n == 1)
	{
		return (int64_t)c;
	}
	return -1;   /* 0 = nothing pending; -1/EAGAIN = same to the caller. */
#endif
}
