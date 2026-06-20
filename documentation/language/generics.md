# Generics

**Generics** let you write a class once and reuse it for many types. Instead of writing a separate `IntBox`, `StringBox`, and `DogBox`, you write one `Box<T>` where `T` is a **type parameter** filled in when you use it. Breezy generics are **monomorphized**, so they cost nothing at run time: there is no boxing and no extra dispatch.

← [Back to the guide](../guide.md)

---

## Declaring a generic class

Put one or more type parameters in angle brackets after the class name. Inside the class, the parameter is used like any other type.

```breezy
class Box<T>
{
	T value;

	Box(T value)
	{
		this.value = value;
	}

	T get()
	{
		return this.value;
	}
}
```

`T` is a placeholder. `Box<int>` makes `T` mean `int`; `Box<string>` makes `T` mean `string`.

---

## Using a generic class

Name the concrete type in the angle brackets both when you declare the variable and when you call `new`:

```breezy
void main()
{
	Box<int> a = new Box<int>(42);
	print(a.get());        // 42

	Box<string> b = new Box<string>("hello");
	print(b.get());        // hello
}
```

---

## Multiple type parameters

A class can take several type parameters, separated by commas. A `Pair<K, V>` holds two values of independent types:

```breezy
class Pair<K, V>
{
	K k;
	V v;

	Pair(K k, V v)
	{
		this.k = k;
		this.v = v;
	}

	K first()
	{
		return this.k;
	}

	V second()
	{
		return this.v;
	}
}

void main()
{
	Pair<int, string> p = new Pair<int, string>(42, "answer");
	print(p.first());    // 42
	print(p.second());   // answer
}
```

---

## Interface bounds

By default a type parameter is opaque - you can store and pass a `T`, but you cannot call methods on it, because the compiler does not know what `T` can do. An **interface bound** fixes that: `<T: Speaker>` promises that whatever fills `T` implements [`Speaker`](interfaces.md), so you may call `Speaker`'s methods on a `T`.

```breezy
interface Speaker
{
	string speak();
}

class Announcer<T: Speaker>
{
	T who;

	Announcer(T who)
	{
		this.who = who;
	}

	string announce()
	{
		return this.who.speak();   // Allowed: T is bound to Speaker.
	}
}
```

Now `Announcer<Dog>` works because `Dog` implements `Speaker`; a type that does not implement `Speaker` is rejected at compile time.

---

## Parametric inheritance

A generic class can **extend another generic class**, forwarding its type
parameter to the parent. Each instantiation monomorphizes both layers
independently:

```breezy
class Holder<T>
{
	T v;

	Holder(T x)
	{
		this.v = x;
	}

	T get()
	{
		return this.v;
	}
}

class Tagged<T> extends Holder<T>   // Pass T through to the parent.
{
	int tag;

	Tagged(T x)
	{
		this.v = x;
		this.tag = 7;
	}
}

void main()
{
	Tagged<int> t = new Tagged<int>(42);
	print(t.get());          // 42  -- inherited from Holder<int>.

	Holder<int> h = t;       // A subclass value is usable as the parent type.
	print(h.get());          // 42
}
```

`Tagged<int>` synthesizes `Tagged$int` extending `Holder$int`; `Tagged<string>`
synthesizes a separate `Tagged$string` extending `Holder$string`. The parent
instantiation is created automatically, and the usual field/method inheritance
and [virtual dispatch](classes.md) apply across the two monomorphized layers.

---

## How it performs: monomorphization

Breezy generics are **monomorphized**. For each concrete instantiation, the compiler synthesizes one ordinary class - `Box$int`, `Pair$int$string`, `Announcer$Dog` - exactly as if you had written it by hand. The consequences:

- **No boxing.** `Box<int>` stores a real 32-bit `int`, not a heap-allocated wrapper.
- **No dispatch overhead.** A generic method call is a direct call.
- **Zero cost when unused.** A class that uses no generics compiles to exactly the same code as before generics existed.

You get the flexibility of generics with the performance of hand-specialized code.

---

## Rules & gotchas

- **Name the type arguments on both sides:** `Box<int> a = new Box<int>(...);`.
- **Bounds are interfaces, not classes** - `<T: Speaker>` requires `T` to *implement* the interface `Speaker`.
- **Without a bound, you cannot call methods on a `T`** - you can only store, pass, and return it.
- The standard-library [collections](../types/collections.md) (`List`, `Set`, and friends) use this same monomorphizing mechanism, which is why they also have no boxing.

---

← [Interfaces](interfaces.md) · [Back to the guide](../guide.md) · Next: [Enums](enums.md)
