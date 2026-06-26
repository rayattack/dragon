# Variadics: `*args` and `**kwargs`

Sometimes a function can't know in advance how many arguments it will get.
`print` takes any number of values; a logger takes a message and then any number
of extras; a builder takes a bag of named options. Every language solves this,
and the solutions reveal each language's priorities. C's `stdarg` varargs are
untyped and unsafe - the callee guesses at the types and a mismatch is undefined
behavior. Go's `...T` is typed but monomorphic: every variadic argument must be
the same `T`. Python's `*args`/`**kwargs` are maximally flexible and completely
untyped.

Dragon keeps Python's `*args`/`**kwargs` spelling but, true to its nature,
**requires the element type**. That single annotation is what lets the compiler
give you a real, monomorphized container inside the function instead of a bag of
untyped values - and it's why a bare, unannotated `*args` is a compile error, not
a default.

## `*args` - any number of positional arguments

Annotate the variadic parameter with its *element* type. Inside the body, `*args`
is an ordinary `list[T]` - you can take its `len`, iterate it, index it, and pass
it to `sum`:

```dragon
def sum_all(*nums: int) -> int {
    total: int = 0
    for n in nums {
        total = total + n
    }
    return total
}

print(sum_all(1, 2, 3, 4))   # 10
print(sum_all())             # 0
```

The annotation is mandatory. Writing `def sum_all(*nums)` with no type is
rejected - the message tells you to use `*nums: int` for a concrete element type,
or `*nums: Any` when the arguments really are heterogeneous. This is the static
analogue of Python's untyped `*args`: you state the element type once, and in
return the body gets a fully typed `list[int]` with native storage, not a list of
boxed objects.

A regular positional parameter can come before the variadic one:

```dragon
def log(prefix: str, *messages: str) -> None {
    for msg in messages {
        print(f"[{prefix}] {msg}")
    }
}

log("INFO", "server started", "listening on 8080")
log("WARN", "low memory")
log("DEBUG")             # zero variadic args is fine
```

```text
[INFO] server started
[INFO] listening on 8080
[WARN] low memory
```

## `**kwargs` - any number of named arguments

`**kwargs` collects arbitrary *keyword* arguments. Annotate it with the value
type; inside the body it is a `dict[str, T]`, so iteration, subscripting, and
`.get` with a default all work:

```dragon
def show_config(**opts: str) -> None {
    host: str = opts.get("host", "localhost")
    port: str = opts.get("port", "5432")
    print(f"host={host} port={port}")
}

show_config(host="mydb", port="3306")    # host=mydb port=3306
show_config(port="8080")                  # host=localhost port=8080
show_config()                             # host=localhost port=5432
```

The value type is the contract: `**opts: str` gives you a `dict[str, str]`,
`**values: int` gives you a `dict[str, int]`. As with `*args`, the annotation is
required - and it's what keeps the dictionary monomorphic and fast.

## Combining them

A function can take fixed parameters, then `*args`, then `**kwargs`, in that
order - the same layout Python uses:

```dragon
def full_call(name: str, *values: int, **opts: str) -> None {
    print(f"name={name}")
    for v in values {
        print(f"  value={v}")
    }
    for k in opts {
        print(f"  opt {k}={opts[k]}")
    }
}

full_call("test", 1, 2, 3, mode="fast", debug="yes")
```

```text
name=test
  value=1
  value=2
  value=3
  opt mode=fast
  opt debug=yes
```

## Spreading at the call site

The mirror image of collecting arguments is *spreading* them. A `*` in front of a
list or tuple unpacks it into positional arguments; a `**` in front of a dict
unpacks it into keyword arguments:

```dragon
def add3(a: int, b: int, c: int) -> int {
    return a + b + c
}

const nums: list[int] = [1, 2, 3]
print(add3(*nums))            # 6   - list spread into positional params

const coords: tuple[int, int, int] = (10, 20, 30)
print(add3(*coords))          # 60  - tuple spread

const opts: dict[str, int] = {"a": 1, "b": 2, "c": 3}
print(add3(**opts))           # 6   - dict spread into keyword params

print(add3(1, *[2, 3]))       # 6   - a leading positional plus a spread
```

A `*` spread also feeds a variadic: `sum_all(*[5, 10, 15])` forwards the list
straight into `sum_all`'s `*nums`. This is how you relay a collected `*args` on
to another variadic function - `def outer(*nums: int) -> int { return inner(*nums) }`.

## At a glance

| You want to... | Write |
|----------------|-------|
| Any number of positional args | `def f(*xs: int) -> int` (annotation required) |
| Use them in the body | `xs` is a `list[int]` - `len`, `for`, `xs[i]`, `sum(xs)` |
| Any number of named args | `def f(**opts: str) -> None` |
| Read a kwarg with a fallback | `opts.get("host", "localhost")` |
| A fixed param before variadics | `def f(name: str, *xs: int, **opts: str)` |
| Spread a list/tuple positionally | `f(*items)` |
| Spread a dict by keyword | `f(**options)` |

`*args` and `**kwargs` close out the call side of functions. For functions that
capture surrounding state, see [Closures and Lambdas](/docs/0801-closures); for
one function that works across many types at once, see
[Generics](/docs/0705-generics).
