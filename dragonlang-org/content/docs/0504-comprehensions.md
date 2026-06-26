# Comprehensions

A comprehension builds a container from an iterable in a single expression. It's
the most Pythonic thing in the language and the idiom you'll reach for constantly
- and because Dragon's containers are monomorphized, a list comprehension that
produces a `list[int]` fills a flat `int64` array directly, with no boxing. The
ergonomics are Python's; the output is C-shaped.

There are three forms - list, dict, and set - and they all share one shape:
`[ expression for variable in iterable ]`, optionally with an `if` filter.

## List comprehensions

The basic form transforms each element of an iterable:

```dragon
const squares: list[int] = [n * n for n in [1, 2, 3, 4, 5]]
print(squares)              # [1, 4, 9, 16, 25]
```

Add an `if` clause to keep only the elements that pass a test:

```dragon
const evens: list[int] = [n for n in range(10) if n % 2 == 0]
print(evens)                # [0, 2, 4, 6, 8]
```

Transform and filter combine - the `if` decides what's kept, the expression
decides what's produced:

```dragon
const big_squares: list[int] = [n * n for n in range(10) if n * n > 25]
print(big_squares)          # [36, 49, 64, 81]
```

The loop variable can be any iterable's element, including strings, and the
expression can call methods on it:

```dragon
const upper: list[str] = [w.upper() for w in ["hello", "world"]]
print(upper)                # ['HELLO', 'WORLD']
```

### Nested iteration

Two `for` clauses flatten a nested structure - the leftmost loop is the outer
one, exactly as if you'd written nested `for` statements:

```dragon
const nested: list[list[int]] = [[1, 2], [3, 4], [5]]
const flat: list[int] = [x for row in nested for x in row]
print(flat)                 # [1, 2, 3, 4, 5]
```

## Dict comprehensions

Wrap a `key: value` pair in braces to build a dict. Both the key and the value
are expressions over the loop variable:

```dragon
const lengths: dict[str, int] = {w: len(w) for w in ["a", "bb", "ccc"]}
print(lengths)              # {'a': 1, 'bb': 2, 'ccc': 3}

const squares: dict[int, int] = {n: n * n for n in [1, 2, 3]}
print(squares)              # {1: 1, 2: 4, 3: 9}
```

The same `if` filter applies:

```dragon
const long_words: dict[str, int] = {w: len(w) for w in ["hi", "hello", "hey"] if len(w) > 2}
print(long_words)           # {'hello': 5, 'hey': 3}
```

## Set comprehensions

Braces with a single expression (no colon) build a set - duplicates collapse, so
this is a natural way to compute a set of distinct results:

```dragon
const remainders: set[int] = {n % 4 for n in [1, 2, 3, 4, 5, 6, 7, 8]}
print(len(remainders))      # 4 - {0, 1, 2, 3}
```

## A comprehension is an expression

Because a comprehension *is* an expression, it can go anywhere a value can - as a
function argument, inside an f-string, as a `return` value:

```dragon
const total: int = sum([n * n for n in range(5)])
print(total)                # 30
```

## At a glance

| Build a... | Write |
|------------|-------|
| List | `[expr for x in xs]` |
| Filtered list | `[expr for x in xs if cond]` |
| Flattened list | `[x for row in rows for x in row]` |
| Dict | `{k: v for x in xs}` |
| Filtered dict | `{k: v for x in xs if cond}` |
| Set | `{expr for x in xs}` |

That completes Part 5. The four built-in containers - list, dict, set, tuple -
plus comprehensions to build them, are the shapes nearly every function passes
around. Next, Dragon models your *own* data with
[Classes and Objects](/docs/0601-classes).
