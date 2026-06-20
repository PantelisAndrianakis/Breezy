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

## Status

`f64x2` construction and lane access ship today (Domain 2a, part 1). Element-wise
arithmetic (`add`/`mul`, mapping to `addpd`/`mulpd`) and wider types (`f32x4`,
`f64x4`, integer vectors) follow in later parts.
