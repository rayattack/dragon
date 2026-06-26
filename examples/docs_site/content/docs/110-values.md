# Values and Types

Dragon's primitive types each flow at a native machine type - no boxing, no
hidden allocation:

| Type | Example | Stored as |
|------|---------|-----------|
| `int` | `42` | 64-bit integer |
| `float` | `3.14` | 64-bit double |
| `bool` | `true` | one bit |
| `str` | `"dragon"` | heap string |

A name is introduced once with `:` and reassigned with `=`:

```dragon
count: int = 0
count = count + 1
```

Use `const` when a binding never changes. Reassigning it is a compile-time
error, which makes the intent clear to the reader and the compiler alike:

```dragon
const port: int = 2018
```

Inline `code`, **bold**, *italic*, and [links](200-lists.md) all render from
the same single-pass Markdown parser this site ships with.
