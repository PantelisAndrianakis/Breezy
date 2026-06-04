# Strings

A `string` is **immutable UTF-8 text**. It is a heap-allocated, length-prefixed value held through a single 8-byte reference - the characters live inline with the object header in one allocation, so there is exactly one allocation and one free per string. Strings are managed by [ARC](../memory/automatic-memory.md) like any other object.

← [Back to the guide](../guide.md)

---

## Creating and joining strings

Write a string literal in double quotes, and join strings with `+` (or `+=`). **Both operands of `+` must be strings** - Breezy does not auto-convert a number or boolean to text, so you cannot write `"n = " + n` when `n` is a number (print such values on their own; see [Console I/O](../language/console-io.md)).

```breezy
string greeting;
greeting = "Hello";

string who;
who = "Breezy";

string message;
message = greeting + ", " + who + ".";   // Hello, Breezy.
print(message);
```

Each `+` allocates one new string sized to the sum of its parts. Strings are **immutable**: joining produces a *new* string and never changes an existing one.

---

## StringBuilder - efficient accumulation

Building a string by repeated `+` in a loop is O(n²) because each step copies everything so far. For heavy accumulation, use a **`StringBuilder`**, which appends into a doubling buffer (O(n) total) and snapshots to an immutable `string` with `toString()`.

```breezy
StringBuilder sb;
sb = new StringBuilder();
sb.append("Hello, ");
sb.append("Breezy");
print(sb.toString());          // Hello, Breezy
```

---

## String methods

Because strings are immutable, every **transforming** method returns a **new** string - the receiver is never changed.

```breezy
string path;
path = "/api/users";

boolean ok;
ok = path.startsWith("/api");   // true.

int at;
at = path.indexOf("users");     // 5.

string up;
up = path.toUpper();            // /API/USERS  (path itself is unchanged).
```

**Query methods** (return a `boolean`, `int`, or character):

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

**Split into an array:**

```breezy
string[] parts;
parts = "red,green,blue".split(",");
print(parts.length);            // 3.
foreach (string p in parts)
{
	print(p);                   // red / green / blue.
}
```

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
- **`+` concatenates strings only** - both operands must be strings; there is currently no built-in number-to-string conversion.
- **Strings are ARC-managed** - you never free them.

---

← [Type system overview](overview.md) · [Back to the guide](../guide.md) · Next: [Arrays](arrays.md)
