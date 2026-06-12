# Classes & Objects

A **class** is a blueprint that bundles **data** (fields) with the **behaviour** that acts on it (methods). An **object** is one concrete thing built from that blueprint. Breezy is object-oriented in the Java/C# tradition, so if you have seen those languages this will feel familiar - with a few Breezy-specific rules that this page spells out completely.

← [Back to the guide](../guide.md)

---

## Declaring a class

A class has a name, zero or more **fields**, and zero or more **methods**. The opening brace goes on its own line (Allman style), and the body is indented with tabs.

```breezy
class Animal
{
	int age;            // A field: every Animal has its own age.

	void birthday()     // A method: behaviour that acts on this object.
	{
		age = age + 1;  // Inside a method, fields are reached by name.
	}
}
```

- A **field** is a variable that belongs to each object: `int age;`.
- A **method** is a function that belongs to the class: `void birthday() { ... }`.
- Inside a method, you reach the current object's fields directly by name (`age`), or explicitly through `this` (`this.age`). Both refer to the same field.

> **Two ways to initialize a field.** A field may be initialized right at its declaration (`int age = 0;`) or in a [constructor](#constructors) - use whichever reads best. An initializer may be any expression, including calls and `new`. Initializers run when the object is built: parent-class fields first, then the class's own, each in declaration order, and *then* the constructor body - so a constructor assignment overrides a field initializer. Fields without an initializer start at their zero value (`0`, `0.0`, `false`, `null`).
>
> ```breezy
> class Animal
> {
> 	int legs = 4;                      // Set at the declaration.
> 	List<int> tags = new List<int>();  // Any expression works.
> 	string name;                       // Or leave it to the constructor.
>
> 	Animal(string name)
> 	{
> 		this.name = name;
> 	}
> }
> ```

---

## Creating and using an object

Declare a variable of the class type and build an object for it with `new`. The idiomatic form does both in one line:

```breezy
void main()
{
	Animal a = new Animal();   // Declare and construct in one line.

	a.birthday();              // Call a method with the dot operator.
	print(a.age);              // Read a field the same way.  Prints: 1
}
```

> **Tip: initialize where you declare.** When a local has a value right away, the combined form `Type name = value;` is idiomatic and reads best. This works for every type:
>
> ```breezy
> int x = 42;
> string greeting = "hello";
> Animal a = new Animal();
> ```
>
> The two-step form (`Type name;` then `name = value;` on a later line) is still valid - reach for it when you declare a variable now and assign it later, or assign it differently in separate branches.

The dot operator `.` reaches into an object: `a.age` reads a field, `a.birthday()` calls a method.

---

## Inheritance

A class can **extend** exactly one other class with `extends`. The new class inherits every field and method of its parent, and may add its own or replace inherited methods. Breezy has **single inheritance** (one parent per class); reuse across unrelated hierarchies is done with [interfaces](interfaces.md).

```breezy
class Animal
{
	int age;

	void speak()
	{
		print("...");        // Base implementation.
	}
}

class Dog extends Animal     // Dog is an Animal, plus more.
{
	void speak()             // Override: replaces Animal.speak for Dogs.
	{
		print("Woof.");
	}

	void fetch()             // A method only Dogs have.
	{
		print("Fetching.");
	}
}
```

A `Dog` now has everything an `Animal` has (`age`, `speak`) plus its own `fetch`, and its `speak` replaces the inherited one.

---

## Virtual dispatch (polymorphism)

Because a `Dog` *is an* `Animal`, an `Animal` variable may hold a `Dog`:

```breezy
Animal a = new Dog();   // Allowed: a Dog is an Animal.
a.speak();       // Prints "Woof." - not "...".
```

This is **polymorphism**. The variable's declared type is `Animal`, but the call runs `Dog.speak` because the *actual object* is a `Dog`. In Breezy **every method is virtual by default**: the method that runs is chosen from the object's real type at run time, through a per-object **vtable** (a table of method pointers). There is no `virtual` keyword to remember and nothing to opt into - it just works.

> **Note (performance):** Although every method is virtual in meaning, the
> compiler emits a **direct call** wherever it can prove the target is unique -
> when no subclass overrides the method (class-hierarchy analysis). Leaf classes,
> `record`s, and any method that is never overridden pay no vtable indirection.
> Genuinely polymorphic calls still dispatch through the vtable. You write the
> same code; the compiler removes the cost where it is provably safe.

---

## Runtime class name

Every object carries its real (dynamic) type, and you can read it with the built-in `getClassName()`, which returns a `string`. This is a lightweight form of reflection. Because Breezy has no packages, the simple name *is* the full name (class names are globally unique; see [one class per file](one-class-per-file.md)).

```breezy
Animal a = new Dog();
print(a.getClassName());   // Dog  -- the most-derived type, read from the live object.
```

---

## Constructors

A **constructor** is a special member that runs when you call `new`, used to put an object into a valid starting state. It is written as the class name followed by parameters - no return type - and you pass arguments through `new`.

```breezy
class Point
{
	int x;
	int y;

	Point(int x, int y)      // Constructor: same name as the class, no return type.
	{
		this.x = x;          // 'this.x' is the field; 'x' is the parameter.
		this.y = y;
	}
}

void main()
{
	Point p = new Point(3, 4);     // Runs the constructor with x = 3, y = 4.
	print(p.x);              // 3
	print(p.y);              // 4
}
```

Use `this.` to disambiguate when a parameter has the same name as a field, as above. Field initializers (see [Declaring a class](#declaring-a-class)) run first, then the constructor body - so the constructor is the right place for anything that depends on constructor arguments, and it wins when both set the same field.

---

## Default parameter values

A parameter of a constructor, method, or function may be given a **default value** with `= <literal>`. When a caller omits that argument, the default is used. This lets one constructor or method serve several call shapes from a single signature; for several *different* signatures under one name, see [Overloading](#overloading) below.

```breezy
class Box
{
	int w;
	int h;

	Box(int w = 1, int h = 2)
	{
		this.w = w;
		this.h = h;
	}
}

void main()
{
	Box a = new Box();        // Uses both defaults: w = 1, h = 2.

	Box b = new Box(7);       // First given, second defaults: w = 7, h = 2.

	Box c = new Box(7, 9);    // Both given: w = 7, h = 9.
}
```

It works the same on methods and free functions:

```breezy
int add(int a, int b = 10)
{
	return a + b;
}

// add(5) is 15; add(5, 1) is 6.
```

**Rules:**

- The default must be a **literal constant** (a number, `true`/`false`, or a string).
- Defaults may only be given to **trailing** parameters - once a parameter has a default, every parameter after it must have one too. So `f(int a, int b = 0)` is valid, but `f(int a = 0, int b)` is not.
- This is how the [built-in vector types](vector-types.md) support `new Vector2f()` - their components default to `0`.

---

## Overloading

Constructors, methods (instance and static), and free functions may be **overloaded**: several declarations may share a name as long as their **parameter types** differ. There is no special keyword - just declare more than one.

```breezy
class Point
{
	int x;
	int y;

	Point(int x, int y) { this.x = x; this.y = y; }   // Two coordinates.
	Point(int v)        { this.x = v; this.y = v; }    // One value for both.
}

void main()
{
	Point a = new Point(3, 4);
	Point b = new Point(7);
}
```

It works the same for methods and free functions:

```breezy
int describe(int n)    { return n; }
int describe(string s) { return s.length(); }

// describe(42) is 42; describe("hello") is 5.
```

### How an overload is chosen

The call's **argument types** select the overload. Each candidate is scored per argument, best (lowest) first:

1. **Exact** - same type (and, for objects, the same class).
2. **Widening** - an implicit numeric conversion the language already allows: a smaller integer to a wider one of the same signedness, or an integer to `double`.
3. **Permissive** - any object to a parameter of a different object type, or `null` to any reference parameter.

The compiler picks the one candidate that is no worse than every other on every argument and strictly better on at least one. So `pick(int)` is chosen over `pick(long)` for an `int` argument, and `who(Dog)` over `who(Cat)` for a `Dog`.

If no candidate matches, or two are equally good, it is a **compile error**. A common case: `f(null)` when two overloads each take a different reference type is **ambiguous** - cast the `null` to choose, e.g. `f((Dog) null)`.

### Overriding an overload

A subclass method overrides a base method only when **both the name and the parameter signature match**. A same-named method with a different signature is a *new overload*, not an override - it gets its own slot in the class. Dynamic dispatch then selects the most-derived body of the matching signature.

**Rules & gotchas:**

- **Overloads must differ in parameter types.** Two declarations with identical signatures are a compile error.
- **Return type is not part of the signature** - overloads cannot differ by return type alone.
- **Default parameters interact with overloading.** An overload set whose argument-count ranges overlap ambiguously (e.g. `Box(int)` alongside `Box(int, int = 0)`) is rejected at declaration time.
- **`extern` functions and `main` cannot be overloaded** - an `extern` name binds one-to-one to a C symbol, and `main` is the fixed entry point.

---

## Complete example

Putting it together - declaration, inheritance, a constructor, an override, and polymorphic dispatch:

```breezy
class Animal
{
	string name;

	Animal(string name)
	{
		this.name = name;
	}

	void speak()
	{
		print(name + " makes a sound.");
	}
}

class Dog extends Animal
{
	Dog(string name)
	{
		this.name = name;     // Set the inherited field.
	}

	void speak()
	{
		print(name + " barks.");
	}
}

void main()
{
	Animal a = new Dog("Rex");
	a.speak();                    // Rex barks.
	print(a.getClassName());      // Dog
}
```

---

## Rules & gotchas

- **One class per file (recommended).** Each class usually lives in its own `.bzy` file whose name matches the class, though the compiler allows several per file. The program's entry point is a top-level `void main()`. See [One class per file](one-class-per-file.md).
- **Initialize where you declare.** `Type x = value;` is the idiomatic form. The two-step `Type x;` then `x = ...;` is still valid for declare-now-assign-later.
- **Field initializers run before the constructor.** `int age = 0;` at the declaration is fine; initializers run parent-first in declaration order, then the constructor body (the constructor wins on conflict).
- **Single inheritance only.** A class `extends` at most one parent. For multiple contracts, use [interfaces](interfaces.md).
- **All methods are virtual.** An override always wins, chosen by the object's real type at run time. There is no way to make a method non-virtual.
- **Memory is automatic.** You never `free` an object. Objects that escape their scope are reference-counted; objects that do not are stack-allocated for free. See [Automatic memory](../memory/automatic-memory.md).

---

← [Back to the guide](../guide.md) · Next: [Interfaces](interfaces.md)
