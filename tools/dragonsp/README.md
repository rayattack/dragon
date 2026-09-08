# dragonsp

The Dragon language server, written in Dragon on top of the stdlib. It speaks
the Language Server Protocol over stdio and answers with what the real
compiler says: diagnostics come from `dragon check`, never from a second
implementation of the type rules.

## Build

```bash
dragon build tools/dragonsp/dragonsp.dr -o dragonsp
```

The CMake tree builds it as the `dragonsp` target (it lands at
`build/dragonsp`) and `make install` puts it beside `dragon`.

## Launch contract

- `dragonsp` with no arguments serves LSP over stdin/stdout. `--stdio` is
  accepted and ignored. `--version` prints one line and exits 0.
- stdout carries protocol bytes only. Logs go to stderr (`--verbose` or
  `DRAGONSP_LOG=1` traces every method).
- Framing is `Content-Length: N\r\n\r\n` plus a UTF-8 JSON body, both ways.
- Positions are UTF-16 line/character (`positionEncoding: "utf-16"`).
- Document sync is full text (`TextDocumentSyncKind.Full`); incremental
  changes are applied if a client sends them anyway.
- `shutdown` then `exit` ends the process with status 0; `exit` without
  `shutdown` exits 1; a closed stdin ends it with status 0.

Flags and environment:

| Flag | Environment | Meaning |
|---|---|---|
| `--compiler <path>` | `DRAGONSP_COMPILER` | the `dragon` binary to run `check` with (default: `dragon` on PATH) |
| `--stdlib <dir>` | `DRAGONSP_STDLIB`, then `DRAGON_STDLIB_PATH` | stdlib root for module completion (default: found next to the compiler) |
| `--verbose` | `DRAGONSP_LOG` | trace to stderr |

## Capabilities

| Request | What it does |
|---|---|
| `textDocument/publishDiagnostics` | runs `dragon check` on the buffer (a scratch copy with `-I <file dir>` when the buffer differs from disk, the real file when it matches), maps every `DRAGON SCALE ERROR/WARNING` to a range, echoes the document version. Errors in imported files are published under their own URI. Checks run when a burst of client messages ends and on every save. |
| `textDocument/documentSymbol` | hierarchical symbols: classes, enums, methods, constructors, properties, fields, functions, constants, module variables, enum members. |
| `textDocument/hover` | signature and docstring of the symbol under the cursor, including symbols from imported modules and the stdlib; builtins and keywords have short docs. |
| `textDocument/definition` | jumps to a definition in the open file, a sibling module, or the stdlib; a module name jumps to the module file. |
| `textDocument/completion` | trigger `.`; members after `self.`, a class name, a variable with a declared type, or a module alias; module names in `import` and `from` lines; module members after `from x import `; otherwise scope symbols, imports, builtins, types and keywords. |
| `workspace/symbol` | substring search over open documents and every `.dr` file under the workspace root (first 2000 files). |

The symbol side is a scanner over Dragon syntax, not a type checker: hover and
definition follow declared types and imports textually. The compiler remains
the only source of truth for what is wrong.

## Editors

Neovim (0.10+):

```lua
vim.filetype.add({ extension = { dr = "dragon" } })
vim.api.nvim_create_autocmd("FileType", {
  pattern = "dragon",
  callback = function()
    vim.lsp.start({ name = "dragonsp", cmd = { "dragonsp" }, root_dir = vim.fs.root(0, { "dragon.drs", ".git" }) })
  end,
})
```

Helix (`languages.toml`):

```toml
[language-server.dragonsp]
command = "dragonsp"

[[language]]
name = "dragon"
scope = "source.dragon"
file-types = ["dr"]
roots = ["dragon.drs"]
language-servers = ["dragonsp"]
```

Emacs (eglot):

```elisp
(add-to-list 'eglot-server-programs '(dragon-mode . ("dragonsp")))
```

## Tests

`tools/dragonsp/test_dragonsp.dr` is a dogfooded unittest: scanner, position
and URI helpers, compiler-output parsing, and a full protocol session driven
through a pipe against the built binary. ctest runs it as `dragonsp` with
`DRAGONSP_BIN`, `DRAGONSP_COMPILER` and `DRAGONSP_STDLIB` set; by hand:

```bash
DRAGONSP_BIN=build/dragonsp DRAGONSP_COMPILER=build/dragon DRAGONSP_STDLIB=stdlib \
  dragon run tools/dragonsp/test_dragonsp.dr
```
