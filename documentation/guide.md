# The Breezy Language Guide

Welcome to Breezy - a statically-typed, object-oriented language that compiles straight to native x86-64 assembly, with memory and concurrency handled for you automatically.

This guide teaches the whole language one feature at a time. Each page is self-contained: it explains the concept, the exact syntax, complete worked examples, the rules the compiler enforces, and the gotchas to avoid. If you have written Java or C#, most of it will feel familiar; if you are new to programming, start at the top and work down.

> **New here?** Read [What is Breezy?](../README.md) first for the high-level picture, then come back.

---

## 1. Language basics

| Page | What it covers |
| --- | --- |
| [Classes & objects](language/classes.md) | Classes, fields, methods, single inheritance, virtual dispatch, `getClassName`, constructors. |
| [Interfaces](language/interfaces.md) | Abstract contracts, `implements`, polymorphism without inheritance, zero-overhead dispatch. |
| [Generics](language/generics.md) | User-defined parametric classes, multiple type parameters, interface bounds, monomorphization. |
| [Enums](language/enums.md) | Singleton constants, per-constant fields/constructors/bodies, `values`/`valueOf`/`name`/`ordinal`. |
| [Static members & static classes](language/static-members.md) | `static` fields and methods, `static class`, field initializers, the singleton pattern. |
| [Built-in vector types](language/vector-types.md) | The eight `Vector2/3 x i/l/f/d` types: `equals`, `calculateDistance`. |
| [No primitive wrapper classes](language/no-wrapper-classes.md) | Raw machine integers, no boxing, `int` vs `long`. |
| [Console I/O](language/console-io.md) | Built-in `print`, `input`, `inputInt` - no imports needed. |
| [One class per file](language/one-class-per-file.md) | Project structure, no headers, no imports, order-independent compilation. |
| [Allman braces & style](language/allman-braces.md) | The official brace and formatting style. |
| [Control flow](language/control-flow.md) | `if`/`else`, `while`, `for`, `foreach`, `switch`, `break`/`continue`, `return`. |
| [Exceptions](language/exceptions.md) | `throw`/`try`/`catch`, the exception hierarchy, is-a matching, zero-cost-when-not-thrown. |

## 2. The type system

| Page | What it covers |
| --- | --- |
| [Type system overview](types/overview.md) | Every built-in type at a glance, and the v1 rules. |
| [Strings](types/strings.md) | Immutable UTF-8 strings, `StringBuilder`, the full string method set, parsing to numbers. |
| [Arrays](types/arrays.md) | Fixed-length, bounds-checked heap arrays. |
| [Maps](types/maps.md) | `map<K,V>` hash maps, views, and key/value iteration. |
| [Generic collections](types/collections.md) | `List`/`Stack`/`Queue`/`Deque`/`Set`, monomorphized with no boxing. |
| [Records](types/records.md) | Final classes with synthesized value `equals`/`hashCode`, for value keys. |

## 3. Automatic memory

| Page | What it covers |
| --- | --- |
| [Automatic memory](memory/automatic-memory.md) | Escape analysis, Automatic Reference Counting, the cycle collector, and the object layout. |

## 4. Concurrency

| Page | What it covers |
| --- | --- |
| [Breezes](concurrency/breezes.md) | Lightweight coroutines, `spawn`, `yield`, the M:N scheduler. |
| [Channels](concurrency/channels.md) | Bounded buffered `channel<T>`, `send`/`recv`, backpressure. |
| [The zone model](concurrency/zone-model.md) | Sharding state into zones for lock-free, contention-free scaling. |

## 5. Input / output

| Page | What it covers |
| --- | --- |
| [Native I/O](io/native-io.md) | Synchronous-looking, asynchronous-underneath networking; the echo server; `Network.readUrl`. |
| [File writing & logging](io/file-writing.md) | Buffered `FileWriter` and the channel-fed `Logger`. |

## 6. C interoperability

| Page | What it covers |
| --- | --- |
| [C library interop (FFI)](ffi/c-interop.md) | `extern` declarations, the marshalling contract, `blocking` dispatch, per-project linking. |

## 7. Standard library

| Page | What it covers |
| --- | --- |
| [Math](stdlib/math.md) | The `Math` namespace - SSE-inlined and libm operations. |
| [Clock](stdlib/clock.md) | Time and dates - monotonic durations and wall-clock formatting. |
| [Random](stdlib/random.md) | The fast `xoshiro256**` pseudo-random number generator. |
| [Regex](stdlib/regex.md) | The linear-time, ReDoS-safe `Regex` engine. |
| [File](stdlib/file.md) | The `File` namespace - filesystem work with `IOException`. |
| [System](stdlib/system.md) | The `System` namespace - launching OS commands, reading arguments. |

## 8. Building & shipping

| Page | What it covers |
| --- | --- |
| [Compilation pipeline](build/compilation.md) | How `.bzy` source becomes a native binary. |
| [Building the compiler](build/building.md) | Bootstrap scripts, the toolchain, and `--target` selection. |

---

*This guide is part of the [Breezy](../README.md) project.*
