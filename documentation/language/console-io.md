# Console I/O

Reading from and writing to the console needs **no imports and no ceremony**. `print` and `input` are built into the language - call them anywhere.

← [Back to the guide](../guide.md)

---

## print - write a line

`print` writes its argument followed by a newline. It accepts a `string`, and also prints numbers and bools directly.

```breezy
void main()
{
	print("Hello, Breezy.");   // A string.

	int n = 7;
	print(n);                  // 7  -- prints an int.

	print(n + n);              // 14 -- the expression is evaluated first.
}
```

To combine text and values, build a `string` with `+`. When either side of `+` is a string, the other operand may be a number or bool, which is converted to its text form automatically:

```breezy
int score = 42;
print("Your score is " + score + ".");   // Your score is 42.

bool ok = true;
print("Passed: " + ok);                   // Passed: true
```

---

## input - read a line

`input()` reads one line from standard input and returns it as a `string` (without the trailing newline).

```breezy
void main()
{
	print("What is your name?");

	string name = input();                  // Reads one line from stdin.

	print("Hello, " + name + ".");
}
```

To read a number, parse the line with a string method such as `toInt()`:

```breezy
void main()
{
	print("Enter a number:");

	int n = input().toInt();             // Reads a line, then parses it.

	print(n + n);                    // Prints double the number.
}
```

`toInt()` throws a catchable `NumberFormatException` on malformed input; see [Strings](../types/strings.md) for the full set (`toLong`, `toDouble`, …).

---

## Rules & gotchas

- **No imports needed** - `print` and `input` are part of the language.
- **`print` adds a newline** automatically.
- **Build composite output with `+`**: when one side is a string, a number or bool on the other side is converted to text, so `"x = " + x` produces a `string`.
- **`input` returns a `string`** (the line, without its trailing newline), and returns an empty string at end of input. To read a number, parse it with the appropriate [string method](../types/strings.md) such as `toInt()`.

---

← [No primitive wrapper classes](no-wrapper-classes.md) · [Back to the guide](../guide.md) · Next: [One class per file](one-class-per-file.md)
