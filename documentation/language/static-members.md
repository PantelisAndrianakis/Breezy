# Static Members & Static Classes

Most fields and methods belong to an **object** - each `Counter` has its own count. A **static** member belongs to the **class itself**: there is exactly one copy shared by the whole program, reached through the class name. Breezy supports both levels: mark individual members `static`, or mark a whole class `static` to make it a non-instantiable bag of shared state and utilities.

← [Back to the guide](../guide.md)

---

## Static fields

A `static` field has **one shared slot** across every instance of the class. Unlike instance fields, a static field **may** have a declaration initializer, which runs once at program start.

```breezy
class Counter
{
	static int total = 0;     // One shared slot; runs once at startup.
	int id;                   // Per-object: each Counter has its own id.

	Counter()
	{
		Counter.total = Counter.total + 1;
		this.id = Counter.total;
	}
}

void main()
{
	Counter a = new Counter();
	Counter b = new Counter();
	print(a.id);              // 1
	print(b.id);              // 2
	print(Counter.total);     // 2  -- shared across all Counters.
}
```

Reach a static member through the **class name** (`Counter.total`), including from inside the class's own methods.

---

## Static methods

A `static` method has no `this` - it does not act on a particular object, so you call it on the class:

```breezy
class MathUtil
{
	static int square(int n)
	{
		return n * n;
	}
}

void main()
{
	print(MathUtil.square(5));   // 25
}
```

---

## Static classes

Mark an entire class `static` to make **every** member static and the class **non-instantiable**. This is the right tool for a named collection of global configuration or pure utility functions - there is nothing to construct.

```breezy
static class Config
{
	int maxPlayers = 100;
	string name = "Breezy";

	int dbl(int n)
	{
		return n + n;
	}
}

void main()
{
	print(Config.maxPlayers);    // 100
	Config.maxPlayers = 50;      // Writable.
	print(Config.dbl(21));       // 42
}
```

Trying to write `new Config()` is a compile error - a static class cannot be instantiated.

---

## The singleton pattern

Breezy has no `singleton` keyword because it does not need one: a singleton is simply a static field holding the one instance. Because that instance is a real object, it can be passed around and implement [interfaces](interfaces.md).

```breezy
interface Greeter
{
	string greet();
}

class Server implements Greeter
{
	static Server INSTANCE;

	string greet()
	{
		return "Hello.";
	}
}

void main()
{
	Server.INSTANCE = new Server();

	Greeter g = Server.INSTANCE;         // The one instance, used through its interface.
	print(g.greet());            // Hello.
}
```

---

## How it performs

Static fields are global slots (named `__static_<Class>_<field>` internally), excluded from the per-object layout, so they cost an object nothing. Static methods are plain functions with no `this` pointer. There is no hidden run-time cost - static members are exactly the globals and free functions you would write by hand, just namespaced under a class.

---

## Rules & gotchas

- **Reach static members by class name** (`Class.field`, `Class.method()`), even inside the class itself.
- **Static field initializers run once, at program start** - instance field initializers run per object, just before the constructor body (see [Classes](classes.md)).
- **A `static class` cannot be instantiated** and all of its members are static.
- **A singleton is just a static field** holding one instance - a pattern, not a keyword.

---

← [Enums](enums.md) · [Back to the guide](../guide.md) · Next: [Built-in vector types](vector-types.md)
