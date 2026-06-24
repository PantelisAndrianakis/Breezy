# Strings

A `string` is **immutable UTF-8 text**. It is a heap-allocated, length-prefixed value held through a single 8-byte reference - the characters live inline with the object header in one allocation, so there is exactly one allocation and one free per string. Strings are managed by [ARC](../memory/automatic-memory.md) like any other object.

← [Back to the guide](../guide.md)

---

## Creating and joining strings

Write a string literal in double quotes, and join values with `+` (or `+=`). When either side of `+` is a string, the other operand may be a string or any scalar (a number or bool), which is converted to its text form - so `"n = " + n` works.

```breezy
string greeting = "Hello";
string who = "Breezy";
string message = greeting + ", " + who + ".";   // Hello, Breezy.
print(message);
```

The parts of a `+` chain are joined in a single pass into one new string sized to their total - so `greeting + ", " + who + "."` allocates a single result, not one per `+` (scalars are first converted to text). Strings are **immutable**: joining produces a *new* string and never changes an existing one.

---

## StringBuilder - efficient accumulation

Building a string by repeated `+` in a loop is O(n²) because each step copies everything so far. The compiler **automatically lowers the common `s = s + …` (and `s += …`) accumulation loop to a StringBuilder** (O(n) total), so the straightforward code is already fast. An explicit **`StringBuilder`** is still the right choice when the accumulator is read mid-loop, the loop can exit early, or the accumulation isn't a simple append-on-the-end — the cases the automatic lowering deliberately leaves untouched. It appends into a doubling buffer (O(n) total) and snapshots to an immutable `string` with `toString()`.

```breezy
StringBuilder sb = new StringBuilder();
sb.append("Hello, ");
sb.append("Breezy");
print(sb.toString());          // Hello, Breezy
```

---

## String methods

Because strings are immutable, every **transforming** method returns a **new** string - the receiver is never changed.

```breezy
string path = "/api/users";

bool ok = path.startsWith("/api");   // true.

int at = path.indexOf("users");     // 5.

string up = path.toUpper();            // /API/USERS  (path itself is unchanged).
```

**Query methods** (return a `bool`, `int`, or character):

- `length()`, `isEmpty()`
- `isNumeric()` - non-empty and all digits `0-9`
- `isAlphaNumeric()` - non-empty and all letters or digits
- `equals(o)`, `equalsIgnoreCase(o)`
- `contains(x)`, `startsWith(p)`, `endsWith(p)`
- `indexOf(x)`, `lastIndexOf(x)`
- `charAt(i)` - the byte at `i` as an `int`, or `-1` if out of range

**Transform methods** (return a new string):

- `substring(a, b)`, `replace(from, to)`, `repeat(n)`
- `trim()`, `toUpper()`, `toLower()`
- `padLeft(width)` / `padRight(width)` - pad with spaces to at least `width`; a
  string already that long is returned unchanged. Pass a second argument to pad
  with a different character: `("" + id).padLeft(6, "0")` -> `"000017"`.

**Formatting numbers:** a floating-point value has `toFixed(digits)`, which
returns a string with exactly that many fraction digits (rounded):

```breezy
double price = 1.5;
print(price.toFixed(3));        // 1.500
print((3.14159).toFixed(2));    // 3.14
```

**Split into an array:**

```breezy
string[] parts = "red,green,blue".split(",");
print(parts.length);            // 3.
foreach (string p in parts)
{
	print(p);                   // red / green / blue.
}
```

`split(sep)` cuts on the whole `sep` **substring** and keeps empty fields
(`"a,,b".split(",")` is three elements). Two extras cover the common
field-parsing needs:

- `split(sep, skipEmpty)` - pass `true` to drop empty fields, so a trailing
  separator or a run of separators leaves no blanks.
- `splitAny(chars)` / `splitAny(chars, skipEmpty)` - cut on **any single
  character** in `chars`, not the whole string: `"a,b;c".splitAny(",;")` is
  `["a", "b", "c"]`.

```breezy
string[] cols = line.split("\t", true);     // Tab-separated, no blank fields.
string[] toks = "a, b; c".splitAny(", ;", true);   // ["a", "b", "c"].
```

**Raw bytes:** `s.toBytes()` returns a `byte[]` of the string's UTF-8 bytes, and `fromBytes(byte[])` builds a string back from raw bytes - a verbatim copy both directions (`fromBytes(s.toBytes())` reproduces `s` exactly). This is the serialization bridge for native interop; see [C interop -> byte[] ↔ string](../ffi/c-interop.md).

---

## Parsing to numbers

These methods convert a string to a number: `toInt()`, `toLong()`, `toByte()`, `toShort()`, `toFloat()`, `toDouble()`, and `toBool()` (`true`/`false`, case-insensitive). Each **throws a catchable `NumberFormatException`** on malformed or out-of-range input, so a "use a default on failure" reads naturally:

```breezy
int port;
try
{
	port = config.toInt();
}
catch (NumberFormatException e)
{
	port = 8080;            // Fallback when the value is not a valid int.
}
```

See [Exceptions](../language/exceptions.md) for the `try`/`catch` mechanics.

---

## Rules & gotchas

- **Strings are immutable** - transforming methods return a new string, never mutating the original.
- **Use `StringBuilder` for loops** of many appends to avoid O(n²) copying.
- **`charAt(i)` returns an `int`** (the byte value), or `-1` when out of range.
- **Parsing throws `NumberFormatException`** - wrap it in `try`/`catch` if the input might be invalid.
- **`+` builds strings** - when one operand is a string, a scalar (number or bool) on the other side is converted to its text form; objects cannot be concatenated.
- **Strings are ARC-managed** - you never free them.

---

← [Type system overview](overview.md) · [Back to the guide](../guide.md) · Next: [Arrays](arrays.md)
