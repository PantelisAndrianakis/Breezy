#ifndef BZY_LSP_H
#define BZY_LSP_H

/* The `breezy --lsp` language server entry point. Runs a stdio JSON-RPC 2.0
   loop that publishes the compiler's diagnostics to an editor; returns the
   process exit status. `self_exe` is argv[0] -- the path the server re-invokes
   with `--check` to obtain diagnostics for a document. */
int lsp_main(const char *self_exe);

#endif
