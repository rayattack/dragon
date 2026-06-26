# Built-in Functions

The functions on this page are always in scope - no `import` needed. They are
the workhorses you reach for every day: printing, measuring length, looping
over indices, converting between types, and inspecting values. Dragon mirrors
Python's names and signatures wherever the typed, compiled model allows, so
most of what you know from Python transfers directly.

Every entry in this appendix was **verified by compiling and running it**
against the Dragon compiler. Where a Python builtin is recognized by the
grammar but does not yet have a working implementation, it is listed in
[Not yet available](#not-yet-available) with the exact error - so you always
know what is real.

> **One caveat threads through this page.** A builtin (or operator) that
> returns a value works reliably when you **bind it to a typed variable first**,
> then use that variable. Passing certain expressions *inline* to `print(...)`
> can mis-render, because the inline result does not always carry its type to
> the print site. When in doubt, write `const r: T = f(...)` then `print(r)`.

## Output and input

| Function | Signature | Description |
|----------|-----------|-------------|
| `print` | `print(*values) -> None` | Writes each value to stdout, space-separated, ending in a newline. |
| `input` | `input(prompt: str = "") -> str` | Prints the prompt (no newline) and reads one line from stdin. |

```dragon
print("hello")           # hello
print(1, 2, 3)           # 1 2 3

const name: str = input("Name: ")   # reads a line after printing "Name: "
print("Hi " + name)
```

## Length and ranges

| Function | Signature | Description |
|----------|-----------|-------------|
| `len` | `len(x) -> int` | Number of elements in a `str`, `list`, `dict`, `set`, `tuple`, or `bytes`. |
| `range` | `range(stop)` / `range(start, stop)` / `range(start, stop, step)` | A lazy sequence of integers for iteration. |

```dragon
print(len("abc"))        # 3
print(len([1, 2, 3]))    # 3

for i in range(5) {
    print(i)             # 0 1 2 3 4
}
```

`range` is an **iteration construct**, not an annotation type - write
`for i in range(n)`, not `r: range = range(n)`. To materialize the values into
a list, use a [comprehension](/docs/0501-lists):
`const xs: list[int] = [i for i in range(5)]`.

## Type conversion

| Function | Signature | Description |
|----------|-----------|-------------|
| `int` | `int(x) -> int` | Parses a `str`, or truncates a `float`, to an integer. |
| `float` | `float(x) -> float` | Parses a `str`, or widens an `int`, to a float. |
| `str` | `str(x) -> str` | The human-readable text form of any value (incl. lists, dicts). |
| `bool` | `bool(x) -> bool` | Truthiness of a value (see the note below). |
| `bytes` | `bytes()` / `bytes(n: int)` / `bytes(xs: list[int])` | Empty, `n` zero bytes, or bytes from a list of ints. |

```dragon
print(int("17"))         # 17
print(int(3.9))          # 3
print(float("2.5"))      # 2.5
print(str([1, 2, 3]))    # [1, 2, 3]
```

> **`bytes("text", "utf-8")` is not supported** - the two-argument
> string-encode form errors. Build bytes via the `bytes(list[int])` form or the
> [io / encoding stdlib](/docs/1402-stdlib-io) instead.

## Numeric helpers

| Function | Signature | Description |
|----------|-----------|-------------|
| `abs` | `abs(x: int) -> int` | Absolute value of an **integer**. |
| `min` | `min(a, b)` / `min(xs: list)` | Smaller of two values, or the minimum of a list. |
| `max` | `max(a, b)` / `max(xs: list)` | Larger of two values, or the maximum of a list. |
| `sum` | `sum(xs: list)` | Sum of a list of numbers. |

```dragon
print(abs(-5))           # 5
print(min(3, 1))         # 1
print(max([4, 2, 9]))    # 9
print(sum([1, 2, 3, 4])) # 10
```

## Sequence operations

| Function | Signature | Description |
|----------|-----------|-------------|
| `sorted` | `sorted(xs: list[T]) -> list[T]` | A new list with the elements in ascending order. |
| `reversed` | `reversed(xs: list[T]) -> list[T]` | A new list with the elements in reverse order. |
| `enumerate` | `enumerate(xs)` | Yields `(index, value)` pairs while iterating. |
| `zip` | `zip(a, b)` | Yields tuples pairing elements of two sequences. |
| `any` | `any(xs: list[bool]) -> bool` | `True` if any element is truthy. |
| `all` | `all(xs: list[bool]) -> bool` | `True` if every element is truthy. |

`sorted` and `reversed` return a `list[T]`; **bind the result to a typed
variable** before printing, otherwise the inline `print(sorted(...))` mis-renders:

```dragon
const s: list[int] = sorted([3, 1, 2])
print(s)                 # [1, 2, 3]

const r: list[int] = reversed([1, 2, 3])
print(r)                 # [3, 2, 1]
```

`enumerate` and `zip` are used as loop drivers:

```dragon
for i, v in enumerate([10, 20, 30]) {
    print(i, v)          # 0 10  /  1 20  /  2 30
}

for a, b in zip([1, 2, 3], [10, 20, 30]) {
    print(a, b)          # 1 10  /  2 20  /  3 30
}
```

```dragon
print(any([False, True, False]))   # True
print(all([True, False]))          # False
```

## Introspection

| Function | Signature | Description |
|----------|-----------|-------------|
| `isinstance` | `isinstance(obj, Cls) -> bool` | Whether `obj` is an instance of `Cls` (or a subclass). |
| `repr` | `repr(x) -> str` | A debugging representation - strings are quoted. |
| `id` | `id(x) -> int` | A stable integer identity for the object. |
| `hash` | `hash(x) -> int` | A hash value for the object. |
| `chr` | `chr(code: int) -> str` | The one-character string for a code point. |
| `ord` | `ord(ch: str) -> int` | The code point of a one-character string. |

```dragon
print(repr("hi"))        # 'hi'
print(repr(42))          # 42
print(chr(65))           # A
print(ord("A"))          # 65
```

`isinstance` works for **class hierarchies**, checking against a base type:

```dragon
class Animal {
    name: str
    def(n: str) { self.name = n }
}
class Dog(Animal) {
    def(n: str) { super(n) }
}

const d: Animal = Dog("Rex")
print(isinstance(d, Animal))   # True
```

> **`isinstance` against builtin primitive types returns `False`.**
> `isinstance(5, int)`, `isinstance("x", str)`, and `isinstance(3.0, float)`
> all return `False` today - it is reliable only for user-defined class
> hierarchies, and only when the runtime value matches (or subclasses) the
> type being tested against the declared base.

## Number-base formatting

| Function | Signature | Description |
|----------|-----------|-------------|
| `hex` | `hex(n: int) -> str` | Hexadecimal string with a `0x` prefix. |
| `oct` | `oct(n: int) -> str` | Octal string with a `0o` prefix. |
| `bin` | `bin(n: int) -> str` | Binary string with a `0b` prefix. |

```dragon
print(hex(255))          # 0xff
print(oct(8))            # 0o10
print(bin(5))            # 0b101
```

## Collection constructors

| Function | Signature | Description |
|----------|-----------|-------------|
| `set` | `set()` / `set(xs: list[T]) -> set[T]` | An empty set, or a set built from a list (dedupes). |
| `dict` | `dict() -> dict[K, V]` | An empty dictionary. |
| `list` | `list() -> list[Any]` | An empty list (typed `list[Any]`). |

```dragon
const s: set[int] = set([1, 2, 2, 3])
print(s)                 # {1, 2, 3}

const d: dict[str, int] = dict()
print(len(d))            # 0
```

> **`list()` always yields `list[Any]`.** It cannot be assigned to a
> concretely-typed binding such as `list[int]` (you get
> `cannot assign 'list[Any]' to variable of type 'list[int]'`). For a typed
> empty list use the literal: `const xs: list[int] = []`. The `list(iterable)`
> copy form is likewise not yet wired - copy with a comprehension,
> `[x for x in src]`.

## Not yet available

These names are recognized by the grammar (they highlight as builtins) but do
**not** compile-and-run today. Each was tested; the error is shown so you know
exactly where the boundary is.

| Function | Status | Error when used |
|----------|--------|-----------------|
| `pow` | Front-end rejects the name | `undefined name 'pow'` |
| `round` | Front-end rejects the name | `undefined name 'round'` |
| `divmod` | Front-end rejects the name | `undefined name 'divmod'` |
| `map` | No codegen implementation | `Unknown function: map` |
| `filter` | No codegen implementation | `Unknown function: filter` |
| `tuple` | No codegen implementation | `Unknown function: tuple` |
| `type` | Returns a stub | always prints `object` |
| `iter`, `next`, `getattr`, `setattr`, `hasattr`, `delattr`, `dir`, `vars`, `globals`, `locals`, `eval`, `exec`, `compile`, `callable`, `format`, `issubclass`, `slice`, `frozenset` | Reserved, unverified | varies - not yet usable as documented |

Workarounds for the most common gaps:

- **`pow(a, b)`** - use the exponent operator `a ** b`, or `math.pow` from the
  [math stdlib](/docs/2004-appendix-stdlib).
- **`round`, `divmod`** - implement inline (`q = a // b`, `r = a % b`) or reach
  for the math module.
- **`map`, `filter`** - use a [comprehension](/docs/0501-lists):
  `[f(x) for x in xs]` and `[x for x in xs if cond]`. These are also the
  faster, more idiomatic Dragon form.
- **`tuple(xs)`** - build the tuple literal directly: `(a, b, c)`.

## At a glance

| You want to... | Write |
|----------------|-------|
| Print values | `print(a, b, c)` |
| Read a line | `const s: str = input("> ")` |
| Measure size | `len(x)` |
| Loop over indices | `for i in range(n) { ... }` |
| Loop with index | `for i, v in enumerate(xs) { ... }` |
| Pair two sequences | `for a, b in zip(xs, ys) { ... }` |
| Convert to text | `str(x)` |
| Parse a number | `int(s)` / `float(s)` |
| Sort | `const s: list[T] = sorted(xs)` |
| Reverse | `const r: list[T] = reversed(xs)` |
| Dedupe a list | `set(xs)` |
| Any / all true | `any(flags)` / `all(flags)` |
| Char and code | `chr(n)` / `ord(c)` |
| Number base | `hex(n)` / `oct(n)` / `bin(n)` |
| Transform a list | `[f(x) for x in xs]` (no `map`) |
| Filter a list | `[x for x in xs if c]` (no `filter`) |

For the operators (`**`, `//`, `in`, `not in`, comparison, bitwise) see the
[operators appendix](/docs/2002-appendix-operators); for keywords see the
[keywords appendix](/docs/2001-appendix-keywords); and for everything behind an
`import` see the [stdlib appendix](/docs/2004-appendix-stdlib).
