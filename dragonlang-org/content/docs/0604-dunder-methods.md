# Dunder Methods

Dunder ("double-underscore") methods hook your class into the language's operators
and built-in functions. Define `__str__` and `print(obj)` works; define `__eq__`
and `==` works; define `__add__` and `+` works. This is how a user-defined type
comes to *feel* built-in - the same protocol the standard library's own types use.
Because [`self` is implicit](/docs/0601-classes), the signatures are one parameter
shorter than Python's: for a binary operator, `other` is the only parameter.

## String representation: `__str__` and `__repr__`

`__str__` is consulted by `print()`, `str()`, and f-string interpolation;
`__repr__` backs `repr()`:

```dragon
class Vector {
    def(x: float, y: float) {
        self.x = x
        self.y = y
    }
    def __str__() -> str {
        return f"Vector({self.x}, {self.y})"
    }
}

v: Vector = Vector(1.5, 2.5)
print(v)            # Vector(1.5, 2.5)
print(f"got {v}")   # got Vector(1.5, 2.5)
print(str(v))       # Vector(1.5, 2.5)
```

Without a `__str__`, an instance prints as a safe default like
`<Vector instance>` - never a crash.

## Equality and ordering

`__eq__` powers `==`, and `!=` derives from it automatically. Each comparison
operator has its own dunder - `__lt__` for `<`, `__le__` for `<=`, and so on:

```dragon
class Money {
    def(cents: int) {
        self.cents = cents
    }
    def __eq__(other: Money) -> bool {
        return self.cents == other.cents
    }
    def __lt__(other: Money) -> bool {
        return self.cents < other.cents
    }
}

a: Money = Money(150)
b: Money = Money(299)
print(a == Money(150))   # True
print(a != b)            # True   - derived from __eq__
print(a < b)             # True
print(b < a)             # False
```

## Arithmetic operators

`__add__`, `__sub__`, `__mul__`, and the rest of the arithmetic family map
operators onto your type. The return type is yours to choose:

```dragon
class Vec {
    def(x: int, y: int) {
        self.x = x
        self.y = y
    }
    def __add__(o: Vec) -> Vec {
        return Vec(self.x + o.x, self.y + o.y)
    }
    def __sub__(o: Vec) -> Vec {
        return Vec(self.x - o.x, self.y - o.y)
    }
    def __mul__(k: int) -> Vec {       # scale by an int
        return Vec(self.x * k, self.y * k)
    }
    def __str__() -> str {
        return f"({self.x}, {self.y})"
    }
}

const a: Vec = Vec(5, 6)
const b: Vec = Vec(1, 2)
print(a + b)     # (6, 8)
print(a - b)     # (4, 4)
print(a * 3)     # (15, 18)
```

## The container protocol

Make your type behave like a sequence with `__len__`, `__getitem__`, and
`__contains__` - they wire up `len()`, `obj[i]`, and `x in obj`. Add
`__setitem__` to support `obj[i] = v`:

```dragon
class Deck {
    def(cards: list[str]) {
        self.cards = cards
    }
    def __len__() -> int {
        return len(self.cards)
    }
    def __getitem__(i: int) -> str {
        return self.cards[i]
    }
    def __contains__(c: str) -> bool {
        return c in self.cards
    }
}

d: Deck = Deck(["ace", "king", "queen"])
print(len(d))           # 3
print(d[0])             # ace
print("king" in d)      # True
print("joker" in d)     # False
```

## At a glance

| To support... | Define |
|---------------|--------|
| `print(obj)` / `str(obj)` / f-strings | `__str__() -> str` |
| `repr(obj)` | `__repr__() -> str` |
| `==` / `!=` | `__eq__(other: T) -> bool` |
| `<` `<=` `>` `>=` | `__lt__`, `__le__`, `__gt__`, `__ge__` |
| `+` `-` `*` | `__add__`, `__sub__`, `__mul__` (other operand is the only param) |
| `len(obj)` | `__len__() -> int` |
| `obj[i]` / `obj[i] = v` | `__getitem__(i) -> T` / `__setitem__(i, v)` |
| `x in obj` | `__contains__(x) -> bool` |

The broader protocol follows the same one-parameter-shorter pattern: type
conversions (`__int__`, `__float__`), the iterator protocol
(`__iter__`/`__next__`, covered in [Iterators and Generators](/docs/0803-iterators)),
and context managers (`__enter__`/`__exit__`, in
[Context Managers](/docs/0901-exceptions) - note Dragon's native `__exit__`
signature). Last in this part: locking members down with
[Member Privacy](/docs/0605-privacy).
