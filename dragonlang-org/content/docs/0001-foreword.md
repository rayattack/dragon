# Foreword

Dragon is a typed, compiled language that looks and feels like Python. You
keep the syntax and semantics you already know - classes, dicts and lists,
list comprehensions, exceptions, context managers, decorators - and you get
a native binary at the end of `dragon build`, with the kind of performance
you'd expect from C or Rust.

The book in your hands is a tutorial, not a reference. It teaches you how
to write Dragon by example, in the order you would naturally need each
piece: a working program first, then variables and types, then control
flow, then how to organize larger programs into modules and classes, and
finally the parts of the language you reach for less often - concurrency,
foreign-function calls, the standard library you'll actually use day to
day.

Two things to know before you begin.

**Dragon has two file modes, and both are statically typed.** A `.dr`
file uses curly-brace blocks:

```dragon
def add(a: int, b: int) -> int {
    return a + b
}
```

A `.py` file uses Python's familiar indentation instead of braces - but
the type annotations are exactly as mandatory:

```python
def add(a: int, b: int) -> int:
    return a + b
```

Type annotations are required wherever you *declare* a variable - function
signatures, module-level variables, and locals alike (`total: int = 3`).
A bare `total = total * 2` only *reassigns* a name you already declared;
it never introduces or infers a new one. `.py` mode relaxes the *syntax* -
indentation, `def __init__(self)`, `global x` - never the type discipline;
a missing annotation is a compile error either way. It is the on-ramp for
compiling already-typed Python files, not "optional typing."

Both modes compile to the same binary. The book uses `.dr` for clarity -
the explicit braces make code listings easier to read out of context -
but every example translates directly to `.py` if you prefer that style.
We point out the rare places where the two modes differ.

**Dragon aims at Python parity.** When the standard library has a name and
a signature in CPython, Dragon's standard library uses the same name and
the same signature. `len(s)` returns the code-point count, not the byte
count. `s.split("\n")` splits on newlines and returns a `list[str]`. Dicts
preserve insertion order. If you've written Python, almost everything you
already know is correct in Dragon. We'll flag the places where it isn't.

You can read this book straight through, or skip ahead - the early
chapters are the foundation; later ones largely stand on their own. One
of them walks through the docs site you're reading right now, end to
end, in Dragon, and the chapters after it build a database-backed web
service, a concurrent program, and command-line tools.

Welcome aboard.
