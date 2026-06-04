# Clock & Random

`Clock` gives you time - monotonic for measuring durations, wall-clock for dates. `Random` is a fast `xoshiro256**` pseudo-random number generator with a familiar API. Both are static namespaces.

← [Back to the guide](../guide.md)

---

## Clock - measuring time

```breezy
long start;
start = Clock.currentTimeNanos();    // Monotonic nanoseconds, for durations.

long now;
now = Clock.currentTimeMillis();     // Wall-clock milliseconds since the epoch.
```

- `Clock.currentTimeNanos()` - a monotonic nanosecond counter; subtract two readings to measure an elapsed duration.
- `Clock.currentTimeMillis()` - wall-clock milliseconds since the Unix epoch.

---

## Clock - formatting dates

`Clock.getDateString` turns epoch milliseconds into a **local-time** date string - either a fixed ISO default or a Java-style pattern.

```breezy
long now;
now = Clock.currentTimeMillis();

string iso;
iso = Clock.getDateString(now);                          // 2026-06-02 14:30:09.

string custom;
custom = Clock.getDateString(now, "yyyy/MM/dd HH:mm");   // 2026/06/02 14:30.
```

**Pattern tokens:** `yyyy` `yy` `MM` `dd` `HH` (24-hour) `hh` (12-hour) `mm` `ss` `SSS` (milliseconds) `a` (AM/PM). Any other character is copied through literally.

---

## Random - random numbers

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

- **Use `currentTimeNanos` for durations** (monotonic) and `currentTimeMillis` for dates/timestamps (wall-clock).
- **`getDateString` is local time** and accepts a Java-style pattern; unknown characters pass through literally.
- **`Random.get(origin, bound)` is inclusive of both ends** for the integer forms (e.g. a `1..6` die roll).
- **Both namespaces are static** - call through `Clock.` / `Random.`.

---

← [Math](math.md) · [Back to the guide](../guide.md) · Next: [Regex](regex.md)
