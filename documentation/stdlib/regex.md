# Regex

`Regex` matches text against patterns. It is a hand-written **Thompson NFA / Pike VM**, which means **linear-time** matching with **no catastrophic backtracking** - it is ReDoS-safe. That property matters when you match untrusted input on a server, where a pathological pattern could otherwise hang a core.

← [Back to the guide](../guide.md)

---

## The four operations

```breezy
boolean ok;
ok = Regex.matches("a+b", "aaab");          // Full match -> true.

boolean found;
found = Regex.test("[0-9]+", "abc123");     // Search anywhere -> true.

string hit;
hit = Regex.find("[0-9]+", "abc123def");    // Leftmost match -> "123".

string masked;
masked = Regex.replace("[0-9]+", "a1b22c333", "#");   // -> "a#b#c#".
```

- `matches(pattern, text)` - returns `boolean`; the pattern must match the **whole** text.
- `test(pattern, text)` - returns `boolean`; succeeds if the pattern matches **anywhere**.
- `find(pattern, text)` - returns the **leftmost** matching substring, or `""` if none.
- `replace(pattern, text, with)` - replaces **every** non-overlapping match.

---

## Supported syntax

- Literals and `.` (any character).
- Quantifiers `*` `+` `?` and bounded repetition `{n}` / `{n,}` / `{n,m}`.
- Alternation `|` and grouping `()`.
- Anchors `^` (start) and `$` (end).
- Character classes `[a-z]` and negated classes `[^...]`.
- Escapes `\d` `\D` `\w` `\W` `\s` `\S`.

Patterns are ordinary runtime strings, so you can build and pass them dynamically.

---

## Rules & gotchas

- **`matches` is full-match; `test` is search** - pick the one you mean.
- **`find` returns `""` when there is no match**, not an error.
- **`replace` replaces all** non-overlapping matches.
- **Matching is linear-time and ReDoS-safe** - safe to run on untrusted input.
- **Patterns are runtime strings** - they can be constructed at run time.

---

← [Random](random.md) · [Back to the guide](../guide.md) · Next: [File](file.md)
