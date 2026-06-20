# Enums

An **enum** is a type with a fixed set of named constant values - directions, colours, operations, states. Breezy enums follow the Java model: each constant is a **singleton object** of the enum's class, so constants can carry fields, take constructor arguments, define methods, and even give each constant its own behaviour. They are far more than named integers.

← [Back to the guide](../guide.md)

---

## A simple enum

List the constants, separated by commas. Each constant is a single, shared instance of the enum type.

```breezy
enum Direction
{
	NORTH, EAST, SOUTH, WEST;
}

void main()
{
	Direction d = Direction.NORTH;
	print(d.name());      // NORTH
	print(d.ordinal());   // 0
}
```

Reach a constant through the enum name: `Direction.NORTH`.

---

## Built-in members

Every enum constant comes with these built-ins for free:

- `name()` - the constant's name as a `string` (`"NORTH"`).
- `ordinal()` - its zero-based position in the declaration (`NORTH` is `0`, `EAST` is `1`, ...).
- `Enum.values()` - an array of every constant, in declaration order.
- `Enum.valueOf(string)` - the constant whose name matches the string.

```breezy
Direction d = Direction.valueOf("SOUTH");
print(d.ordinal());      // 2

foreach (Direction each in Direction.values())
{
	print(each.name());  // NORTH EAST SOUTH WEST
}
```

---

## Constants with fields and a constructor

A constant may carry data. Declare fields and a constructor on the enum, then pass arguments in each constant's parentheses. Note the semicolon that separates the constant list from the members.

```breezy
enum Color
{
	RED(255, 0, 0), GREEN(0, 255, 0), BLUE(0, 0, 255);

	int r;
	int g;
	int b;

	Color(int r, int g, int b)
	{
		this.r = r;
		this.g = g;
		this.b = b;
	}
}

void main()
{
	print(Color.GREEN.ordinal());   // 1
	print(Color.GREEN.name());      // GREEN
	print(Color.BLUE.b);            // 255
}
```

---

## Methods and per-constant bodies

An enum can define methods shared by all constants, and any constant can **override** a method with its own body in braces. This makes each constant behave differently while sharing one type.

```breezy
enum Op
{
	ADD
	{
		int apply(int a, int b)
		{
			return a + b;
		}
	},
	SUB
	{
		int apply(int a, int b)
		{
			return a - b;
		}
	};

	int apply(int a, int b)
	{
		return 0;   // Default, overridden by each constant above.
	}
}

void main()
{
	print(Op.ADD.apply(3, 4));   // 7
	print(Op.SUB.apply(3, 4));   // -1
}
```

An enum may also `implements` an [interface](interfaces.md), with each constant fulfilling the contract.

---

## Enums in a switch

[`switch`](control-flow.md) accepts an enum value, and the `case` labels are the **unqualified** constant names (write `RED`, not `Color.RED`):

```breezy
Color c = Color.valueOf("BLUE");
switch (c)
{
	case RED:
		print("Stop.");
		break;
	case BLUE:
		print("Cool.");
		break;
	default:
		print("Other.");
		break;
}
```

---

## Payload variants (sum types)

A constant may instead declare its own **typed fields** in the parentheses,
making it a *variant* that is constructed per use rather than a shared
singleton. This turns the enum into a **sum type** - a value that is exactly one
of several shapes, each carrying its own data.

```breezy
enum Shape
{
	Circle(double r),
	Rect(double w, double h);
}

void main()
{
	Shape s = Shape.Circle(2.0);     // Build a fresh Circle.
	Shape t = Shape.Rect(3.0, 4.0);  // ...or a Rect.

	print(s.name());      // Circle
	print(t.ordinal());   // 1

	match (t)             // Dispatch on the variant.
	{
		Circle => print("round");
		Rect   => print("boxy");
	}
}
```

A variant is built with arguments (`Shape.Circle(2.0)`); naming it bare is an
error. Each variant lowers to its own subclass (`Shape$Circle`) carrying its
fields, so a non-escaping variant is **stack-allocated** like any other object -
no heap, no reference counting - and `name()`/`ordinal()`/`match` work exactly
as for plain constants.

> A payload enum and singleton constants are different uses of the same `enum`:
> bare names (`RED`) are singletons; `Name(type field, ...)` declares a variant.

---

## How it performs

Enums lower to ordinary classes: each constant becomes a singleton object constructed once at program start, and per-constant bodies become subclasses. Dispatch is therefore the same zero-overhead [virtual call](classes.md#virtual-dispatch-polymorphism) as any method - there is no special run-time machinery. A [payload variant](#payload-variants-sum-types) is built on demand through its own subclass constructor and rides the same escape-analysis [stack allocation](../memory/automatic-memory.md) as any short-lived object.

---

## Rules & gotchas

- **A semicolon ends the constant list** before any fields, constructor, or methods.
- **`case` labels are unqualified** constant names inside a `switch`.
- **Constants are singletons** - the same `Color.RED` object every time, so it can be compared and passed around freely.
- Switch supports enums (matched by ordinal), plus any integer type, `bool`, and `string`; it rejects `float`/`double` because exact-equality matching is unreliable for them.

---

← [Generics](generics.md) · [Back to the guide](../guide.md) · Next: [Match](match.md)
