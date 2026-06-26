# Data Types

Dragon's type system is small. There are eight built-in types you'll
use constantly, and a handful of generic containers built on top of
them. This chapter walks through each, with the operations you'll
reach for most often.

## Numbers

`int` is a 64-bit signed integer. `float` is an IEEE 754 double-
precision number. Both behave the way Python's `int` and `float`
behave, with two differences:

- Dragon's `int` is fixed-width. `2 ** 63` overflows. Python's `int` is
  arbitrary precision.
- Integer division is `//`, true division is `/`, just like Python.
  `5 / 2` is `2.5`; `5 // 2` is `2`.

```dragon
const lifespan: int = 80
const seconds: float = 3.14159
const total: float = lifespan * 365.25 * 24.0 * 3600.0
```

`bool` is a separate type, not an alias for `int`. Comparison operators
return `bool`. Conversion to `int` is explicit (`int(True)` is `1`).

## Strings

`str` is a Unicode string. `len(s)` returns the number of code points,
not bytes. Indexing returns a single-character `str` (not a byte).
Slicing works exactly like Python:

```dragon
const name: str = "Dragon"
print(len(name))       # 6
print(name[0])         # D
print(name[-1])        # n
print(name[:3])        # Dra
print(name[3:])        # gon
```

Strings support `+` for concatenation, `*` for repetition, and `in`
for substring tests:

```dragon
print("rag" in "Dragon")       # True
print("Dr" + "agon")           # Dragon
print("ab" * 3)                # ababab
```

F-strings work as in Python, including format specifiers:

```dragon
const x: float = 3.14159
print(f"pi is {x:.2f}")        # pi is 3.14
const n: int = 255
print(f"{n:08b}")              # 11111111
```

We cover strings in detail in chapter 4.

## Bytes

`bytes` is an immutable sequence of bytes (0-255). It's what you get
from reading a file in binary mode, sending or receiving raw network
data, or computing a hash:

```dragon
const data: bytes = b"\x00\x01\x02"
print(len(data))               # 3
print(data[0])                 # 0
```

Use `bytes` for raw binary data. Use `str` for text. The boundary
between them is conversion - `s.encode("utf-8")` turns a `str` into
`bytes`; `b.decode("utf-8")` turns it back. Don't mix the two casually.

## Lists

`list[T]` is a homogeneous, mutable sequence. The element type is part
of the type - you cannot mix `int` and `str` in a `list[int]`:

```dragon
numbers: list[int] = [1, 2, 3]
numbers.append(4)
print(numbers)                 # [1, 2, 3, 4]
print(numbers[0])              # 1
print(len(numbers))            # 4
```

Operations: `append`, `extend`, `insert`, `remove`, `pop`, `index`,
`count`, `sort`, `reverse`, `clear`. Slicing and concatenation work as
in Python.

If you need a sequence with mixed types, use `list[Any]` or a `tuple`.

## Dicts

`dict[K, V]` is a hash map. Key and value types are both part of the
type:

```dragon
ages: dict[str, int] = {"Alice": 30, "Bob": 25}
ages["Carol"] = 40
print(ages["Alice"])           # 30
print("Bob" in ages)           # True
del ages["Bob"]
```

Iteration order is insertion order, like Python 3.7+. Operations
mirror Python's dict: `get`, `pop`, `setdefault`, `keys`, `values`,
`items`, `update`, `clear`.

In `.dr` mode, you can also write `ages.Alice` as shorthand for
`ages["Alice"]` when the key is a valid identifier and known at
compile time.

## Tuples

`tuple[T1, T2, ...]` is an immutable, fixed-size sequence with a
type-per-position:

```dragon
const point: tuple[int, int] = (3, 4)
print(point[0])                # 3
print(point[1])                # 4

# Unpacking
const x: int, y: int = point
print(x + y)                   # 7
```

Use tuples for structured pairs and small fixed-shape records. For
larger or more meaningful records, prefer a class.

## Sets

`set[T]` is an unordered collection of unique elements:

```dragon
const names: set[str] = {"Alice", "Bob"}
print("Alice" in names)        # True
names.add("Carol")
names.remove("Bob")
```

Operations: `add`, `remove`, `discard`, `pop`, `union`, `intersection`,
`difference`. Comparison operators (`<`, `<=`, `>`, `>=`) test subset
relationships.

## None

`None` is the no-value value. It's its own type. Functions that don't
return anything implicitly return `None`:

```dragon
def log(message: str) -> None {
    print(f"[LOG] {message}")
}
```

If a value can be either `None` or some other type, declare it as a
union - `int | None` for "an int or nothing":

```dragon
result: int | None = lookup(key)
if result == none {
    print("not found")
} else {
    print(f"got {result}")
}
```

## Type conversions

Conversions are explicit, never implicit. Use `int(x)`, `float(x)`,
`str(x)`, `bool(x)`, `bytes(x)` to convert between primitive types:

```dragon
const n: int = 42
const s: str = str(n)            # "42"
const back: int = int(s)         # 42
const f: float = float(n)        # 42.0
```

`int(s)` and `float(s)` raise `ValueError` if the string isn't a valid
number. We cover error handling in chapter 9.

The next section covers operators - the arithmetic, comparison,
logical, and bitwise operations that combine the values these types
hold.
