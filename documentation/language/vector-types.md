# Built-in Vector Types

Breezy ships **eight ready-made vector types** for 2D and 3D math - one per combination of dimension and number type. They are ordinary classes you can use immediately, with a constructor, component-wise `equals`, and a `calculateDistance` method. Because they are plain classes, a vector that does not escape its scope is **stack-allocated** (no heap, no reference counting).

← [Back to the guide](../guide.md)

---

## The eight types

| Type | Components | Element type |
| --- | --- | --- |
| `Vector2i` | `x`, `y` | `int` |
| `Vector2l` | `x`, `y` | `long` |
| `Vector2f` | `x`, `y` | `float` |
| `Vector2d` | `x`, `y` | `double` |
| `Vector3i` | `x`, `y`, `z` | `int` |
| `Vector3l` | `x`, `y`, `z` | `long` |
| `Vector3f` | `x`, `y`, `z` | `float` |
| `Vector3d` | `x`, `y`, `z` | `double` |

The suffix encodes the element type: `i` = `int`, `l` = `long`, `f` = `float`, `d` = `double`. The number `2` or `3` is the dimension.

> **Note:** there is no plain `Vector2` or `Vector3` - always use a suffixed type such as `Vector2f` or `Vector3i`.

---

## Constructing and reading

Pass one value per component to the constructor, then read the fields directly:

```breezy
void main()
{
	Vector3f a;
	a = new Vector3f(0.0f, 0.0f, 0.0f);

	Vector3f b;
	b = new Vector3f(2.0f, 3.0f, 6.0f);

	print(b.x);   // 2
	print(b.y);   // 3
	print(b.z);   // 6
}
```

---

## equals - component-wise comparison

`equals` returns a `boolean` and compares every component exactly:

```breezy
Vector2i p;
p = new Vector2i(3, 4);

Vector2i q;
q = new Vector2i(3, 4);

print(p.equals(q));   // true  -- same components.
```

---

## calculateDistance - euclidean distance

`calculateDistance` returns the straight-line distance between two vectors. The return type depends on the element type:

- For the `*f` (float) variants it returns a **`float`**.
- For the integer (`*i`, `*l`) and `*d` (double) variants it returns a **`double`** - integer distances are usually irrational, so they promote to `double`.

```breezy
Vector3f a;
a = new Vector3f(0.0f, 0.0f, 0.0f);
Vector3f b;
b = new Vector3f(2.0f, 3.0f, 6.0f);
print(a.calculateDistance(b));   // 7   (float result for *f).

Vector2i origin;
origin = new Vector2i(0, 0);
Vector2i point;
point = new Vector2i(3, 4);
print(origin.calculateDistance(point));   // 5   (double result for integer vectors).
```

---

## Why they are cheap

These are normal classes, so they ride the same [automatic memory](../memory/automatic-memory.md) rules as anything else. A vector local that does not escape the function is allocated on the **stack** - no `malloc`, no reference counting, gone at scope exit. The per-tick scratch vectors a game or physics loop creates by the million never touch the heap.

---

## Rules & gotchas

- **Always use a suffixed type** (`Vector2f`, `Vector3i`, ...); plain `Vector2`/`Vector3` do not exist.
- **Match your literals to the element type** - use `0.0f` for `*f` types, `0.0` for `*d` types, plain integers for `*i`.
- **`calculateDistance` returns `float` only for `*f` types**; everything else returns `double`.
- **`equals` is exact** component-wise comparison.

---

← [Static members](static-members.md) · [Back to the guide](../guide.md) · Next: [No primitive wrapper classes](no-wrapper-classes.md)
