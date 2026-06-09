# System

`System` is a **static namespace** for launching operating-system commands and reading the program's command-line arguments. The command launcher is modelled on VB.NET's `Shell`.

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

## Rules & gotchas

- **`shell(command)` is fire-and-forget** - it returns a process id and does not capture output.
- **`shell(command, true)` waits** and returns the exit code, parking the breeze during the wait.
- **`System.args()` excludes the program path** and is empty when no arguments were passed.
- **Cross-platform:** Windows launches via `cmd /c` (`CreateProcess`), Linux via `/bin/sh -c` (`fork`/`exec`). The same code runs on both. No output capture, stdin, or timeout yet.

---

← [File](file.md) · [Back to the guide](../guide.md) · Next: [Compilation pipeline](../build/compilation.md)
