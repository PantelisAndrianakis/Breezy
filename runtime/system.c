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

/* ---- Mouse-reporting state + shared terminal-restore wiring ---- */

static int g_mouse_active = 0;
#ifdef _WIN32
static DWORD g_mouse_saved_mode = 0;
#endif

static void mouse_disable(void)
{
	if (!g_mouse_active)
	{
		return;
	}

#ifdef _WIN32
	SetConsoleMode(GetStdHandle(STD_INPUT_HANDLE), g_mouse_saved_mode);
#else
	const char *off = "\x1b[?1006l\x1b[?1003l";
	ssize_t w = write(STDOUT_FILENO, off, strlen(off));
	(void)w;
#endif
	g_mouse_active = 0;
}

/* Restore everything the terminal touched: raw attributes and mouse reporting. */
static void term_restore_all(void)
{
	raw_restore();
	mouse_disable();
}

/* Termination-signal handler: restore the terminal, then re-raise with the
   default disposition so the process dies as it normally would. Decoupled from
   awaitShutdown; a program that uses both should call rawMode(false) in its
   drain path (the last-installed handler otherwise wins). */
static void raw_signal_restore(int sig)
{
	term_restore_all();
	signal(sig, SIG_DFL);
	raise(sig);
}

static int g_restore_hook_done = 0;

static void ensure_restore_hook(void)
{
	if (g_restore_hook_done)
	{
		return;
	}

	atexit(term_restore_all);
	signal(SIGINT,  raw_signal_restore);
	signal(SIGTERM, raw_signal_restore);
	g_restore_hook_done = 1;
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
		ensure_restore_hook();
	}
	else
	{
		raw_restore();
	}
}

/* ---- System.mouseMode(on): enable/disable terminal mouse reporting ---- */

void bzy_sys_mouse_mode(int64_t on)
{
	if (on)
	{
		if (g_mouse_active)
		{
			return;   /* Idempotent. */
		}

#ifdef _WIN32
		HANDLE h = GetStdHandle(STD_INPUT_HANDLE);
		if (h == INVALID_HANDLE_VALUE || h == NULL)
		{
			return;
		}
		if (!GetConsoleMode(h, &g_mouse_saved_mode))
		{
			return;   /* Not a console (piped): no-op. */
		}

		/* Mouse events on; extended flags required to set it; quick-edit off so
		   the console does not swallow drags for text selection. */
		DWORD mode = (g_mouse_saved_mode | ENABLE_MOUSE_INPUT | ENABLE_EXTENDED_FLAGS)
					 & ~ENABLE_QUICK_EDIT_MODE;
		SetConsoleMode(h, mode);
#else
		if (!isatty(STDIN_FILENO))
		{
			return;   /* Piped: no-op. */
		}

		/* xterm: 1003 = any-motion tracking, 1006 = SGR extended coordinates. */
		const char *onseq = "\x1b[?1003h\x1b[?1006h";
		ssize_t w = write(STDOUT_FILENO, onseq, strlen(onseq));
		(void)w;
#endif
		g_mouse_active = 1;
		ensure_restore_hook();
	}
	else
	{
		mouse_disable();
	}
}

/* Shared non-blocking stdin buffer feeding pollKey + pollMouse on POSIX. One
   drain pulls available bytes; pollMouse consumes SGR mouse escapes, pollKey
   consumes everything else, so neither steals the other's bytes. */
#ifndef _WIN32
#define BZY_IN_CAP 256
static unsigned char g_in_buf[BZY_IN_CAP];
static int g_in_head = 0;
static int g_in_tail = 0;

static void in_drain(void)
{
	unsigned char tmp[BZY_IN_CAP];
	ssize_t n = read(STDIN_FILENO, tmp, sizeof(tmp));
	for (ssize_t i = 0; i < n; i++)
	{
		int next = (g_in_tail + 1) % BZY_IN_CAP;
		if (next == g_in_head)
		{
			break;   /* Full: drop the remainder this tick. */
		}
		g_in_buf[g_in_tail] = tmp[i];
		g_in_tail = next;
	}
}

static int  in_count(void)    { return (g_in_tail - g_in_head + BZY_IN_CAP) % BZY_IN_CAP; }
static int  in_peek(int i)     { return g_in_buf[(g_in_head + i) % BZY_IN_CAP]; }
static void in_advance(int k)  { g_in_head = (g_in_head + k) % BZY_IN_CAP; }
#endif

/* Pack a mouse event. Flags: 0 left,1 right,2 middle,3 press,4 release,
   5 motion,6 wheel-up,7 wheel-down. Always non-negative (low 48 bits). */
static int64_t mouse_pack(int x, int y, int flags)
{
	if (x < 0) { x = 0; }
	if (y < 0) { y = 0; }
	return ((int64_t)(x & 0xFFFF) << 32) | ((int64_t)(y & 0xFFFF) << 16) | (int64_t)(flags & 0xFFFF);
}

/* ---- System.pollKey(): non-blocking next input byte, or -1 ---- */

int64_t bzy_sys_poll_key(void)
{
#ifdef _WIN32
	HANDLE h = GetStdHandle(STD_INPUT_HANDLE);
	INPUT_RECORD rec;
	DWORD navail = 0, nread = 0;
	for (;;)
	{
		if (!GetNumberOfConsoleInputEvents(h, &navail) || navail == 0)
		{
			return -1;
		}
		if (!PeekConsoleInput(h, &rec, 1, &nread) || nread == 0)
		{
			return -1;
		}
		if (rec.EventType == KEY_EVENT)
		{
			if (rec.Event.KeyEvent.bKeyDown && rec.Event.KeyEvent.uChar.AsciiChar)
			{
				ReadConsoleInput(h, &rec, 1, &nread);   /* Consume the key. */
				return (int64_t)(unsigned char)rec.Event.KeyEvent.uChar.AsciiChar;
			}
			ReadConsoleInput(h, &rec, 1, &nread);       /* Drop key-up / modifier. */
			continue;
		}
		return -1;   /* Mouse (or other) at head: leave it for pollMouse. */
	}
#else
	in_drain();
	int n = in_count();
	if (n == 0)
	{
		return -1;
	}

	/* Defer a mouse sequence (ESC [ <) to pollMouse; wait on an incomplete ESC [. */
	if (in_peek(0) == 0x1b && n >= 2 && in_peek(1) == '[')
	{
		if (n < 3)
		{
			return -1;            /* Incomplete: wait one tick for the 3rd byte. */
		}
		if (in_peek(2) == '<')
		{
			return -1;            /* Mouse sequence: pollMouse owns it. */
		}
	}

	int c = in_peek(0);
	in_advance(1);
	return (int64_t)c;            /* Plain byte (incl. arrow-key ESC/[/letter, one per call). */
#endif
}

/* ---- System.pollMouse(): non-blocking next mouse event, packed, or -1 ---- */

int64_t bzy_sys_poll_mouse(void)
{
#ifdef _WIN32
	HANDLE h = GetStdHandle(STD_INPUT_HANDLE);
	INPUT_RECORD rec;
	DWORD navail = 0, nread = 0;
	if (!GetNumberOfConsoleInputEvents(h, &navail) || navail == 0)
	{
		return -1;
	}
	if (!PeekConsoleInput(h, &rec, 1, &nread) || nread == 0)
	{
		return -1;
	}
	if (rec.EventType != MOUSE_EVENT)
	{
		return -1;   /* Key at head: leave it for pollKey. */
	}
	ReadConsoleInput(h, &rec, 1, &nread);

	MOUSE_EVENT_RECORD *me = &rec.Event.MouseEvent;
	int x = me->dwMousePosition.X;
	int y = me->dwMousePosition.Y;
	DWORD bs = me->dwButtonState;
	DWORD ef = me->dwEventFlags;
	int flags = 0;
	if (ef & MOUSE_WHEELED)
	{
		flags |= ((int)(short)HIWORD(bs) > 0) ? 64 : 128;
	}
	else
	{
		if (ef & MOUSE_MOVED)                     { flags |= 32; }
		if (bs & FROM_LEFT_1ST_BUTTON_PRESSED)    { flags |= 1; }
		if (bs & RIGHTMOST_BUTTON_PRESSED)        { flags |= 2; }
		if (bs & FROM_LEFT_2ND_BUTTON_PRESSED)    { flags |= 4; }
		flags |= bs ? 8 : 16;   /* Any button down = press; none = release. */
	}
	return mouse_pack(x, y, flags);
#else
	in_drain();
	int n = in_count();
	if (n < 3 || !(in_peek(0) == 0x1b && in_peek(1) == '[' && in_peek(2) == '<'))
	{
		return -1;   /* No mouse sequence at the head. */
	}

	/* Parse ESC [ < b ; x ; y (M|m). Collect up to three numbers. */
	int vals[3] = {0, 0, 0};
	int vi = 0, num = 0, fin = 0, complete = 0, i = 3;
	for (; i < n; i++)
	{
		int c = in_peek(i);
		if (c >= '0' && c <= '9')
		{
			num = num * 10 + (c - '0');
		}
		else if (c == ';')
		{
			if (vi < 3) { vals[vi++] = num; }
			num = 0;
		}
		else if (c == 'M' || c == 'm')
		{
			if (vi < 3) { vals[vi++] = num; }
			fin = c;
			i++;            /* Consume the final byte too. */
			complete = 1;
			break;
		}
		else
		{
			in_advance(1);  /* Malformed: drop the ESC and resync next tick. */
			return -1;
		}
	}
	if (!complete)
	{
		return -1;          /* Incomplete sequence: wait for more bytes. */
	}

	int b = vals[0];
	int x = vals[1] - 1;    /* xterm reports 1-based cells. */
	int y = vals[2] - 1;
	in_advance(i);          /* Consume the whole sequence. */

	int flags = 0;
	int btn = b & 3;
	if (b & 64)
	{
		flags |= (btn == 0) ? 64 : 128;   /* Wheel up / down. */
	}
	else
	{
		if (b & 32)        { flags |= 32; }          /* Motion. */
		if (btn == 0)      { flags |= 1; }           /* Left. */
		else if (btn == 2) { flags |= 2; }           /* Right. */
		else if (btn == 1) { flags |= 4; }           /* Middle. */
		flags |= (fin == 'M') ? 8 : 16;              /* Press / release. */
	}
	return mouse_pack(x, y, flags);
#endif
}
