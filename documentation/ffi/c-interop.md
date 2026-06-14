# C Library Interop - Zero-Cost FFI

Because Breezy's heap is **non-moving** and its objects are plain structs, you call C libraries **directly** - no wrappers, no marshalling layer, no pinning. You declare a C function's signature with `extern`, link the library, and call it like any function. This is what lets Breezy reach native database and crypto libraries (MySQL, OpenSSL, zlib) at zero cost.

← [Back to the guide](../guide.md)

---

## Declaring an extern function

`extern` declares a C function by its signature. The linker resolves it against the libraries you link.

```breezy
extern long strlen(string s);   // 'string' marshals to its char* data pointer.
extern int  abs(int n);

void main()
{
	print(strlen("hello"));   // 5.
	print(abs(-7));           // 7.
}
```

Link native libraries with `--link <lib>` (repeatable) on the command line, or declare them per-project in a `breezy.toml` (below).

---

## The marshalling contract

How Breezy types map to C:

| Breezy | C |
| --- | --- |
| `int` / `byte` / `short` | 32-/8-/16-bit int. |
| `long` | 64-bit int (a handle or pointer). |
| `float` / `double` | C `float` / `double`. |
| `bool` | C int. |
| `string` **argument** | NUL-terminated `char*` (the string's data pointer). |
| value array **argument** (`byte[]`, `int[]`, `double[]`, ...) | pointer to the array's element data - a C buffer the function can read or fill in place. Pass `.length` separately for the size. |

**Returns** are limited to `void`, the int family, `long`, `float`, `double`, and `bool`. A C function that returns `char*` is declared to return `long` and read back with `fromCString` / `fromCBytes` (below). An embedded NUL in a `string` argument truncates on the C side, since C strings end at the first NUL.

Object and string arrays are **not** valid extern arguments - they hold pointers C cannot use as a flat buffer, and are rejected at compile time. The data pointer is valid for the duration of the call (the heap is non-moving); if C retains it past the call, that is your responsibility, exactly as for a `string` argument.

---

## Reading a C string back

A C function that returns `char*` is declared to return `long`; two builtins copy that pointer's bytes into an owned Breezy `string`:

- `fromCString(ptr)` copies up to the first NUL - for NUL-terminated C strings (error text, environment values, diagnostic strings).
- `fromCBytes(ptr, len)` copies exactly `len` bytes, embedded NULs preserved - for length-counted data (a binary column read alongside its length).

Both copy into a fresh string independent of the C buffer's lifetime, so there is no aliasing, no leak, and no dangling pointer. A NULL pointer (`0`) yields the `null` reference, so an absent value is distinguishable from an empty string.

```breezy
extern long getenv(string name);   // char* -> long.

void main()
{
	string path = fromCString(getenv("PATH"));
	print(path.length());
}
```

---

## byte[] ↔ string

Where `fromCString` / `fromCBytes` bridge a *C pointer*, two in-language conversions bridge a Breezy `byte[]` and a Breezy `string` - the buffer/text round-trip a database driver (text protocols, BLOB columns) or a crypto library (hash a string, read result bytes) needs:

- `string.toBytes()` returns a `byte[]` holding the string's raw UTF-8 bytes.
- `fromBytes(byte[])` builds a string from a `byte[]` or `ubyte[]`'s raw bytes.

A Breezy string *is* byte-counted UTF-8, so the conversion is a verbatim byte copy both directions: `fromBytes(s.toBytes())` reproduces `s` exactly, including bytes above 127. `fromBytes` accepts only `byte[]` / `ubyte[]`; a wider or object array is a compile-time error (the element width and endianness would be ambiguous). `fromBytes(null)` yields the `null` string.

```breezy
extern blocking long recv(long fd, byte[] buf, long len, int flags);

void main()
{
	byte[] buf = new byte[4096];
	long n = recv(fd, buf, buf.length, 0);   // C fills buf in place.
	string text = fromBytes(buf);            // raw bytes -> string for parsing.
	print(text.length());
}
```

Together with `fromCBytes` (C pointer -> string) and value-array argument marshalling (`string` / `byte[]` -> C buffer), this completes the byte/text bridge across the FFI boundary.

---

## Callbacks

A C library can call back into Breezy through a function pointer - a `qsort` comparator, an OpenSSL verify hook, an event handler. Declare the callback parameter with a **C-style function type** `ret(paramtypes)` and pass a matching Breezy function **by name**; the compiler checks the signature and hands C the function's address directly - **no trampoline, no wrapper**, because a Breezy function already uses the platform C ABI.

```breezy
extern long qsort(long[] base, long n, long size, long(long,long) cmp);

long ascending(long a, long b)
{
	return a - b;
}

void main()
{
	long[] xs = new long[5];
	// ... fill xs ...
	qsort(xs, xs.length, 8, ascending);   // C calls 'ascending' back, in place.
}
```

A callback signature is **C-ABI-native scalars only** (`void`/int-family/`long`/`float`/`double`, and pointers as `long`); a `char*` the C side passes arrives as a `long` and is read inside the callback with `fromCString` / `fromCBytes`. A function whose signature does not match the declared type is a compile-time error.

**A callback runs synchronously inside the C call's stack frame**, so it **must not park or throw**: no blocking I/O, channel operations, `yield()`, or spawning-and-waiting, and no exception that escapes the callback. Doing so would corrupt the C frame, so the runtime **aborts with a clear message** rather than fail silently. Keep callbacks short and pure - compute a result and return it.

---

## Variadic functions

A C function with a variable argument list (the `printf`/`snprintf` family) is declared with a trailing `...`:

```breezy
extern int snprintf(byte[] buf, long size, string fmt, ...);

void main()
{
	byte[] buf = new byte[16];
	snprintf(buf, 16, "%d-%d", 7, 42);   // -> "7-42"
	print(fromBytes(buf).substring(0, 4));
}
```

The fixed parameters are typed normally; each variadic argument must be a scalar (`long`/`int`/`double`/...) or a `string` (marshalled to `char*`). Arrays, objects, and maps cannot pass through `...`. The compiler places the variadic arguments per the platform ABI automatically.

---

## blocking - keep the scheduler flowing

A C call that might block (a synchronous query, a slow syscall) should be marked `extern blocking`. The runtime then dispatches it to the [offload pool](../io/native-io.md): the calling breeze **parks** while a worker thread runs the call, so the scheduler core keeps serving other breezes. The marshalling is identical to a plain `extern`; `blocking` changes only **how** the call is dispatched.

```breezy
extern blocking long strlen(string s);   // Parks the breeze; a worker runs strlen.

void main()
{
	print(strlen("hello"));   // 5.
}
```

Use `blocking` for anything that could take a while (database round-trips, heavy native work) so one slow call cannot freeze every breeze on a core.

```breezy
// Example signatures for libmysqlclient.
extern long mysql_init(long ptr);
extern long mysql_real_connect(long conn, string host, string user, string pass, string db, int port);
extern blocking int mysql_real_query(long conn, string stmt, long len);
```

---

## Per-project configuration with breezy.toml

Place a `breezy.toml` next to your project to configure linking and application identity. A missing file is a no-op.

### [link] — native library references

Instead of repeating `--link` on every build, declare native libraries and search paths here:

```toml
[link]
libs = ["m", "mysqlclient"]    # Appended as -lm -lmysqlclient.
lib_paths = ["C:/libs"]         # Appended as -LC:/libs.
```

Config `libs` and any `--link` flags **compose** - both reach the linker.

### [app] — application identity and icon

Declare your application's name, version, and other metadata:

```toml
[app]
name        = "My App"
version     = "1.0.0"
description = "A short description"
author      = "Your Name"
icon        = "app.ico"
```

All fields are optional. What each field does:

- **`name`** / **`version`** / **`description`** / **`author`** — embedded in the binary on both platforms as named symbols (`bzy_app_name`, `bzy_app_version`, `bzy_app_author`, `bzy_app_description`). Readable via `strings myapp` or `readelf -p .bzy_app myapp` on Linux.
- **`icon`** — path to a `.ico` file, relative to `breezy.toml`. **Windows only.** The icon is compiled into the executable via `windres` and displayed in Explorer, the taskbar, and the Alt-Tab switcher. Also embeds a `VERSIONINFO` resource so the Properties dialog shows your name, version, and description. Silently ignored on Linux.

The `version` field is parsed as `major.minor.patch.build`; missing parts default to `0`.

---

## Why it is zero-cost

A [non-moving heap](../memory/automatic-memory.md) means an object's address never changes, so you can hand a pointer straight to C with no pinning and no copy. Breezy objects are plain structs with a small header, so there is no boxing or marshalling layer between your data and the C ABI. The call is an ordinary C-ABI call.

---

## Rules & gotchas

- **Declare each C function with `extern`** and a matching signature.
- **`string` arguments marshal to `char*`** (NUL-terminated); embedded NULs truncate.
- **Value arrays marshal to a data pointer** (a C buffer); pass `.length` for the size. Object/string arrays are rejected.
- **Returns are limited** to `void`/int-family/`long`/`float`/`double`/`bool`; a returned `char*` is declared `long` and read with `fromCString` / `fromCBytes`.
- **Callbacks**: declare the parameter as a function type `ret(types)` and pass a Breezy function by name (scalar-only signature, checked at compile time). A callback runs inside the C frame, so it must not park, yield, or throw - the runtime aborts if it does.
- **Variadic functions**: a trailing `...` accepts scalar or `string` extra arguments (not arrays/objects); the ABI placement is automatic on both platforms.
- **Process lifecycle**: read the environment with `System.getenv(name)` and wait for a termination signal with `System.awaitShutdown()` - see [System](../stdlib/system.md). (No need to bind raw `getenv`/`signal` yourself.)
- **Mark possibly-slow calls `extern blocking`** so they offload instead of stalling the core; an `extern blocking` call takes any number of arguments.
- **Link with `--link <lib>` or `breezy.toml [link]`** - the two compose.
- **`breezy.toml [app]`** embeds name/version/author/description in both platforms; `icon` is Windows-only.

---

← [File writing & logging](../io/file-writing.md) · [Back to the guide](../guide.md) · Next: [Math](../stdlib/math.md)
