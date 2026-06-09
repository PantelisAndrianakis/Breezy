# Records

A **record** is a `class` defined by its data: the compiler synthesizes value
equality for it. Two records of the same type are **equal when their fields are
equal**, so a record is what you reach for when you want to use a composite value
as a [map](maps.md) key or a [`Set`](collections.md) element and look it up with a
freshly built, structurally-equal value.

← [Back to the guide](../guide.md)

---

## Declaring a record

A record has the same body as a [class](../language/classes.md) — fields, a
constructor, and methods — but is written with `record`:

```breezy
record Point
{
	int x;
	int y;
	Point(int x, int y)
	{
		this.x = x;
		this.y = y;
	}
}
```

The compiler generates two methods over the declared fields:

- `bool equals(Point other)` - true when every field is equal.
- `int hashCode()` - a hash consistent with `equals` (equal records hash equal).

Both are callable directly, like any method:

```breezy
Point a = new Point(3, 4);
Point b = new Point(3, 4);
print(a.equals(b));                  // true  -- equal by value, not identity.
print(a.hashCode() == b.hashCode()); // true
```

---

## Value keys

The point of a record is that maps and sets key it **by value**. A rebuilt,
structurally-equal record finds the existing entry:

```breezy
map<Point,int> grid = new map<Point,int>();
grid.put(new Point(3, 4), 42);
print(grid.get(new Point(3, 4)));        // 42  -- a different object, same value.
print(grid.containsKey(new Point(3, 4))); // true

Set<Point> seen = new Set<Point>();
seen.add(new Point(1, 2));
seen.add(new Point(1, 2));   // Deduplicated -- equal by value.
print(seen.size);            // 1
```

Contrast this with a plain `class` key, which matches by **identity**: a distinct
object is always a distinct key, no matter its field values.

---

## How fields are compared

`equals`/`hashCode` treat each field according to its type:

| Field type | Compared / hashed by |
| --- | --- |
| any integer, `bool` | value |
| `float` / `double` | value (bit pattern) |
| `string` | content |
| another **record** | recursively, by value |
| any other object / array / map / collection | **identity** (reference) |

So a record that holds another record compares deeply, but a record that holds a
plain object is equal only when that field is the *same* object:

```breezy
record Holder
{
	Tag tag;             // Tag is a plain class.
	Holder(Tag tag) { this.tag = tag; }
}

Tag t = new Tag(1);
map<Holder,int> m = new map<Holder,int>();
m.put(new Holder(t), 5);
print(m.containsKey(new Holder(t)));          // true  -- same Tag reference.
print(m.containsKey(new Holder(new Tag(1)))); // false -- different Tag identity.
```

---

## Memory and performance

A record is an ordinary heap object: ARC-managed, reachable by the
[cycle collector](../memory/automatic-memory.md), passed by reference. `record`
changes **equality**, not storage — it is not a stack/by-value type. A record used
as a map key participates in cycles and is reclaimed like any other object.

Map and set operations on record keys cost one `hashCode` call plus one `equals`
call per probe; because `hashCode` mixes every field (string fields by content,
nested records recursively), record keys distribute across buckets and stay
**O(1)** on average.

---

## Rules & gotchas

- **A record is final** - no class may `extend` a record, and a record may not
  extend another type. This keeps a record key always exactly its own type.
- **All declared instance fields participate** in `equals`/`hashCode`.
- **Don't mutate a field of a record after using it as a key** - its hash changes
  and the entry becomes unreachable, the same caveat as any hash map.
- **`float`/`double` fields are fine** (compared by value), but `float`/`double`
  are still rejected as map/set **keys** directly.
- User-written `hashCode`/`equals` are not supported; the compiler always
  synthesizes them.

---

← [Generic collections](collections.md) · [Back to the guide](../guide.md) · Next: [Strings](strings.md)
