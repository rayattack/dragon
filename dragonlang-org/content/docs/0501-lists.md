# Lists

A list is an ordered, growable sequence - the workhorse container of almost every
program. Dragon's lists have the methods and syntax you know from Python, but
with one structural advantage you never have to think about and always benefit
from: they are **monomorphized**.

A `list[int]` is a flat `int64` array. A `list[float]` is a flat array of
`double`s. A `list[bool]` packs one byte per element. There is no per-element
boxing, no tag word on every slot, no pointer-chase to read element seventeen.
The compiler picks the storage from the element type in the annotation, so a list
of a million integers is a million contiguous 8-byte words - the same memory a C
programmer would lay out by hand. You write Python; you get C's data layout.

## Creating and typing

A list is square brackets, typed `list[T]`:

```dragon
const primes: list[int] = [2, 3, 5, 7, 11]
const names: list[str] = ["Ada", "Linus", "Grace"]
const empty: list[int] = []
```

Monomorphization is also why the **element type must be knowable**. A non-empty
literal infers it - `xs := [1, 2, 3]` is a `list[int]` - but an empty literal has
nothing to infer from, and a mixed one has no single element type, so both are a
compile error when left to inference:

```dragon
# doc: no-check
xs := [1, 2, 3]        # ok - list[int]
ys := []               # error: cannot infer the element type - annotate it
zs := [1, "a"]         # error: mixed literal has no single element type
```

The fix is an annotation, never a silent fallback: `ys: list[int] = []`. An empty
literal is fine the moment its type is given, and genuinely heterogeneous data is
written `list[Any] = [1, "a", 3.0]` - you opt into the box explicitly. This is the
honest-types rule from [Any](/docs/0703-any): ambiguity is something you annotate
away at compile time, not a box the compiler picks for you behind your back (which
would quietly cost you the C data layout this whole page is about).

## One list, two layouts

`list[str]` and `list[Any]` are not the same container with different labels -
they have different memory layouts. A concrete list stores raw values at 8
bytes per element; a `list[Any]` stores a 16-byte `{tag, value}` box per
element so every slot can carry its own type. That split is what keeps the
concrete tier C-fast, and it has one visible consequence: **a list value never
changes layout by flowing through an annotation.**

A fresh literal is fine - the compiler sees it being born and builds the
layout the annotation asks for:

```dragon
xs: list[Any] = ["a", "b"]     # built as a box list from the start
```

But a *named* concrete list cannot be passed off as `list[Any]`, and a
`list[Any]` cannot be passed off as a concrete list - reading one layout at
the other's stride would corrupt memory, so the compiler stops the value at
the boundary:

```dragon
# doc: no-check
names: list[str] = ["a", "b"]
xs: list[Any] = names
# error: cannot assign 'list[str]' to variable of type 'list[Any]' (the two
# have different element layouts: monomorphized vs boxed; build the value
# with this element type at its declaration, or copy it element-wise)
```

The same rule guards the boundary the compiler cannot see. An `Any` box can
hold either kind of list, so unboxing one into a typed list view checks the
value's actual layout at runtime and raises `TypeError` on a mismatch rather
than misreading. Parsed JSON arrays are box lists, so reading them as
`list[Any]` and narrowing per element is the natural path:

```dragon
from json import decode

const doc: dict[str, Any] = decode[dict[str, Any]]('{"tags": ["a", "b"]}'.encode("utf-8"))
const tags: list[Any] = doc["tags"]    # parsed arrays are box lists - ok
for t in tags {
    const s: str = t                   # narrow each element
    print(s)                           # a, then b
}
```

When you genuinely need to cross layouts, copy element-wise - the loop above
is exactly that shape. And note the rule pins the *typed view* `list[Any]`,
not dynamism itself: a bare `Any` accepts any list and every operation on it
dispatches on the value's real layout at runtime. You can `len()` an `Any`
and iterate it directly - each element arrives as an `Any` to narrow - and
that works for **both** layouts, which makes it the right shape for code
that must accept any list at all (the stdlib JSON Schema validator walks
payloads exactly this way):

```dragon
def count_strings(v: Any) -> int {
    n: int = 0
    for x in v {                  # dispatches per element, either layout
        if isinstance(x, str) {
            n = n + 1
        }
    }
    return n
}

names: list[str] = ["a", "b"]     # monomorphized
print(count_strings(names))       # 2
import json
print(count_strings(json.loads('["c", 1]')))   # 1 - a parsed box list
```

## Indexing and slicing

Index from the front with `0`-based positions and from the back with negatives;
slice with `[start:stop]` (stop exclusive):

```dragon
const primes: list[int] = [2, 3, 5, 7, 11]
print(primes[0])     # 2
print(primes[-1])    # 11
print(primes[1:4])   # [3, 5, 7]
print(len(primes))   # 5
```

## Growing and shrinking

Lists are mutable. `append` adds to the end, `insert` puts an element at a
position, `pop` removes and returns the last element, and `remove` deletes the
first matching value:

```dragon
nums: list[int] = [10, 20, 30]
nums.append(40)        # [10, 20, 30, 40]
nums.insert(0, 5)      # [5, 10, 20, 30, 40]
const last: int = nums.pop()   # returns 40
nums.remove(5)         # [10, 20, 30]
print(nums)            # [10, 20, 30]
```

## Combining

`extend` appends every element of another list in place; `+` builds a new list
from two:

```dragon
xs: list[int] = [1, 2, 3]
xs.extend([4, 5])
print(xs)                          # [1, 2, 3, 4, 5]

const combined: list[int] = [1, 2] + [3, 4]
print(combined)                    # [1, 2, 3, 4]
```

## Searching

Membership and search work by **value**, not identity - a freshly built string
finds an equal element:

```dragon
const words: list[str] = ["apple", "pear", "fig"]
print("pear" in words)       # True
print("kiwi" not in words)   # True
print(words.count("pear"))   # 1
print(words.index("fig"))    # 2
```

`if x in items { ... }` is the idiomatic membership check, and it reads
character-for-character like the Python you know.

## Sorting, reversing, copying

`sort` and `reverse` work in place; `copy` makes an independent shallow copy:

```dragon
xs: list[int] = [3, 1, 2]
xs.sort()
print(xs)        # [1, 2, 3]
xs.reverse()
print(xs)        # [3, 2, 1]

const ys: list[int] = xs.copy()
ys.append(99)
print(xs)        # [3, 2, 1] - unchanged; ys is independent
```

`sort` is content-aware: a `list[str]` sorts alphabetically, a `list[float]`
numerically - not by raw machine representation.

## Repetition

`[v] * n` builds a list of `n` copies - a quick way to pre-fill:

```dragon
const zeros: list[int] = [0] * 4
print(zeros)     # [0, 0, 0, 0]
```

## Nesting

Lists nest, and the element type is checked all the way down:

```dragon
const grid: list[list[int]] = [[1, 2], [3, 4], [5, 6]]
print(grid[1][0])   # 3
print(grid)         # [[1, 2], [3, 4], [5, 6]]
```

## Other element types print as themselves

`list[float]` and `list[bool]` render to their familiar form:

```dragon
const floats: list[float] = [1.5, 2.5, 3.5]
print(floats)    # [1.5, 2.5, 3.5]
const flags: list[bool] = [True, False, True]
print(flags)     # [True, False, True]
```

## At a glance

| You want to... | Write |
|----------------|-------|
| A list | `xs: list[int] = [1, 2, 3]` |
| Index / slice | `xs[0]`, `xs[-1]`, `xs[1:4]` |
| Append / insert | `xs.append(v)` / `xs.insert(i, v)` |
| Remove | `xs.pop()` / `xs.remove(v)` |
| Combine | `a.extend(b)` / `a + b` |
| Membership / find | `v in xs` / `xs.index(v)` / `xs.count(v)` |
| Sort / reverse / copy | `xs.sort()` / `xs.reverse()` / `xs.copy()` |
| Pre-fill | `[0] * n` |

The compact way to *build* a list from another iterable is the comprehension,
covered in [Comprehensions](/docs/0504-comprehensions). Next, the key-value
companion to the list: [Dictionaries](/docs/0502-dictionaries).
