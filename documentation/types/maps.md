# Maps

A `map<K,V>` associates **keys** with **values** - a phone book, a cache, a set of counters. Breezy's map is an open-addressing hash table (Swiss-style control bytes) keyed by any integer type, `string`, an `enum`, or an object, with any value type. Keys and values that are objects are reference-counted automatically, and a map caught in a reference cycle is reclaimed by the cycle collector.

← [Back to the guide](../guide.md)

---

## Creating a map and adding entries

Declare `map<K,V>` with the key and value types, create it with `new`, and add entries with `put`.

```breezy
map<string,int> counts = new map<string,int>();
counts.put("apples", 3);
counts.put("apples", counts.get("apples") + 1);   // Overwrite with 4.
```

Map keys may be any integer type, `string`, an `enum`, an object, or a [record](records.md); values can be any type. Integer and string keys match by value. Enum and plain object keys match by **identity** (reference equality) - a distinct but structurally-equal object is a different key, the same as Java's `IdentityHashMap`. A [record](records.md) key matches by **value**: a rebuilt, structurally-equal record finds the same entry.

---

## Looking things up

```breezy
print(counts.get("apples"));         // 4.
print(counts.containsKey("pears"));  // false.
print(counts.containsValue(4));      // true.
print(counts.size);                  // 1  -- a field, not a call.
counts.remove("apples");             // Delete an entry.
```

Core operations: `put(k, v)`, `get(k)`, `containsKey(k)`, `containsValue(v)`, `remove(k)`, and the `.size` field.

---

## Views and iteration

You can pull keys, values, or entries out of a map:

- `getKeys()` returns an owned `K[]` snapshot.
- `getValues()` returns an owned `V[]` snapshot.
- `getEntries()` returns an `Entry[]`, where each `Entry` exposes `getKey()` and `getValue()`.

```breezy
foreach (int v in counts.getValues())
{
	print(v);
}

foreach (Entry e in counts.getEntries())
{
	print(e.getKey());
	print(e.getValue());
}
```

The most direct way to walk a map is the **pair `foreach`**, which gives you each key and value together. The keys-only form also works.

```breezy
foreach (string name, int count in counts)   // Key and value together.
{
	print(name + " = " + count);
}

foreach (string name in counts)              // Keys only.
{
	print(name);
}
```

---

## Maps and memory

Managed keys and values are **retained** while stored and **released** when removed or when the map is reclaimed. If a map ends up in a reference cycle (it holds an object that, directly or indirectly, points back at the map), the [cycle collector](../memory/automatic-memory.md) cleans it up - you never annotate a weak reference.

---

## Rules & gotchas

- **Keys** may be any integer type, `string`, an `enum`, or an object; values can be any type. Enum and object keys match by **identity**.
- **`put` overwrites** an existing key's value.
- **`.size` is a field**, not a method.
- **`getKeys`/`getValues`/`getEntries` return owned snapshots** - iterating them is safe.
- **Use `containsKey` to test membership** (it replaced an older `has`).

---

← [Arrays](arrays.md) · [Back to the guide](../guide.md) · Next: [Generic collections](collections.md)
