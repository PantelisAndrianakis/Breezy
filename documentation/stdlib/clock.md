# Clock

`Clock` gives you time - monotonic for measuring durations, wall-clock for dates. It is a static namespace.

← [Back to the guide](../guide.md)

---

## Measuring time

```breezy
long start = Clock.currentTimeNanos();    // Monotonic nanoseconds, for durations.

long now = Clock.currentTimeMillis();     // Wall-clock milliseconds since the epoch.
```

- `Clock.currentTimeNanos()` - a monotonic nanosecond counter; subtract two readings to measure an elapsed duration.
- `Clock.currentTimeMillis()` - wall-clock milliseconds since the Unix epoch.

---

## Formatting dates

`Clock.getDateString` turns epoch milliseconds into a **local-time** date string - either a fixed ISO default or a Java-style pattern.

```breezy
long now = Clock.currentTimeMillis();

string iso = Clock.getDateString(now);                          // 2026-06-02 14:30:09.

string custom = Clock.getDateString(now, "yyyy/MM/dd HH:mm");   // 2026/06/02 14:30.
```

**Pattern tokens:** `yyyy` `yy` `MM` `dd` `HH` (24-hour) `hh` (12-hour) `mm` `ss` `SSS` (milliseconds) `a` (AM/PM). Any other character is copied through literally.

---

## Rules & gotchas

- **Use `currentTimeNanos` for durations** (monotonic) and `currentTimeMillis` for dates/timestamps (wall-clock).
- **`getDateString` is local time** and accepts a Java-style pattern; unknown characters pass through literally.
- **The namespace is static** - call through `Clock.`.

---

← [Math](math.md) · [Back to the guide](../guide.md) · Next: [Random](random.md)
