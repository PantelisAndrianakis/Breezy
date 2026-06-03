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

**Constructors.** A `ClassName(params) { ... }` member initializes fields at construction; call it with `new Class(args)`.

```breezy
class Point
{
    int x;
    int y;
    Point(int x, int y)
    {
        this.x = x;
        this.y = y;
    }
}

Point p;
p = new Point(3, 4);   // x=3, y=4
```

### Built-in Vector Types

Eight ready-made vector types ship with the language — `Vector2i` / `Vector2l` / `Vector2f` / `Vector2d` (fields `x`, `y`) and `Vector3i` / `Vector3l` / `Vector3f` / `Vector3d` (fields `x`, `y`, `z`) — each with a constructor, `equals` (component-wise), and `calculateDistance` (euclidean). They're ordinary classes, so a non-escaping vector local is stack-allocated (no heap, no reference counting).

```breezy
Vector3f a;
a = new Vector3f(0.0f, 0.0f, 0.0f);
Vector3f b;
b = new Vector3f(2.0f, 3.0f, 6.0f);
print(a.calculateDistance(b));   // 7   (float result for *f)
print(a.equals(b));              // false

Vector2i p;
p = new Vector2i(3, 4);
Vector2i q;
q = new Vector2i(0, 0);
print(q.calculateDistance(p));   // 5   (double result for integer vectors)
```

`calculateDistance` returns `float` for the `*f` variants and `double` for the integer and `*d` variants (integer distances are irrational, so they promote to `double`); `equals` is exact component-wise comparison.

### No Primitive Wrapper Classes

`int` is a raw 32-bit machine integer (use `long` for 64-bit). There is no `Integer`, no boxing, no unboxing. What you write is what runs.

```breezy
int x;
x = 42;         // A raw machine integer - nothing more.
long big;
big = 10000000000L;   // 64-bit when you need it.
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

Breezy takes the Java/C# approach to project structure, and pushes it further: **one class per `.bzy` file**, where the file name matches the class (`Client.bzy` → `class Client`). There are **no header files, no forward declarations, no `#include`, and no hand-written imports.** You write logic, not boilerplate.

```
src/
├── Client.bzy     // Class Client.
├── Zone.bzy       // Class Zone.
└── Main.bzy       // The entry point: void main().
```

The compiler scans every `.bzy` file in your project and gathers **all** class, field, and method signatures *before* it generates a single instruction. So `Client.bzy` can freely reference `Zone` even if `Zone.bzy` is compiled later - order never matters, and you never declare anything twice.

```breezy
// Client.bzy - no imports, no headers; Zone is just visible.
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

> **Implemented today (Part 6a-1):** the breeze runtime and a **single-thread cooperative scheduler** are live. `spawn f();` enqueues a breeze running the zero-argument `void` function `f`; `yield();` hands control back to the scheduler so ready breezes interleave. `main` itself is breeze 0, so it interleaves with what it spawns, and a program that never `spawn`s runs unchanged. Stacks are Windows Fibers behind a portable seam (Linux lands in Part 8). Still to come: **channels** (`channel<T>`, cross-breeze communication — 6a-2), **multi-core** scheduling with atomic refcounts for shared objects (6a-3), and the I/O integration that parks a breeze on a blocking call (Part 6b). Today `yield()` is explicit; the auto-yield-on-I/O shown above is the roadmap.

```breezy
void a()
{
    print(1);
    yield();        // Hand off to the scheduler; resumes here later.
    print(3);
}

void b()
{
    print(2);
    yield();
    print(4);
}

void main()        // Breeze 0.
{
    spawn a();
    spawn b();
    yield();
    print(9);
}
// Deterministic FIFO interleaving: 1 2 9 3 4
```

### Channels

Breezes communicate by passing values over channels - *share memory by communicating*. A `channel<T>` is **bounded buffered**: `new channel<T>(N)` reserves a ring of `N` slots, `send` parks the breeze when the ring is full, and `recv` parks when it is empty - so a slow consumer applies **backpressure** to its producer instead of letting an unbounded queue grow until the server runs out of memory. A spawned worker takes its channel (and up to four arguments of any type) directly.

```breezy
void producer(channel<int> out)
{
    out.send(10);
    out.send(20);
    out.send(30);
}

void main()
{
    channel<int> c;
    c = new channel<int>(2);     // capacity 2: the 3rd send parks until main drains
    spawn producer(c);

    int total;
    total = 0;
    int i;
    for (i = 0; i < 3; i++)
    {
        total += c.recv();       // parks while the channel is empty
    }
    print(total);                // 60
}
```

> **Implemented today (Part 6a-2):** bounded `channel<T>` with `send`/`recv` (parking on full/empty), `spawn` with up to 4 arguments, and a deadlock check (if every breeze ends up blocked the program aborts with a diagnostic). Managed values **move** across a channel - the owned reference transfers from sender to receiver with no extra retain. Still to come: **multi-core** scheduling with atomic refcounts for objects that cross breezes (6a-3), and the I/O integration that parks a breeze on a blocking call (Part 6b).

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
| `byte` `short` `int` `long` | Signed integers: 8, 16, **32**, 64-bit (two's complement) |
| `ubyte` `ushort` `uint` `ulong` | Unsigned integers: 8, 16, 32, 64-bit |
| `float` `double` | IEEE-754 floating point: 32, 64-bit |
| `boolean` | `true` / `false` |
| `string` | UTF-8 text; concatenate with `+` / `+=` |
| `T[]` | Heap-allocated array of any type, bounds-checked |
| `map<K,V>` | Hash map: open addressing with Swiss-style control bytes (`int`/`string` keys) |
| `List<T>` `Stack<T>` `Queue<T>` `Deque<T>` `Set<T>` | Monomorphized generic collections (no boxing); `.size`, `.contains(T)`, `foreach` |
| `channel<T>` | Bounded buffered channel for passing values between breezes (`send`/`recv`, parking) |
| `ClassName` | Heap-allocated object, memory managed automatically (ARC + cycles) |
| `void` | No value; used as a function return type |

`int` is 32-bit; use `long` for 64-bit. There is no unsigned floating point. The generic
collections (`List`/`Stack`/`Queue`/`Deque`/`Set`) are **stdlib-only and monomorphized** —
specialized per element type with no boxing. User-defined generics (`class Foo<T>`) are a
future version.

### Strings

A `string` is an immutable, heap-allocated, length-prefixed UTF-8 value held by a single 8-byte reference - the characters live inline with the object header in one allocation, so there is one `malloc` and one `free` per string. Strings are ARC-managed like any other object.

Concatenation with `+` allocates one new string sized to the sum of its parts. For heavy accumulation in a loop, use a **`StringBuilder`**, which appends into a doubling buffer so `n` appends cost O(n) total, not O(n²), then snapshots to an immutable `string` with `toString()`.

```breezy
StringBuilder sb;
sb = new StringBuilder();
sb.append("Hello, ");
sb.append("Breezy");
print(sb.toString());          // Hello, Breezy
```

Strings carry a full set of methods. Because strings are immutable, every transforming method returns a **new** string - the receiver is never changed.

```breezy
string path;
path = "/api/users";

boolean ok;
ok = path.startsWith("/api");   // true
int at;
at = path.indexOf("users");     // 5
string up;
up = path.toUpper();            // /API/USERS  (path itself unchanged)

string[] parts;
parts = "red,green,blue".split(",");
print(parts.length);            // 3
foreach (string p in parts)
{
    print(p);                   // red / green / blue
}
```

- **Query:** `length()`, `isEmpty()`, `equals(o)`, `equalsIgnoreCase(o)`, `contains(x)`, `startsWith(p)`, `endsWith(p)`, `indexOf(x)`, `lastIndexOf(x)`, `charAt(i)` (returns the byte as an `int`, or `-1` out of range).
- **Transform (return a new string):** `substring(a, b)`, `replace(from, to)`, `repeat(n)`, `trim()`, `toUpper()`, `toLower()`.
- **`split(sep)`** returns a `string[]` you can iterate with `foreach`.

### Arrays

Fixed-length, heap-allocated, and **bounds-checked** - an out-of-range index aborts rather than reading stray memory. Element loads are width-correct, and arrays of objects participate in ARC and the cycle collector.

```breezy
int[] squares;
squares = new int[5];
int i;
i = 0;
while (i < squares.length)
{
    squares[i] = i * i;
    i = i + 1;
}
```

### Maps

`map<K,V>` is an open-addressing hash table (Swiss-style control bytes) keyed by `int` or `string`, with any value type. `put` / `get` / `containsKey` / `containsValue` / `remove` / `.size`; managed keys and values are retained and released automatically, and a map caught in a reference cycle is reclaimed by the cycle collector.

```breezy
map<string,int> counts;
counts = new map<string,int>();
counts.put("apples", 3);
counts.put("apples", counts.get("apples") + 1);
print(counts.containsKey("pears"));    // false
print(counts.containsValue(4));        // true
print(counts.size);                    // 1
```

**Views and iteration.** `getKeys()` / `getValues()` return owned `K[]` / `V[]` snapshots, and `getEntries()` returns an `Entry[]` whose elements expose `getKey()` / `getValue()`. You can also iterate keys and values together with the `foreach (k, v in m)` pair form (the keys-only `foreach (k in m)` form still works).

```breezy
foreach (string name, int count in counts)
{
    print(name);
    print(count);
}

foreach (int v in counts.getValues())
{
    print(v);
}

foreach (Entry e in counts.getEntries())
{
    print(e.getKey());
    print(e.getValue());
}
```

### Generic Collections — No Boxing

`List`, `Stack`, `Queue`, `Deque`/`ArrayDeque`, and `Set` are **monomorphized per element type**: a `List<int>` stores raw 32-bit integers inline; a `List<Dog>` stores pointers with automatic reference counting. There is **no boxing** — primitives never get wrapped onto the heap. Every collection has `.size`, `.contains(T)`, and works with `foreach`.

```breezy
List<int> nums;
nums = new List<int>();
nums.add(10);
nums.add(20);
nums.add(30);
print(nums.size);          // 3
print(nums.contains(20));  // true

int sum;
sum = 0;
foreach (int n in nums)
{
    sum = sum + n;
}
print(sum);                // 60

Set<string> seen;
seen = new Set<string>();
seen.add("a");
seen.add("a");             // deduped
print(seen.size);          // 1
```

A growable vector (doubling) backs `List`/`Stack`; a ring buffer over it backs `Queue`/`Deque`; `Set` reuses the hash table. Managed elements are retained on insert and released on removal, and a collection caught in a reference cycle is reclaimed by the cycle collector — all with no per-element allocation overhead for primitives.

### Math

`Math` is a built-in static namespace. The common operations inline to a few SSE instructions — no call, no boxing — and `int` `min`/`max`/`clamp`/`abs` stay on the integer unit; only the transcendental functions defer to libm.

```breezy
print(Math.sqrt(16.0));         // 4
print(Math.max(3, 7));          // 7 (integer, no FP)
print(Math.clamp(value, 0, 100));
double r;
r = Math.cos(Math.toRadians(60.0));   // ~0.5
print(Math.pow(2.0, 10.0));     // 1024
```

Inlined to SSE: `min` `max` `clamp` `abs` `sqrt` `floor` `ceil` `round` `toRadians`. Via libm: `cos` `tan` `exp` `pow`.

### Clock & Random

`Clock` exposes monotonic and wall-clock time; `Random` is a fast `xoshiro256**` PRNG mirroring a familiar API.

```breezy
long start;
start = Clock.currentTimeNanos();

int roll;
roll = Random.get(1, 6);        // [1, 6] inclusive
double d;
d = Random.nextDouble();        // [0, 1)
boolean flip;
flip = Random.nextBoolean();
```

`Random`: `get(bound)` / `get(origin, bound)` for `int`/`long`/`float`/`double`, plus `nextInt`/`nextLong`/`nextFloat`/`nextDouble`/`nextBoolean`/`nextGaussian`/`nextBytes`.

`Clock.getDateString` turns epoch milliseconds into a local-time date string — a fixed ISO default, or a Java-style pattern.

```breezy
long now;
now = Clock.currentTimeMillis();

string iso;
iso = Clock.getDateString(now);                       // 2026-06-02 14:30:09
string custom;
custom = Clock.getDateString(now, "yyyy/MM/dd HH:mm");  // 2026/06/02 14:30
```

Pattern tokens: `yyyy` `yy` `MM` `dd` `HH` (24h) `hh` (12h) `mm` `ss` `SSS` (millis) `a` (AM/PM); any other character is copied literally.

### Regex

`Regex` is a hand-written **Thompson NFA / Pike VM** — linear-time matching with no catastrophic backtracking (ReDoS-safe), which matters when matching untrusted input on a server.

```breezy
boolean ok;
ok = Regex.matches("a+b", "aaab");          // full match -> true
boolean found;
found = Regex.test("[0-9]+", "abc123");     // search -> true

string hit;
hit = Regex.find("[0-9]+", "abc123def");    // leftmost match -> "123"
string masked;
masked = Regex.replace("[0-9]+", "a1b22c333", "#");   // -> "a#b#c#"
```

- `matches(pattern, text)` (full) and `test(pattern, text)` (search) return `boolean`; `find` returns the leftmost match substring (`""` if none); `replace` replaces every non-overlapping match.
- Syntax: literals, `.`, `*` `+` `?`, alternation `|`, grouping `()`, anchors `^` `$`, classes `[a-z]`/`[^...]`, escapes `\d \D \w \W \s \S`, and bounded repetition `{n}` / `{n,}` / `{n,m}`.
- Patterns are runtime strings, so they can be built and passed dynamically.

### File

`File` is a static namespace for filesystem work. Predicates (`exists`/`isFile`/`isFolder`) return `boolean`; everything else throws an `IOException` on failure, so errors surface where they happen instead of silently corrupting state.

```breezy
File.createFolder("data/logs");           // mkdir -p
File.writeText("data/note.txt", "hello\nworld\n");
File.appendText("data/note.txt", "again\n");

print(File.exists("data/note.txt"));      // true
print(File.readText("data/note.txt"));    // the file's contents
foreach (string line in File.readLines("data/note.txt")) { print(line); }

// Glob search (in-folder and recursive), returning full paths.
foreach (string p in File.search("data", "*.txt")) { print(p); }
string[] all;
all = File.searchRecursive("data", "*");

// Binary I/O.
byte[] bytes;
bytes = File.readBytes("data/note.txt");
File.writeBytes("data/copy.bin", bytes);

// Windows attributes (read-only / hidden / system / archive).
File.setAttribute("data/note.txt", File.READONLY, true);
print(File.hasAttribute("data/note.txt", File.READONLY));   // true
File.setAttribute("data/note.txt", File.READONLY, false);

try
{
    File.delete("data/missing.txt");
}
catch (IOException e)
{
    print("delete failed");
}

File.deleteRecursive("data");             // remove the tree
```

- **Create/delete:** `createFile`, `createFolder` (creates intermediate folders), `delete` (a file or empty folder), `deleteRecursive` (a folder tree).
- **Read/write:** `readText`/`readLines` and `writeText`/`appendText` for text; `readBytes`/`writeBytes` for `byte[]`.
- **Search:** `list(folder)`, `search(folder, glob)`, `searchRecursive(folder, glob)` → `string[]` of full paths (glob `*`/`?`).
- **Attributes:** `setAttribute(path, attr, on)` / `hasAttribute(path, attr)` with the `File.READONLY` / `File.HIDDEN` / `File.SYSTEM` / `File.ARCHIVE` constants.
- Calls are **synchronous** today; hot-path file I/O moves onto the offload pool (Part 6b) without changing this surface.

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

// C-style for, with ++ / --.
for (int j = 0; j <= 10; j++)
{
    sum = sum + j;
}

// foreach over arrays, strings, maps, and collections.
foreach (int n in nums)
{
    sum = sum + n;
}

// break / continue work in while, for, and foreach.
foreach (int n in nums)
{
    if (n == 0)
    {
        continue;
    }

    if (n > 100)
    {
        break;
    }
}

// switch — C-style fallthrough, default; dense cases lower to a jump table.
switch (code)
{
    case 1:
    case 2:
        handleLow();      // 1 and 2 share this body
        break;
    case 3:
        handleThree();
        // falls through into default unless a break is added
    default:
        handleOther();
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

## Exceptions

Java-style `throw` / `try` / `catch` with an exception hierarchy and stack traces. The implementation is **zero-cost when nothing is thrown** — entering a `try` emits no instructions; the only cost is the stack walk at throw time. It is homegrown (static per-function EH side tables + an `rbp`-chain-walking unwinder), not OS SEH.

```breezy
class NotFound extends Exception { }   // user exceptions extend the builtin root

void lookup(int id)
{
    if (id < 0)
    {
        throw new Exception("bad id");
    }

    int[] table;
    table = new int[3];
    print(table[id]);                  // out-of-range throws a builtin IndexOutOfBounds
}

void main()
{
    try
    {
        lookup(5);
    }
    catch (IndexOutOfBounds e)         // first matching clause wins
    {
        print(e.message);             // "array index 5 out of bounds for length 3"
    }
    catch (Exception e)                // catches any subclass via is-a matching
    {
        print(e.message);
    }
}
```

- **`throw expr;`** — the operand must be an `Exception` (or subclass).
- **`try { } catch (Type e) { } ...`** — one or more `catch` clauses; the first whose type matches the thrown object (by **is-a**, walking the class hierarchy) wins. A `try` whose clauses don't match keeps unwinding to an outer handler.
- **Builtins:** `Exception` (root, with a `string message`) and `IndexOutOfBounds` (thrown by out-of-range array indexing). User classes `extends Exception`.
- **Uncaught** exceptions print `Uncaught exception: <message>` plus a function-name stack trace, then abort.
- **ARC-correct while unwinding:** object locals of abandoned frames are released; the thrown object survives the unwind and is freed once the handler's scope exits.

---

## Compilation Pipeline

```
source.bzy → Breezy compiler → output.asm → NASM → output.o ─┐
                                                             ├─ GCC ─→ native binary
                              Breezy runtime (lib_breezy.a) ─┘
```

The Breezy compiler is written in C99 with no external dependencies. It parses to a typed AST, performs escape and reference analysis, then lowers to NASM x86-64 assembly. The compiled program links against a small **Breezy runtime** (`lib_breezy.a`) providing the allocator, ARC + cycle collector, the breeze scheduler, channels, and the async I/O facade - plus whatever native C libraries you declared.

**Linux (ELF64):**
```sh
breezy app.bzy
./app
```

**Windows (PE64):**
```sh
breezy app.bzy --target windows
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

The language design is settled. The compiler and runtime are being built from scratch. Parts 1–3 are complete and green, and Part 4 has delivered core types **and the full collection library**: Breezy `.bzy` source compiles to native Windows executables today, with automatic memory management (escape analysis, ARC, and an incremental cycle collector), the full scalar type system (sized signed/unsigned integers, `boolean`, and IEEE-754 `float`/`double`), the reference types `string` (+ `StringBuilder`), arrays (`T[]`), and `map<K,V>`, full control flow (`if`/`else`, `while`, `for`, `foreach`, `switch`, `break`/`continue`, `++`/`--`), and **no-boxing generic collections** — `List`/`Stack`/`Queue`/`Deque`/`Set` over a monomorphizing mechanism — all ARC- and cycle-collector-aware. **Parts 4 and 5 are complete** — Part 5 added block comments, compound assignment, the `Math`/`Clock`/`Random` namespaces, and full **exceptions** (`throw` / `try` / `catch` with an exception hierarchy, is-a matching, and zero-cost-when-not-thrown table-based unwinding). Part 6 (concurrency & I/O) is next.

**Compiler core (Part 1)**
- [x] Lexer, parser, typed AST
- [x] Type table: classes, inheritance, virtual dispatch
- [x] x86-64 codegen (Windows PE64)

**Memory & runtime (Part 2)**
- [x] Escape analysis → stack allocation
- [x] Automatic Reference Counting
- [x] Incremental cycle collector

**Scalar types (Part 3)**
- [x] Sized integers `byte`/`short`/`int`/`long` + unsigned `ubyte`/`ushort`/`uint`/`ulong`
- [x] `boolean` (`true`/`false`)
- [x] `float` / `double` (IEEE-754, SSE path)

**Core types & collections (Part 4)**
- [x] Strings with amortized append (`StringBuilder`)
- [x] Arrays (`T[]`) with bounds checks
- [x] Maps (`map<K,V>`, open addressing, ARC + cycle-collected)
- [x] Loop control: `for`, `break`/`continue`, `++`/`--`
- [x] `foreach` loop + iterator protocol (arrays, strings, maps, collections)
- [x] Stdlib-only monomorphized generics (specialized per element type, no boxing)
- [x] Generic collections — `List` / `Stack` / `Queue` / `Deque` / `Set`, holding primitives or objects, each with `.contains()`
- [x] `switch` (C-style fallthrough, `default`, jump-table lowering for dense cases)

**Language & standard-library essentials (Part 5)**
- [x] `/* */` block comments + compound assignment (`+=` `-=` `*=` `/=`)
- [x] `Math` (SSE-inlined `min`/`max`/`clamp`/`abs`/`round`/`floor`/`ceil`/`sqrt`/`toRadians`; libm `cos`/`tan`/`exp`/`pow`)
- [x] `Clock.currentTimeMillis()` / `currentTimeNanos()` / `getDateString(millis[, format])`
- [x] String methods (`contains`/`startsWith`/`indexOf`/`substring`/`replace`/`split`/`trim`/...)
- [x] `Regex` (`matches`/`test`/`find`/`replace`; Thompson NFA / Pike VM, linear-time, ReDoS-safe)
- [x] `Random` (fast PRNG: `get`/`next*`/`nextGaussian`/`nextBytes`)
- [x] Exceptions: `try`/`catch`/`throw` + stack traces (multiple clauses, is-a matching, user `extends Exception`, builtin `IndexOutOfBounds`; zero-cost-when-not-thrown)
- [x] Map views: `containsKey` (replaces `has`) / `containsValue`, `getKeys`/`getValues` → `K[]`/`V[]`, `getEntries` → `Entry[]` (`getKey`/`getValue`), `foreach (k, v in m)`
- [x] `File` namespace: exists/create/delete, read/write text + binary, glob search, Windows attributes (throws `IOException`)
- [x] Constructor arguments (`new Class(args)`) + eight built-in vector types (`Vector2/3 × i/l/f/d`: `equals`, `calculateDistance`)

**Concurrency & I/O (Part 6)**
- [x] Breezes + cooperative scheduler — `spawn` / `yield` (single thread; Windows Fibers behind a portable seam)
- [x] `spawn` with arguments (up to 4, any type) + bounded `channel<T>` (`send`/`recv` with parking, deadlock detection)
- [ ] Multi-core scheduler (one thread per core) + atomic refcounts for shared objects
- [ ] Async I/O facade (epoll/IOCP + offload pool; `io_uring` later)

**Interop (Part 7)**
- [ ] `extern` C FFI with `blocking` dispatch

**Other targets & beyond**
- [ ] x86-64 codegen (Linux ELF64)
- [ ] Interfaces, user-definable generics (`class Foo<T>`), extended standard library
- [ ] Self-hosting compiler
