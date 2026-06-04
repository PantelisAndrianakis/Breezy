# Math

`Math` is a built-in **static namespace** of numeric functions. The common operations inline to a handful of SSE instructions - no function call, no boxing - and integer `min`/`max`/`clamp`/`abs` stay on the integer unit. Only the transcendental functions defer to the C math library (`libm`).

← [Back to the guide](../guide.md)

---

## Using Math

Call functions through the `Math` name; there is nothing to import or construct.

```breezy
print(Math.sqrt(16.0));               // 4.
print(Math.max(3, 7));                // 7  -- integer, no floating point.
print(Math.clamp(value, 0, 100));     // value, pinned into [0, 100].

double r;
r = Math.cos(Math.toRadians(60.0));   // ~0.5.
print(Math.pow(2.0, 10.0));           // 1024.
```

---

## What is available

**Inlined to SSE (fast, no call):**

`min`, `max`, `clamp`, `abs`, `sqrt`, `floor`, `ceil`, `round`, `toRadians`.

**Via libm (transcendental):**

`cos`, `tan`, `exp`, `pow`.

`min`, `max`, `clamp`, and `abs` operate on integers as well as floating point - the integer forms stay on the integer unit and never touch floating point, so `Math.max(3, 7)` is exact integer work.

---

## Rules & gotchas

- **`Math` is static** - call `Math.fn(...)`, never construct it.
- **Integer `min`/`max`/`clamp`/`abs` stay integer** - no float conversion.
- **Pass floating-point arguments to the float functions** (`Math.sqrt(16.0)`, not `Math.sqrt(16)`).
- **Use `toRadians` to convert degrees** before `cos`/`tan`, which expect radians.

---

← [C library interop](../ffi/c-interop.md) · [Back to the guide](../guide.md) · Next: [Clock & Random](clock-random.md)
