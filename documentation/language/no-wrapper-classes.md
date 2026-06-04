# No Primitive Wrapper Classes

In Breezy a number is just a number. `int` is a raw 32-bit machine integer - there is no `Integer` wrapper, no boxing, and no unboxing. What you write is what the CPU runs. This page explains what that means and how to pick the right numeric type.

← [Back to the guide](../guide.md)

---

## Raw machine numbers

In some managed languages a value can quietly become a heap-allocated "boxed" object - an `int` turning into an `Integer` - which costs an allocation and a pointer chase. Breezy never does this. An `int` is a value living in a register or on the stack, nothing more.

```breezy
void main()
{
	int x;
	x = 42;          // A raw 32-bit machine integer - nothing more.

	long big;
	big = 10000000000L;   // 64-bit when you need it (note the L suffix).
}
```

Because there is no wrapper type, there is no hidden allocation, no garbage to collect, and no surprise pointer indirection when you do arithmetic.

---

## int vs long

- **`int`** is **32-bit**. Use it for ordinary counts, indexes, and loop variables.
- **`long`** is **64-bit**. Use it when a value can exceed about two billion, or for things that are conceptually 64-bit (timestamps in nanoseconds, file sizes, native handles).

Write a `long` literal with an `L` suffix when it would not fit in an `int`:

```breezy
long nanos;
nanos = 10000000000L;   // Too big for int; the L makes it a long literal.
```

The full set of integer and floating-point types - signed, unsigned, and their widths - is covered in the [type system overview](../types/overview.md).

---

## Why this matters

This is a core part of Breezy's promise of *C-like performance*. Numeric code compiles to the same instructions you would get in C: arithmetic happens in registers, collections of numbers store the raw values inline (a [`List<int>`](../types/collections.md) holds real 32-bit integers, not boxed objects), and there is never a wrapper object to allocate or free.

---

## Rules & gotchas

- **There is no `Integer`, `Long`, `Float`, etc.** - only the primitive types themselves.
- **`int` is 32-bit; reach for `long` at 64-bit.** Overflowing an `int` wraps around (two's complement), it does not promote automatically.
- **Suffix large literals with `L`** so they are typed as `long`.
- **Primitives are never boxed**, including inside generic [collections](../types/collections.md), thanks to monomorphization.

---

← [Built-in vector types](vector-types.md) · [Back to the guide](../guide.md) · Next: [Console I/O](console-io.md)
