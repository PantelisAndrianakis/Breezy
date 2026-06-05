# Random

`Random` is a fast `xoshiro256**` pseudo-random number generator with a familiar API. It is a static namespace.

← [Back to the guide](../guide.md)

---

## Random numbers

`Random` mirrors a familiar API over a fast `xoshiro256**` PRNG.

```breezy
int roll;
roll = Random.get(1, 6);        // [1, 6] inclusive.

double d;
d = Random.nextDouble();        // [0, 1).

boolean flip;
flip = Random.nextBoolean();
```

- **Ranged:** `get(bound)` and `get(origin, bound)` for `int`, `long`, `float`, and `double`.
- **Next-value:** `nextInt`, `nextLong`, `nextFloat`, `nextDouble`, `nextBoolean`, `nextGaussian`, `nextBytes`.

---

## Rules & gotchas

- **`Random.get(origin, bound)` is inclusive of both ends** for the integer forms (e.g. a `1..6` die roll).
- **The namespace is static** - call through `Random.`.

---

← [Clock](clock.md) · [Back to the guide](../guide.md) · Next: [Regex](regex.md)
