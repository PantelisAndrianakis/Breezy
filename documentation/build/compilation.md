# Compilation Pipeline

Breezy compiles your `.bzy` source **straight to native x86-64 assembly**, assembles it, and links it against a small runtime to produce a native executable. There is no virtual machine, no bytecode, and no JIT - what you write becomes what the CPU runs.

← [Back to the guide](../guide.md)

---

## The pipeline

```
source.bzy → Breezy compiler → output.asm → NASM → output.o ─┐
                                                             ├─ GCC ─→ native binary
                              Breezy runtime (lib_breezy.a) ─┘
```

1. The **Breezy compiler** (written in C99, no external dependencies) parses your source into a typed AST.
2. It performs **escape and reference analysis** (the basis of [automatic memory](../memory/automatic-memory.md)).
3. It **lowers to NASM x86-64 assembly**, except pure numeric loops and leaf functions, which go to **GCC's optimizer** through the [C route](#the-c-route).
4. **NASM** assembles that to an object file.
5. **GCC** links the object file (and the C route's object, when there is one) with the **Breezy runtime** and any native C libraries you declared, producing the final binary.

---

## The C route

One step sits between the compiler and the link. The compiler picks out **pure numeric code** and hands it to GCC's optimizer instead of emitting assembly for it:

- **hot loops** whose bodies use only integer, `bool` and `double` locals, arrays of those, [final arrays](../types/arrays.md#final-arrays), plain control flow and integer `switch`;
- **leaf functions** of the same kind that call nothing and can never throw.

That code is written to `out_c.c` next to `out.asm`, compiled with `gcc -O3` into `out_c.obj` and linked into the same binary. Each hot loop is compiled twice, once for the **x86-64-v3** feature level (AVX2, BMI1/2, FMA) and once for the baseline x86-64 that every 64-bit CPU runs; the first time the loop runs, the program checks the CPU and uses the v3 copy whenever it is supported, so the same binary runs everywhere. Everything else (objects, strings, calls, exceptions, breezes) stays on the assembly path, and the two halves share the same frames, bounds checks and exceptions, so a program prints exactly the same results either way. A loop the built-in path already vectorizes stays on that path.

```sh
BZY_C=0 breezy app.bzy app    # Emit everything as assembly instead.
```

The route is **on by default**; set the environment variable `BZY_C=0` while compiling to keep every loop on the assembly path. Its cost is one extra GCC compile, only for programs whose own code has loops or functions the route takes; the standard library never does, so a program without such code builds exactly as it would without the route.

---

## The runtime

The compiled program links against a small **Breezy runtime** (`lib_breezy.a`) that provides the allocator, [ARC and the cycle collector](../memory/automatic-memory.md), the [breeze scheduler](../concurrency/breezes.md), [channels](../concurrency/channels.md), and the [async I/O facade](../io/native-io.md) - plus whatever native C libraries you linked via [FFI](../ffi/c-interop.md).

The runtime is written in C and compiled ahead of time into `lib_breezy.a`. Your program's *logic* is lowered to fresh assembly by the compiler; the runtime services it calls (allocate an object, send on a channel, open a socket) are pre-written C that the generated assembly simply calls into. So a finished binary is **your code as native assembly, statically linked against the C runtime** - there is no separate runtime to install or ship.

---

## Compiling a program

**Linux (ELF64):**

```sh
breezy app.bzy app
./app
```

**Windows (PE64):**

```sh
breezy app.bzy app.exe --target windows
app.exe
```

An optional second positional argument names the final executable. Omit it and the
binary is written to `out.exe` in the current directory:

```sh
breezy app.bzy   # → out.exe
```

The target backend is selectable with `--target`; see [Building the compiler](building.md) for the full target details.

---

← [System](../stdlib/system.md) · [Back to the guide](../guide.md) · Next: [Building the compiler](building.md)
