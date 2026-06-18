# JSON

`Json` is a **static namespace** for reading **and writing** JSON. `Json.parse(text)`
turns a JSON string into a tree of `JsonValue` nodes; `Json.stringify(value)`
serializes a `JsonValue` back to compact JSON; `Json.of(...)` lifts ordinary
Breezy values into `JsonValue`. You navigate a parsed tree with the ordinary
[collection combinators](../types/collections.md) and [closures](../types/functions.md) —
arrays are `List<JsonValue>`, objects are keyed lookups — so there is no separate
query language to learn. It is the sibling of the [XML reader](xml.md).

← [Back to the guide](../guide.md)

---

## Parsing

```breezy
string text = File.readText("books.json");   // Get the bytes however you like.
JsonValue root = Json.parse(text);            // Any top-level value.
print(root.isArray());                        // e.g. true.
```

`Json.parse` accepts any top-level value (object, array, string, number, `true`,
`false`, `null`). Malformed input **throws a catchable `JsonException`** whose
message carries the line and column:

```breezy
try
{
	JsonValue v = Json.parse("{bad}");
}
catch (JsonException e)
{
	print(e.message);   // JSON parse error at line 1, column 2: Expected a string key.
}
```

---

## The value

A `JsonValue` is dynamically typed: ask what it is, then extract it.

**Kind tests** (never throw):

| Member | Type | Meaning |
| --- | --- | --- |
| `v.isNull()` / `isBool()` / `isNumber()` / `isString()` / `isArray()` / `isObject()` | `bool` | The kind. |
| `v.type()` | `string` | `"null"`, `"bool"`, `"number"`, `"string"`, `"array"`, or `"object"`. |

**Scalar extraction** — **strict**: a wrong kind throws `JsonException`.

| Member | Type | Meaning |
| --- | --- | --- |
| `v.asLong()` | `long` | Either number kind (a fractional value truncates). |
| `v.asDouble()` | `double` | Either number kind (an integer widens). |
| `v.asString()` | `string` | The string. |
| `v.asBool()` | `bool` | The boolean. |

JSON numbers keep their lane: a literal with no `.`/`e`/`E` is an integer (full
64-bit `long`); otherwise it is a double. `asLong`/`asDouble` coerce across both.

**Object navigation** — **forgiving** (chains never crash):

| Member | Type | Meaning |
| --- | --- | --- |
| `v.get(key)` | `JsonValue` | The value at `key`; a `null` value for a missing key **or** a non-object, so `a.get("x").get("y")` is always safe. |
| `v.has(key)` | `bool` | Whether `key` is present (distinguishes an absent key from a present `null`). |
| `v.keys()` | `List<string>` | The object's keys, in insertion order. |

**Array navigation:**

| Member | Type | Meaning |
| --- | --- | --- |
| `v.items()` | `List<JsonValue>` | The elements (an empty list for a non-array). |
| `v.at(i)` | `JsonValue` | The element at `i`; out of range, or on a non-array, throws `JsonException`. |
| `v.size` | `int` | Element count (array) or key count (object). |

The policy is **forgiving navigation, strict extraction**: walking into missing
data yields a `null` value so chains stay safe, but pulling a typed scalar out of
the wrong kind, or indexing past an array, throws.

```breezy
JsonValue root = Json.parse("{\"title\":\"Dune\",\"tags\":[\"sf\",\"classic\"]}");
print(root.get("title").asString());        // Dune.
print(root.get("missing").isNull());         // true (forgiving).
print(root.get("tags").size);                // 2.
print(root.get("tags").at(0).asString());    // sf.
```

---

## Traversal is just collections + closures

Because `items()` is an ordinary `List<JsonValue>`, you query a document with
`forEach` / `filter` / `reduce` and lambdas — the same tools you use on any list:

```breezy
JsonValue catalog = Json.parse(text);   // { "books": [ {"title":..,"price":..}, ... ] }

// Print each book's title.
catalog.get("books").items().forEach(b => print(b.get("title").asString()));

// The books priced over 30.
List<JsonValue> pricey = catalog.get("books").items()
									.filter(b => b.get("price").asLong() > 30);

// Sum every price.
long total = catalog.get("books").items()
				   .reduce(0L, (acc, b) => acc + b.get("price").asLong());
```

---

## Building and serializing

`Json.of(...)` lifts a Breezy value into a `JsonValue`; composites reuse `List`
and `map`, so there is no separate builder type. `Json.stringify` walks a value
back to **compact** JSON text.

| Call | Produces |
| --- | --- |
| `Json.of(long)` / `Json.of(double)` | a number |
| `Json.of(string)` | a string |
| `Json.of(bool)` | a boolean |
| `Json.ofNull()` | a null |
| `Json.of(List<JsonValue>)` | an array |
| `Json.of(map<string,JsonValue>)` | an object |

```breezy
map<string,JsonValue> obj = new map<string,JsonValue>();
obj.put("name", Json.of("Ada"));
obj.put("age", Json.of(36));

string out = Json.stringify(Json.of(obj));   // {"name":"Ada","age":36}

// Round-trips: stringify then parse yields an equal document.
JsonValue again = Json.parse(out);
print(again.get("name").asString());          // Ada.
```

`stringify` escapes `"`, `\`, and control characters (`\n` `\t` `\r` `\b` `\f`,
else `\uXXXX`) and emits other UTF-8 bytes raw. Numbers print in their lane
(integer or double). A **non-finite double** (`NaN`/`Inf`) has no JSON form and
throws `JsonException`.

---

## The supported grammar

Strict RFC 8259: objects, arrays, strings (with the `\" \\ \/ \b \f \n \r \t`
and `\uXXXX` escapes, including UTF-16 surrogate pairs), numbers, `true`,
`false`, `null`. Trailing commas, comments, and unquoted keys are rejected.
Duplicate object keys are last-wins.

---

## v1 boundaries

- **Compact output only** — no pretty-printer yet.
- **No reflection / struct mapping, no schema or validation** — `Json.of` and the
  `JsonValue` accessors are the whole surface.
- **Numbers are `long` + double** — no bignum / arbitrary-precision decimal.
- **Whole document in memory** — there is no streaming/pull cursor.
- **`Json.parse(string)` only** — read the text yourself (e.g.
  [`File.readText`](file.md)) and pass the string.

A parsed tree is an ordinary managed value: it is reference-counted and reclaimed
when its last reference goes away, and the cycle collector traces it like any
object.

---

← [Back to the guide](../guide.md) · [XML](xml.md) · [Generic collections](../types/collections.md) · [First-class functions](../types/functions.md)
