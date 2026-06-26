# Dragon Language Support for Zed

Syntax highlighting, bracket matching, and smart indentation for the [Dragon](https://github.com/tbhi/dragon) programming language in [Zed](https://zed.dev).

## Features

- **Syntax Highlighting** - full Tree-sitter grammar covering:
  - Keywords, control flow, concurrency (`fire`, `thread`, `await`)
  - Function/class definitions, `self()` constructors, decorators
  - Type annotations and return types
  - Built-in functions, types, stdlib types, and exception hierarchy
  - String literals (regular, f-strings, raw, bytes)
  - `extern "C"` FFI blocks
  - `match`/`case` pattern matching
  - `const`, `static`, `lambda`
- **Bracket Matching** - `{}`, `[]`, `()`
- **Auto-Indentation** - blocks, classes, functions, collections

## Installation

### From Zed Extensions (when published)

1. Open Zed
2. Go to **Extensions** (Cmd+Shift+X / Ctrl+Shift+X)
3. Search for "Dragon"
4. Click Install

### Manual / Development

1. Clone this repo
2. In the `grammars/tree-sitter-dragon/` directory, run:
   ```bash
   npm install
   npx tree-sitter generate
   ```
3. Symlink or copy the extension into your Zed extensions directory:
   ```bash
   ln -s /path/to/zed-dragon ~/.local/share/zed/extensions/installed/dragon
   ```

## File Association

The extension associates with `.dr` files.

## Structure

```
zed-dragon/
├── extension.toml                    # Zed extension manifest
├── languages/
│   └── dragon/
│       ├── config.toml               # Language configuration
│       ├── highlights.scm            # Syntax highlighting queries
│       ├── brackets.scm              # Bracket matching
│       └── indents.scm               # Indentation rules
└── grammars/
    └── tree-sitter-dragon/
        ├── grammar.js                # Tree-sitter grammar definition
        ├── package.json
        └── queries/
            └── highlights.scm        # Standalone grammar highlights
```

## License

MIT
