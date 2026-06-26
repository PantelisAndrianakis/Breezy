# Allman Braces & Style

Breezy has one official code style, and every page in this guide follows it. Writing your code the same way keeps projects consistent and makes the standard library and official examples easy to read.

← [Back to the guide](../guide.md)

---

## Allman braces

**Allman style** means an opening brace always appears on **its own line**, lined up with the statement it belongs to. This is the default and recommended style for Breezy.

```breezy
void main()
{
	int x = 1;

	if (x == 1)
	{
		print("One.");
	}
}
```

Same-line brace style (the opening brace on the same line as the statement) is *accepted* by the compiler, but **all standard-library code and official examples use Allman** - so prefer it in your own code.

---

## Tab indentation

Breezy source is indented with **tab characters**, not spaces. One tab per nesting level. Every example in this guide uses tabs.

---

## Comments

- Use `//` for a single-line comment and `/* ... */` for a block comment.
- Block comments **nest**, so you can comment out a region that already contains a block comment without it closing early.
- Write comments as **proper sentences**: start with a capital letter and end with a period.

```breezy
// Calculate the running total.
int total = 0;

/*
 * A block comment can span
 * several lines when you need it.
 */
```

---

## Messages

The same sentence rule applies to **user-facing message strings** - the text you pass to `print`, the message in an exception you `throw`, or any diagnostic. Start with a capital letter and end with a period.

```breezy
print("Saved successfully.");
throw new Exception("The id must not be negative.");
```

---

## At a glance

| Aspect | Breezy style |
| --- | --- |
| Braces | Allman - opening brace on its own line. |
| Indentation | Tabs, one per level. |
| Comments | Full sentences: capital first letter, trailing period. |
| Messages | Same as comments: capital first letter, trailing period. |

---

← [One class per file](one-class-per-file.md) · [Back to the guide](../guide.md) · Next: [Control flow](control-flow.md)
