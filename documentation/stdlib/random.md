# Random

`Random` is a static namespace backed by a fast `xoshiro256**` pseudo-random number generator. There is a **single global generator**, lazily seeded from the system clock the first time you draw from it - so sequences differ from run to run, and there is no seed to set (draws are not reproducible).

← [Back to the guide](../guide.md)

---

## Quick start

```breezy
int roll;
roll = Random.get(1, 6);          // [1, 6] - inclusive, a die roll.

int idx;
idx = Random.get(n);              // [0, n) - a valid index into a length-n array.

double d;
d = Random.nextDouble();          // [0, 1).

bool flip;
flip = Random.nextBool();
```

---

## Next-value draws

Each takes no arguments and returns one fresh value.

| Call | Returns | Range |
| --- | --- | --- |
| `Random.nextBool()` | `bool` | `true` or `false`, each ~50%. |
| `Random.nextInt()` | `int` | any 32-bit value, **including negatives**. |
| `Random.nextLong()` | `long` | any 64-bit value, including negatives. |
| `Random.nextFloat()` | `float` | `[0, 1)`, 24 bits of precision. |
| `Random.nextDouble()` | `double` | `[0, 1)`, 53 bits of precision. |
| `Random.nextGaussian()` | `double` | normal distribution, mean `0`, standard deviation `1`. |

`nextInt` and `nextLong` span the **full signed range**, so they are often negative. For a non-negative bounded draw, use `get` instead.

```breezy
double noise;
noise = Random.nextGaussian();    // Most results land within [-3, 3].
```

---

## Ranged draws: `get`

`get` works on `int`, `long`, `float`, and `double`. The result has the same type as its arguments, and the two-argument form requires **both arguments to be that same type**.

| Call | Returns | Range |
| --- | --- | --- |
| `Random.get(bound)` | type of `bound` | `[0, bound)` - upper end **excluded**. |
| `Random.get(origin, bound)` — `int` / `long` | `int` / `long` | `[origin, bound]` - both ends **included**. |
| `Random.get(origin, bound)` — `float` / `double` | `float` / `double` | `[origin, bound)` - upper end **excluded**. |

The two-argument form is **inclusive of `bound` for integers but exclusive for floats** - this mirrors how integer ranges (`1..6` dice) and continuous ranges (`[0, 1)` fractions) are normally expressed.

```breezy
int die;
die = Random.get(1, 6);           // int: 1, 2, 3, 4, 5, or 6.

long big;
big = Random.get(0, 1000000000);  // long: [0, 1000000000].

float pct;
pct = Random.get(0.0, 1.0);       // float/double: [0.0, 1.0) - 1.0 never occurs.
```

Out-of-order or empty ranges are clamped rather than throwing: `get(bound)` with `bound <= 0` returns `0`, and `get(origin, bound)` with `origin >= bound` returns `origin`.

---

## Filling bytes

`nextBytes(byte[] buffer)` fills every element of an existing `byte[]` with random bytes in place and returns nothing. The array must already be allocated.

```breezy
byte[] buf;
buf = new byte[16];
Random.nextBytes(buf);            // buf now holds 16 random bytes.
```

---

## Rules & gotchas

- **One global generator, seeded from the clock.** There is no seed API, so output is not reproducible across runs.
- **`get(bound)` excludes the upper end** (`[0, bound)`) for every type - ideal for array indices: `Random.get(a.length())`.
- **`get(origin, bound)` is inclusive for `int`/`long`, exclusive for `float`/`double`.**
- **`nextInt` / `nextLong` can be negative** - they cover the full signed range. Use `get` for a bounded, non-negative draw.
- **`get` arguments share one type** - `Random.get(1, 6)` draws ints; `Random.get(0.0, 1.0)` draws doubles. Mixing types is rejected at compile time.
- **`nextBytes` requires a `byte[]`** and writes into it in place; allocate the array first.
- **Degenerate ranges are clamped, not errors:** `get(bound)` returns `0` when `bound <= 0`; `get(origin, bound)` returns `origin` when `origin >= bound`.
- **The namespace is static** - always call through `Random.`.

---

← [Clock](clock.md) · [Back to the guide](../guide.md) · Next: [Regex](regex.md)
