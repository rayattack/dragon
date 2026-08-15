# Sets and Tuples

Two more containers round out the built-in set, and they sit at opposite ends of
the design space. A **set** is an unordered collection of unique values, built
for one question - *is this in here?* - answered fast. A **tuple** is a
fixed-size, heterogeneous group whose length and element types are part of its
type. This chapter also introduces the **deque**, a double-ended queue for the
cases where you push and pop at both ends.

## Sets

A set is written with braces and holds each value at most once - duplicates
collapse on the way in:

```dragon
const tags: set[str] = {"red", "green", "red"}
print(len(tags))            # 2 - the duplicate "red" is dropped
```

The empty braces `{}` are a *dict*, not a set, so the empty set is `set()`. That
same constructor turns a list into a set, which is the one-liner for deduping:

```dragon
empty: set[int] = set()
const unique: set[int] = set([1, 1, 2, 3, 3])
print(len(unique))          # 3
```

### Membership and mutation

Membership is the set's whole reason for being - reach for a set over a list
whenever you only care *whether* a value is present:

```dragon
seen: set[str] = set()
seen.add("ada")
seen.add("ada")             # no-op - already present
print("ada" in seen)        # True
print(len(seen))            # 1
seen.discard("ada")         # remove if present; no error if absent
print(len(seen))            # 0
```

### Set algebra

Union, intersection, difference, and symmetric difference are available both as
methods and as operators - use whichever reads better:

```dragon
const a: set[int] = {1, 2, 3}
const b: set[int] = {2, 3, 4}

print(a.union(b))                 # {1, 2, 3, 4}
print(a.intersection(b))          # {2, 3}
print(a.difference(b))            # {1}
print(a.symmetric_difference(b))  # {1, 4}

print(a | b)                      # {1, 2, 3, 4}  - union operator
print(a & b)                      # {2, 3}        - intersection
print(a - b)                      # {1}           - difference
```

### Subset and superset

The comparison operators test containment - `<` and `>` are *proper* subset and
superset, `<=` and `>=` allow equality - and the `issubset`/`issuperset` methods
say the same thing in words:

```dragon
const sub: set[int] = {1, 2}
const sup: set[int] = {1, 2, 3}
print(sub.issubset(sup))    # True
print(sub < sup)            # True  - proper subset
print(sup >= sup)           # True  - superset-or-equal
```

A set comprehension builds one in a single expression - see
[Comprehensions](/docs/0504-comprehensions).

## Tuples

A tuple is a **fixed-size, heterogeneous** group: the element types may differ,
and the length is part of the type. It's written with parentheses, typed
`tuple[...]`:

```dragon
const point: tuple[int, int] = (3, 4)
const record: tuple[int, str, float] = (1, "Ada", 9.5)
```

Index like a list, and **unpack** several values at once - the same move that
catches a function's multiple return values:

```dragon
const point: tuple[int, int] = (3, 4)
print(point[0])             # 3

x, y = point
print(f"{x},{y}")           # 3,4

const record: tuple[int, str, float] = (1, "Ada", 9.5)
n, name, score = record
print(f"{n} {name} {score}")   # 1 Ada 9.5
```

Reach for a tuple when the shape is fixed and each position *means* something - a
coordinate, a record, a `(key, value)` pair - and a list when the length grows
and the elements are uniform. Iterating `d.items()` is iterating tuples under the
hood, which is why `for k, v in d.items()` unpacks so naturally.

## Deque - a double-ended queue

When you need to push and pop at *both* ends in constant time - a work queue, a
sliding window, a breadth-first frontier - import a `deque` from `collections`:

```dragon
from collections import deque

dq: deque[int] = deque([1, 2, 3])
dq.appendleft(0)            # add to the front
dq.append(4)                # add to the back
print(len(dq))              # 5

const first: int = dq.popleft()    # remove from the front (FIFO)
const last: int = dq.pop()         # remove from the back  (LIFO)
print(first, last)          # 0 4
```

Used as a queue, you push with `append` and drain with `popleft`:

```dragon
from collections import deque

work: deque[str] = deque(["task1", "task2", "task3"])
while len(work) > 0 {
    const job: str = work.popleft()
    print(f"processing {job}")
}
```

```text
processing task1
processing task2
processing task3
```

## At a glance

| You want to... | Write |
|----------------|-------|
| A set / empty set | `s: set[int] = {1, 2}` / `set()` |
| Dedupe a list | `set(xs)` |
| Add / remove / test | `s.add(v)` / `s.discard(v)` / `v in s` |
| Algebra | `a.union(b)` `a.intersection(b)` `a.difference(b)`, or `a \| b` `a & b` `a - b` |
| Subset / superset | `a.issubset(b)`, `a < b`, `a >= b` |
| A tuple | `t: tuple[int, str] = (1, "a")` |
| Unpack | `x, y = t` |
| Double-ended queue | `from collections import deque` → `appendleft`/`append`/`popleft`/`pop` |

Next, the one expression form that builds all three of these from an iterable:
[Comprehensions](/docs/0504-comprehensions).
