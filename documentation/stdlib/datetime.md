# DateTime

`DateTime` is a mutable date-time object: a single instant in time that you can read field by field, mutate in place, compare, and format. Every accessor presents **local time**; internally the instant is one epoch-millisecond value.

← [Back to the guide](../guide.md)

---

## Creating a DateTime

```breezy
DateTime now = new DateTime();                       // The current instant.

DateTime fromEpoch = new DateTime(1750000000000L);   // From epoch milliseconds.

DateTime made = new DateTime(2026, 6, 12, 14, 30, 9); // From local components.
```

- `new DateTime()` - the current wall-clock instant.
- `new DateTime(long millis)` - a specific instant, in epoch milliseconds.
- `new DateTime(int year, int month, int day, int hour = 0, int minute = 0, int second = 0)` - built from local-time components. The trailing time fields default to zero, so `new DateTime(2026, 6, 12)` is midnight on that date.

**Months are 1-based**: `1` is January, `12` is December.

---

## Reading fields

```breezy
DateTime d = new DateTime(1750000000000L);

print(d.getYear());        // 2025
print(d.getMonth());       // 6   (June)
print(d.getDay());         // 15
print(d.getHour());        // 15
print(d.getMinute());      // 6
print(d.getSecond());      // 40
print(d.getMillisecond()); // 0   (the within-second remainder, 0..999)
print(d.getDayOfWeek());   // 7   (Sunday)
print(d.toMillis());       // 1750000000000
```

- `getYear` `getMonth` `getDay` `getHour` `getMinute` `getSecond` - the broken-down local fields.
- `getMillisecond()` - the within-second part only, `0`-`999` (not the epoch value).
- `getDayOfWeek()` - the ISO weekday: **1 = Monday … 7 = Sunday**.
- `toMillis()` - the raw instant as epoch milliseconds.

---

## Setting fields

Setters mutate the instant in place and are **lenient**: an out-of-range value rolls over into neighbouring fields rather than erroring.

```breezy
DateTime a = new DateTime(2026, 1, 15);
a.setMonth(13);            // Rolls into the next year: January 2027.
print(a.getYear());        // 2027
print(a.getMonth());       // 1

DateTime b = new DateTime(2026, 1, 31);
b.setDay(32);              // Rolls past January: February 1.
print(b.getMonth());       // 2
print(b.getDay());         // 1
```

Available: `setYear` `setMonth` `setDay` `setHour` `setMinute` `setSecond` `setMillisecond`.

---

## Arithmetic

Two families, both mutating in place:

**Absolute** units add a fixed amount of elapsed time:

```breezy
DateTime c = new DateTime(2026, 6, 1, 23, 0, 0);
c.addHours(25);            // 23:00 + 25h crosses two midnights -> June 3, 00:00.
print(c.getDay());         // 3
print(c.getHour());        // 0
```

`addMillis` `addSeconds` `addMinutes` `addHours` - each adds exactly that many units of real time.

**Calendar** units work on the local fields, and **clamp the day** to the target month's length:

```breezy
DateTime d = new DateTime(2026, 1, 31);
d.addMonths(1);            // Jan 31 has no Feb 31 -> clamped to Feb 28.
print(d.getMonth());       // 2
print(d.getDay());         // 28

DateTime e = new DateTime(2024, 2, 29);
e.addYears(1);             // 2025 is not a leap year -> Feb 28.
print(e.getDay());         // 28
```

- `addDays(int)` - rolls across month and year boundaries.
- `addMonths(int)` - shifts the month, clamping the day to the new month's last valid day.
- `addYears(int)` - shifts the year (clamps Feb 29 in a non-leap target).

Pass a negative count to subtract.

---

## Comparison

```breezy
DateTime a = new DateTime(2026, 6, 12, 14, 30, 9);
DateTime b = new DateTime(a.toMillis());
b.addDays(7);

print(a.before(b));        // true
print(b.after(a));         // true
print(a.equals(a));        // true
print(a.equals(b));        // false
```

`before` / `after` / `equals` compare the underlying instant.

---

## Formatting

```breezy
DateTime a = new DateTime(2026, 6, 12, 14, 30, 9);

print(a.toString());                   // 2026-06-12 14:30:09
print(a.format("yyyy/MM/dd HH:mm"));   // 2026/06/12 14:30
```

- `toString()` - the fixed default form, `yyyy-MM-dd HH:mm:ss`.
- `format(string pattern)` - a token-based pattern.

**Pattern tokens:** `yyyy` `yy` `MM` `dd` `HH` (24-hour) `hh` (12-hour) `mm` `ss` `SSS` (milliseconds) `a` (AM/PM). Any other character is copied through literally.

Both delegate to [`Clock.getDateString`](clock.md).

---

## Rules & gotchas

- **Months are 1-based** (`1`-`12`); `getDayOfWeek` is **ISO** (`1` = Monday … `7` = Sunday).
- **`getMillisecond` is the within-second remainder** (`0`-`999`), while **`toMillis` is the full epoch value**.
- **Setters and the component constructor are lenient** - out-of-range fields roll over by arithmetic rather than erroring.
- **Absolute vs calendar arithmetic differ:** `addHours` etc. add real elapsed time; `addDays`/`addMonths`/`addYears` work on local fields and clamp the day to the month length.
- **All fields are local time.** Conversion uses the host's current time-zone rules; for an instant inside a daylight-saving transition window the offset is taken at the provisional reading, so a value can be ambiguous or shifted by an hour - as it is on any local clock.

---

← [Clock](clock.md) · [Back to the guide](../guide.md)
