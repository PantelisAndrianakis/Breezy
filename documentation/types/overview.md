# Type System Overview

Breezy is **statically typed**: every variable has a type known at compile time, and the compiler checks your code against those types before producing a native binary. This page lists every built-in type and the rules that govern them.

← [Back to the guide](../guide.md)

---

## The built-in types

| Type | Description |
| --- | --- |
| `byte` `short` `int` `long` | Signed integers: 8, 16, **32**, 64-bit (two's complement). |
| `ubyte` `ushort` `uint` `ulong` | Unsigned integers: 8, 16, 32, 64-bit. |
| `float` `double` | IEEE-754 floating point: 32, 64-bit. |
| `boolean` | `true` / `false`. |
| `string` | Immutable UTF-8 text; concatenate with `+` / `+=`. |
| `T[]` | Heap-allocated array of any type, bounds-checked. |
| `map<K,V>` | Hash map (open addressing, Swiss-style control bytes), `int` or `string` keys. |
| `List<T>` `Stack<T>` `Queue<T>` `Deque<T>` `Set<T>` | Monomorphized generic collections (no boxing). |
| `channel<T>` | Bounded buffered channel for passing values between breezes. |
| `Timer` | Handle for a scheduled or periodic breeze. |
| `ClassName` | Heap-allocated object, memory managed automatically (ARC + cycle collector). |
| `void` | No value; used as a function return type. |

---

## Numbers

- **Signed integers:** `byte` (8-bit), `short` (16-bit), `int` (**32-bit**), `long` (64-bit).
- **Unsigned integers:** `ubyte`, `ushort`, `uint`, `ulong` at the same widths.
- **Floating point:** `float` (32-bit) and `double` (64-bit), both IEEE-754. There is **no unsigned floating point**.

`int` is the everyday integer; reach for `long` at 64-bit. Numbers are raw machine values - there is no boxing and no wrapper types. See [No primitive wrapper classes](../language/no-wrapper-classes.md).

---

## Mixing numeric types

When an arithmetic expression mixes numeric types, the result **promotes** to the more general type - you do not need a cast for the common cases:

- **Floating wins.** If either operand is `double`, the result is `double`; otherwise if either is `float`, the result is `float`. Integers convert into the floating type automatically. So `int + float` is a `float`, and `int + double` is a `double`.
- **Wider integer wins.** For two integers of the **same** signedness, the result is the wider one: `byte + int` is an `int`, `int + long` is a `long`.
- **Unsigned wins on a sign mix.** If one operand is signed and the other unsigned, the result is the **unsigned** type at the wider rank: `int + uint` is a `uint`, `long + uint` is a `ulong`. As in C, a negative value treated as unsigned wraps around - so mix signs deliberately.

```breezy
int i;
i = 3;
float f;
f = 2.0f;
print(i + f);        // 5   -- int + float promotes to float.

byte b;
b = 100;
int n;
n = 50;
print(b + n);        // 150 -- byte + int promotes to int.
```

The result type is what the expression *produces*; assigning it to a narrower or differently-signed variable can still require an explicit cast. **Comparisons** (`<`, `==`, ...) still require both operands to share signedness - add a cast to compare a signed and an unsigned value, since the safe answer is rarely obvious.

---

## Reference types

`string`, arrays, maps, collections, channels, and your own classes are **reference types** - they live on the heap (when they escape their scope) and are managed automatically by [ARC and the cycle collector](../memory/automatic-memory.md). You never free them.

Each has its own page:

- [Strings](strings.md) - immutable UTF-8 text and `StringBuilder`.
- [Arrays](arrays.md) - fixed-length, bounds-checked `T[]`.
- [Maps](maps.md) - `map<K,V>` hash maps.
- [Generic collections](collections.md) - `List`, `Stack`, `Queue`, `Deque`, `Set`.

---

## Generics: built-in vs user-defined

There are two layers of generics, and both are **monomorphized** (specialized per concrete type, with **no boxing**):

- **Standard-library collections** - `List`, `Stack`, `Queue`, `Deque`, `Set` - are specialized per element type. A `List<int>` stores raw 32-bit integers inline.
- **User-defined generics** - your own `class Box<T>`, `class Pair<K, V>`, with optional interface bounds - are covered in [Generics](../language/generics.md).

---

## Rules & gotchas

- **Every variable has a static type** - declare it before use.
- **`int` is 32-bit; `long` is 64-bit.** There is no automatic widening on overflow.
- **No unsigned floating point.**
- **Reference types are managed automatically** - no manual allocation or freeing.
- **Map keys are `int` or `string`**; values can be any type.

---

← [Exceptions](../language/exceptions.md) · [Back to the guide](../guide.md) · Next: [Strings](strings.md)
