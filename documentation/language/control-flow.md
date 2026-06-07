# Control Flow

Control flow decides *which* statements run and *how many times*. Breezy's control structures are the familiar C/Java family: `if`/`else`, `while`, `for`, `foreach`, `switch`, `break`/`continue`, and `return`. This page covers each one completely.

← [Back to the guide](../guide.md)

---

## if / else

Run a block when a condition is true, with an optional `else` for the alternative. The condition must be a `bool` expression in parentheses.

```breezy
if (x < 10)
{
	x = x + 1;
}
else
{
	x = 0;
}
```

You can chain alternatives with `else if`:

```breezy
if (score >= 90)
{
	print("A.");
}
else if (score >= 80)
{
	print("B.");
}
else
{
	print("C.");
}
```

---

## Combining conditions

Combine boolean conditions with the logical operators `and`, `or`, and `not`. `and` and `or` **short-circuit** - the right side is skipped once the left decides the result.

```breezy
if (count > 0 and count < 10)   // Both must hold.
{
	process();
}

if (cached or load())           // load() runs only when cached is false.
{
	render();
}

if (not done)                   // Negation.
{
	step();
}
```

Logical operators work on `bool` only; to manipulate the bits of an integer use the bitwise operators `&`, `|`, `^`, `~` instead. The symbols `&&`, `||`, and `!` are accepted as alternatives for `and`, `or`, and `not` - see [the alternative spellings table](../types/overview.md#alternative-operator-spellings).

---

## while

Repeat a block as long as a condition stays true. The condition is checked **before** each iteration.

```breezy
int i;
i = 0;
while (i < 100)
{
	i = i + 1;
}
```

---

## for

A C-style `for` loop bundles three parts: an initializer, a condition, and a step. This is the **one place** you may declare and assign a variable on the same line. The `++` and `--` operators increment and decrement.

```breezy
int sum;
sum = 0;
for (int j = 0; j <= 10; j++)
{
	sum = sum + j;
}
print(sum);   // 55
```

---

## foreach

`foreach` iterates over the elements of a collection without managing an index. It works over **arrays, strings, maps, and the generic collections**.

```breezy
foreach (int n in nums)
{
	sum = sum + n;
}
```

Maps support iterating values, or keys and values together as a pair:

```breezy
foreach (string key, int value in counts)
{
	print(key + " = " + value);
}
```

For the full map iteration forms, see [Maps](../types/maps.md); for collections, see [Generic collections](../types/collections.md).

---

## break and continue

`break` exits the nearest loop immediately. `continue` skips to the next iteration. Both work inside `while`, `for`, and `foreach`.

```breezy
foreach (int n in nums)
{
	if (n == 0)
	{
		continue;      // Skip zeros.
	}

	if (n > 100)
	{
		break;         // Stop at the first value over 100.
	}

	print(n);
}
```

---

## switch

`switch` selects a branch by value. It uses **C-style fallthrough**: a `case` runs into the next one unless you `break`. A `default` branch handles everything unmatched. Dense integer cases compile to a fast jump table.

```breezy
switch (code)
{
	case 1:
	case 2:
		handleLow();      // 1 and 2 share this body.
		break;
	case 3:
		handleThree();
		// Falls through into default unless a break is added.
	default:
		handleOther();
		break;
}
```

`switch` accepts any integer type (`byte short int long` and the unsigned variants), `bool`, `string`, and [enum](enums.md) values (enums are matched by ordinal, and `case` labels use the unqualified constant name). It **rejects** `float` and `double`, because exact-equality matching is unreliable for them.

---

## return

`return` ends a function and, for a non-`void` function, supplies its result:

```breezy
int twice(int x)
{
	int result;
	result = x * 2;
	return result;
}
```

A `void` function may use a bare `return;` to exit early.

---

## Rules & gotchas

- **Conditions are `bool` expressions** in parentheses - combine them with `and`, `or`, and `not`.
- **`for` headers are the only place to combine declaration and assignment** (`for (int i = 0; ...)`); elsewhere, declare then assign on separate lines.
- **`switch` falls through** - add `break` unless you intend a case to run into the next.
- **`switch` rejects `float`/`double`** - use `if`/`else` for floating-point comparisons.
- **`break`/`continue` act on the nearest enclosing loop.**

---

← [Allman braces & style](allman-braces.md) · [Back to the guide](../guide.md) · Next: [Exceptions](exceptions.md)
