# Union, Optional, and Narrowing

A statically typed slot holds one type - that's the whole point. But real programs
have values that are genuinely *one of several* things: a lookup that returns a
result or nothing, a parser that yields a number or a string, a field that's
present or absent. Dragon expresses these with **union types**, and gives you two
ways to recover the concrete type when you need it: `isinstance` narrowing and
`match`. The cost model is explicit - a pointer-or-nothing union is free, a
multi-type union is a tagged box - so you always know what you're paying for.

## `None` and `T | None`

`None` is its own type and value. A function that returns nothing returns `None`:

```dragon
def log(msg: str) -> None {
    print(msg)
}
```

When a value can be a real `T` *or* nothing, write `T | None`:

```dragon
def find(key: str) -> str | None {
    if key == "a" {
        return "found"
    }
    return none
}

r: str | None = find("a")
if r is none {
    print("missing")
} else {
    print(r)
}
# found
```

`Optional[T]` is the Python spelling of the same type, but it must be imported
(`from typing import Optional`), exactly as in Python. The bare `T | None` form is
Dragon-canonical and needs no import - prefer it.

For pointer-shaped `T` - `str`, `bytes`, a class instance, a `list`, a `dict` -
`T | None` is **zero-cost**. It compiles to a single nullable pointer: `None` is
`nullptr`, a value is the pointer, and `r is none` is one null check. No box, no
tag byte. This is the niche optimization Rust uses for `Option<&T>`, and it's why
returning `T | None` from a hot function costs nothing over returning `T`.

## `Union[A, B]` - the tagged box

A union of two non-`None` types holds a value that is one of several. Write it
with `|`:

```dragon
v: int | str = 42
```

An `int | str` is **not** an `int` and **not** a `str` - it's a third thing, a
16-byte box carrying a tag (which type is in here right now) and a payload. You
can't use `v` as an `int` directly, because the compiler can't prove it is one:
`x: int = v` is a compile error. To get at the value you **narrow**.

## Narrowing with `isinstance`

Inside an `if isinstance(...)` the compiler knows the concrete type and unboxes it
to its native form for free. The narrowed type is live in **both** branches - the
`then` branch sees the tested type, and the `else` branch sees the complement:

```dragon
def describe(x: int | str) -> str {
    if isinstance(x, int) {
        return f"number: {x + 1}"     # x is a native int here
    } else {
        return f"text: {x.upper()}"   # x is narrowed to str in else
    }
}

print(describe(42))     # number: 43
print(describe("hi"))   # text: HI
```

This is Dragon's answer to Go's `v, ok := x.(int)` - a checked, explicit narrowing
that produces a native-typed value. The union stays a box only while its type is
unknown; the moment you prove the type, the cost evaporates. Unions of three or
more types narrow the same way, one `isinstance` per type:

```dragon
def label(v: int | str | bool) -> str {
    if isinstance(v, int) { return "number" }
    if isinstance(v, str) { return "text" }
    return "flag"
}
```

`int | None` is itself a union, so `isinstance` is also how you *use* the value
(not just test for `None`):

```dragon
r: int | None = 5
if isinstance(r, int) {
    print(r + 100)      # 105
}
```

## Narrowing with `match`

For a value tested against several types, a `match` with type patterns is often
cleaner than a stack of `if isinstance`. A `case int()` matches when the subject is
an `int` and narrows it inside the case body:

```dragon
def kind(v: int | str) -> str {
    match v {
        case int() { return "number" }
        case str() { return "text" }
    }
    return "?"
}

print(kind(5))      # number
print(kind("x"))    # text
```

`match` is covered in full in [Control Flow](/docs/0205-control-flow); here it's
just the second, often tidier, tool for the same narrowing job.

## At a glance

| You want... | Write |
|-------------|-------|
| A value or nothing | `T \| None` (or `Optional[T]`) |
| Test for nothing | `if r is none { ... }` |
| One of several types | `int \| str` |
| Narrow (then + else) | `if isinstance(v, int) { ... } else { ... }` |
| Narrow several ways | `match v { case int() { ... } case str() { ... } }` |
| Use a narrowed value | inside the branch, `v` is native - `v + 1`, `v.upper()` |

A `T | None` over a pointer is free; a multi-type union is a 16-byte box you narrow
to spend. When the type genuinely isn't known until runtime - not "one of these
two" but "anything" - that's the next chapter: [Any](/docs/0703-any).
