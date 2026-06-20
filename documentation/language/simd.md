# SIMD vector types

Fixed-width packed numeric values that live in a single SSE register instead of
being spread across scalar slots. The first type is `f64x2`: two packed `double`
lanes (low lane `x`, high lane `y`) held in one 128-bit xmm register.

A `f64x2` is a value, not a heap object — it occupies a 16-byte stack slot and is
never reference-counted. It is constructed and consumed through the `Simd`
namespace; it does not flow through `print`, string concatenation, or the scalar
arithmetic operators (lane access yields ordinary `double`s for that).

## Constructing and reading lanes

```breezy
void main()
{
	f64x2 v = Simd.pack(3.5, 7.25);   // low lane = x = 3.5, high lane = y = 7.25.

	double a = Simd.x(v);             // 3.5
	double b = Simd.y(v);             // 7.25

	print(a + b);                     // 10.75
}
```

| Call | Result | Meaning |
|------|--------|---------|
| `Simd.pack(x, y)` | `f64x2` | Pack two floating-point values into the low and high lanes. |
| `Simd.x(v)` | `double` | The low lane. |
| `Simd.y(v)` | `double` | The high lane. |

## Element-wise arithmetic

Each call operates on both lanes at once (one SSE instruction), returning a new
`f64x2`.

```breezy
void main()
{
	f64x2 a = Simd.pack(3.0, 8.0);
	f64x2 b = Simd.pack(1.5, 2.0);

	f64x2 s = Simd.add(a, b);   // [4.5, 10]
	f64x2 p = Simd.mul(a, b);   // [4.5, 16]

	print(Simd.x(s));           // 4.5
	print(Simd.y(p));           // 16
}
```

| Call | Maps to | Meaning |
|------|---------|---------|
| `Simd.add(a, b)` | `addpd` | Per-lane sum. |
| `Simd.sub(a, b)` | `subpd` | Per-lane difference. |
| `Simd.mul(a, b)` | `mulpd` | Per-lane product. |
| `Simd.div(a, b)` | `divpd` | Per-lane quotient. |

## Status

`f64x2` construction, lane access, and element-wise `add`/`sub`/`mul`/`div` ship
today (Domain 2a, parts 1–2). Wider types (`f32x4`, `f64x4`, integer vectors)
follow in later parts.
