# Option & Result

`Option<T>` and `Result<T, E>` are built-in [generic sum types](../language/enums.md#generic-enums)
for the two most common "might not have a value" shapes. They need no import or
definition - every program has them.

← [Back to the guide](../guide.md)

---

## Option - a value or nothing

```breezy
enum Option<T>
{
	Some(T v),
	None;
}
```

Use it for a result that may be absent, instead of a sentinel or null:

```breezy
Option<int> found = lookup(key);

match (found)
{
	Some(v) => print(v);
	None    => print("missing");
}
```

Build `Some` with its value and `None` bare; both need a typed context (a typed
local, field, return, or argument) so the element type is known:

```breezy
Option<int> a = Option.Some(5);
Option<int> b = Option.None;
```

---

## Result - a value or an error

```breezy
enum Result<T, E>
{
	Ok(T v),
	Err(E e);
}
```

Use it to return either a success value or an error, without exceptions:

```breezy
Result<int, string> parsed = parseInt(text);

match (parsed)
{
	Ok(n)  => print(n);
	Err(m) => print(m);
}
```

---

## Rules & gotchas

- **No import needed** - both are always in scope.
- **Construction needs a typed context** so `T` (and `E`) are known - a typed
  local, field, return type, or argument.
- **Match to consume them** - bind the payload in the arm (`Some(v)`, `Ok(n)`,
  `Err(m)`).
- They are ordinary [generic enums](../language/enums.md#generic-enums); a
  non-escaping value is stack-allocated like any other.

---

← [Back to the guide](../guide.md)
