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
3. It **lowers to NASM x86-64 assembly**.
4. **NASM** assembles that to an object file.
5. **GCC** links the object file with the **Breezy runtime** and any native C libraries you declared, producing the final binary.

---

## The runtime

The compiled program links against a small **Breezy runtime** (`lib_breezy.a`) that provides the allocator, [ARC and the cycle collector](../memory/automatic-memory.md), the [breeze scheduler](../concurrency/breezes.md), [channels](../concurrency/channels.md), and the [async I/O facade](../io/native-io.md) - plus whatever native C libraries you linked via [FFI](../ffi/c-interop.md).

The runtime is written in C and compiled ahead of time into `lib_breezy.a`. Your program's *logic* is lowered to fresh assembly by the compiler; the runtime services it calls (allocate an object, send on a channel, open a socket) are pre-written C that the generated assembly simply calls into. So a finished binary is **your code as native assembly, statically linked against the C runtime** - there is no separate runtime to install or ship.

---

## Compiling a program

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

The target backend is selectable with `--target`; see [Building the compiler](building.md) for the full target details.

---

← [System](../stdlib/system.md) · [Back to the guide](../guide.md) · Next: [Building the compiler](building.md)
