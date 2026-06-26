# How a Dragon Program Runs

Most compiled languages make you name an entry point. Go insists on
`func main()` inside `package main`. Java wants
`public static void main(String[] args)` with the signature exactly so. C needs
a `main` that returns `int`. Python splits the difference with a convention
rather than a rule - the `if __name__ == "__main__":` guard that every script
grows so it can tell "run me" from "import me."

Dragon throws the whole ceremony out. **The file you hand to `dragon run` or
`dragon build` is the program, and its top-level statements are the program
body.** There is no entry function, no magic name, no `__name__` guard. You
write statements at the top level of a file, and they execute, top to bottom,
when you run that file. That's the entire model.

```dragon
def greet(name: str) -> str {
    return f"Hello, {name}!"
}

print(greet("Dragon"))   # Hello, Dragon!
```

Run that file and it prints `Hello, Dragon!`. The `print` is not inside any
function - it *is* the program. The `def` above it contributes a definition that
the `print` then uses.

## No name is special

Because the file is the entry point, no function name carries any privilege. A
function called `main` is just a function; it runs only if something calls it:

```dragon
def setup() -> None { print("2. setup() called explicitly") }
def main() -> None { print("3. main() called explicitly") }

print("1. top-level: program starts")
setup()
main()
print("4. top-level: program ends")
```

```text
1. top-level: program starts
2. setup() called explicitly
3. main() called explicitly
4. top-level: program ends
```

`main` ran only because the top-level code called it - exactly like `setup`.
Remove the call and it never runs at all. This is the trap to be aware of coming
from Go or Java: defining `main` and expecting it to fire on its own is a silent
no-op.

```dragon
def main() -> None {
    print("this never runs - nothing calls it")
}

print("the file is the program")   # the only line that executes
```

There is no `__name__` either. Dragon has no dual "am I the main module or an
import" identity to disambiguate, because top-level execution is the *only*
model - so the guard it would protect doesn't exist. Writing `print(__name__)`
is a compile error (`Undefined variable: __name__`), not a runtime surprise. If
you're porting Python, delete the `if __name__ == "__main__":` line and de-indent
its body: that body simply *is* your top-level program.

## Two surface modes, one language

Dragon reads two file extensions. `.dr` is the canonical surface - curly-brace
blocks, `def()` constructors, the works. `.py` is an adoption ramp for Python
developers - indentation-based blocks and `def __init__(self)` - but it is the
**same language** underneath, with the **same mandatory type annotations** and
the same scoping rules. It is not "optional typing for Python files"; the types
are enforced just as strictly.

```dragon
# point.dr  - canonical surface
class Point {
    def(x: float, y: float) {
        self.x = x
        self.y = y
    }
}

const p: Point = Point(3.0, 4.0)
print(f"({p.x}, {p.y})")     # (3.0, 4.0)
```

```python
# point.py  - adoption surface, identical semantics
class Point:
    x: float
    y: float

    def __init__(self, x: float, y: float) -> None:
        self.x = x
        self.y = y

p: Point = Point(3.0, 4.0)
print(f"({p.x}, {p.y})")     # (3.0, 4.0)
```

Both compile to the same thing and run identically. The differences are purely
syntactic plus the constructor spelling (`def()` versus `def __init__(self)`).
Constructors are covered in [Classes and Objects](/docs/0601-classes).

## What an import contributes

If the file you run is the program, what is every *other* file? An import. An
imported module contributes its **definitions** - functions, classes - and runs
its **module-level variable initializers**, so the constants and tables it
declares are ready when your code calls into it:

```dragon
# util.dr  (imported)
factor: int = 6                      # initializer runs - `factor` is ready

def multiply(n: int) -> int {
    return n * factor
}
```

```dragon
# main.dr  (the file you run)
from util import multiply

print(multiply(7))                   # 42
```

```bash
dragon run main.dr      # 42
```

The rule of thumb that follows: put a module's *definitions and constants* at its
top level, but keep program *behavior* - the statements that do the work - in the
file you actually run. The full resolution rules, package layout, and the
`global` keyword for writing module-level state live in
[Modules and Packages](/docs/1001-modules) and
[Variables, Constants, and Statics](/docs/0201-variables).

## Source to native binary

There is no interpreter, no bytecode VM, and no JIT warm-up between your source
and the CPU. `dragon build` compiles your file (and everything it imports) ahead
of time into a standalone native executable; `dragon run` does the same to a
temporary binary, runs it, and cleans up. What executes is machine code. The
mechanics of those subcommands - `run`, `build`, `check`, the optimization flags,
and the module search path - are the subject of
[The dragon CLI](/docs/0102-dragon-cli).

## At a glance

| Fact | Consequence |
|------|-------------|
| The file you run **is** the program | top-level statements execute top to bottom |
| No magic `main` | a `def main` runs only if called; uncalled, it's a no-op |
| No `__name__` | `if __name__ == "__main__":` is unnecessary - delete it, de-indent the body |
| `.dr` and `.py` are one language | same types, same scoping; only surface syntax differs |
| Imports contribute definitions + run their initializers | keep behavior in the entry file |
| Source compiles to a native binary | no interpreter, VM, or JIT |

With the toolchain and the execution model both in hand, [Part 2](/docs/0201-variables)
begins the language proper - variables and constants, the built-in types, operators,
comments, and control flow.
