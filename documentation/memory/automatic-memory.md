# Automatic Memory - C Footprint, Zero Bookkeeping

In Breezy you **never write `free`**, you never think about ownership, and you still **never pay for a tracing garbage collector**. Memory is reclaimed deterministically, with a steady-state footprint close to hand-written C. Breezy reaches that with **three cooperating layers**, all applied automatically by the compiler.

← [Back to the guide](../guide.md)

---

## Layer 1: Escape analysis - the fast path

The compiler analyses whether an object can outlive the scope that created it. If it **cannot** escape, the object is **allocated on the stack** - no heap allocation and no reference counting at all. This is the C-like core: the per-tick scratch objects, network packets, and temporaries a server creates by the million never touch the heap.

```breezy
void tick()
{
	Vector2f delta;          // Does not escape -> stack-allocated, free.
	delta = new Vector2f(1.0f, 2.0f);
	// No malloc, no refcount; gone at scope exit.
}
```

---

## Layer 2: Automatic Reference Counting - for objects that escape

Anything that **does** outlive its scope - a `Client` stored in a map, long-lived world state - gets a small **reference count**. The compiler inserts the retain and release operations for you, and frees the object the instant its last reference drops. Reclamation is **deterministic**: no pause, no heap headroom, no background GC thread spiking your latency.

This is what lets Breezy promise "the ergonomics of a managed language, the footprint and latency of C" - memory tracks your live data, not a bloated collector heap.

---

## Layer 3: A cycle-collecting backstop

Reference counting alone cannot reclaim reference **cycles** - say an order that points at a customer who points back at their orders. Each keeps the other's count above zero forever. Breezy adds a **periodic, incremental cycle collector** that finds and frees such cycles in the background - **without you annotating a single weak reference**. Everything stays automatic.

The result is a steady-state memory profile close to hand-written C, with the convenience of a managed language and none of the stop-the-world pauses.

---

## Object memory layout

Every heap object begins with a small fixed header, then its fields:

```
offset  0:   vtable pointer  (8 bytes)
offset  8:   reference count (8 bytes)
offset 16:   field 0         (8 bytes)
offset 24:   field 1         (8 bytes)
...
```

- The **vtable pointer** drives [virtual dispatch](../language/classes.md#virtual-dispatch-polymorphism) and `getClassName`.
- The **reference count** is Layer 2's bookkeeping.

The heap is **non-moving**: an object's address never changes for its lifetime. That stability is exactly what makes [zero-cost C interop](../ffi/c-interop.md) possible - you can hand a pointer straight to a C library with no pinning and no fear of the object being relocated.

---

## A note on concurrency

Reference-count updates are kept **non-atomic on the hot path** and only pay the atomic cost for objects that actually cross between breezes. The [zone model](../concurrency/zone-model.md) is what makes this safe and fast.

---

## Rules & gotchas

- **You never call `free`** - all three layers are automatic.
- **Non-escaping objects are stack-allocated** and cost nothing; this is the common, fast case.
- **There is no tracing GC** and no stop-the-world pause.
- **Cycles are handled for you** - no weak-reference annotations required.
- **The heap is non-moving** - object addresses are stable, which enables direct C interop.

---

← [Generic collections](../types/collections.md) · [Back to the guide](../guide.md) · Next: [Breezes](../concurrency/breezes.md)
