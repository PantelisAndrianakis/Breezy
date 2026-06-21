# Building the Compiler

This page covers building the Breezy compiler itself from source, and selecting the code-generation target for the programs you compile with it.

← [Back to the guide](../guide.md)

---

## One-command bootstrap

The bootstrap scripts install everything the build needs (GCC/MinGW, NASM, GNU Make) and then build the compiler:

```sh
# Windows (normal, non-admin prompt) - installs deps via scoop.
build.bat

# Linux / macOS - installs deps via apt/dnf/pacman/zypper/apk or Homebrew.
./build.sh
```

Already have the toolchain? Build directly with Make:

```sh
make
./breezy --help
```

**Requirements** (handled automatically by the scripts): GCC (or MinGW-w64 on Windows), NASM, and GNU Make.

---

## Targets

The code-generation backend is selectable with `--target`. The default is the build host.

```sh
breezy myproject                    # Windows PE64 (Microsoft x64 ABI) on a Windows host.
breezy myproject --target linux     # System V AMD64 / ELF64 emission.
```

- **Windows PE64** uses the Microsoft x64 ABI.
- **`--target linux`** emits System V AMD64 assembly (integer arguments in `rdi, rsi, rdx, rcx, r8, r9`, floating-point arguments numbered independently of position) and drives `nasm -f elf64` plus `gcc -no-pie`.

### Checking without building

`--check` runs the front-end only — parse and resolve — then exits without emitting code. Diagnostics are written to stdout as a single JSON object, which makes it the integration point for editors and language tooling:

```sh
breezy myproject --check
```

- On success it prints `{"ok":true}` and exits `0`.
- On the first error it prints `{"file":"...","line":N,"col":C,"message":"..."}` and exits `1`. A `col` of `0` means the error is line-level (resolve and type errors carry no column; syntax errors do).

On a Linux host, `breezy` builds and runs native ELF64 for compute, concurrency, file I/O, and networking. The one external runtime dependency on Linux is **libcurl** (`libcurl4-openssl-dev`), used by [`Network.readUrl`](../io/native-io.md).

**Per-host difference:** a few [`File`](../stdlib/file.md) attributes (`HIDDEN`, `SYSTEM`, `ARCHIVE`) are Windows concepts with no POSIX equivalent.

> **Cross-building from one tree:** Windows and Linux build artifacts live in separate per-host directories (`build/win/`, `build/linux/`), so you can build for both from one shared tree without running `make clean` between them.

---

← [Compilation pipeline](compilation.md) · [Back to the guide](../guide.md)
