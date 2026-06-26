# Keywords

This appendix is the authoritative list of every word the Dragon
compiler reserves. It is drawn directly from the lexer's keyword table,
so it reflects what the compiler actually treats specially - nothing
more, nothing less. Two kinds of word appear here: **reserved
keywords**, which the lexer recognises everywhere and which you can
never use as a name; and **contextual keywords**, which the lexer hands
through as ordinary identifiers and which the parser only treats
specially in one position - so `thread`, `match`, `case`, and
`template` are still legal variable names anywhere else.

Dragon is *inspired* by Python, not a superset of it. The keyword set
overlaps with Python's by design, but it adds its own (`const`,
`static`, `extern`, `fire`, `catch`, `template`) and drops Python words
that have no place in a typed, compiled language. Where a word behaves
differently in `.dr` files versus `.py` files, the difference is noted
in the table.

## Declarations and storage

These introduce names - functions, classes, and the storage class of a
binding. See [Functions](/docs/0301-functions) and [Classes](/docs/0601-classes).

| Keyword | Meaning |
|---|---|
| `def` | Declares a function or method. In `.dr` source, a class **constructor** is the nameless `def() { ... }`; the `__init__` spelling is `.py`-mode only (both normalise to the same constructor internally). |
| `class` | Declares a class: `class Point { x: int = 0 }`. |
| `lambda` | An inline anonymous function: `f: ... = lambda x: x + 1`. |
| `const` | Marks an immutable binding (Dragon extension): `const PI: float = 3.14159`. Reassignment is a compile error. |
| `static` | A class-body field shared across all instances, or a module-level singleton (Dragon extension). Contrast a non-`static` field, which is a fresh per-instance default. |
| `extern` | Declares a C FFI binding: `extern "C" { ... }` (Dragon extension). See [FFI](/docs/1501-ffi). |
| `global` | Inside a function, opts into **assigning** a module-level global. **Required to write** a module global from a function - without it, a bare `=` or `+=` is a compile error (not a silent new local) - in both `.dr` and `.py`. Reading a global needs no keyword in either mode. For rebinding an *enclosing function's* variable (not a module global), use `nonlocal`. |
| `nonlocal` | Inside a nested function, opts into rebinding a variable owned by an **enclosing function** (for closures). **Required**: without it, a bare `=` or `+=` to such a name is a compile error - not a silent new local - in both `.dr` and `.py`. Reading an enclosing variable needs no keyword; only rebinding does. Blocks (`if`/`for`/…) inside the same function are *not* a boundary and need no keyword. Cannot reach module globals - use `global` for those. |

## Control flow

The branching, looping, and jump statements. See [Control flow](/docs/0205-control-flow).

| Keyword | Meaning |
|---|---|
| `if` | Conditional branch. |
| `elif` | Chained alternative branch (Python spelling, not `else if`). |
| `else` | Fallback branch; also the trailing clause on `for`/`while`/`try`. |
| `while` | Loop while a condition holds. |
| `for` | Iterate over an iterable: `for x in items { ... }`. |
| `break` | Exit the innermost loop. |
| `continue` | Skip to the next loop iteration. |
| `return` | Return a value from a function. |
| `pass` | A statement that does nothing (an explicit empty body). |
| `with` | Context-manager block: `with open(path) as f { ... }`. |
| `yield` | Produce a value from a generator function. |

## Pattern matching

Structural pattern matching. `match` and `case` are **contextual** - the
lexer treats them as identifiers, and the parser only recognises them at
the start of a `match` statement / a case clause, so they stay usable as
ordinary names. The `.dr` form uses braces and **no colon** after the
pattern:

```dragon
match command {
    case "start" {
        print("starting")
    }
    case _ {
        print("unknown")
    }
}
```

| Keyword | Meaning |
|---|---|
| `match` | Begins a structural match over a subject value (contextual). |
| `case` | A pattern clause inside a `match` block (contextual). An optional `if` guard may follow the pattern. |

## Exceptions

Raising and handling errors. See [Errors](/docs/0901-exceptions).

| Keyword | Meaning |
|---|---|
| `try` | Begins a block whose exceptions may be caught. |
| `except` | Handles a matching exception type: `except ValueError as e { ... }`. |
| `catch` | A Dragon-extension synonym usable in place of `except`. |
| `finally` | A block that always runs, whether or not an exception occurred. |
| `raise` | Throws an exception: `raise ValueError("bad input")`. |
| `assert` | Raises `AssertionError` if a condition is false: `assert n > 0`. |

## Operators that are keywords

These are spelled as words rather than symbols. The logical operators
are short-circuiting; `in`/`not in` test membership and `is`/`is not`
test identity. See [Expressions](/docs/2002-appendix-operators).

| Keyword | Meaning |
|---|---|
| `and` | Logical AND (short-circuiting). |
| `or` | Logical OR (short-circuiting). |
| `not` | Logical negation; also the second word of `not in`. |
| `in` | Membership test: `x in items`. Also drives `for ... in`. |
| `is` | Identity test: `x is None`. Combines as `is not`. |

## Imports

Bringing names in from other modules. See [Modules](/docs/1001-modules).

| Keyword | Meaning |
|---|---|
| `import` | Imports a module: `import os.path`. |
| `from` | Imports specific names: `from os.path import join`. Also opens an `extern "C" from "..."` block. |
| `as` | Binds an alias: `import numpy as np`, `except E as e`, `with ctx as c`. |

## Concurrency

Dragon's concurrency keywords are colorless - there is no async/sync
function split. See [Concurrency](/docs/1101-green-threads).

| Keyword | Meaning |
|---|---|
| `fire` | Spawns a green-thread (vthread) task: `t: Task[int] = fire work()`, or fire-and-forget `fire { ... }`. |
| `thread` | A scoped OS-thread block that auto-joins at scope exit: `thread { ... }` (contextual keyword - only special at statement start). |
| `async` | Marks a function whose call yields a `Task[T]`. |
| `await` | Awaits a `Task[T]`, yielding to the scheduler when the awaiter is itself a vthread. Awaiting a non-`Task` is a compile error. |

## Templates

Compile-time text templating. `template` is contextual - special only
when it opens a `template { ... }` or `template[X] { ... }` block, and a
plain identifier everywhere else. See [Templates](/docs/1201-templates).

| Keyword | Meaning |
|---|---|
| `template` | Opens a compile-time template block: `s: str = template {Hello !{name}}`. |

## Other statements

| Keyword | Meaning |
|---|---|
| `del` | Deletes a binding or a container entry: `del d["key"]`. |

## Constants

The three literal values. In `.dr` source the lowercase aliases
`true`, `false`, and `none` are also accepted and mean exactly the same
thing as their capitalised forms.

| Keyword | Meaning |
|---|---|
| `True` | Boolean true (`.dr` alias: `true`). |
| `False` | Boolean false (`.dr` alias: `false`). |
| `None` | The absence of a value (`.dr` alias: `none`). |

## Not keywords

A few words that exist in Python (or look like they might be reserved
here) are **not** Dragon keywords, and are perfectly legal identifiers:

| Word | Status |
|---|---|
| `Ellipsis`, `NotImplemented` | Ordinary names - not reserved (despite appearing in editor highlighting). |
| `match`, `case`, `thread`, `template` | Contextual only - reserved at one position, free everywhere else. |
| `async`, `await` | Reserved, but **do not** split functions into colors; every `def` is awaitable-agnostic. |
| `print`, `len`, `range`, `int`, `str`, … | Builtins, not keywords - they live in an outer namespace and may be shadowed by a fresh declaration. |
