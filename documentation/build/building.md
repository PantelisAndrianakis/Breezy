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

On a Linux host, `breezy` builds and runs native ELF64 for compute, concurrency, file I/O, and networking. The one external runtime dependency on Linux is **libcurl** (`libcurl4-openssl-dev`), used by [`Network.readUrl`](../io/native-io.md).

**Per-host difference:** a few [`File`](../stdlib/file.md) attributes (`HIDDEN`, `SYSTEM`, `ARCHIVE`) are Windows concepts with no POSIX equivalent.

> **Cross-building from one tree:** object files share paths, so run `make clean` when switching between the Windows and Linux builds.

---

← [Compilation pipeline](compilation.md) · [Back to the guide](../guide.md)
