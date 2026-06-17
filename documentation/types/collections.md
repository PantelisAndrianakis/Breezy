# Generic Collections - No Boxing

Beyond fixed-length [arrays](arrays.md), Breezy ships a family of growable generic collections: `List`, `Stack`, `Queue`, `Deque` (also spelled `ArrayDeque`), `Set`, and the **ordered** containers `PriorityQueue` and `TreeSet`. They are **monomorphized per element type**, so they have **no boxing**: a `List<int>` stores raw 32-bit integers inline, while a `List<Dog>` stores reference-counted object pointers.

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
| `PriorityQueue<T>` | Smallest element out first. | Binary min-heap. |
| `TreeSet<T>` | Unique elements, kept **sorted**. | B-tree. |

The unordered collections have a `.size` field, a `.contains(T)` method, and work with `foreach`. The ordered containers (`PriorityQueue`, `TreeSet`) report their count with a **`size()` method** instead, and order their elements either by the **built-in order** of a primitive/`string`, or - for object elements - by a [`Comparable`](#ordering-objects-with-comparable) implementation you provide.

---

## List

A growable, ordered sequence:

```breezy
List<int> nums = new List<int>();
nums.add(10);
nums.add(20);
nums.add(30);
print(nums.size);          // 3.
print(nums.contains(20));  // true.

int sum = 0;
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
Set<string> seen = new Set<string>();
seen.add("a");
seen.add("a");             // Deduplicated.
print(seen.size);          // 1.
print(seen.contains("a")); // true.
```

Because a `Set` is backed by the [map](maps.md) hash table, its elements must be **hashable**: any integer type, `string`, an `enum`, an object, or a [record](records.md). Integer and string elements deduplicate by value; enum and plain object elements deduplicate by **identity** (reference equality) - a distinct but structurally-equal object is a separate element. A [record](records.md) element deduplicates by **value**, so two structurally-equal records collapse to one. `float`/`double` elements are rejected, the same as map keys.

---

## Stack, Queue, and Deque

`Stack` is last-in first-out; `Queue` is first-in first-out; `Deque`/`ArrayDeque` is a double-ended queue you can push and pop at either end. All share the `.size`, `.contains`, and `foreach` surface and store their elements with the same no-boxing layout.

---

## PriorityQueue

A `PriorityQueue<T>` always hands back its **smallest** element first - the engine behind schedulers, Dijkstra/A\* frontiers, and event loops. It is a binary min-heap: `add` sifts up, `poll` removes and returns the minimum, `peek` looks without removing.

```breezy
PriorityQueue<int> pq = new PriorityQueue<int>();
pq.add(5);
pq.add(1);
pq.add(3);
print(pq.poll());     // 1  -- smallest out first.
print(pq.poll());     // 3.
print(pq.peek());     // 5  -- next, not removed.
print(pq.size());     // 1.
print(pq.isEmpty());  // false.
```

Surface: `add(T)`, `poll()`, `peek()`, `size()`, `isEmpty()`. Primitive and `string` elements order by their natural order (numeric, lexicographic); object elements need [`Comparable`](#ordering-objects-with-comparable). `poll`/`peek` on an empty queue return the zero value of `T`.

---

## TreeSet

A `TreeSet<T>` is a `Set` that stays **sorted**: it deduplicates like a `Set`, but iterates in order and answers nearest-neighbour queries. It is backed by a B-tree.

```breezy
TreeSet<int> s = new TreeSet<int>();
s.add(30);
s.add(10);
s.add(20);
s.add(10);            // Deduplicated.
print(s.size());      // 3.
print(s.first());     // 10  -- smallest.
print(s.last());      // 30  -- largest.
print(s.floor(25));   // 20  -- greatest element <= 25.
print(s.ceiling(25)); // 30  -- least element >= 25.
s.remove(20);
```

Surface: `add(T)`, `remove(T)`, `contains(T)`, `first()`, `last()`, `floor(T)`, `ceiling(T)`, `size()`. The sorted-map cousin is [`TreeMap`](maps.md#treemap-a-sorted-map).

---

## Ordering objects with Comparable

Primitive and `string` elements of `PriorityQueue`, `TreeSet`, and [`TreeMap`](maps.md#treemap-a-sorted-map) keys order by their built-in order. To put **objects** in an ordered container, the class must implement the reserved `Comparable` interface - a single method that returns a negative number, zero, or a positive number when `this` orders before, equal to, or after `other`:

```breezy
class Job
{
	int priority;

	Job(int p)
	{
		priority = p;
	}

	int compareTo(Job other)
	{
		return priority - other.priority;
	}
}

PriorityQueue<Job> jobs = new PriorityQueue<Job>();
jobs.add(new Job(5));
jobs.add(new Job(1));
print(jobs.poll().priority);   // 1  -- lowest priority first.
```

The contract is exactly `int compareTo(SelfClass other)`. Adding an object that does not implement it to an ordered container is a **compile-time error**, so a missing ordering is caught before the program runs - never at a random insert.

---

## No boxing, and automatic memory

This is the headline guarantee: **primitives are never wrapped onto the heap**. A `List<int>` stores raw `int`s inline. For object element types, the collection participates in [ARC and the cycle collector](../memory/automatic-memory.md) - elements are retained on insert, released on removal, and a collection caught in a reference cycle is reclaimed automatically. Primitives carry no per-element allocation overhead at all.

A growable doubling vector backs `List` and `Stack`; a ring buffer over that backs `Queue` and `Deque`; `Set` reuses the hash table machinery. These collections are part of the standard library and use the same monomorphizing mechanism as [user-defined generics](../language/generics.md).

---

## Thread safety

Collections are **automatically safe to share across [breezes](../concurrency/breezes.md)**. When any of these collections - including `PriorityQueue` and `TreeSet` - crosses to another breeze over a [channel](../concurrency/channels.md), as a `spawn` argument, or through a static field, every operation (`add`, `get`, `set`, `poll`, pops, growth) runs atomically from then on, with no locking in your code. A collection that never leaves its breeze keeps today's full inline speed - it pays nothing.

The guarantee is **per operation**: individual calls are atomic, but a compound sequence (check `size`, then `get`) can interleave with other breezes' updates. Iteration over a shared collection is safe and never crashes; it may or may not reflect updates that race with the loop.

---

## Rules & gotchas

- **No boxing for primitives** - `List<int>` stores real 32-bit integers.
- **`.size` is a field; `.contains(T)` is a method** - except the ordered containers, where **`size()` is a method**.
- **All collections support `foreach`.**
- **`Set` and `TreeSet` deduplicate** - re-adding an existing element does nothing; `TreeSet` also keeps them sorted.
- **Ordered containers order objects via `Comparable`** - a missing `compareTo` is a compile-time error.
- **Object elements are managed automatically** - retained while held, released when removed.

---

← [Maps](maps.md) · [Back to the guide](../guide.md) · Next: [Automatic memory](../memory/automatic-memory.md)
