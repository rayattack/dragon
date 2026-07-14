# Dragon Language Support

VS Code support for the Dragon programming language.

## Features

- Syntax highlighting for `.dr` files
- Python-like syntax with curly braces
- Highlights for keywords, builtins, stdlib types, and Dragon-specific syntax
- `const`, `static`, `extern` storage modifiers
- `own`, `dub`, `del` ownership keywords
- Embedded syntax highlighting inside typed templates: `template[HTML]`, `template[SQL]`, `template[JSON]`, `template[CSS]`, `template[XML]`
- `!{expr}` interpolation and `:{ ... }` markup fragments highlighted through arbitrary nesting
- `fire` keyword for thread spawning
- `Lock()` threading primitive
- `extern "C"` FFI declarations with `ptr` type
- Exception hierarchy types
- Dunder method recognition (`__init__`, `__str__`, `__enter__`, etc.)
- `self()` constructor syntax
- `@staticmethod` / `@classmethod` decorators
- f-string interpolation highlighting
- Bytes literal (`b"..."`) support
- Auto-indentation for blocks
