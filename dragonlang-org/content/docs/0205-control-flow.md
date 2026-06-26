# Control Flow

Control flow in Dragon is small, familiar, and Python-shaped - `if`
/ `elif` / `else`, `while`, `for`, `match`, plus `break` and
`continue`. The two adjustments for `.dr` mode are curly-brace blocks
and parentheses-free conditions.

## `if` / `elif` / `else`

```dragon
const score: int = 78

if score >= 90 {
    print("excellent")
} elif score >= 70 {
    print("good")
} elif score >= 50 {
    print("passing")
} else {
    print("needs work")
}
```

The condition is any expression that evaluates to `bool`. Truthiness is
*not* automatic - `if 0 { ... }` is a type error. Use
`if value != 0`, `if not value`, or `if len(s) > 0` instead.

## Conditional expressions

The Python ternary works:

```dragon
const status: str = "ok" if response_code < 400 else "error"
```

Use this for short, one-shot value selections. For anything longer or
with side effects, write a normal `if` statement.

## `while` loops

```dragon
n: int = 10
while n > 0 {
    print(n)
    n = n - 1
}
print("liftoff")
```

A `while` runs its body as long as the condition is true. Use `break`
to exit early, `continue` to skip to the next iteration:

```dragon
while true {
    const line: str = read_line()
    if line == "" {
        break
    }
    if line.startswith("#") {
        continue
    }
    process(line)
}
```

## `for` loops

`for x in iterable` walks any iterable - lists, tuples, sets, dicts,
strings, generators, ranges:

```dragon
for name in ["Alice", "Bob", "Carol"] {
    print(f"hi, {name}")
}
```

```dragon
for i in range(0, 10) {
    print(i)
}
```

Iterating a dict iterates its keys, just like Python. Use `.items()`
to get key-value pairs:

```dragon
for word in counts {
    print(f"{word}: {counts[word]}")
}

# Or:
for word, count in counts.items() {
    print(f"{word}: {count}")
}
```

`break` and `continue` work in `for` loops the same way they do in
`while` loops. The Python `else` clause on loops works too:

```dragon
for candidate in primes {
    if candidate == target {
        print("found")
        break
    }
} else {
    print("not found")
}
```

The `else` runs only when the loop completed without hitting a `break`.

## `match` statements

`match` does structural pattern matching, like Python 3.10+. In `.dr`
mode each arm uses a brace body (the `.py` form uses the indented
`case …:` block):

```dragon
const value: str | int = parse(token)

match value {
    case 0 { print("zero") }
    case int() { print("a non-zero int") }
    case "" { print("empty string") }
    case str() { print("a non-empty string") }
}
```

Supported patterns today: **literals** (`0`, `"hi"`, `True`, `None`),
**type tests** (`case int()`, `case str()`, `case MyClass()` - a type
test matches by runtime tag for a `Union`/`Any` subject and by a
non-null check for a `Class | None`, and an instance of a subclass
matches its base), **OR-patterns** (`case int() | bool()`), **sequence**
patterns (`case [a, b]` / `case (a, b)`), **capture** (`case n`, binds),
and **wildcard** (`case _`).

A type test does not bind or narrow the subject - use it to dispatch,
then read the value in the body. Class *field* destructuring
(`case Point(x, y)`) and dict/mapping patterns are not implemented yet
and report a clear error.

## Range and enumerate

`range(stop)` and `range(start, stop[, step])` produce numeric
iterators:

```dragon
for i in range(5) {
    print(i)              # 0, 1, 2, 3, 4
}

for i in range(10, 0, -1) {
    print(i)              # 10, 9, ..., 1
}
```

`enumerate(iterable)` pairs items with their index:

```dragon
for i, name in enumerate(["Alice", "Bob"]) {
    print(f"{i}: {name}")
}
```

Both are the same as Python's builtins and behave identically.

## A worked example

A function that finds the first non-empty line in a list of lines and
returns its index, or `-1` if there isn't one:

```dragon
def first_nonempty(lines: list[str]) -> int {
    for i, line in enumerate(lines) {
        if len(line.strip()) > 0 {
            return i
        }
    }
    return -1
}
```

Try a few variations: rewrite it without `enumerate`. Rewrite it as a
`while` loop. Rewrite it using a list comprehension and `next()`. Each
form has its place; pick the one that reads best for your team.

You now have everything you need to write small, useful Dragon
programs. The next chapter turns to functions - defining them, passing
arguments and keyword arguments, returning values, and writing
higher-order functions.
