# Regex

`Regex` matches text against patterns. It is a hand-written **Thompson NFA / Pike VM**, which means **linear-time** matching with **no catastrophic backtracking** - it is ReDoS-safe. That property matters when you match untrusted input on a server, where a pathological pattern could otherwise hang a core.

← [Back to the guide](../guide.md)

---

## The four operations

```breezy
bool ok = Regex.matches("a+b", "aaab");          // Full match -> true.

bool found = Regex.test("[0-9]+", "abc123");     // Search anywhere -> true.

string hit = Regex.find("[0-9]+", "abc123def");    // Leftmost match -> "123".

string masked = Regex.replace("[0-9]+", "a1b22c333", "#");   // -> "a#b#c#".
```

- `matches(pattern, text)` - returns `bool`; the pattern must match the **whole** text.
- `test(pattern, text)` - returns `bool`; succeeds if the pattern matches **anywhere**.
- `find(pattern, text)` - returns the **leftmost** matching substring, or `""` if none.
- `replace(pattern, text, with)` - replaces **every** non-overlapping match.

---

## Capture groups & all-matches

When you need the text *inside* the parentheses, or every match in one pass:

```breezy
string[] g = Regex.capture("(\\w+)=(\\d+)", "level=42");
// g[0] = "level=42" (whole match), g[1] = "level", g[2] = "42".

string[] none = Regex.capture("(\\d+)", "abc");   // No match -> length 0.

string[] xs = Regex.findAll("[0-9]+", "a12b345c6");
// Every whole match: ["12", "345", "6"].

string[][] ms = Regex.captureAll("(\\w)=(\\w)", "a=b c=d");
// One row per match, each [whole, g1, ...]: ms[0] = ["a=b","a","b"], ms[1] = ["c=d","c","d"].
```

- `capture(pattern, text)` -> `string[]` of `[whole, g1, g2, ...]` for the
  **leftmost** match, or an **empty array** if there is no match. Index `0` is
  the whole match; group `k` is at index `k`.
- `findAll(pattern, text)` -> `string[]` of every **whole** match (empty if none).
- `captureAll(pattern, text)` -> `string[][]`: `findAll` plus groups - one row
  per match, each row shaped like `capture`'s result.

A group that did not participate (e.g. an optional `(x)?` that matched nothing)
yields `""`. Up to 31 capturing groups are recorded; beyond that the pattern
still matches but the extra groups are not captured.

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
- **`capture` returns an empty array on no match** - check `.length` before indexing.
- **A non-participating group is `""`**, not absent - the result length is fixed by the pattern.
- **Matching is linear-time and ReDoS-safe** - safe to run on untrusted input.
- **Patterns are runtime strings** - they can be constructed at run time.

---

← [Random](random.md) · [Back to the guide](../guide.md) · Next: [File](file.md)
