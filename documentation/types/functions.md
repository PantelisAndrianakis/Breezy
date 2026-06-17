# First-Class Functions - Lambdas & Closures

Functions are values in Breezy. A variable can hold one, a function can return one, a collection can store one, and a lambda can **capture** the variables around it. The function type is written `(P)->R` - parameter types in parentheses, an arrow, then the return type - and a lambda literal is written `params => body`.

← [Back to the guide](../guide.md)

---

## The function type `(P)->R`

A function value has a type just like any other value:

```breezy
(int)->int        // Takes an int, returns an int.
(int, int)->bool  // Takes two ints, returns a bool.
()->int           // Takes nothing, returns an int.
(string)->void    // Takes a string, returns nothing.
```

Use it anywhere a type is expected - a variable, a parameter, a return type, or a collection element type:

```breezy
(int)->int f;
List<(int)->int> handlers = new List<(int)->int>();

(int)->int makeAdder(int k)   // Returns a function value.
{
	return x => x + k;
}
```

---

## Lambda literals `params => body`

A lambda is a parameter list, `=>`, then either a single expression or a `{ ... }` block:

```breezy
(int)->int dbl = x => x * 2;                 // Expression body.
(int, int)->int add = (a, b) => a + b;       // Two parameters.
()->int answer = () => 42;                    // No parameters.
(int)->void log = x => { print(x); };        // Block body.

print(dbl(21));     // 42.
print(add(2, 3));   // 5.
```

Call a function value by naming the variable and applying arguments: `dbl(21)`.

### Parameter-type inference

When the target type is known - from the variable being assigned, the argument slot being passed, or the `return` type - the parameter types are **inferred**, so you can leave them off:

```breezy
(int)->int inc = x => x + 1;        // x is inferred to be int from the target.
```

When there is no target type to infer from, annotate the parameters explicitly:

```breezy
(int)->int g = (int x) => x + 1;    // Explicit when no context drives inference.
```

A bare named function can be used directly where a matching function type is expected:

```breezy
int triple(int x) { return x * 3; }

(int)->int t = triple;   // The named function as a value.
print(t(4));             // 12.
```

---

## Capture is by value

A lambda captures the enclosing variables it references. Capture is **by value**: the lambda takes a snapshot of each captured variable at the moment the closure is created.

```breezy
int base = 10;
(int)->int f = x => x + base;   // Captures the value of base (10).
base = 99;                       // Reassigning the outer variable does NOT
print(f(5));                     // change the closure: prints 15, not 104.
```

The snapshot is of the **variable**, not a deep copy of what it points at. When the captured variable holds a reference (an object, a `List`, a `string`), the closure shares that same target, so mutating the target through the closure is visible outside:

```breezy
List<int> sink = new List<int>();
(int)->void push = v => { sink.add(v); };   // Captures the reference to sink.
push(7);
push(9);
print(sink.size);   // 2  - the mutation landed on the shared list.
```

So: **reassigning** a captured variable's outer binding has no effect on the closure; **mutating the object** a captured reference points at does.

---

## Closures are managed values

A closure is a reference-counted heap value, like an object. It is retained on assignment, released when its last holder goes away, transferred (not copied) on `return`, and retained/released as it moves in and out of a collection - all automatically.

Because the captured variables are part of the closure, the [cycle collector](../memory/automatic-memory.md) traces them. A closure that captures the very object that holds it forms a reference cycle, and that cycle is reclaimed like any other:

```breezy
class Holder
{
	int k;
	(int)->int fn;
}

void makeCycle()
{
	Holder h = new Holder();
	h.k = 5;
	h.fn = x => x + h.k;   // The closure captures h; h.fn holds the closure -> cycle.
}
// After makeCycle returns, both h and its closure are reclaimed by the collector.
```

A non-capturing lambda (and a named-function value) needs no per-call allocation - it is created once and reused for the life of the program.

---

## Collection combinators

`List` (and the other vector-backed collections) accept lambdas through four combinators:

| Combinator | Signature | Result |
| --- | --- | --- |
| `map` | `(T)->T` | A new `List<T>`, one element mapped per source element. |
| `filter` | `(T)->bool` | A new `List<T>` of the elements the predicate keeps. |
| `forEach` | `(T)->void` | Runs the lambda for each element; returns nothing. |
| `reduce` | `reduce(U seed, (U,T)->U)` | Folds the elements into a single `U`. |

```breezy
List<int> n = new List<int>();
for (int i = 1; i <= 5; i = i + 1) { n.add(i); }

int base = 10;
List<int> bumped = n.map(x => x + base);              // 11, 12, 13, 14, 15.

int threshold = 13;
List<int> big = n.filter(x => x + base > threshold);  // Keeps 4 and 5.

int sum = n.reduce(0, (acc, x) => acc + x);           // 15.

print(bumped.get(0));   // 11.
print(big.size);        // 2.
print(sum);             // 15.
```

A statement-level combinator over a literal lambda is compiled to an ordinary loop with the body inlined, so it runs at hand-written-loop speed - there is no per-element function-call overhead in the common case.

### `reserve`

When you know how many elements a `List` will hold, `reserve(capacity)` grows its backing store once up front so the fills that follow never reallocate:

```breezy
List<int> out = new List<int>();
out.reserve(1000);      // One allocation instead of repeated doubling.
```

---

## v1 boundaries

- **`map` preserves the element type.** `List<T>.map` returns a `List<T>`; a type-changing map (`(T)->U` with `U != T`) is not yet supported.
- **Capture is by value only.** A lambda cannot write back to an enclosing scalar variable (there is no by-reference capture). Accumulate with `reduce` instead of mutating an outer variable from `forEach`.
- **Method references are not a value form.** Use a lambda that calls the method (`x => x.foo()`); a bare `obj.foo` is not a function value.
- **Calling a function-valued expression directly** (for example `list.get(0)(5)`) is not supported - bind it to a variable first, then call the variable.
- **Thread safety.** A closure is an ordinary managed value; sharing one across cores follows the same rules as sharing any object. Capturing mutable state and calling the closure from multiple workers is not synchronized for you.

---

← [Back to the guide](../guide.md) · [Generic collections](collections.md)
