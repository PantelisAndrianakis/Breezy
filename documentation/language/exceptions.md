# Exceptions

When something goes wrong - a bad argument, a missing file, an out-of-range index - Breezy uses **exceptions** to signal the error and let an outer handler deal with it, instead of threading error codes through every return value. The model is Java-style `throw` / `try` / `catch` with an exception hierarchy and stack traces, and it is **zero-cost when nothing is thrown**.

← [Back to the guide](../guide.md)

---

## Throwing an exception

`throw` raises an exception. The thrown value must be an `Exception` (or a subclass). Construct one with `new` and a message.

```breezy
void lookup(int id)
{
	if (id < 0)
	{
		throw new Exception("The id must not be negative.");
	}
}
```

Raising an exception abandons the current function and starts **unwinding** the call stack, looking for a handler.

---

## Catching an exception

Wrap risky code in `try` and handle failures in one or more `catch` clauses. Each `catch` names an exception type and a variable. The **first** clause whose type matches the thrown object wins.

```breezy
void main()
{
	try
	{
		lookup(5);
	}
	catch (Exception e)
	{
		print(e.message);   // Read the message off the caught exception.
	}
}
```

Every `Exception` carries a `string message` field, set from the constructor argument.

---

## The exception hierarchy and is-a matching

A `catch` matches by **is-a**: it catches the named type *and any subclass*, walking up the class hierarchy. So a `catch (Exception e)` catches everything, while a more specific clause listed first catches just its type. Order your clauses **most specific first**.

```breezy
class NotFound extends Exception     // User exceptions extend the built-in root.
{
}

void main()
{
	try
	{
		int[] table = new int[3];
		print(table[5]);                 // Out of range -> throws IndexOutOfBounds.
	}
	catch (IndexOutOfBounds e)           // First matching clause wins.
	{
		print(e.message);                // "array index 5 out of bounds for length 3".
	}
	catch (Exception e)                  // Catches any other subclass via is-a.
	{
		print(e.message);
	}
}
```

**Built-in exceptions:**

- **`Exception`** - the root, with a `string message`. User classes `extends Exception`.
- **`IndexOutOfBounds`** - thrown automatically by out-of-range array indexing.

Other parts of the standard library throw their own subclasses - for example [`NumberFormatException`](../types/strings.md) from a failed string parse and [`IOException`](../stdlib/file.md) from filesystem and network errors.

---

## A common pattern: default on failure

Because parsing throws on bad input, a "use a fallback when invalid" reads naturally:

```breezy
int port;
try
{
	port = config.toInt();
}
catch (NumberFormatException e)
{
	port = 8080;            // Fallback when the value is not a valid int.
}
```

---

## Uncaught exceptions

If no handler matches, the exception is **uncaught**: the program prints `Uncaught exception: <message>` plus a stack trace — one frame per function on the abandoned call chain, each with its source file and line — then aborts. This makes unexpected failures loud and traceable rather than silent.

```
Uncaught exception: boom
  at deep (Main.bzy:1)
  at mid (Main.bzy:9)
  at main (Main.bzy:14)
```

The line shown is where each function appears in its source file.

---

## How it performs

The implementation is **zero-cost when nothing is thrown** - entering a `try` emits no instructions, so wrapping code in `try` is free on the success path. The only cost is the stack walk at the moment something is thrown. It is homegrown (static per-function exception side tables plus an `rbp`-chain-walking unwinder), not OS-level SEH. Unwinding is **ARC-correct**: object locals in the abandoned frames are released, while the thrown object itself survives the unwind and is freed once the handler's scope exits. See [Automatic memory](../memory/automatic-memory.md).

---

## Rules & gotchas

- **You can only `throw` an `Exception` or a subclass.**
- **Catch clauses match by is-a** and are tried top to bottom - put the most specific types first.
- **`Exception` has a `message` field**; read it as `e.message`.
- **Define your own exceptions** by extending `Exception`.
- **`try` is free on the success path** - do not avoid it for performance reasons.

---

← [Control flow](control-flow.md) · [Back to the guide](../guide.md) · Next: [Type system overview](../types/overview.md)
