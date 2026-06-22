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

When you **open** or **save** a `.bzy` file, the server re-checks the project and
publishes diagnostics:

- A compile error → one `Error` diagnostic at the compiler's own position, with the
  compiler's own message (e.g. `Expected ';', got '}'.`).
- A clean project → the squiggles clear.

Under the hood the server runs `breezy --check` on the file's project and forwards
the result, so the diagnostics are **exactly** what a command-line build would
report — no second, divergent analyzer to keep in sync.

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

- **Diagnostics only.** No hover, go-to-definition, find-references, completion,
  signature help, formatting, or rename yet. These need the compiler to expose
  symbol and type information it currently uses internally.
- **On open and save, not on every keystroke.** A check reads the file from disk,
  which is authoritative at open and save. Live-as-you-type checking of the unsaved
  buffer is a planned follow-up.
- **One diagnostic per check.** The compiler stops at the first error and reports
  it; the next error appears after you fix that one and save. Batch diagnostics wait
  on multi-error recovery in the compiler.
- **A single-character squiggle** at the caret column. Widening it to span the whole
  offending token is a follow-up (it needs end-column information on the syntax
  tree).

A compiler error that is printed as plain text rather than a structured diagnostic
is still surfaced — at line granularity — so an error is never silently invisible.

---

← [Back to the guide](../guide.md)
