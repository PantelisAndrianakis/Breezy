# Arrays

An array (`T[]`) is a **fixed-length, heap-allocated, bounds-checked** sequence of values of one type. "Bounds-checked" means an out-of-range index **aborts** with an exception rather than silently reading stray memory - a safety guarantee that costs almost nothing.

← [Back to the guide](../guide.md)

---

## Creating an array

Declare the element type with `[]`, then create the array with `new T[length]`. A fresh array starts zero-filled.

```breezy
int[] squares = new int[5];     // Five ints, all 0.
```

Arrays work for any type - `string[]`, `Dog[]`, `Vector2f[]`, and so on.

---

## Reading, writing, and length

Index with `[i]` (zero-based). The `.length` field gives the element count.

```breezy
int[] squares = new int[5];

int i = 0;
while (i < squares.length)
{
	squares[i] = i * i;   // Write.
	i = i + 1;
}

print(squares[4]);        // 16  -- read.
print(squares.length);    // 5.
```

---

## Iterating with foreach

When you do not need the index, `foreach` reads each element in turn:

```breezy
int[] squares = new int[5];
// ... fill it ...

foreach (int n in squares)
{
	print(n);
}
```

---

## Bounds checking

Indexing outside `0 .. length - 1` throws a built-in `IndexOutOfBounds` exception (see [Exceptions](../language/exceptions.md)):

```breezy
int[] table = new int[3];
print(table[5]);   // Throws: "array index 5 out of bounds for length 3".
```

This turns what would be an out-of-bounds memory error into a clean, catchable error.

---

## Arrays and memory

Arrays are heap-allocated reference types. An array of **objects** participates fully in [ARC and the cycle collector](../memory/automatic-memory.md): its elements are retained while in the array and released when the array goes away. Element loads are width-correct for the element type. You never free an array yourself.

---

## Thread safety

Arrays are **automatically safe to share across [breezes](../concurrency/breezes.md)**. When an array crosses to another breeze - over a [channel](../concurrency/channels.md), as a `spawn` argument, or through a static field - the runtime marks it shared, and from then on each element read and write is atomic and memory-safe, with no locking in your code:

- **Value arrays** (`int[]`, `double[]`, ...) pay nothing at all: the length is fixed and element access is naturally atomic.
- **Object and string arrays** synchronize each shared element access automatically, so a read can never see a half-replaced element or touch a freed one.

Each *operation* is atomic; a sequence of operations can still interleave with other breezes (two breezes writing the same slot - last write wins). An array that never leaves its breeze pays no synchronization cost whatsoever.

---

## Final arrays

A **final array** is a table of fixed values: its length and every element are set once, in the source, and never change. Declare it with `final` and a brace list of literal values:

```breezy
final int[] SQUARES = { 0, 1, 4, 9, 16 };

print(SQUARES[3]);        // 9
print(SQUARES.length);    // 5
```

A shared table belongs in a class as a `static final` field, read through the class name:

```breezy
static class Dct
{
	static final int[] BASIS = { 2896, 4017, 3784, 3406 };
}

int c = Dct.BASIS[i];
```

- The element type is an integer type (`byte short int long` and the unsigned variants), `bool` or `double`. Each value must be a literal that fits the element type; an integer literal is fine in a `double[]`.
- A final array can only be **indexed** or read for its **`.length`**. Writing an element, assigning the variable, passing it to a function, returning it, iterating it with `foreach` or capturing it in a lambda is a compile error.
- Indexing still checks bounds, and because the length is known at compile time those checks disappear wherever the index range is provably inside.

The compiler places each final array once in read-only memory: it is never allocated, never reference-counted and costs nothing at startup.

| code | error |
| --- | --- |
| `SQUARES[0] = 1;` | Cannot write to an element of final array SQUARES. |
| `SQUARES = other;` | Cannot assign to final array SQUARES. |
| `f(SQUARES)` | Final array SQUARES can only be indexed or read for its length. |
| `final int x = 3;` | A final array needs an initializer of literal values, like { 1, 2, 3 }. |
| `final byte[] b = { 300 };` | Value does not fit the final array's element type. |
| `final` on an instance field | Only local variables and static fields can be final. |
| `int[] a = { 1, 2 };` | An array literal can only initialize a final array. |

---

## Rules & gotchas

- **Arrays are fixed-length** - the size is set at `new` and does not change. For a growable sequence, use [`List<T>`](collections.md).
- **Indexes are zero-based**; valid range is `0 .. length - 1`.
- **Out-of-range access throws `IndexOutOfBounds`** - it never reads stray memory.
- **`.length` is a field**, not a method call.
- **A new array is zero-filled** (numbers `0`, bools `false`, object references empty).
- **A brace list `{ ... }` creates only a final array** - ordinary arrays come from `new`.
- **`final` is a reserved word** and cannot name a variable.

---

← [Strings](strings.md) · [Back to the guide](../guide.md) · Next: [Maps](maps.md)
