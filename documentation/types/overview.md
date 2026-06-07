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
| `bool` | `true` / `false`. |
| `string` | Immutable UTF-8 text; concatenate with `+` / `+=`. |
| `T[]` | Heap-allocated array of any type, bounds-checked. |
| `map<K,V>` | Hash map (open addressing, Swiss-style control bytes); keys are any integer type, `string`, an `enum`, or an object. |
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

**Literal suffixes.** An unsuffixed whole number is an `int` and an unsuffixed decimal is a `double`; suffixes opt into another type:

- `L` - `long` (e.g. `5L`), and `u` - unsigned (e.g. `5u`); combine them for an unsigned 64-bit literal (`5uL`).
- `f` - `float` (e.g. `2.0f`), and `d` - `double` (e.g. `2.0d`). Since a bare decimal is already `double`, `d` is for explicitness; `f` is the one you need to get a 32-bit float. Both accept upper case (`2.0F`, `2.0D`).

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

## Arithmetic operators

The arithmetic operators are `+`, `-`, `*`, `/`, and `%` (modulo / remainder), each with a compound-assignment form (`+=`, `-=`, `*=`, `/=`, `%=`). `*`, `/`, and `%` share the same precedence and bind tighter than `+` and `-`, as in C and Java.

`%` returns the remainder of an integer division and **requires integer operands** - applying it to a `float` or `double` is a compile error (use a library routine for floating-point remainder). The result follows the **sign of the dividend**, matching C: `-7 % 3` is `-1`, and `7 % -3` is `1`. Mixed-width and mixed-signedness operands promote by the same rules as the other arithmetic operators above.

```breezy
print(17 % 5);       // 2
print(-7 % 3);       // -1  -- sign follows the dividend.

int i;
i = 100;
i %= 7;
print(i);            // 2
```

---

## Bitwise shift operators

`<<` (left shift) and `>>` (right shift) move an integer's bits, with compound forms `<<=` and `>>=`. They **require integer operands** (a `float`/`double` is a compile error) and sit at their own precedence level: **looser than `+`/`-`, tighter than the comparisons** - so `1 + 1 << 3` is `(1 + 1) << 3` and `4 >> 1 == 2` is `(4 >> 1) == 2`, matching C and Java.

The result takes the **left operand's type**; the shift count's type does not affect it. Right shift follows the left operand's signedness: a **signed** value shifts arithmetically (the sign bit is preserved), an **unsigned** value shifts logically (zeros fill from the top).

```breezy
print(1 << 4);       // 16
print(256 >> 2);     // 64
print(-16 >> 2);     // -4   -- signed: arithmetic shift preserves the sign.

uint u;
u = 4000000000u;
print(u >> 1);       // 2000000000  -- unsigned: logical shift fills with zeros.

int a;
a = 1;
a <<= 5;
print(a);            // 32
```

---

## Bitwise logical operators

`&` (AND), `|` (OR), `^` (XOR), and the prefix `~` (NOT) combine integers bit by bit, with compound forms `&=`, `|=`, and `^=`. Like the shifts, they **require integer operands** - applying them to a `float`/`double` is a compile error.

Precedence follows C and Java: `|` is loosest, then `^`, then `&`, all **looser than the comparisons** (so `a & b == c` is `a & (b == c)`). A binary `& | ^` promotes its operands the same way arithmetic does - the wider operand's type wins, and unsigned wins on mixed signedness. `~` keeps its operand's type.

```breezy
print(0xF0 | 0x0F);  // 255
print(0xFF & 0x0F);  // 15
print(5 ^ 1);        // 4
print(~0);           // -1

int flags;
flags = 0;
flags |= 0x04;       // Set a bit.
flags &= ~0x04;      // Clear it.
print(flags);        // 0
```

Integer literals may be written in **hexadecimal** (`0x`/`0X`) or **binary** (`0b`/`0B`) as well as decimal; a bare leading zero stays decimal (there is no octal).

```breezy
print(0xFF);         // 255
print(0b1010);       // 10
```

---

## Logical operators

`and`, `or`, `xor`, and prefix `not` combine **booleans** and produce a boolean. The symbols `&&`, `||`, and `!` are exact synonyms for `and`, `or`, and `not`, but the **word forms are the idiomatic Breezy style**: logical operators (and equality - see below) are written as words, while arithmetic, bitwise, shift, and relational (`< > <= >=`) operators stay as symbols. `and`/`or` **short-circuit** - the right side is skipped once the left decides the result - so `a or expensive()` never calls `expensive()` when `a` is true. `xor` always evaluates both sides. Operands must be `bool`; an integer or float is a compile error.

Precedence (loosest first): `or` → `xor` → `and`, all **looser than the bitwise and comparison operators**, so `a > 0 and b > 0` is `(a > 0) and (b > 0)`. `not`/`!` is a tight prefix, binding before the binary operators.

```breezy
print(true and false);   // false
print(true or false);    // true
print(true xor true);    // false
print(not false);        // true
print(2 > 1 and 3 > 1);  // true

bool ready;
ready = true;
if (ready and not done())   // `done()` only runs when `ready` is true.
{
	start();
}
```

---

## Equality: equals and differs

`equals` and `differs` are the **idiomatic** way to write equality in Breezy: `a equals b` is `a == b`, and `a differs b` is `a != b`. The `==` and `!=` symbols remain valid, but prefer the words - like the logical operators, equality reads as words. They are **soft keywords** - recognized as operators only between two expressions - so the words remain usable as identifiers and method names (`a.equals(b)`, the record value-equality method, is unaffected).

```breezy
print(3 equals 3);    // true
print(3 differs 4);   // true
```

---

## Reference types

`string`, arrays, maps, collections, channels, and your own classes are **reference types** - they live on the heap (when they escape their scope) and are managed automatically by [ARC and the cycle collector](../memory/automatic-memory.md). You never free them.

Each has its own page:

- [Strings](strings.md) - immutable UTF-8 text and `StringBuilder`.
- [Arrays](arrays.md) - fixed-length, bounds-checked `T[]`.
- [Maps](maps.md) - `map<K,V>` hash maps.
- [Generic collections](collections.md) - `List`, `Stack`, `Queue`, `Deque`, `Set`.
- [Records](records.md) - final classes with synthesized value `equals`/`hashCode`, for use as value keys.

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
- **Map keys** may be any integer type, `string`, an `enum`, or an object; values can be any type. Integer and string keys match by value; enum and object keys match by **identity** (reference equality).

---

← [Exceptions](../language/exceptions.md) · [Back to the guide](../guide.md) · Next: [Strings](strings.md)
