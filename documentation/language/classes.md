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

> **Rule: no instance-field initializers.** You may *not* write `int age = 0;` on an instance field - the compiler rejects it. Give fields their starting values in a [constructor](#constructors) instead. (Only `static` fields may have an initializer; see [Static members](static-members.md).)

---

## Creating and using an object

Working with an object is always **two steps**: first **declare** a variable of the class type, then **assign** a new object to it with `new`.

```breezy
void main()
{
	Animal a;            // Step 1: declare a variable that can hold an Animal.
	a = new Animal();    // Step 2: build a fresh Animal and store it.

	a.birthday();        // Call a method with the dot operator.
	print(a.age);        // Read a field the same way.  Prints: 1
}
```

> **Rule: declaration and assignment are separate statements.** Breezy locals are written as `Type name;` on one line and `name = value;` on the next. Do **not** combine them as `Animal a = new Animal();` - that combined form is not how Breezy locals are written. This applies to every type, not just classes:
>
> ```breezy
> int x;
> x = 42;              // Correct.
>
> string greeting;
> greeting = "hello";  // Correct.
> ```
>
> (The one exception is the `for` loop header, where `for (int i = 0; ...)` is allowed - see [Control flow](control-flow.md).)

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
Animal a;
a = new Dog();   // Allowed: a Dog is an Animal.
a.speak();       // Prints "Woof." - not "...".
```

This is **polymorphism**. The variable's declared type is `Animal`, but the call runs `Dog.speak` because the *actual object* is a `Dog`. In Breezy **every method is virtual by default**: the method that runs is chosen from the object's real type at run time, through a per-object **vtable** (a table of method pointers). There is no `virtual` keyword to remember and nothing to opt into - it just works.

---

## Runtime class name

Every object carries its real (dynamic) type, and you can read it with the built-in `getClassName()`, which returns a `string`. This is a lightweight form of reflection. Because Breezy uses [one class per file](one-class-per-file.md) with no packages, the simple name *is* the full name.

```breezy
Animal a;
a = new Dog();
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
	Point p;
	p = new Point(3, 4);     // Runs the constructor with x = 3, y = 4.
	print(p.x);              // 3
	print(p.y);              // 4
}
```

Use `this.` to disambiguate when a parameter has the same name as a field, as above. Constructors are the right place to initialize fields, since instance fields cannot have initializers of their own.

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
	Animal a;
	a = new Dog("Rex");
	a.speak();                    // Rex barks.
	print(a.getClassName());      // Dog
}
```

---

## Rules & gotchas

- **One class per file.** Each class lives in its own `.bzy` file whose name matches the class. The program's entry point is a top-level `void main()` in its own file. See [One class per file](one-class-per-file.md).
- **Declare, then assign.** Locals are two steps (`Type x;` then `x = ...;`). Never combine them outside a `for` header.
- **No instance-field initializers.** Initialize fields in a constructor, not at the field declaration.
- **Single inheritance only.** A class `extends` at most one parent. For multiple contracts, use [interfaces](interfaces.md).
- **All methods are virtual.** An override always wins, chosen by the object's real type at run time. There is no way to make a method non-virtual.
- **Memory is automatic.** You never `free` an object. Objects that escape their scope are reference-counted; objects that do not are stack-allocated for free. See [Automatic memory](../memory/automatic-memory.md).

---

← [Back to the guide](../guide.md) · Next: [Interfaces](interfaces.md)
