# Arrays

An array (`T[]`) is a **fixed-length, heap-allocated, bounds-checked** sequence of values of one type. "Bounds-checked" means an out-of-range index **aborts** with an exception rather than silently reading stray memory - a safety guarantee that costs almost nothing.

← [Back to the guide](../guide.md)

---

## Creating an array

Declare the element type with `[]`, then create the array with `new T[length]`. A fresh array starts zero-filled.

```breezy
int[] squares;
squares = new int[5];     // Five ints, all 0.
```

Arrays work for any type - `string[]`, `Dog[]`, `Vector2f[]`, and so on.

---

## Reading, writing, and length

Index with `[i]` (zero-based). The `.length` field gives the element count.

```breezy
int[] squares;
squares = new int[5];

int i;
i = 0;
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
int[] squares;
squares = new int[5];
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
int[] table;
table = new int[3];
print(table[5]);   // Throws: "array index 5 out of bounds for length 3".
```

This turns what would be undefined behaviour in C into a clean, catchable error.

---

## Arrays and memory

Arrays are heap-allocated reference types. An array of **objects** participates fully in [ARC and the cycle collector](../memory/automatic-memory.md): its elements are retained while in the array and released when the array goes away. Element loads are width-correct for the element type. You never free an array yourself.

---

## Rules & gotchas

- **Arrays are fixed-length** - the size is set at `new` and does not change. For a growable sequence, use [`List<T>`](collections.md).
- **Indexes are zero-based**; valid range is `0 .. length - 1`.
- **Out-of-range access throws `IndexOutOfBounds`** - it never reads stray memory.
- **`.length` is a field**, not a method call.
- **A new array is zero-filled** (numbers `0`, bools `false`, object references empty).

---

← [Strings](strings.md) · [Back to the guide](../guide.md) · Next: [Maps](maps.md)
