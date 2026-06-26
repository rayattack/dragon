# Modules and Imports

Most compiled languages make you fight their module system before you write a line
of logic. C wants headers and a separate link step. Go ties package names to
directory names and forbids cycles loudly. Rust has `mod`, `crate`, `pub use`, and
a `mod.rs`/`lib.rs` dance. Python is friendlier at the surface - `import x` - but
pays for it at runtime: every import parses a file, executes its top-level code,
builds a module object, and stashes it in `sys.modules`, so a "fast" program can
spend its first 200 ms just importing.

Dragon takes Python's syntax and throws away the runtime. `import` is a
**compile-time directive**, not a statement that executes. The compiler walks your
import graph at build time, compiles every reachable module into **one LLVM
module**, and resolves cross-module references to static symbols at link time.
There is no `sys.modules`, no module object, no per-import parsing cost. A
`from math import sqrt` costs exactly what a direct call costs - nothing extra.

The whole model rests on one idea from [Functions](/docs/0301-functions):

> The **file** you hand to `dragon run` *is* the program. Its top-level statements
> are the program body. Every **other** file is an import that only contributes
> definitions.

## The three import forms

Dragon supports the three Python forms, and they mean what you expect:

```dragon
import math                 # bind the module; reach members via math.X
from math import gcd        # bind one name directly
import math as m            # bind the module under an alias
```

`import math` binds the whole module - reach members through the dot,
`math.gcd(12, 18)`. `from math import gcd` lifts `gcd` into your namespace so you
call it bare. Both compile to the same direct call; the choice is purely how the
name reads:

```dragon
from math import gcd
print(gcd(12, 18))           # 6
```

## Aliasing with `as`

`as` renames the binding - on whole-module imports and on individual names:

```dragon
import math as m
from math import gcd as divisor

print(m.gcd(12, 18))         # 6
print(divisor(12, 8))        # 4
```

Aliases earn their keep when two modules export the same name, or when a long
package path clutters the call site.

## Importing your own files

Nothing about imports is special to the standard library. A multi-file program is
just files that import each other. Put a helper beside your entry file:

```dragon
# shapes.dr - a sibling module
const PI: float = 3.14159
count: int = 0                       # a module-level global

def area(radius: float) -> float {
    return PI * radius * radius
}

def bump() -> int {
    global count                     # opt in to writing the module global
    count = count + 1
    return count
}
```

```dragon
# main.dr - the file you pass to `dragon run`
from shapes import area, bump

print("area(2.0) =", area(2.0))      # area(2.0) = 12.56636
print("bump:", bump())               # bump: 1
print("bump:", bump())               # bump: 2
```

The resolver finds `shapes.dr` next to `main.dr`. Its `area`, `bump`, `PI`, and the
module global `count` are all contributed to the build; `main.dr`'s top-level
statements are the program. (Organizing many such files into packages is the next
chapter, [Packages and Project Layout](/docs/1002-packages-and-layout).)

## What "runs on import" - and what does not

This is the one place Python intuition misleads you. When Dragon compiles a
dependency module, it emits that module's **declarations**: functions, classes,
`const`s, and module globals **with their initializers**. Those initializers run
once at program startup, before your entry file's body - the same primitive as a
C++ global constructor. So a `const` that calls a function *does* run it at startup:

```dragon
# lib.dr
def compute() -> int {
    print("[lib] initializer ran")
    return 7
}
const VALUE: int = compute()         # runs at startup
def get() -> int { return VALUE }
```

```dragon
# main.dr
from lib import get
print("[main] start")
print(get())
```

```text
[lib] initializer ran
[main] start
7
```

What does **not** run is a bare, side-effecting statement at a dependency's top
level - a standalone `print("hello")` in `lib.dr` is silently skipped. A module is
a bag of definitions, not a script that prints when imported. If you have
import-time work, express it as an initializer (`const X = setup()`), not a loose
statement.

## Module globals

A variable declared at a module's **top level** is a module global - one storage
location shared by every function in that module. A function may *read* it freely,
but to *assign* it the function must declare `global` first, in both `.dr` and `.py`:

```dragon
total: int = 100                     # module global

def peek() -> int {
    return total                     # read - no keyword needed
}

def add(n: int) -> None {
    global total                     # opt in to mutating module state
    total = total + n
}

add(5)
add(5)
print(total)                         # 110
```

Without `global total`, the assignment is a compile error - where Python silently
makes `total` a function-local (so the outer never changes and the bug hides),
Dragon rejects it. Reading never needs a keyword. (Use
[`nonlocal`](/docs/0801-closures) for the sibling case - rebinding an *enclosing
function's* variable rather than a module global.) Note that only a **top-level**
declaration becomes a global; a `count: int = 0` nested inside a top-level `if`/`for`
is block-local, because Dragon uses [block scoping](/docs/0201-variables) everywhere.

## Everything compiles into one module

Worth stating plainly, because it explains the performance: Dragon does **not** do
separate compilation. `ModuleResolver` walks the import graph, every reachable
module is lowered into a **single LLVM module**, and the optimizer sees the whole
program at once. Cross-module calls are direct calls - the optimizer can inline
`area()` from `shapes.dr` into `main.dr` as if you'd written them in one file. The
cost is no incremental per-module rebuild: change one file and the whole program
recompiles. For a language whose first commandment is speed of the *emitted code*,
that's the right trade.

## Selecting a callable by name at runtime

Dragon's `import` is a compile-time directive: the whole program is linked into one
binary, there is no `sys.modules`, no runtime module objects, and no compiler in the
runtime to parse a `.dr` file. Python's runtime-import machinery
(`importlib.import_module`, `getattr(mod, ...)`, `reload`, source-from-disk loading)
therefore has no Dragon equivalent and is **not** something the compiler provides -
this is by design, not a missing feature.

When you need to pick a callable by a string known only at runtime - the use case
people reach for `importlib` for - use an explicit **dispatch table**: a typed
`dict` mapping names to function references. It is fully static, type-checked, and a
plain hash lookup at runtime (no compiler magic, no `Any` ceremony):

```dragon
def double(x: int) -> int { return x * 2 }
def square(x: int) -> int { return x * x }

# A registry of named handlers - the static, typed equivalent of a plugin table.
_ops: dict[str, Callable[[int], int]] = {}
_ops["double"] = double
_ops["square"] = square

name: str = read_choice()             # computed at runtime
op: Callable[[int], int] = _ops[name] # look the handler up by name
print(op(5))
```

Each module populates its own table at top level (`const`/module-body code runs once
at startup, in dependency order), so a "plugin" is just a function that registers
itself into the shared `dict`. This covers string-keyed dispatch and plugin-style
selection over the set of handlers compiled into your binary - which, in a
whole-program-compiled language, is the set that can exist. Loading code that was
never compiled in is intentionally out of scope; embed a separate `dragon` binary as
a subprocess if you need true runtime extensibility.

## At a glance

| You want to... | Write |
|----------------|-------|
| Bind a whole module | `import math` then `math.gcd(...)` |
| Bind one name | `from math import gcd` then `gcd(...)` |
| Alias a module / name | `import math as m` / `from math import gcd as divisor` |
| Import a sibling file | `from shapes import area` (resolves `shapes.dr`) |
| Declare a module global | top-level `count: int = 0` |
| Write a global from a function | `global count` inside the function (both modes) |
| Import-time setup | `const X = setup()` (a loose statement won't run) |
| Pick a callable by string at runtime | a `dict[str, Callable[...]]` dispatch table |

The file you run is the program; every other file contributes definitions. Next,
how to organize many of those files: [Packages and Project Layout](/docs/1002-packages-and-layout).
