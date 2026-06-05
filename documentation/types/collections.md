# Generic Collections - No Boxing

Beyond fixed-length [arrays](arrays.md), Breezy ships a family of growable generic collections: `List`, `Stack`, `Queue`, `Deque` (also spelled `ArrayDeque`), and `Set`. They are **monomorphized per element type**, so they have **no boxing**: a `List<int>` stores raw 32-bit integers inline, while a `List<Dog>` stores reference-counted object pointers.

← [Back to the guide](../guide.md)

---

## The collections

| Collection | Shape | Backed by |
| --- | --- | --- |
| `List<T>` | Growable sequence, indexable. | Doubling vector. |
| `Stack<T>` | Last-in first-out. | Doubling vector. |
| `Queue<T>` | First-in first-out. | Ring buffer. |
| `Deque<T>` / `ArrayDeque<T>` | Double-ended queue. | Ring buffer. |
| `Set<T>` | Unique elements, no duplicates. | Hash table. |

Every collection has a `.size` field, a `.contains(T)` method, and works with `foreach`.

---

## List

A growable, ordered sequence:

```breezy
List<int> nums;
nums = new List<int>();
nums.add(10);
nums.add(20);
nums.add(30);
print(nums.size);          // 3.
print(nums.contains(20));  // true.

int sum;
sum = 0;
foreach (int n in nums)
{
	sum = sum + n;
}
print(sum);                // 60.
```

---

## Set

A `Set` holds unique elements - adding a value already present is a no-op:

```breezy
Set<string> seen;
seen = new Set<string>();
seen.add("a");
seen.add("a");             // Deduplicated.
print(seen.size);          // 1.
print(seen.contains("a")); // true.
```

Because a `Set` is backed by the [map](maps.md) hash table, its elements must be **hashable**: any integer type, `string`, an `enum`, or an object. Integer and string elements deduplicate by value; enum and object elements deduplicate by **identity** (reference equality) - a distinct but structurally-equal object is a separate element. `float`/`double` elements are rejected, the same as map keys.

---

## Stack, Queue, and Deque

`Stack` is last-in first-out; `Queue` is first-in first-out; `Deque`/`ArrayDeque` is a double-ended queue you can push and pop at either end. All share the `.size`, `.contains`, and `foreach` surface and store their elements with the same no-boxing layout.

---

## No boxing, and automatic memory

This is the headline guarantee: **primitives are never wrapped onto the heap**. A `List<int>` stores raw `int`s inline. For object element types, the collection participates in [ARC and the cycle collector](../memory/automatic-memory.md) - elements are retained on insert, released on removal, and a collection caught in a reference cycle is reclaimed automatically. Primitives carry no per-element allocation overhead at all.

A growable doubling vector backs `List` and `Stack`; a ring buffer over that backs `Queue` and `Deque`; `Set` reuses the hash table machinery. These collections are part of the standard library and use the same monomorphizing mechanism as [user-defined generics](../language/generics.md).

---

## Rules & gotchas

- **No boxing for primitives** - `List<int>` stores real 32-bit integers.
- **`.size` is a field; `.contains(T)` is a method.**
- **All collections support `foreach`.**
- **`Set` deduplicates** - re-adding an existing element does nothing.
- **Object elements are managed automatically** - retained while held, released when removed.

---

← [Maps](maps.md) · [Back to the guide](../guide.md) · Next: [Automatic memory](../memory/automatic-memory.md)
