# Callable, TypedDict, and intc

Three more annotations finish the type system's vocabulary: `Callable` types a
function value, `TypedDict` types a dict with a fixed schema, and `intc` bridges
the gap to C's 32-bit `int` at the FFI boundary. Each fills a specific need a
plain `int`/`str`/`list` annotation can't.

## `Callable[[ArgTs], R]` - a function value

Functions are first-class, and the type of "a function taking an `int` and
returning an `int`" is `Callable[[int], int]` - the argument types in a bracketed
list, then the return type:

```dragon
def apply(f: Callable[[int], int], x: int) -> int {
    return f(x)
}

def square(n: int) -> int {
    return n * n
}

print(apply(square, 6))   # 36
```

It accepts a `lambda` just as readily, which is the idiom for passing behavior
into a higher-order function:

```dragon
def transform(xs: list[int], fn: Callable[[int], int]) -> list[int] {
    out: list[int] = []
    for x in xs {
        out.append(fn(x))
    }
    return out
}

print(transform([1, 2, 3], lambda (n: int) -> int { return n * 10 }))
# [10, 20, 30]
```

The syntax mirrors Python's `typing.Callable`. A no-argument function is
`Callable[[], R]`; a two-argument one is `Callable[[int, str], bool]`. See
[Functions](/docs/0301-functions) for the closures and higher-order patterns this
type unlocks, and [Type Annotations](/docs/0701-type-annotations) for aliasing a
long callable type with `type Handler = Callable[[Request], Response]`.

## `TypedDict` - a dict with a fixed schema

When a dict has a *known* set of keys with *known* per-key types - a config object,
an API response, a database row - describe it with a `TypedDict`: a class with a
`TypedDict` base and one field declaration per key.

```dragon
class User(TypedDict) {
    name:   str
    age:    int
    active: bool
}
```

Construct it by calling with the fields, and read them back with **dot-access**
(the typed shorthand for string-keyed dicts) or the subscript form:

```dragon
u: User = User(name="Ada", age=36, active=true)
print(u.name)        # Ada    - typed as str
print(u.age)         # 36     - typed as int
print(u.active)      # True   - typed as bool
```

Under the hood a `TypedDict` *is* a dict - deliberately. That means `**spread`,
`.items()`, runtime-key access, and passing it where a `dict` is expected all
work. A `TypedDict` is the right tool when the schema is fixed and you want field
access checked at compile time; a plain [`dict[K, V]`](/docs/0502-dictionaries) is
the tool when keys are dynamic. `TypedDict` slots also compose - a `list[User]` is
a list of typed dicts, and `users[0].name` reads through to the typed field.

## `intc` - the C-FFI bridge

A Dragon `int` is 64-bit; C's `int` is usually 32-bit. When you declare a foreign
function with `extern "C"`, the types must match the C ABI *exactly* or you'll
pass 64 bits where the callee reads 32. `intc` is the type for that boundary - it
lowers to `i32`:

```dragon
extern "C" def usleep(usec: intc) -> intc
```

`intc` is an FFI tool, not a general-purpose number. Use it *only* in the
`extern "C"` signature; the value widens to a normal `int` the moment it's back in
Dragon code:

```dragon
import random
random.srand(42)
n: int = random.rand()      # rand() returns intc; widens to int
print(n > 0)                # True
```

The [FFI chapter](/docs/1501-ffi) covers the full set of boundary types.

## At a glance

| You want to express... | Annotation |
|------------------------|------------|
| A function value | `Callable[[int], int]` |
| A no-arg function | `Callable[[], R]` |
| A fixed-schema dict | a class with a `TypedDict` base |
| Read a typed key | `u.name` (dot) or `u["name"]` (subscript) |
| A C `int` at the boundary | `intc` (only in `extern "C"`) |

That rounds out the concrete annotations. The last piece of the type system writes
one definition that specializes to native code for *every* type you use it with:
[Generics](/docs/0705-generics).
