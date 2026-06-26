# Variables, Constants, and Statics

Dragon has three ways to bind a name to a value, and the choice is part
of how you communicate intent to the reader of your code.

## Plain variables

```dragon
counter: int = 0
counter = counter + 1
```

A name introduced without `const` or `static` is a regular variable.
The annotation (`: int` here) is mandatory - Dragon requires a type on
every binding, in both `.dr` and `.py` source. After the
binding, the name can be reassigned as many times as you like, as long
as every assignment matches the declared type:

```dragon
name: str = "Alice"
name = "Bob"            # ok - still str
name = 42               # error - int doesn't match str
```

You can declare a variable without an initial value if you give it a
type:

```dragon
result: int
if some_condition {
    result = compute_left()
} else {
    result = compute_right()
}
```

Dragon does not perform definite-assignment analysis. A
declared-but-unassigned variable is not a compile error; it is
zero-initialized at its declared type. If a code path leaves `result`
unset, reading it yields that zero value rather than failing to
compile.

The zero value depends on the type:

```dragon
i: int          # 0
f: float        # 0.0
b: bool         # False
s: str          # None
```

`int`, `float`, and `bool` get genuine zero values (`0`, `0.0`,
`False`). `str` is initialized to `None`. Class instances and the
containers `list` and `dict` are also initialized to `None` - they are
*not* an empty instance or an empty container. Reading the value back is
safe (it compares equal to `None`), but calling a method on a `None`
container - `lst.append(1)` on a declared-but-unassigned `lst:
list[int]` - dereferences null and crashes at runtime. Always assign a
real value before you use a `str`, container, or instance variable; the
compiler will not catch a missing assignment for you.

## Constants

```dragon
const port: int = 2018
const greeting: str = "Hello, World!"
```

`const` declares a name that can be assigned exactly once. Trying to
reassign a `const` is a compile-time error:

```dragon
const port: int = 2018
port = 8080         # error - cannot reassign const
```

Use `const` whenever the binding doesn't change. It's a hint to the
reader and a check the compiler enforces - when you see `const x = ...`,
you know `x` keeps that value for its entire lifetime in scope.

`const` is a binding modifier, not a memory-location modifier. The
*value* `const` binds to may itself be mutable: a `const lst: list[int]`
still lets you do `lst.append(1)`. What `const` forbids is reassigning
`lst` to a different list.

## Statics

```dragon
static buffer: list[int] = []
static cache: dict[str, str] = {}
```

`static` declares module-level state with the same shape as `const`,
but with a stronger guarantee: every module sees the same value. A
`static` lives for the lifetime of the program, is initialized exactly
once at program start, and is shared across all functions and threads
that reach it.

Use `static` for caches, buffers, configuration tables, and other
values that should be shared. Use `const` for compile-time constants
like ports, paths, and limits. The difference matters most in
multithreaded code (chapter 11).

`static` is a `.dr`-mode-only construct; in `.py` mode you write a
module-level variable and the runtime gives it the same lifetime
without the keyword.

## Naming conventions

Dragon follows Python's naming conventions:

- `snake_case` for variables, constants, and functions.
- `PascalCase` for classes and exception types.
- `SCREAMING_SNAKE_CASE` for names you treat as compile-time constants.

The compiler does not enforce these - they are conventions, not rules.

## Scope

Dragon scopes by **block**, not by function. Every `{ }` body is its own
scope - not just a function, but also an `if`, `elif`, `else`, `while`,
`for`, `with`, `except`, or `try` block. A name declared inside a block
is not visible once that block ends. This is a deliberate difference from
Python, which scopes by function and leaks `if`/`for` locals into the
rest of the enclosing function:

```dragon
if condition {
    result: int = 42
}
print(result)            # error - 'result' went out of scope with the block
```

Because each block is its own scope, sibling blocks can reuse a name
freely; they are independent variables that never collide:

```dragon
if ready {
    msg: str = "go"
    print(msg)
}
if done {
    msg: str = "stop"    # a different variable from the msg above
    print(msg)
}
```

The same rule applies in `.py` mode, where each indented suite is the
block - surface syntax never changes scoping.

This block rule stops at the *function* boundary. A nested function does
not get to rebind its enclosing function's locals with a bare `=`: that is
a compile error, not a silent new local (Python's classic gotcha, which
Dragon refuses to inherit). To mutate an enclosing function's variable you
opt in with `nonlocal`; to make a fresh one you declare it with `:`. We
cover this in chapter 10.

## Shadowing

Inside a nested block you can declare a name that already exists in an
enclosing scope. The inner declaration is a fresh binding - it may even
have a different type - and the outer one is untouched once the block
ends:

```dragon
count: int = 3
if count > 0 {
    count: str = "many"            # a new binding, local to this block
    print(count)                   # many
}
print(count)                       # 3 - the outer count is unchanged
```

This is shadowing across *different* scopes. You still cannot declare the
same name twice in one scope: redeclaring a parameter or an earlier local
in the same block is an error - use `=` to reassign it instead.

Shadowing is occasionally useful but easy to abuse. Most of the time,
give the new binding a new name.

The next section covers the types you'll be annotating these bindings
with.
