# Language Server (`breezy --lsp`)

`breezy --lsp` starts a **Language Server**: a small stdio process that speaks the
Language Server Protocol so your editor shows Breezy's compiler diagnostics inline
— a red squiggle at the exact `line:col` the compiler reports, cleared the moment
the code is fixed.

It is not a separate tool to install: it is the compiler in a different mode. Any
editor with an LSP client can use it.

← [Back to the guide](../guide.md)

---

## What it does

- **Diagnostics.** When you **open** or **save** a `.bzy` file, the server re-checks
  the project and publishes a compile error at the compiler's own position with the
  compiler's own message (e.g. `Expected ';', got '}'.`), or clears the squiggles
  when the project is clean. The squiggle spans the whole offending token.
- **Hover.** Hovering a name shows its type — `c : Counter`, `get : int`,
  `make : ()->Box`.
- **Go to definition.** Jumping from a use takes you to its declaration — a method
  call to the method, a field to the field, `new Foo` and a class name to the class,
  a function call to the function.
- **Find references.** From any use (or the declaration) the server lists every use
  of that symbol across the project, plus the declaration itself.
- **Completion.** After `receiver.` the server offers that type's methods and fields;
  on a bare word it offers the project's class and function names.

All of this is **live**: it reflects your **unsaved** edits, not just the file on
disk. As you type, diagnostics, hover, definition, and references re-evaluate against
the current buffer.

Under the hood the server forwards what the compiler itself reports — diagnostics
from `breezy --check`, and hover/definition/references/completion from `breezy
--symbols` (a symbol index the compiler emits from the same resolved program) — so
the editor never disagrees with a command-line build, and there is no second,
divergent analyzer to keep in sync. For unsaved edits it mirrors the project to a
temporary copy with your in-memory buffers substituted and runs the compiler against
that.

The **project** is the directory holding the file, or the nearest ancestor
containing a `breezy.toml` (the same scoping `breezy <dir>` uses). All `.bzy` files
in that directory are checked together, so cross-file references resolve correctly.

---

## Editor setup

The server reads LSP messages on stdin and writes them on stdout; point any LSP
client at the command `breezy --lsp`. Make sure `breezy` is on your `PATH`.

### Neovim (built-in LSP client)

```lua
-- Map *.bzy to a 'breezy' filetype, then start the server for it.
vim.filetype.add({ extension = { bzy = "breezy" } })

vim.api.nvim_create_autocmd("FileType", {
	pattern = "breezy",
	callback = function()
		vim.lsp.start({
			name = "breezy",
			cmd = { "breezy", "--lsp" },
			root_dir = vim.fs.dirname(
				vim.fs.find({ "breezy.toml" }, { upward = true })[1]
			),
		})
	end,
})
```

### Helix (`languages.toml`)

```toml
[language-server.breezy]
command = "breezy"
args = ["--lsp"]

[[language]]
name = "breezy"
scope = "source.breezy"
file-types = ["bzy"]
roots = ["breezy.toml"]
language-servers = ["breezy"]
```

### VS Code

VS Code has no built-in generic LSP client, so it needs a thin wrapper extension
that launches the server with the
[`vscode-languageclient`](https://www.npmjs.com/package/vscode-languageclient)
library — a `ServerOptions` whose `command` is `breezy` and `args` is `["--lsp"]`,
registered for the `breezy` language with `*.bzy` files. That small extension is the
only glue; the server itself is the same `breezy --lsp`.

---

## v1 boundaries

The server is intentionally small and grows from here.

- **Diagnostics, hover, go-to-definition, find-references, and completion** are
  served; **signature help, formatting, and rename** are not yet.
- **Definition and references are project-local and reach named declarations.** They
  work on a class, method, field, free function, or constructor in your own files.
  They do not yet resolve a **local variable or parameter** (no in-scope information in
  the index yet), an **inherited** member, or a **built-in** (the prelude has no
  user-visible definition site); those hover with a type but do not jump or list uses.
  A `new Foo` occurrence is anchored at the `new` keyword.
- **Completion resolves the receiver's type from the last parse that succeeded.** A
  half-typed line does not parse, so the member list comes from the most recent
  buildable version; a variable just introduced and not yet usable elsewhere offers
  names but not its members.
- **A recheck per change, no debounce.** Every edit re-mirrors the project and runs the
  compiler. Correct and simple; incremental/debounced checking is the follow-up for a
  large project.
- **One diagnostic per check.** The compiler stops at the first error and reports
  it; the next error appears after you fix that one. Batch diagnostics wait on
  multi-error recovery in the compiler.

A compiler error that is printed as plain text rather than a structured diagnostic
is still surfaced — at line granularity — so an error is never silently invisible.

---

← [Back to the guide](../guide.md)
