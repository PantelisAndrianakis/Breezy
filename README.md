<p align="center"> <img src="https://github.com/PantelisAndrianakis/Breezy/blob/main/logo.png"/></p>

### What is Breezy?

Breezy is a statically-typed, object-oriented language with clean, familiar syntax that compiles **straight to native x86-64 assembly**. No virtual machine. No bytecode. No tracing garbage collector. Just your code, lowered to the metal - with memory and concurrency handled *for* you, automatically.

The promise is simple: **the ergonomics of a managed language, the footprint and latency of C.**

> ⚠️ **Status: in active design & construction.** The language design below is settled; the compiler is being built from the ground up. See the [Roadmap](#roadmap) for what's real today.

---

## Why Breezy is fast - and stays fast

Most "easy" languages buy their convenience with a heavy runtime: a JIT that warms up, a GC that pauses, a heap that balloons to 2–5× your live data. Breezy refuses that trade.

| What you get | How Breezy delivers it |
|---|---|
| 🚀 **Native speed** | Compiles directly to x86-64 ASM. No interpreter, no bytecode, no warm-up. What you write is what the CPU runs. |
| 🪶 **C-like memory footprint** | Automatic Reference Counting frees objects the *instant* they're done - memory tracks your live set, not a bloated GC heap. No `-Xmx`, no multi-GB baseline. |
| ⚡ **No stop-the-world pauses** | There is **no tracing GC**. Frees are deterministic and spread out - nothing freezes your loop. Built for hard deadlines. |
| 🧵 **Massive concurrency, cheap** | Lightweight coroutines ("breezes") let one process juggle tens of thousands of connections on a handful of OS threads. |
| 🔌 **Zero-cost C interop** | A non-moving heap means you hand pointers straight to C libraries - MySQL, OpenSSL, zlib - with no marshalling and no pinning. |
| ✍️ **Nothing to manage** | No `free`, no `unsafe`, no borrow checker, no lifetimes. You write logic; the compiler writes the memory and I/O plumbing. |

**The headline use case:** Breezy is being built so you can write **high-throughput enterprise software** in it - backend services and APIs that hold tens of thousands of concurrent connections, with predictable low latency (no GC pauses to spike your p99), a tight RAM footprint that cuts your hosting bill, and direct access to native database and crypto libraries. That goal drives every design decision below.

---

## Hello, Breezy

```breezy
class Greeter
{
    string name;

    void init(string who)
    {
        name = who;
    }

    void greet()
    {
        print("Hello, " + name + "!");
    }
}

void main()
{
    Greeter g;
    g = new Greeter();
    g.init("Breezy");
    g.greet();              // Prints: Hello, Breezy!
}
```

Familiar on purpose. If you've written Java or C#, you already know how to read it - and it compiles straight to a native executable with no runtime to ship.

---

## Language Features

### Object-Oriented

Classes with fields and methods, single inheritance via `extends`, and virtual dispatch through implicit vtables - all methods are virtual by default.

```breezy
class Animal
{
    int age;

    void speak()
    {
        // Base implementation.
    }
}

class Dog extends Animal
{
    void speak()
    {
        // Overrides Animal.speak.
    }
}

void main()
{
    Dog d;
    d = new Dog();
    d.speak();   // Calls Dog.speak via the vtable.
}
```

### No Primitive Wrapper Classes

`int` is a raw 64-bit integer. There is no `Integer`, no boxing, no unboxing. What you write is what runs.

```breezy
int x;
x = 42;         // A raw machine integer - nothing more.
```

### Built-in Console I/O

Reading and writing the console takes no imports and no ceremony - `print`, `input`, and `inputInt` are built into the language.

```breezy
void main()
{
    print("What is your name?");
    string name;
    name = input();             // Reads one line from stdin.

    print("Hello, " + name + "!");

    print("Enter a number:");
    int n;
    n = inputInt();             // Reads an integer.

    print(n + n);               // Also prints an int.
}
```

### Allman Braces

Allman style is the default and recommended style for Breezy - opening braces always appear on their own line. K&R is accepted for interoperability, but all standard library code and official examples use Allman.

```breezy
void main()
{
    int x;
    x = 1;
}
```

### One Class Per File - No Headers, Ever

Breezy takes the Java/C# approach to project structure, and pushes it further: **one class per `.bz` file**, where the file name matches the class (`Client.bz` → `class Client`). There are **no header files, no forward declarations, no `#include`, and no hand-written imports.** You write logic, not boilerplate.

```
src/
├── Client.bz     // Class Client.
├── Zone.bz       // Class Zone.
└── Main.bz       // The entry point: void main().
```

The compiler scans every `.bz` file in your project and gathers **all** class, field, and method signatures *before* it generates a single instruction. So `Client.bz` can freely reference `Zone` even if `Zone.bz` is compiled later - order never matters, and you never declare anything twice.

```breezy
// Client.bz - no imports, no headers; Zone is just visible.
class Client
{
    string name;
    Zone  zone;   // Forward reference to a class in another file - fine.

    void enterZone(Zone z)
    {
        zone = z;
    }
}
```

The entry point lives in its own file as a top-level `void main()`. Every other file is exactly one class.

---

## Automatic Memory - C Footprint, Zero Bookkeeping

You never write `free`. You never think about ownership. And you still don't pay for a tracing garbage collector. Breezy gets there with **three cooperating layers**, applied automatically by the compiler:

**1. Escape analysis - the fast path.**
Objects that don't outlive their scope are **allocated on the stack**, with no heap cost and no reference counting at all. Per-tick scratch objects, network packets, temporaries - the high-churn stuff a server allocates millions of times - never touch the heap. This is the C-like core.

```breezy
void tick()
{
    Vector2 delta;          // Does not escape → stack-allocated, free.
    delta = new Vector2();  // No malloc, no refcount, gone at scope exit.
}
```

**2. Automatic Reference Counting - for objects that escape.**
Anything that outlives its scope (a `Client` stored in a map, world state) gets a small reference count. The compiler inserts the retain/release operations for you and frees the object the moment its last reference drops. Memory is reclaimed **deterministically** - no pause, no heap headroom, no GC thread.

**3. A cycle-collecting backstop - so "automatic" stays honest.**
Reference counting alone leaks reference *cycles* (an order that points at a customer who points back at their orders). A periodic, incremental cycle collector cleans those up in the background - **without you annotating a single weak reference.** Everything stays automatic.

The result: a steady-state memory profile close to hand-written C, with the convenience of a managed language and none of the pauses.

### Memory layout

```
offset  0:   vtable pointer  (8 bytes)
offset  8:   reference count (8 bytes)
offset 16:   field 0         (8 bytes)
offset 24:   field 1         (8 bytes)
...
```

The heap is **non-moving**: an object's address never changes for its lifetime. That's what makes zero-cost C interop possible (below).

---

## Concurrency - Thousands of Clients, a Handful of Threads

Breezy's concurrency model is built for exactly one thing first: a backend service that holds tens of thousands of live connections *and* runs scheduled work that must finish on time.

### Breezes (lightweight coroutines)

A **breeze** is a coroutine with its own stack, multiplexed M:N onto one scheduler thread per CPU core. You write **plain, blocking-looking code** - the runtime transparently parks a breeze when it waits on I/O and resumes it when the data arrives. No `async`/`await`, no callbacks, no function coloring.

```breezy
void handleClient(Socket sock)
{
    while (true)
    {
        Packet p;
        p = sock.read();     // Looks blocking - actually yields the breeze.
        handle(p);
    }
}

void main()
{
    Listener l;
    l = listen(7777);
    while (true)
    {
        Socket sock;
        sock = l.accept();
        spawn handleClient(sock);   // One cheap breeze per connection.
    }
}
```

A breeze stack is tens of KB, not the megabytes an OS thread costs. **10,000 concurrent connections ≈ a few hundred MB of stacks**, instead of tens of GB.

### Channels

Breezes communicate by passing values over channels - *share memory by communicating*:

```breezy
chan<Packet> inbox;
inbox = new chan<Packet>();

spawn producer(inbox);
Packet p;
p = inbox.recv();
```

### The zone model (and why it's fast)

The recommended architecture shards your state into **zones** (say, per tenant, account, or region), each owned by a single breeze that exclusively owns its data. **No locks inside a zone.** Cross-zone interactions go through channels. This scales a service across all your cores by *zone*, with near-zero contention - and it lets the compiler keep reference counts **non-atomic** on the hot path, paying the atomic cost only for objects that actually cross breezes.

---

## Native I/O - Synchronous to Write, Asynchronous Underneath

Every I/O call in Breezy *looks* blocking and *behaves* non-blocking. One facade, three backends - and your scheduler thread never stalls.

- **Network** rides epoll (Linux) / IOCP (Windows) / kqueue (BSD). A breeze waiting on a socket parks and frees the core for others.
- **Files** can't be polled reliably, so file reads/writes are dispatched to a small **offload thread pool** - the breeze yields, a worker does the blocking work, the breeze resumes. (On modern Linux, an `io_uring` backend slots in under the same API for true async file I/O.)
- **Blocking C calls** (a synchronous `mysql_query`) use that *same* offload pool, so one slow database round-trip can't freeze every client on a core.

### File writing done right

Writes are **buffered by default** - small writes coalesce into far fewer syscalls. And logging is a channel, not a syscall: game breezes `send` log lines to a dedicated logger breeze that flushes them off the hot path. **A tick never waits on disk.**

```breezy
log.send("request " + reqId + " from " + user + " completed");  // Enqueue, don't block.
```

---

## C Library Interop - Zero-Cost FFI

Because the heap is non-moving and Breezy objects are plain structs, you call C libraries directly - no wrappers, no marshalling, no pinning. Declare the signature with `extern`, and the linker does the rest.

```breezy
// Link against libmysqlclient.
extern long  mysql_init(long ptr);
extern long  mysql_real_connect(long conn, string host, string user,
                                string pass, string db, int port);
extern blocking int mysql_real_query(long conn, string stmt, long len);
```

The `blocking` keyword tells the runtime a call may block, so it's dispatched to the offload pool and your breezes keep flowing. Native libraries to link are declared per-project (`breezy.toml`) or with `--link mysqlclient`.

---

## Type System (v1)

| Type | Description |
|---|---|
| `int` | 64-bit signed integer |
| `string` | UTF-8 string with Small String Optimization; mutable via `+=` |
| `int[]` | Heap-allocated array of `int`, bounds-checked |
| `string[]` | Heap-allocated array of `string`, bounds-checked |
| `int[string]` | Hash map: string keys → int values (Swiss Table) |
| `string[string]` | Hash map: string keys → string values (Swiss Table) |
| `chan<T>` | Channel for passing values between breezes |
| `ClassName` | Heap-allocated object, memory managed automatically (ARC + cycles) |
| `void` | No value; used as a function return type |

Generics and floating-point types are planned for future versions.

### Strings

Breezy strings use **Small String Optimization (SSO)**. Strings of 22 bytes or fewer are stored entirely inline - no heap allocation. Longer strings store a heap pointer, length, and capacity. The `string` value is 24 bytes regardless of content.

```
Short string (len ≤ 22):       Long string (len > 22):
┌──────────────────────┬───┐   ┌─────────┬─────┬─────┐
│  inline data [22]    │len│   │   ptr   │ len │ cap │
└──────────────────────┴───┘   └─────────┴─────┴─────┘
0                     22  23   0         8    16    24
```

Concatenation with `+` is a **single allocation** - the compiler sums the lengths, allocates once, and copies each part. For accumulation in a loop, `+=` reuses the buffer and doubles capacity when full, so `n` appends cost O(n) total, not O(n²).

```breezy
string log;
log = "";
while (i < count)
{
    log += parts[i];   // O(1) amortized - no alloc unless capacity exceeded.
    i = i + 1;
}
```

---

## Control Flow

```breezy
// If / else.
if (x < 10)
{
    x = x + 1;
}
else
{
    x = 0;
}

// While.
while (i < 100)
{
    i = i + 1;
}

// Return.
int twice()
{
    int result;
    result = x * 2;
    return result;
}
```

---

## Compilation Pipeline

```
source.bz → Breezy compiler → output.asm → NASM → output.o ──┐
                                                             ├─ GCC ─→ native binary
                              Breezy runtime (libbreezy.a) ──┘
```

The Breezy compiler is written in C99 with no external dependencies. It parses to a typed AST, performs escape and reference analysis, then lowers to NASM x86-64 assembly. The compiled program links against a small **Breezy runtime** (`libbreezy.a`) providing the allocator, ARC + cycle collector, the breeze scheduler, channels, and the async I/O facade - plus whatever native C libraries you declared.

**Linux (ELF64):**
```sh
breezy app.bz
./app
```

**Windows (PE64):**
```sh
breezy app.bz --target windows
app.exe
```

---

## Building the Compiler

**One command - the bootstrap script installs everything it needs** (GCC/MinGW, NASM, GNU Make) and then builds:

```sh
# Windows (normal, non-admin prompt) - installs deps via scoop
build.bat

# Linux / macOS - installs deps via apt/dnf/pacman/zypper/apk or Homebrew
./build.sh
```

Already have the toolchain? Build directly:

```sh
make
./breezy --help
```

Requirements (handled automatically by the scripts): GCC (or MinGW-w64 on Windows), NASM, GNU Make.

---

## Roadmap

The language design is settled. The compiler and runtime are being built from scratch. The compiler core (Part 1) is complete: Breezy `.bz` source compiles to native Windows executables today.

**Compiler core**
- [x] Lexer, parser, typed AST
- [x] Type table: classes, inheritance, virtual dispatch
- [x] x86-64 codegen (Windows PE64)
- [ ] x86-64 codegen (Linux ELF64)

**Memory**
- [ ] Escape analysis → stack allocation
- [ ] Automatic Reference Counting
- [ ] Incremental cycle collector

**Core types**
- [ ] Strings with SSO and amortized `+=`
- [ ] Arrays (`int[]`, `string[]`) with bounds checks
- [ ] Maps (Swiss Table + wyhash)

**Concurrency & I/O**
- [ ] Breeze scheduler (M:N, one thread per core)
- [ ] Channels
- [ ] Async I/O facade (epoll/IOCP + offload pool; `io_uring` later)

**Interop**
- [ ] `extern` C FFI with `blocking` dispatch

**Beyond v1**
- [ ] Interfaces
- [ ] Generics
- [ ] Floating-point types
- [ ] Standard library
- [ ] Self-hosting compiler
