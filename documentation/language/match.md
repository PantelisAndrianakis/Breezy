# Match

`match` selects one of several arms by the value of an enum, like
[`switch`](control-flow.md), but with two guarantees `switch` does not give:

- **Exhaustive.** A `match` over an enum must handle *every* constant, or carry a
  `default` arm. A missing constant is a **compile error**, so adding a new enum
  constant turns every now-incomplete `match` into a build failure instead of a
  silent fall-through at run time.
- **No fall-through.** Each arm runs only its own body; there is no implicit
  continuation into the next arm, so no `break` is needed (or allowed as a
  separator).

← [Back to the guide](../guide.md)

---

## Form

A subject in parentheses, then one arm per line: a constant label (or `default`),
`=>`, and either a single statement or a `{ ... }` block.

```breezy
enum Color { RED(255,0,0), GREEN(0,255,0), BLUE(0,0,255); int r; int g; int b;
	Color(int r,int g,int b){ this.r=r; this.g=g; this.b=b; } }

void describe(Color c)
{
	match (c)
	{
		RED   => print("warm");
		GREEN => { print("cool"); print("calm"); }   // Block arm.
		BLUE  => print("cool");
	}
}
```

All three constants are covered, so this compiles. Drop the `BLUE` arm and the
compiler rejects it:

```
Non-exhaustive match; missing case: BLUE
```

---

## The default arm

A `default` arm covers every constant not named explicitly, and satisfies the
exhaustiveness requirement on its own:

```breezy
match (c)
{
	RED     => print("warm");
	default => print("something else");
}
```

---

## How it performs

`match` lowers to exactly the same dispatch as [`switch`](control-flow.md) over
the enum's ordinal: a dense jump table when the constants are contiguous, a
compare chain otherwise. The exhaustiveness check and the implicit per-arm break
are entirely compile-time — there is **no run-time cost** over a hand-written
`switch`.

---

## Rules & gotchas

- **Subject must be an enum.** A `match` with no `default` over a non-enum is
  rejected; use `switch` for integers, bools, and strings.
- **Every constant or a `default`.** Otherwise the match is non-exhaustive and
  will not compile.
- **No fall-through and no `break`.** Each arm is self-contained.
- **Arms are a single statement or a `{ }` block.**

---

← [Enums](enums.md) · [Back to the guide](../guide.md) · Next: [Static members](static-members.md)
