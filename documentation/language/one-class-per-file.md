# One Class Per File - No Headers, Ever

Breezy takes the Java/C# approach to project structure and pushes it further: **one class per `.bzy` file is the recommended layout** (file name matching the class), though the compiler lets you put several classes in one file when that reads better. There are **no header files, no forward declarations, no `#include`, and no hand-written imports**. You write logic, not boilerplate.

← [Back to the guide](../guide.md)

---

## The recommended layout

Each class lives in its own file, named after the class:

```
src/
├── Client.bzy     // class Client.
├── Zone.bzy       // class Zone.
└── Main.bzy       // The entry point: void main().
```

`Client.bzy` contains `class Client`, `Zone.bzy` contains `class Zone`, and so on. The program's **entry point** is a top-level `void main()` living in its own file (here, `Main.bzy`).

This one-class-per-file layout is a **convention, not a requirement**. You may declare several classes in a single `.bzy` file; each behaves exactly as if it lived alone in its own file - globally visible, with a globally unique name, and not nested inside any other. The convention is recommended because it keeps a class exactly where its name says it is.

---

## No imports, no headers, no ordering

The compiler scans **every** `.bzy` file in your project and gathers **all** class, field, and method signatures *before* it generates a single instruction. So any file can reference any class freely - even one defined in a file compiled later. **Order never matters, and you never declare anything twice.**

```breezy
// Client.bzy - no imports, no headers; Zone is simply visible.
class Client
{
	string name;
	Zone zone;                   // Forward reference to another file - fine.

	void enterZone(Zone z)
	{
		zone = z;
	}
}
```

There is nothing to `#include`, no header to keep in sync with an implementation, and no forward declaration to write. If a class exists anywhere in your project, it is visible everywhere.

---

## The entry point

Exactly one file holds the top-level entry point:

```breezy
// Main.bzy
void main()
{
	Client c;
	c = new Client();
	print("Started.");
}
```

`void main()` is a free function, not a method on a class - it is where execution begins.

---

## Why this design

- **Less boilerplate.** No headers, no imports, no forward declarations - the things that, in C and C++, you maintain by hand and that drift out of sync.
- **Order independence.** Because the compiler reads all signatures first, mutually referencing classes "just work" with no special handling.
- **Predictable layout.** One class per file means a class is always exactly where its name says it is.

---

## Rules & gotchas

- **File name matching the class name is recommended** (`Client.bzy` -> `class Client`) but not enforced.
- **Class names are globally unique** - two classes may not share a name, whether in the same file or different files.
- **`void main()` is top-level**, in its own file, and is the single entry point.
- **No imports or headers anywhere** - cross-file references resolve automatically.

---

← [Console I/O](console-io.md) · [Back to the guide](../guide.md) · Next: [Allman braces & style](allman-braces.md)
