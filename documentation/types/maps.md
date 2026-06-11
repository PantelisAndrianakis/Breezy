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

## Compound operations

Two operations combine a lookup and its follow-up into **one atomic step**:

```breezy
int id = ids.putIfAbsent("alice", nextId);   // Insert if absent; returns the value now at the key.
int n  = counts.getOrDefault("pears", 0);    // The value, or the default when absent.
```

- `putIfAbsent(k, v)` inserts `v` only when `k` is absent and returns **the value now associated
  with `k`** - the existing value when the key was already present, else `v`.
- `getOrDefault(k, d)` returns the value, or `d` when the key is absent - no ambiguity about
  whether a returned `0` was stored or missing.

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

## Thread safety

Maps are **automatically safe to share across [breezes](../concurrency/breezes.md)**. Once a map crosses to another breeze (channel, `spawn` argument, or static field), every operation - `put`, `get`, `remove`, `containsKey`, even the internal rehash on growth - runs atomically, with no locking in your code. A map that stays within one breeze pays no synchronization cost.

The guarantee is **per operation**: a check-then-act sequence like `if (!m.containsKey(k)) m.put(k, v)` can still interleave with another breeze between the two calls. For exactly those patterns, use the [compound operations](#compound-operations) - `putIfAbsent` and `getOrDefault` are single atomic steps, so on a shared map they replace the check-then-act sequences outright. Iterating a shared map is always safe and sees a consistent point-in-time view; updates made by other breezes during the loop may not appear until the next iteration over the map.

---

## Rules & gotchas

- **Keys** may be any integer type, `string`, an `enum`, or an object; values can be any type. Enum and object keys match by **identity**.
- **`put` overwrites** an existing key's value.
- **`.size` is a field**, not a method.
- **`getKeys`/`getValues`/`getEntries` return owned snapshots** - iterating them is safe.
- **Use `containsKey` to test membership** (it replaced an older `has`).

---

← [Arrays](arrays.md) · [Back to the guide](../guide.md) · Next: [Generic collections](collections.md)
