# SIMD vector types

Fixed-width packed numeric values that live in a single SSE register instead of
being spread across scalar slots:

- `f64x2` — two packed `double` lanes (`x`, `y`).
- `f32x4` — four packed `float` lanes (`x`, `y`, `z`, `w`).
- `i32x4` — four packed `int` lanes (`x`, `y`, `z`, `w`).
- `f64x4` — four packed `double` lanes in a 256-bit AVX register (`x`, `y`, `z`, `w`).
- `f32x8` — eight packed `float` lanes in a 256-bit AVX register.
- `i32x8` — eight packed `int` lanes in a 256-bit AVX2 register.

A vector is a value, not a heap object — it occupies a 16-byte stack slot and is
never reference-counted. It is constructed and consumed through the `Simd`
namespace; it does not flow through `print`, string concatenation, or the scalar
arithmetic operators (lane access yields an ordinary `double`/`float`/`int` for
that). `i32x4` has no `div` (there is no packed integer divide instruction).

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
| `Simd.pack(x, y, z, w)` | `f32x4`/`i32x4` | Pack four floats (`f32x4`) or four ints (`i32x4`) into the four lanes. |
| `Simd.x(v)` | `double`/`float` | Lane 0. |
| `Simd.y(v)` | `double`/`float` | Lane 1. |
| `Simd.z(v)` | `float` | Lane 2 (`f32x4` only). |
| `Simd.w(v)` | `float` | Lane 3 (`f32x4` only). |

`Simd.pack` and the lane accessors pick the vector type by arity: two lanes give
an `f64x2`, four give an `f32x4`. The element-wise and load/store calls below
work on either type.

## Element-wise arithmetic

Each call operates on every lane at once (one SSE instruction), returning a new
vector of the same type — `addpd`/`mulpd`… for `f64x2`, `addps`/`mulps`… for
`f32x4`, `paddd`/`pmulld`/`pminsd`/`pmaxsd` for `i32x4`. `i32x4` supports
`add`/`sub`/`mul`/`min`/`max` but not `div`.

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
| `Simd.min(a, b)` | `minpd` | Per-lane minimum. |
| `Simd.max(a, b)` | `maxpd` | Per-lane maximum. |

## Horizontal reductions

Collapse a vector to a single scalar of its lane type.

| Call | Result | Meaning |
|------|--------|---------|
| `Simd.sum(v)` | `double`/`float` | Sum of all lanes. |
| `Simd.dot(a, b)` | `double`/`float` | Sum of the per-lane products (`a·b`). |

```breezy
f32x4 a = Simd.pack(1.0, 2.0, 3.0, 4.0);
f32x4 b = Simd.pack(2.0, 2.0, 2.0, 2.0);
print(Simd.sum(a));      // 10
print(Simd.dot(a, b));   // 20
```

For a reduction over a long array, an in-place packed accumulator beats
`Simd.dot` per element — accumulate packed products in a loop, then `Simd.sum`
(or `Simd.x + Simd.y`) the accumulator once at the end.

## Packed array load and store

`Simd.load`/`Simd.store` move one packed lane group to or from an array in a
single 16-byte op — two elements for a `double[]` (`f64x2`), four for a `float[]`
(`f32x4`). These are raw packed primitives: they do **not** bounds-check, so the
caller guarantees the whole lane group is in range (typically by striding the
loop in steps of the lane count over an array sized to a multiple of it).

| Call | Result | Meaning |
|------|--------|---------|
| `Simd.load(a, i)` | `f64x2`/`f32x4` | Load the lane group starting at index `i` of `double[]`/`float[] a`. |
| `Simd.store(a, i, v)` | — | Store `v` into the lane group starting at index `i`. |

A dot product accumulates packed products; a register-promoted `f64x2`
accumulator carries the reduction in an xmm register with no per-iteration
memory traffic:

```breezy
f64x2 acc;
acc = Simd.pack(0.0, 0.0);
for (int i = 0; i < n; i = i + 2)
{
	acc = Simd.add(acc, Simd.mul(Simd.load(a, i), Simd.load(b, i)));
}

double dot;
dot = Simd.x(acc) + Simd.y(acc);
```

## 256-bit AVX (`f64x4`)

`f64x4` packs four doubles, `f32x8` eight floats, into a 256-bit AVX register.
The constructors and array I/O are width-explicit (`*256`) because the lane
types/counts overlap the SSE widths by arity; everything else (lanes, element
ops, `sum`/`dot`) dispatches by the value's type. `f32x8` lanes 0–3 read with
`x`/`y`/`z`/`w`; all eight reach an array through `Simd.store256`.

| Call | Result | Meaning |
|------|--------|---------|
| `Simd.pack256(a, b, c, d)` | `f64x4` | Pack four doubles. |
| `Simd.pack256(a..h)` | `f32x8`/`i32x8` | Pack eight floats (`f32x8`) or eight ints (`i32x8`). |
| `Simd.load256(a, i)` | `f64x4`/`f32x8`/`i32x8` | Load the packed group at `i` of a `double[]`/`float[]`/`int[]`. |
| `Simd.store256(a, i, v)` | — | Store `v` into the packed group at `i`. |

```breezy
f64x4 a = Simd.pack256(1.0, 2.0, 3.0, 4.0);
f64x4 b = Simd.load256(arr, i);
double s = Simd.sum(Simd.mul(a, b));   // four-lane dot of one block
```

Element ops lower to the VEX-encoded AVX forms (`vaddpd`/`vmulps`/`vpaddd`/…).
A 256-bit program checks for the required CPU feature once at startup and aborts
with a clear message if absent, rather than faulting on the first wide
instruction: `f64x4`/`f32x8` need AVX, the integer `i32x8` needs AVX2. A program
that uses no 256-bit type carries none of this. (There is no scalar fallback — a
256-bit program needs the feature to run.)

## Status

The full vector set ships today (Domain 2 complete): 128-bit `f64x2`/`f32x4`/`i32x4`
and 256-bit `f64x4`/`f32x8`/`i32x8`, with construction, lane access, element-wise
`add`/`sub`/`mul`/`div` (no `div` for integer vectors), `min`/`max`, packed
`load`/`store`, and horizontal `sum`/`dot`. The startup guard aborts cleanly on a
CPU lacking the required AVX/AVX2 feature.
