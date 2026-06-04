# Console I/O

Reading from and writing to the console needs **no imports and no ceremony**. `print`, `input`, and `inputInt` are built into the language - call them anywhere.

← [Back to the guide](../guide.md)

---

## print - write a line

`print` writes its argument followed by a newline. It accepts a `string`, and also prints numbers and booleans directly.

```breezy
void main()
{
	print("Hello, Breezy.");   // A string.

	int n;
	n = 7;
	print(n);                  // 7  -- prints an int.

	print(n + n);              // 14 -- the expression is evaluated first.
}
```

To combine text and values, build a `string` with `+`. When either side of `+` is a string, the other operand may be a number or boolean, which is converted to its text form automatically:

```breezy
int score;
score = 42;
print("Your score is " + score + ".");   // Your score is 42.

boolean ok;
ok = true;
print("Passed: " + ok);                   // Passed: true
```

---

## input - read a line

`input()` reads one line from standard input and returns it as a `string` (without the trailing newline).

```breezy
void main()
{
	print("What is your name?");

	string name;
	name = input();                  // Reads one line from stdin.

	print("Hello, " + name + ".");
}
```

---

## inputInt - read a number

`inputInt()` reads a line and returns it as an `int`, saving you a manual parse:

```breezy
void main()
{
	print("Enter a number:");

	int n;
	n = inputInt();                  // Reads an integer.

	print(n + n);                    // Prints double the number.
}
```

If you need to parse text you already have (rather than read a fresh line), use the string parsing methods such as `toInt()` - see [Strings](../types/strings.md).

---

## Rules & gotchas

- **No imports needed** - `print`, `input`, and `inputInt` are part of the language.
- **`print` adds a newline** automatically.
- **Build composite output with `+`**: when one side is a string, a number or boolean on the other side is converted to text, so `"x = " + x` produces a `string`.
- **`input` returns a `string`; `inputInt` returns an `int`.** To read other numeric types, read a line with `input()` and parse it with the appropriate [string method](../types/strings.md).

---

← [No primitive wrapper classes](no-wrapper-classes.md) · [Back to the guide](../guide.md) · Next: [One class per file](one-class-per-file.md)
