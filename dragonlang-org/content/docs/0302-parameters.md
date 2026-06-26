# Parameters, Defaults, and Keyword Arguments

Two languages can agree on what a function *is* and still disagree completely on
how you *call* it. C and Go keep the call site austere: arguments are
positional, all of them required, matched by order alone - Go's designers
famously left out default and keyword arguments on purpose. Python goes the
other way, with defaults, calling by name, and reordering, which is why a mature
Python API reads like prose: `connect(host="db", ssl=True)`.

Dragon takes Python's expressive call site and puts a static type checker behind
it. Defaults, keyword arguments, and multiple return values all work the way a
Python programmer expects - but every binding is checked at compile time, so a
misspelled keyword or a wrong-typed default is an error before the program runs,
not a `TypeError` in production.

## Positional parameters

The baseline is what [Functions](/docs/0301-functions) already showed: a typed,
ordered parameter list. Callers pass arguments in the same order.

```dragon
def area(width: int, height: int) -> int {
    return width * height
}

print(area(3, 4))        # 12
```

## Default arguments

Give a parameter a default and callers may omit it. A default can be any
expression of the parameter's type - a literal, a module constant, even a
function call:

```dragon
def connect(host: str = "localhost", port: int = 5432, ssl: bool = false) -> None {
    print(f"host={host} port={port} ssl={ssl}")
}

connect()                              # host=localhost port=5432 ssl=False
connect("db.example.com")              # host=db.example.com port=5432 ssl=False
```

There is one quiet but important divergence from Python here. Python evaluates a
default expression **once**, when the function is defined - the source of the
infamous "mutable default argument" bug, where `def f(x=[])` shares one list
across every call. Dragon evaluates a default **on each call that omits it**, so
that trap simply does not exist: a default is recomputed fresh every time it is
needed.

## Keyword arguments

Any parameter can be passed by name, in any order. This is the feature that
makes a call self-documenting and lets you skip over middle defaults you don't
care about:

```dragon
def connect(host: str = "localhost", port: int = 5432, ssl: bool = false) -> None {
    print(f"host={host} port={port} ssl={ssl}")
}

connect(port=9000, ssl=true)                          # reorder + skip host
connect(ssl=true, host="db.example.com", port=5433)   # fully reordered
```

```text
host=localhost port=9000 ssl=True
host=db.example.com port=5433 ssl=True
```

Mixing positional and keyword arguments works the same as Python: positional
first, then keyword.

```dragon
def describe(name: str, age: int, city: str = "unknown") -> str {
    return f"{name} is {age} from {city}"
}

print(describe("Alice", 30))                  # Alice is 30 from unknown
print(describe("Bob", age=25, city="NY"))     # Bob is 25 from NY
print(describe(city="LA", name="Carol", age=35))   # Carol is 35 from LA
```

Because the binding is resolved at compile time, the mistakes Python only finds
at runtime are caught immediately. A name that doesn't match any parameter is a
compile error (`got an unexpected keyword argument typo`); passing the same
parameter both positionally and by keyword is a compile error
(`got multiple values for argument`). The call site is checked as strictly as
the function body.

## Returning more than one value

A function returns several values by returning a `tuple`, and the caller pulls
the pieces apart with tuple unpacking - one binding per element, no subscripting:

```dragon
def minmax(nums: list[int]) -> tuple[int, int] {
    lo: int = nums[0]
    hi: int = nums[0]
    for n in nums {
        if n < lo { lo = n }
        if n > hi { hi = n }
    }
    return (lo, hi)
}

lo, hi = minmax([3, 1, 4, 1, 5, 9, 2, 6])
print(lo, hi)            # 1 9
```

`lo, hi = minmax(...)` is an *implicit binding* - the two names are declared and
typed from the tuple's element types in one move, so you don't annotate them. If
you'd rather keep the tuple whole, bind it with an annotation and index into it:

```dragon
const bounds: tuple[int, int] = minmax([3, 1, 4])
print(bounds[0], bounds[1])   # 1 3
```

Tuples are covered in full in
[Sets and Tuples](/docs/0503-sets-and-tuples); here they are simply the natural
shape for "return a few related values at once."

## At a glance

| You want to... | Write |
|----------------|-------|
| Required positional params | `def f(a: int, b: int) -> int` |
| A default | `def f(port: int = 5432) -> None` |
| Call by name | `f(port=9000)` |
| Reorder / skip middle defaults | `connect(ssl=true, host="db")` |
| Return several values | `return (lo, hi)` with `-> tuple[int, int]` |
| Unpack the result | `lo, hi = minmax(xs)` |
| Keep the tuple whole | `b: tuple[int, int] = minmax(xs)` then `b[0]` |

Defaults and keyword arguments cover the *fixed* shape of a call. When a function
needs to accept an *arbitrary* number of arguments - `print`-style - that is the
job of [Variadics](/docs/0303-variadics).
