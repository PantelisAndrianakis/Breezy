# Roadmap

The **language design is settled**; the compiler and runtime are being built from scratch. This page tracks what is implemented today and what is still to come. Items marked complete are working and tested.

← [Back to the guide](../guide.md)

---

## Where things stand

Breezy `.bzy` source compiles to native **Windows (PE64)** and **Linux (ELF64)** executables today, with automatic memory management, the full scalar type system, strings, arrays, maps, the generic collections, full control flow, concurrency and I/O, exceptions, the C FFI, interfaces, user-defined generics, enums, and static members all in place. The next focus is the extended standard library and, beyond it, a self-hosting compiler.

---

## Compiler core (Part 1)

- [x] Lexer, parser, typed AST.
- [x] Type table: classes, inheritance, virtual dispatch.
- [x] x86-64 codegen (Windows PE64).

## Memory & runtime (Part 2)

- [x] Escape analysis -> stack allocation.
- [x] Automatic Reference Counting.
- [x] Incremental cycle collector.

## Scalar types (Part 3)

- [x] Sized signed integers `byte`/`short`/`int`/`long` and unsigned `ubyte`/`ushort`/`uint`/`ulong`.
- [x] `boolean` (`true`/`false`).
- [x] `float` / `double` (IEEE-754, SSE path).

## Core types & collections (Part 4)

- [x] Strings with amortized append (`StringBuilder`).
- [x] Arrays (`T[]`) with bounds checks.
- [x] Maps (`map<K,V>`, open addressing, ARC + cycle-collected).
- [x] Loop control: `for`, `break`/`continue`, `++`/`--`.
- [x] `foreach` and the iterator protocol (arrays, strings, maps, collections).
- [x] Stdlib-only monomorphized generics (no boxing).
- [x] Generic collections - `List` / `Stack` / `Queue` / `Deque` / `Set`.
- [x] `switch` (C-style fallthrough, `default`, jump-table lowering).

## Language & standard-library essentials (Part 5)

- [x] Block comments and compound assignment (`+=` `-=` `*=` `/=`).
- [x] `Math`, `Clock`, `Random` namespaces.
- [x] Full string method set and parse-to-number (throwing `NumberFormatException`).
- [x] `Regex` (Thompson NFA / Pike VM, linear-time, ReDoS-safe).
- [x] Exceptions: `try`/`catch`/`throw` with a hierarchy, is-a matching, zero-cost-when-not-thrown.
- [x] Map views and pair `foreach`.
- [x] `File` namespace (throws `IOException`).
- [x] `System` namespace (`shell`, `args`).
- [x] Constructor arguments and the eight built-in vector types.

## Concurrency & I/O (Part 6)

- [x] Breezes and the cooperative scheduler (`spawn` / `yield`).
- [x] `spawn` with arguments and bounded `channel<T>`.
- [x] Multi-core work-stealing scheduler and atomic refcounts for shared objects.
- [x] Timers (`scheduleAfter` / `scheduleEvery`, `Timer.cancel`).
- [x] Offload thread pool and async file I/O.
- [x] Network sockets (TCP/UDP) via IOCP.
- [x] `Network.readUrl` (one-call HTTP/HTTPS GET).
- [x] Random-access `FileChannel`, buffered `FileWriter`, channel-fed `Logger`.

## Interop (Part 7)

- [x] `extern` C FFI - direct C-ABI calls, `string`->`char*` marshalling, `--link` and `breezy.toml [link]`, `blocking` offload dispatch.

## Other targets & language features (Parts 8-9)

- [x] x86-64 codegen (Linux ELF64) - full language and runtime as native ELF.
- [x] Interfaces - polymorphism without inheritance, shared-vtable-slot dispatch.
- [x] User-definable generics - `class Box<T>`, `Pair<K, V>`, interface bounds.
- [x] Java-style enums - singleton constants, fields/constructors/methods, per-constant bodies.
- [x] Static members and static classes - per-member `static`, `static class`, field initializers, singletons.

## Still to come

- [ ] Extended standard library (sorted/tree maps, priority queue, more math, time, formatting).
- [ ] Self-hosting compiler.

---

← [Building the compiler](building.md) · [Back to the guide](../guide.md)
