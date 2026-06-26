# Migrating from Python

If you write Python, you already read most of Dragon. The syntax is
deliberately familiar: f-strings, comprehensions, `for x in xs`, the
same four collection types with the same method names, indentation in
`.py` mode. That familiarity is the on-ramp. But Dragon is **inspired
by** Python - it is not a superset and not a dialect. It is a typed,
compiled language that targets LLVM and produces a native binary.

> **Set expectations honestly.** You cannot take an arbitrary Python
> program - `numpy`, `pandas`, Flask, Django, anything reaching for the
> CPython C API - and compile it with Dragon. Those do not work, and
> they never will. What works is a typed file that stays inside the
> subset Dragon implements, plus a batteries-included
> [standard library](/docs/2004-appendix-stdlib) that mirrors the Python module
> shapes Dragon ships.

This appendix walks through what carries over unchanged, what you adjust
slightly, and the handful of places where the mental model genuinely
differs. Read it once and you will stop being surprised.

## Two file modes, one language

Dragon reads two surface syntaxes. Pick by file extension:

| | `.dr` | `.py` |
|---|---|---|
| Blocks | curly braces `{ }` | indentation |
| Type annotations | mandatory | mandatory (PEP-484, enforced) |
| Constructor | `def() { ... }` | `def __init__(self, ...)` |
| `self` in methods | implicit | explicit, as in Python |
| Audience | new Dragon code | porting an existing typed file |

The two modes compile to **identical** semantics - block scoping, the
`:`-declares rule, fixed types. The only differences are the syntactic
ones in the table above. `.py` mode is the adoption ramp: it lets you
bring an individually typed Python file across with minimal edits. It is
not a promise that any `.py` file runs.

Here is the same program in both modes.

```python
# greet.py - .py mode: indentation, explicit self, PEP-484 types
class Point:
    def __init__(self, x: int, y: int) -> None:
        self.x = x
        self.y = y

    def dist(self) -> int:
        return self.x + self.y

p: Point = Point(3, 4)
print(p.dist())  # 7
```

```dragon
# greet.dr - .dr mode: braces, def() constructor, implicit self
class Point {
    x: int
    y: int
    def(x: int, y: int) {
        self.x = x
        self.y = y
    }
    def dist() -> int {
        return self.x + self.y
    }
}
p: Point = Point(3, 4)
print(p.dist())  # 7
```

> **The enforcer is not optional.** In `.py` mode, a missing parameter
> or return annotation is a compile error - `missing type annotation for
> parameter 'name'`. Python's gradual typing is gone; types are the
> contract. See [Type Annotations](/docs/0701-type-annotations).

## `:` declares, `=` reassigns

This is the single rule that catches the most Python bugs, so internalize
it first. A name enters a scope **exactly once**, with an annotation:

```dragon
total: int = 0   # declares - introduces the name with its type
total = 10        # reassigns - the name must already exist
```

A bare `=` to a name that was never declared is an **error**, not a new
variable:

```python
# Python - a typo silently creates a brand-new variable
totl = 0
total = total + 1   # NameError at runtime, if you're lucky
```

```dragon
# Dragon - caught at compile time
totl: int = 0
total = total + 1
# ERROR: 'total' is not declared; introduce it with 'total: <type> = ...'
#        (bare '=' only reassigns an existing variable)
```

That classic Python footgun - a misspelled assignment target quietly
spawning a fresh local - is impossible here. The compiler knows every
declaration site because every declaration site uses `:`.

## A variable's type is fixed at its declaration

In Python a name can be an `int` on line 1 and a `str` on line 5. In
Dragon the type is locked when you declare it. Reassigning an
incompatible type is an error:

```python
# Python - perfectly legal, occasionally a disaster
n = 5
n = "hello"   # n is now a str
```

```dragon
# Dragon
n: int = 5
n = "hello"
# ERROR: cannot assign 'str' to 'n' of type 'int'
#        (a variable's type is fixed at its declaration)
```

This is not pedantry - it is what lets Dragon flow every value at its
[native machine type](/docs/0202-data-types) with no boxing. An `int` is
an `i64` from declaration to last use.

## Block scoping, not function scoping

Python scopes by **function**: a name bound inside an `if` or `for` body
leaks out to the rest of the function. Dragon scopes by **block**: every
`{ }` (or every indented suite in `.py` mode) is its own scope, and names
declared inside it vanish at the closing brace.

```python
# Python - z survives the if block
if True:
    z = 99
print(z)   # 99
```

```dragon
# Dragon - z is local to the if block
if true {
    z: int = 99
}
print(z)
# ERROR: undefined name 'z'
```

If you need a value after the block, declare it before:

```dragon
z: int = 0
if true {
    z = 99   # reassign the outer z, don't redeclare
}
print(z)     # 99
```

This applies to `for`, `while`, `with`, `try`/`except`, and match cases
alike. It is a deliberate, mode-independent choice - the same in `.dr`
and `.py` - and it lets the compiler free heap locals at block exit with
no garbage-collection cost. See
[Variables](/docs/0201-variables) for the full scoping rules.

## No magic `main()`, no `if __name__ == "__main__"`

Python files lead a double life: imported as a module *and* runnable as a
script, with the `__name__` guard deciding which. Dragon has no such
guard, because it has no ambiguity to resolve.

> **The file you run is the program.** Its top-level statements execute
> top to bottom. Every other file is an import that only contributes
> definitions.

```python
# Python - the dual-identity dance
def main():
    print("running")

if __name__ == "__main__":
    main()
```

```dragon
# Dragon - top-level code is the program body
def work() -> None {
    print("running")
}
work()   # it runs only because you called it here
```

There is no privileged function name. A `def main()` is an ordinary
function; defining it without calling it produces no output. Imported
modules' top-level code runs once on import; the entry file's top-level
code is the program. See [Modules](/docs/1001-modules).

## A static binary, not an interpreter

`dragon build app.dr` produces one native executable. There is no
interpreter shipping alongside it, and that has consequences you should
plan for:

| Python | Dragon |
|---|---|
| `eval` / `exec` of runtime strings | not available - there is no interpreter |
| C extension modules (`.so` via CPython API) | not available - use [FFI](/docs/1501-ffi) for C |
| `pip install` from PyPI | not available - the stdlib is statically linked in |
| `import numpy` / `pandas` / Flask | not available, by design |
| `__import__`, monkeypatching, runtime reflection | mostly absent - the program is fixed at compile time |

In exchange you get a single self-contained binary with no runtime
dependency, no virtualenv, and a
[standard library](/docs/2004-appendix-stdlib) - `os`, `io`, `re`, `json`,
`http.server`, `hashlib`, `datetime`, `math`, the `collections` family,
and more - that is compiled directly into your executable. Missing
batteries are added to the language, not fetched from a registry.

## Dynamic Python, statically

Python leans on runtime dynamism: `type(x)` dispatch, `getattr`/`setattr`,
metaclasses, monkeypatching, `@classmethod` constructors that build
whatever subclass `cls` turns out to be. Dragon drops all of it - not to
be austere, but because a static type system expresses the same intent at
compile time, with no runtime cost. These patterns don't fail mysteriously
at runtime; the compiler rejects them up front, and the idiom below is
what you reach for instead.

> **Rule of thumb.** If a Python pattern asks *"what type is this at
> runtime?"*, the answer is already in Dragon's static type - reach for
> `isinstance` narrowing, a base class with virtual dispatch, or a
> generic. If it asks *"let me poke at this object's attributes by string
> name"*, there is no Dragon equivalent by design; model the data
> explicitly.

### A method that `yield`s - but drop `@classmethod`

`yield` works in methods, instance and `@staticmethod` alike, so a
generator method is perfectly ordinary:

```dragon
class Squares {
    @staticmethod
    def of(n: int) {
        k: int = 0
        while k < n {
            yield k * k
            k = k + 1
        }
    }
}
total: int = 0
for v in Squares.of(4) {
    total = total + v   # 0 + 1 + 4 + 9
}
print(total)   # 14
```

What Dragon does **not** support is a `@classmethod` *generator*. In Python
the whole point of `@classmethod` is the late-bound `cls` - the body can do
`cls(...)` to construct whatever subclass it was called on, decided at
runtime. Dragon resolves the class statically, so `cls` carries no
information the type system doesn't already have: a `@classmethod`
generator would be a `@staticmethod` generator wearing a costume. Write the
`@staticmethod` form above, a plain [generator function](/docs/0803-iterators),
or - when the yielded type genuinely varies by caller - a
[generic](/docs/0705-generics):

```dragon
def take[T](xs: list[T], n: int) -> list[T] {
    out: list[T] = []
    i: int = 0
    while i < n {
        out.append(xs[i])
        i = i + 1
    }
    return out
}
first_two: list[int] = take([10, 20, 30, 40], 2)
print(first_two)   # [10, 20]
```

### `type(x)` dispatch → `isinstance` narrowing

Python branches on `type(x)` or `isinstance` and trusts duck typing for the
rest. Dragon keeps `isinstance`, and inside the branch it **narrows** the
static type - the right methods and fields become available with no cast:

```python
# Python
def describe(x):
    if isinstance(x, int):
        return f"doubled: {x * 2}"
    return f"upper: {x.upper()}"
```

```dragon
# Dragon - each branch narrows the union member
def describe(x: int | str) -> str {
    if isinstance(x, int) {
        return f"doubled: {x * 2}"
    }
    if isinstance(x, str) {
        return f"upper: {x.upper()}"
    }
    return "?"
}
print(describe(21))     # doubled: 42
print(describe("hi"))   # upper: HI
```

There is no `type(x)` handing back a class object to compare against or
construct from - the union member *is* the type, and `isinstance` is how
you ask which one it is. See [Type Annotations](/docs/0701-type-annotations).

### Duck typing → a base class + virtual dispatch

"Anything with an `.area()`" becomes a declared base class. A `list` of the
base type dispatches to each subclass's override through a vtable (and the
call is devirtualized to a direct call when the whole program defines no
override):

```dragon
class Shape {
    def area() -> int { return 0 }
}
class Square(Shape) {
    side: int
    def(s: int) { self.side = s }
    def area() -> int { return self.side * self.side }
}
def total(shapes: list[Shape]) -> int {
    t: int = 0
    for s in shapes {
        t = t + s.area()   # dispatches to Square.area
    }
    return t
}
print(total([Square(4)]))   # 16
```

When the variation is over *types* rather than *behavior*, reach for a
[generic](/docs/0705-generics) instead: `def f[T](...)` is monomorphized per
type with zero boxing.

### Reflection → model the data explicitly

`getattr` / `setattr` / `hasattr`, `__dict__`, monkeypatching, and
metaclasses are all absent - the set of fields and methods is fixed when
the program compiles. (Reading an undeclared attribute is a compile error,
not a runtime `AttributeError`.) Two replacements cover almost every case:

- **Known-but-many fields** → declare them. If you reached for `setattr` to
  avoid typing them out, type them out; now every access is checked.
- **Genuinely dynamic, string-keyed data** → that is a `dict[str, V]`, not
  an object. Keep the dynamism in a value, not in the type system:

```dragon
# Python's setattr(cfg, k, v) / getattr(cfg, k) → a dict
cfg: dict[str, str] = {}
cfg["host"] = "localhost"
print(cfg.get("host", "127.0.0.1"))   # localhost
```

Built-in class synthesis (`@dataclass`, enums) covers the common reasons to
reach for a metaclass - see [Classes](/docs/0601-classes).

## Colorless concurrency: `fire` / `await`, not `async def`

Python splits the world in two with `async`/`await`: an `async def` can
only be awaited from another coroutine, you need an event loop to run it,
and calling it from a plain function is a bug. That is "function
coloring."

Dragon has no color split. **Any** function can spawn concurrent work
with `fire`, and **any** function can `await` a task. There is no
`async def`, no `asyncio.run`, no loop to set up.

```python
# Python - async colors the whole call chain
import asyncio

async def work() -> int:
    return 42

async def main() -> None:
    r = await work()
    print(r)

asyncio.run(main())
```

```dragon
# Dragon - fire spawns a green thread; await joins it
def work() -> int {
    return 42
}
t: Task[int] = fire work()
r: int = await t
print(r)   # 42
```

`fire fn()` spawns an M:N-scheduled green thread and hands back a
`Task[T]`; `await` joins it (yielding to the scheduler if the awaiter is
itself a green thread). The `Task[int]` binding annotation is mandatory -
`t = fire work()` is rejected, because types are never inferred into a
bare `=`. See [Concurrency](/docs/1101-green-threads).

## What carries over unchanged

A reassuring amount. If you already know Python, this is muscle memory:

- **f-strings** - `f"Hello, {name}!"` works verbatim. See [Strings](/docs/0401-strings).
- **Comprehensions** - `[i * i for i in range(n)]`, with `if` filters.
- **Collections** - `list`, `dict`, `set`, `tuple` with the same literal syntax and the same methods (`.append`, `.get`, `.items`, slicing, `in`). See [Collections](/docs/0501-lists).
- **Control flow** - `if`/`elif`/`else`, `for`/`else`, `while`, `break`/`continue`, `match`.
- **Operators** - arithmetic, comparison, `and`/`or`/`not`, `//`, `**`, `%`. See [Operators](/docs/2002-appendix-operators).
- **Functions** - default arguments, keyword arguments, `*args`/`**kwargs` (typed), closures. See [Functions](/docs/0301-functions) and [Closures and Lambdas](/docs/0801-closures).
- **Exceptions** - `try`/`except`/`else`/`finally`, `raise`, custom exception classes. See [Error Handling](/docs/0901-exceptions).
- **stdlib shapes** - where Dragon ships a module, the API names and signatures track Python 3. See the [stdlib index](/docs/2004-appendix-stdlib).

The values look the same and read the same. The difference is that Dragon
checks them at compile time and runs them at C speed.

## Where the shapes differ

The stdlib tracks Python 3 names and signatures wherever it can, but a few
modules and types diverge on purpose. Check these before you port:

- **`re` returns no `Match` object.** `re.match(pattern, subject)` returns an
  `int` - positive when the pattern matches, `0` or negative when it does not -
  and `re.search` returns the matched substring as a `str` (`""` for no match).
  Captured groups come from a compiled `Pattern`:
  `re.compile(p).group(subject, n)`. There is no `Match`, no
  `.start()`/`.end()`/`.span()`.
- **`re.sub` is literal replacement only.** The replacement string is inserted
  verbatim - backreferences like `\1` are not expanded.
- **`tomllib.loads` returns a `TomlDoc`, not a `dict`.** Read values through its
  typed dotted-key accessors: `.get(key)`, `.get_int(key)`, `.get_float(key)`,
  `.get_bool(key)`.
- **`csv` exposes `parse_row` / `format_row`, not `reader` / `writer`.** Both
  take the delimiter as a required argument: `parse_row(line, ",")` and
  `format_row(fields, ",")`.
- **`int` is a fixed 64-bit signed integer and wraps silently.** There is no
  arbitrary-precision big-int and no `OverflowError`; overflowing `i64` wraps
  around, as in C.
- **Dicts are monomorphic in their key type.** A `dict[int, V]` and a
  `dict[str, V]` are distinct - one dictionary cannot hold both `1` and `"1"`
  as keys. Indexing a `dict[int, V]` with a `str` is a compile error, unlike
  Python where both coexist.

## A porting checklist

When you bring a typed `.py` file across, walk this list:

1. **Annotate everything.** Every parameter and return type. The enforcer rejects bare `def`s.
2. **Change `=` to `:` at each first binding.** The first time a name appears, give it a type; later assignments stay bare `=`.
3. **Hoist any name you read after a block** to a declaration before the block.
4. **Replace dynamic features.** No `eval`/`exec`, no monkeypatching, no `numpy`/`pandas`. Find the stdlib equivalent or drop to [FFI](/docs/1501-ffi).
5. **Drop the `__main__` guard.** Top-level code is the program; delete the guard and call what you mean to run.
6. **Swap `asyncio` for `fire`/`await`** if you use coroutines.
7. **Verify the imports exist.** Only modules in the [stdlib index](/docs/2004-appendix-stdlib) resolve.

If a file leans on something on this list that has no equivalent, that
file is not portable - and that is the honest answer, not a workaround to
hunt for.

## At a glance

| Concept | Python | Dragon |
|---|---|---|
| Positioning | the language | a typed, compiled language *inspired by* Python |
| Execution | interpreted (CPython) | compiled to a native LLVM binary |
| File modes | `.py` only | `.dr` (braces) and `.py` (indentation) |
| Type annotations | optional, gradual | mandatory, enforced at compile time |
| New variable | `x = 1` | `x: int = 1` (`:` declares) |
| Reassign | `x = 2` | `x = 2` (bare `=`, name must exist) |
| Retype a name | allowed | error - type fixed at declaration |
| Scope | function | block (every `{}` / suite) |
| Rebind an enclosing fn's var | bare `=` silently makes a new local (the forgot-`nonlocal` bug) | compile error - `nonlocal x` to rebind, or `x: T = ...` for a new local |
| Constructor | `def __init__(self, ...)` | `.dr`: `def() { ... }`; `.py`: `__init__` |
| Entry point | `if __name__ == "__main__"` | the file you run; top-level code executes |
| Concurrency | `async def` / `await` (colored) | `fire` / `await` (colorless) |
| Packages | `pip install` from PyPI | statically linked stdlib; no registry |
| Dynamic eval | `eval` / `exec` | not available |
| Runtime type dispatch | `type(x)` / duck typing | `isinstance` narrowing, base-class virtual dispatch, or generics |
| Reflection | `getattr` / `setattr` / metaclasses | not available - declare fields, or use a `dict[str, V]` |
| `@classmethod` generator | late-bound `cls`, yields | use `@staticmethod` / a free generator / a generic |
| C extensions | CPython C API | [FFI](/docs/1501-ffi) to C |
| numpy / pandas / Flask | yes | no, by design |
| f-strings, comprehensions, collections | yes | yes - unchanged |

Welcome aboard. Most of your Python instincts are correct; the few that
aren't, the compiler will tell you about before your program ever runs.
