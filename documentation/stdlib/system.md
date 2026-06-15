# System

`System` is a **static namespace** for launching operating-system commands, reading command-line arguments and environment variables, and waiting for a termination signal. The command launcher is modelled on VB.NET's `Shell`.

← [Back to the guide](../guide.md)

---

## Launching a command

`System.shell(command)` runs the command through the system shell (`cmd /c` on Windows, `/bin/sh -c` on Linux) **asynchronously** and returns the OS process id (`0` if the launch failed). It does not wait and does not capture output - the child inherits the console.

```breezy
int pid = System.shell("notepad");          // Launches and returns immediately.
```

With a second `true` argument, `shell` **waits** for the process to exit and returns its **exit code**. The wait is routed through the [offload pool](../io/native-io.md), so it parks the calling breeze instead of stalling its core.

```breezy
int code = System.shell("robocopy src dst /MIR", true);   // Blocks (parked) until it exits.
```

- `System.shell(command) -> int` - run asynchronously, return the process id (`0` on failure). Output is not captured.
- `System.shell(command, wait) -> int` - when `wait` is `true`, park until exit and return the exit code.

---

## Reading command-line arguments

`System.args()` returns the arguments the program was launched with, as a `string[]`. It excludes the program path, and returns an empty array when none were passed.

```breezy
string[] args = System.args();          // out.exe alpha beta -> ["alpha", "beta"].
print(args.length);            // 2.
foreach (string a in args)
{
	print(a);
}
```

---

## Environment variables

`System.getenv(name)` returns the value of an environment variable as a `string`, or the **`null` string** when the variable is not set - so an unset variable is distinguishable from one set to the empty string.

```breezy
string path = System.getenv("PATH");        // The PATH value, or null if unset.
print(path.length() > 0);
```

---

## Graceful shutdown

`System.awaitShutdown()` **parks the calling breeze until a termination signal** - `SIGINT`/`SIGTERM` on Linux, or a Ctrl+C / console-close event on Windows - then returns. A server starts its work, awaits the signal, and runs its drain-and-exit logic in **normal context**:

```breezy
void main()
{
	startServer();
	System.awaitShutdown();   // Blocks here until Ctrl+C / SIGTERM.
	drainConnections();        // Runs normally; not in a signal handler.
	print("bye");
}
```

The operating-system signal handler installed underneath does the bare minimum - it records that a signal arrived and wakes the parked breeze - and runs **no application code**, so there are no async-signal-safety hazards. A signal that arrives before `awaitShutdown` is called is not lost: the call returns immediately. The handler is installed lazily on the first call.

---

## Real-time terminal I/O

Five primitives drive a real-time console loop - pacing, raw keyboard mode,
non-blocking key reads, and mouse reporting. Together they are enough to run an
interactive frame-by-frame program (input, simulate, render with ANSI escapes,
repeat).

```breezy
System.rawMode(true);                 // Keys arrive instantly, no echo, no Enter.
System.mouseMode(true);               // Motion + button events.
while (running)
{
	int key = System.pollKey();       // -1 when nothing is pending.
	if (key == 113)                   // 'q'.
	{
		running = false;
	}

	long ev = System.pollMouse();     // -1 when nothing is pending.
	if (ev != -1)
	{
		int mx = (int) ((ev >> 32) & 0xFFFF);   // Column.
		int my = (int) ((ev >> 16) & 0xFFFF);   // Row.
		int flags = (int) (ev & 0xFFFF);         // Button/event bits.
	}

	// ... simulate and render the frame with print(...) ...

	System.sleep(16);                 // ~60 frames per second.
}
System.mouseMode(false);
System.rawMode(false);                // Restore the terminal.
```

- `System.sleep(ms)` - park the calling breeze for approximately `ms`
  milliseconds, then resume. The wait is routed through the [offload
  pool](../io/native-io.md), so it parks the breeze instead of stalling its
  core. A non-positive `ms` returns at once. Duration is best-effort OS
  scheduling granularity, not a hard real-time guarantee.
- `System.rawMode(on)` - `true` puts the terminal into character-at-a-time mode
  (no line buffering, no echo) so keystrokes are readable without Enter; `false`
  restores it. The original mode is captured on the first `true` and restored
  automatically at process exit and on Ctrl+C / termination, so a crash never
  leaves the terminal raw. It is idempotent, and a no-op when standard input is
  not a terminal (piped or redirected).
- `System.pollKey() -> int` - return the next pending input byte as `0..255`, or
  `-1` when none is available. It never blocks. Multi-byte sequences (arrow keys
  arrive as `ESC` `[` `A`) are returned one byte per call; decode them in your
  own code. It is meaningful while `rawMode(true)` is in effect.
- `System.mouseMode(on)` - `true` enables terminal mouse reporting (motion and
  button events); `false` disables it. Same auto-restore guarantees as
  `rawMode`. Idempotent, and a no-op on a non-terminal stdin.
- `System.pollMouse() -> long` - return the next pending mouse event as a packed
  `long`, or `-1` when none is available. It never blocks. The packing is `x` in
  bits 47..32 (column), `y` in bits 31..16 (row), and flag bits 15..0: bit 0
  left, 1 right, 2 middle, 3 press, 4 release, 5 motion, 6 wheel-up, 7
  wheel-down. Coordinates are absolute terminal cells; compute relative deltas
  (for mouselook) by differencing successive polls. Meaningful while
  `mouseMode(true)` is in effect. Each tick, drain `pollKey` and `pollMouse` in
  a loop until both return `-1` - they share one input stream and hand off each
  other's events at the head.

---

## Rules & gotchas

- **`shell(command)` is fire-and-forget** - it returns a process id and does not capture output.
- **`shell(command, true)` waits** and returns the exit code, parking the breeze during the wait.
- **`System.args()` excludes the program path** and is empty when no arguments were passed.
- **`System.getenv(name)`** returns the value or the `null` string when unset (distinct from an empty value).
- **`System.awaitShutdown()`** parks until SIGINT/SIGTERM (Ctrl+C on Windows); the underlying handler runs no application code and a signal arriving early is not lost.
- **`System.sleep(ms)` parks the breeze** (offloaded); duration is approximate OS granularity, and `ms <= 0` returns immediately.
- **`System.rawMode(on)` / `System.mouseMode(on)` auto-restore** the terminal at exit and on Ctrl+C, are idempotent, and are no-ops on a non-terminal stdin.
- **`System.pollKey()` / `System.pollMouse()` never block** - each returns its next event or `-1`; they share one input stream, so drain both each tick until both return `-1`.
- **Cross-platform:** Windows launches via `cmd /c` (`CreateProcess`), Linux via `/bin/sh -c` (`fork`/`exec`). The same code runs on both. No output capture, stdin, or timeout yet.

---

← [File](file.md) · [Back to the guide](../guide.md) · Next: [Compilation pipeline](../build/compilation.md)
