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

**Returns** are limited to `void`, the int family, `long`, `float`, `double`, and `bool`. A C function that returns `char*` is declared to return `long` and wrapped by hand for now. An embedded NUL in a `string` argument truncates on the C side, since C strings end at the first NUL.

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
- **Returns are limited** to `void`/int-family/`long`/`float`/`double`/`bool`; treat returned `char*` as `long`.
- **Mark possibly-slow calls `extern blocking`** so they offload instead of stalling the core.
- **Link with `--link <lib>` or `breezy.toml [link]`** - the two compose.
- **`breezy.toml [app]`** embeds name/version/author/description in both platforms; `icon` is Windows-only.

---

← [File writing & logging](../io/file-writing.md) · [Back to the guide](../guide.md) · Next: [Math](../stdlib/math.md)
