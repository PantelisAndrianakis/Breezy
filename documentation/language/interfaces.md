# Interfaces

An **interface** is a contract: a named list of method signatures with no bodies. Any class can promise to fulfil that contract with `implements`, and then a value typed as the interface can hold *any* implementing object and call the contract's methods on it - without those classes sharing a common parent. Interfaces give you polymorphism **without inheritance**.

← [Back to the guide](../guide.md)

---

## Why interfaces

[Single inheritance](classes.md) lets a class have exactly one parent. But often unrelated classes need to share an ability - "can be drawn", "can be compared", "can speak" - without forcing them into the same family tree. An interface expresses that shared ability as a contract, and many different classes can satisfy it independently.

---

## Declaring an interface

List the method signatures (return type, name, parameters) ending with a semicolon. There are no bodies and no fields.

```breezy
interface Speaker
{
	string speak();
}
```

This says: "anything that is a `Speaker` can be asked to `speak()` and will return a `string`."

---

## Implementing an interface

A class declares `implements` and then **must** define every method the interface requires. A class may implement several interfaces at once, separated by commas.

```breezy
class Dog implements Speaker
{
	string speak()
	{
		return "Woof.";
	}
}

class Cat implements Speaker
{
	string speak()
	{
		return "Meow.";
	}
}
```

`Dog` and `Cat` share no parent class, yet both honour the `Speaker` contract.

---

## Using an interface type

A variable whose type is an interface can hold any implementing object, and calls dispatch to that object's concrete method:

```breezy
void main()
{
	Speaker s;
	s = new Dog();
	print(s.speak());   // Woof.  -- dispatched through the interface.

	s = new Cat();
	print(s.speak());   // Meow.  -- same variable, different object.
}
```

You can also accept an interface as a parameter, so one function works with every implementer:

```breezy
void announce(Speaker who)
{
	print("It says: " + who.speak());
}
```

`announce` neither knows nor cares whether it received a `Dog`, a `Cat`, or some class written next year - only that it is a `Speaker`.

---

## How it performs

Interface dispatch in Breezy is **zero-overhead**. Interface methods occupy reserved slots in the same vtable every implementer already has, so an interface call is the *same single indirect load* as an ordinary [virtual call](classes.md#virtual-dispatch-polymorphism). An interface value is just **one object pointer** - it is ARC-tracked like any object and is **not** a "fat pointer". You pay nothing extra for the abstraction.

---

## Rules & gotchas

- **You must define every method** the interface declares, with a matching signature, or the class will not compile.
- **Interfaces have no fields and no method bodies** - they are pure contracts.
- **A class may implement many interfaces** (`class C implements I, J`) while still extending at most one parent class.
- **An interface value is a single pointer**, managed by [ARC](../memory/automatic-memory.md) exactly like a class object.
- Interfaces combine naturally with [generics](generics.md): a type parameter can be *bounded* by an interface (`<T: Speaker>`) so you can call the contract's methods on it.

---

← [Classes & objects](classes.md) · [Back to the guide](../guide.md) · Next: [Generics](generics.md)
