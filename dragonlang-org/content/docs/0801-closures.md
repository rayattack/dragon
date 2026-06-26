# Closures and Lambdas

The [Functions](/docs/0301-functions) chapter covered the shape of a `def` -
parameters, defaults, keyword arguments, variadics, returning values. This
chapter goes after what makes functions *values* rather than just call
targets: passing them around as `Callable`s, writing them inline as lambdas,
and the closures that capture the scope around them.

Dragon is typed and compiled, so this is grounded differently than in Python.
A `Callable` is a real type the compiler checks. A non-capturing lambda is a
bare function pointer with zero overhead. And a closure lowers to a named
environment struct, not a dictionary of cells - captured by reference, with
`nonlocal` required (and enforced) to write back through a scope. Where the
behavior diverges from Python - and in a few places it does - this chapter
says so plainly.

## Functions are values

A function name is a first-class value. You can pass it, store it, and call
it through any binding. The type of "a function from `int` to `int`" is
`Callable[[int], int]` - the argument types in a bracketed list, then the
return type:

```dragon
def apply(f: Callable[[int], int], x: int) -> int {
    return f(x)
}

def square(n: int) -> int {
    return n * n
}

print(apply(square, 5))         # 25
```

`Callable[[], None]` is a no-argument function returning nothing;
`Callable[[Request, Response], None]` is a two-argument handler. The syntax
mirrors `typing.Callable`. A function value stores cleanly in a class field,
which is how callback-driven APIs are wired without inheritance:

```dragon
class Button {
    label: str
    on_click: Callable[[], None]
    def(label: str, on_click: Callable[[], None]) {
        self.label = label
        self.on_click = on_click
    }
    def click() -> None {
        print(f"clicked: {self.label}")
        self.on_click()
    }
}

def say_hi() -> None {
    print("hi!")
}

b: Button = Button("OK", say_hi)
b.click()
# clicked: OK
# hi!
```

This is the same shape the HTTP router in the standard library uses:
`app.GET(route, handler)` takes a `Callable[[Request, Response, Context], None]`.

## Lambdas

An anonymous function is a `lambda`. Note the syntax: typed parameters in
parentheses, a `->` return type, and a **braced body** - Dragon lambdas are
full function bodies, not the single-expression form Python restricts you
to:

```dragon
const square: Callable[[int], int] = lambda (n: int) -> int { return n * n }
print(square(6))                # 36
```

Because a lambda is a value, it slots straight into a higher-order call:

```dragon
print(apply(lambda (n: int) -> int { return n + 100 }, 5))   # 105
```

A lambda that captures nothing compiles to a bare function pointer - zero
overhead, identical to a named function. For anything more than a line or
two, prefer a named `def`: the name documents intent and shows up in
diagnostics.

## Closures: capturing the enclosing scope

A lambda (or a nested `def`) may reference variables from the function that
encloses it. The compiler detects the free variables, packs their values
into a heap-allocated environment struct, and the lambda reads them back.
Non-capturing lambdas pay nothing; capturing ones pay one small allocation:

```dragon
def make_pipeline() -> None {
    total: int = 0

    add: Callable[[int], None] = lambda (x: int) -> None {
        nonlocal total
        total = total + x
    }

    for n in [10, 20, 30] {
        add(n)
    }

    print(total)                # 60
}

make_pipeline()
```

Two things are happening here. The lambda *captures* `total`. And because
it needs to **write back** to `total` - not just read it - it declares
`nonlocal total`. Without `nonlocal`, a bare assignment to a captured name
is a **compile error**, not a silent new local:

```dragon
add: Callable[[int], None] = lambda (x: int) -> None {
    total = total + x   # error: 'total' is owned by an enclosing function;
}                       # add 'nonlocal total' to rebind it, or declare a
                        # new local with 'total: int = ...'
```

This is where Dragon is stricter - and safer - than Python. Python would
quietly make `total` a fresh function-local, so the outer `total` never
changes and the bug hides until you notice the wrong answer. Dragon rejects
it and names the two explicit choices: `nonlocal total` to rebind the outer
one, or `total: int = ...` to declare a new local. Reading a captured
variable needs no keyword; only *rebinding* one does.

The boundary that matters here is the **function**, not the block. An `if`
or `for` body *inside the same function* mutates that function's locals with
a plain `=` - no keyword, because a block is not a new function. `nonlocal`
is only for reaching across into an enclosing function.

With `nonlocal`, the variable is promoted to a heap cell that both the
enclosing function and the closure read and write through, so the mutation
is visible after the closure runs - Python's `nonlocal` runtime semantics,
except Dragon turns a forgotten keyword into a compile error instead of a
silent no-op.

### Closures escape

A capturing closure may **escape** its defining scope. A function can build
a lambda that captures a parameter and return it; the captured environment
travels with the closure, so the returned function carries its configuration
with it:

```dragon
def make_adder(base: int) -> Callable[[int], int] {
    return lambda (n: int) -> int { return n + base }   # captures `base`
}

add5: Callable[[int], int] = make_adder(5)
print(add5(10))   # 15

add9: Callable[[int], int] = make_adder(9)
print(add9(1))    # 10
print(add5(10))   # 15 - each adder keeps its own captured base
```

Each call to `make_adder` produces an independent closure with its own
captured `base`, so `add5` and `add9` do not interfere. The returned
closure also passes straight into a higher-order call:

```dragon
print(apply(make_adder(100), 7))   # 107
```

A returned closure can also be called **inline**, without binding it to a
name first - the chained call reads the captured value correctly:

```dragon
print(make_adder(5)(10))   # 15
```

Binding it to a named `Callable` is still usually clearer when you call it
more than once, but it isn't required.

## At a glance

| You want to... | Write |
|----------------|-------|
| The type of a function | `Callable[[int, str], bool]` |
| Store a function in a field | `on_click: Callable[[], None]` |
| Pass a function to a function | `apply(square, 5)` |
| An inline function | `lambda (x: int) -> int { return x * 2 }` |
| Read an enclosing variable | capture it (automatic, read-only) |
| Write an enclosing variable | `nonlocal name` inside the closure |
| Return a closure and call it | `f: Callable[...] = make(); f(x)`, or inline `make()(x)` |

The rule that carries most of the weight: **types are not optional** - a
`Callable` field without its signature is a build error, and that strictness
is what keeps the emitted code at C speed. Escaping closures work fully -
return them, store them, pass them, and call them inline or through a named
binding.

Functions and closures are the verbs. The next chapter,
[Decorators](/docs/0802-decorators), is the `@name` syntax built on top of
them - registration, `@property`, `@staticmethod`, and `@classmethod`.

