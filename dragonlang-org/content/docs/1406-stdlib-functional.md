# Functional Tools

Some of the oldest ideas in programming are about *combining* small
pieces: fold a list down to a single value, search a sorted array in
logarithmic time, keep a priority queue honest, treat `+` as a value you
can pass around. This chapter covers the standard-library modules that
package those ideas:

- **`itertools`** - eager sequence builders: running sums, concatenation,
  slices, overlapping pairs.
- **`functools`** - higher-order helpers, chiefly `reduce`.
- **`operator`** - the arithmetic, comparison, and sequence operators as
  plain functions you can pass to `reduce`, `map`, or a `key=`.
- **`enum`** - named, type-checked constant sets (`Enum`, `IntEnum`,
  `StrEnum`), lowered at compile time to singletons that cost nothing.
- **`bisect`** - binary search and ordered insertion into a sorted
  `list[int]`.
- **`heapq`** - a min-heap (priority queue) over a `list[int]`.

One theme runs through almost all of it. Dragon has no generic functions
in these modules, and values flow at their native LLVM types with no
boxing. So where Python ships a single polymorphic
function, Dragon ships a *monomorphic* one - typically the `int` case,
with `float` and `str` variants under suffixed names where the operation
is ambiguous. That is the price of zero-overhead, statically-typed
functional code, and these modules pay it deliberately rather than
boxing everything into `Any`. Read each module's real signatures below;
they are not always the Python ones.

## `itertools` - eager sequence builders

Python's `itertools` returns lazy iterators. Dragon's returns eager
`list[int]` values (or `list[tuple[int, int]]` for `pairwise`). For
finite inputs that is the common case, and it keeps everything
unboxed. There is no `count`, `cycle`, or other infinite form - those
need a generic, lazy iterator that the language does not yet provide.

The exported functions are:

| Function | Signature | Result |
|---|---|---|
| `accumulate` | `(iterable: list[int]) -> list[int]` | running sums |
| `chain` | `(a: list[int], b: list[int]) -> list[int]` | concatenation |
| `repeat` | `(value: int, times: int) -> list[int]` | `value` × `times` |
| `islice` | `(iterable: list[int], start: int, stop: int) -> list[int]` | the half-open slice `[start, stop)` |
| `take` | `(iterable: list[int], n: int) -> list[int]` | first `n` elements |
| `pairwise` | `(iterable: list[int]) -> list[tuple[int, int]]` | overlapping pairs |

Because the results are ordinary lists, you consume them with a normal
`for` loop or index into them directly:

```dragon
import itertools

print(itertools.accumulate([1, 2, 3, 4]))       # [1, 3, 6, 10]
print(itertools.chain([1, 2], [3, 4]))           # [1, 2, 3, 4]
print(itertools.repeat(7, 3))                    # [7, 7, 7]
print(itertools.islice([10, 20, 30, 40, 50], 1, 4))  # [20, 30, 40]
print(itertools.take([5, 6, 7, 8], 2))           # [5, 6]
```

`pairwise` yields tuples, so a `for` loop with tuple unpacking reads
cleanly:

```dragon
from itertools import pairwise

for a, b in pairwise([1, 2, 3]) {
    print(f"{a} -> {b}")
}
# 1 -> 2
# 2 -> 3
```

**Differs from Python:** results are eager `list` values, not lazy
iterators; only the finite-input builders above exist (no `count`,
`cycle`, `product`, `permutations`, etc.); everything is `int`-typed.

## `functools` - reduce

The headline export is `reduce`, in three concrete forms. `cache` and
`lru_cache` are not in this module, and `partial`/`wraps` are
intentionally omitted - all three need first-class closures over
arbitrary signatures, which awaits a generic-functions language feature.
Shipping them as `Any`-boxed stubs would violate the no-workaround rule.

| Function | Signature | Notes |
|---|---|---|
| `reduce` | `(function: Callable[[int, int], int], iterable: list[int]) -> int` | uses the first element as the initial accumulator; raises `ValueError` on an empty list |
| `reduce_init` | `(function, iterable: list[int], initial: int) -> int` | the explicit-initializer (3-arg) form |
| `reduce_f` | `(function: Callable[[float, float], float], iterable: list[float]) -> float` | the `float` variant |

`reduce` folds a two-argument function across the list, left to right:

```dragon
from functools import reduce, reduce_init
from operator import add, mul

print(reduce(add, [1, 2, 3, 4, 5]))      # 15
print(reduce(mul, [1, 2, 3, 4]))         # 24
print(reduce_init(add, [1, 2, 3], 100))  # 106
```

The function you pass can be any `def` with the right signature - it does
not have to come from `operator`:

```dragon
from functools import reduce_f

def maxf(a: float, b: float) -> float {
    if a > b { return a }
    return b
}

print(reduce_f(maxf, [1.5, 3.25, 2.0]))  # 3.25
```

**Differs from Python:** the no-initializer and with-initializer forms
are separate functions (`reduce` vs `reduce_init`) rather than one
function with an optional argument; there is a distinct `float` variant;
there is no `lru_cache`, `cache`, `partial`, or `wraps`.

## `operator` - operators as functions

`operator` surfaces every operator as a callable so you can hand it to a
higher-order function. The arithmetic and comparison operators are
`int`-typed by default; where the operation is ambiguous, the `float`
and `str` versions live under a `_f` / `_s` suffix. Trailing-underscore
names (`not_`, `and_`, `or_`, `is_`, `pow_`, `abs_`) avoid clashing with
Dragon keywords and builtins.

A representative slice of the exports:

| Group | Functions |
|---|---|
| comparison (`int`) | `lt` `le` `eq` `ne` `gt` `ge` |
| comparison (`float`/`str`) | `lt_f` … `ge_f`, `lt_s` … `ge_s` |
| arithmetic (`int`) | `add` `sub` `mul` `floordiv` `truediv` `mod` `pow_` `neg` `pos` `abs_` |
| bitwise | `and_` `or_` `xor` `inv`/`invert` `lshift` `rshift` |
| arithmetic (`float`) | `add_f` `sub_f` `mul_f` `truediv_f` `neg_f` `abs_f` |
| logical | `not_` `truth` `truth_s` `is_` `is_not` |
| sequence | `concat` `contains` `contains_s` `countOf` `indexOf` `getitem` `setitem` |

The whole point is composability - these read best as the function
argument to `reduce`, `sorted`, or `map`:

```dragon
import operator
from functools import reduce

print(operator.add(3, 4))            # 7
print(operator.floordiv(17, 5))      # 3
print(operator.contains([1, 2, 3], 2))   # True
print(operator.countOf([1, 2, 2, 3, 2], 2))  # 3
print(operator.indexOf([10, 20, 30], 20))     # 1

print(reduce(operator.mul, [1, 2, 3, 4, 5]))  # 120
```

The `float` and `str` variants are exactly the same operators, just
typed for those operands:

```dragon
import operator

print(operator.add_f(1.5, 2.5))      # 4.0
print(operator.gt_f(3.0, 2.0))       # True
print(operator.concat("foo", "bar")) # foobar
```

Note `truediv` returns a `float` even on `int` inputs (Python `/`
semantics), while `floordiv` is the `//` operator. There is no
`itemgetter` / `attrgetter` factory - those return closures over a
captured key, which again awaits generics; use a small `def` or
`getitem` instead.

**Differs from Python:** every function is monomorphic - pick `add`,
`add_f`, or `concat` for the operand type rather than relying on one
overloaded function; `contains`/`countOf`/`indexOf` work on `list[int]`
(with `contains_s` for substrings); the closure factories `itemgetter`,
`attrgetter`, and `methodcaller` are absent.

## `enum` - named constant sets

`enum` gives you a closed set of named values that type-check as their
own type. `Enum`, `IntEnum`, and `StrEnum` are *compiler-recognized
marker base classes*: a class deriving one of them is rewritten at
compile time into singleton members carrying `.name` and `.value`. There
is no runtime metaclass and no reflection - the synthesis is pure
type-specialized codegen, so an enum costs nothing a hand-written class
of constants would. The import is mandatory (`from enum import Enum`);
these markers are not free builtins, mirroring `from threading import
Lock`.

A plain `Enum` member compares by identity, exposes `.name`/`.value`,
stringifies as `Type.MEMBER`, supports lookup by value via `Type(v)`, and
iterates in definition order. `auto()` fills in "previous value + 1":

```dragon
from enum import Enum, auto

class Color(Enum) {
    RED: int = 1
    GREEN: int = 2
    BLUE: int = auto()      # 3
}

print(Color.RED.value)      # 1
print(Color.RED.name)       # RED
print(str(Color.RED))       # Color.RED
print(Color.BLUE.value)     # 3
print(Color(2).name)        # GREEN  (lookup by value)

for c in Color {
    print(c.name)
}
# RED
# GREEN
# BLUE
```

`IntEnum` makes a member *equal to and ordered against* its integer
value, so you can compare members directly:

```dragon
from enum import IntEnum

class Priority(IntEnum) {
    LOW: int = 1
    HIGH: int = 10
}

print(Priority.HIGH > Priority.LOW)   # True
print(Priority.HIGH.value + 5)        # 15
```

Use `.value` when you need the member *in* an arithmetic expression
(`Priority.HIGH.value + 5`); a member is comparable as an integer but is
not itself an `int` operand for `+`/`-`.

`StrEnum` does the same against the member's string value:

```dragon
from enum import StrEnum

class Suit(StrEnum) {
    HEART: str = "heart"
    SPADE: str = "spade"
}

print(Suit.HEART.value)        # heart
print(Suit.HEART == "heart")   # True
```

For a hot path where you only need fast integer constants and none of the
`.name`/`.value`/iteration machinery, Dragon also has a native `enum`
*keyword* whose members are plain `int` constants starting at zero:

```dragon
enum Token { LPAREN, RPAREN, COMMA }

t: int = Token.LPAREN
print(t == Token.LPAREN)   # True
print(Token.COMMA)         # 2
```

**Differs from Python:** the class-based form needs explicit type
annotations on members (`RED: int = 1`); an `IntEnum` member is
comparable as an integer but you reach for `.value` to use it as an
arithmetic operand; there is also the keyword `enum Name { ... }` form
(int constants, no metadata) with no Python analog.

## `bisect` - binary search on a sorted list

`bisect` maintains a sorted `list[int]` with logarithmic-time search and
insertion. All functions assume the list is already sorted ascending.

| Function | Signature | Returns |
|---|---|---|
| `bisect_left` | `(a: list[int], x: int) -> int` | leftmost insertion point (`a[:i] < x`) |
| `bisect_right` / `bisect` | `(a: list[int], x: int) -> int` | rightmost insertion point (`a[:i] <= x`) |
| `insort_left` | `(a: list[int], x: int)` | insert `x`, left of equals |
| `insort_right` / `insort` | `(a: list[int], x: int)` | insert `x`, right of equals |
| `index` | `(a: list[int], x: int) -> int` | index of `x`, or `ValueError` |
| `contains` | `(a: list[int], x: int) -> bool` | membership test |

```dragon
import bisect

data: list[int] = [10, 20, 30, 40, 50]
print(bisect.bisect_left(data, 30))   # 2
print(bisect.bisect_right(data, 30))  # 3
print(bisect.bisect(data, 35))        # 3  (insertion point)

scores: list[int] = [1, 4, 9, 16]
bisect.insort(scores, 7)
print(scores)                         # [1, 4, 7, 9, 16]

print(bisect.contains(data, 40))      # True
print(bisect.index(data, 40))         # 3
```

The `insort*` functions mutate the list in place. `index` and `contains`
are the binary-search counterparts to the linear `list.index` / `in`,
and are only correct on a sorted list.

**Differs from Python:** the list is fixed to `list[int]` - there is no
`key=` callback and no support for `float`/`str`/object elements (those
need generics); `contains` is provided as a named function rather than
relying on the `in` operator (which would be linear).

## `heapq` - a min-heap

`heapq` turns a `list[int]` into a binary min-heap: the smallest element
is always at index `0`, and pushes and pops are logarithmic. As with
`bisect`, the element type is fixed to `int`.

| Function | Signature | Notes |
|---|---|---|
| `heappush` | `(heap: list[int], item: int)` | push, keep invariant |
| `heappop` | `(heap: list[int]) -> int` | pop the smallest; `ValueError` if empty |
| `heapify` | `(items: list[int])` | make a list a heap in place, `O(n)` |
| `heappushpop` | `(heap: list[int], item: int) -> int` | push then pop, in one step |
| `heapreplace` | `(heap: list[int], item: int) -> int` | pop then push (size unchanged) |
| `nsmallest` | `(n: int, items: list[int]) -> list[int]` | `n` smallest, ascending |
| `nlargest` | `(n: int, items: list[int]) -> list[int]` | `n` largest, descending |

Build a heap with repeated `heappush`, or `heapify` a list you already
have, then drain it smallest-first:

```dragon
import heapq

h: list[int] = []
heapq.heappush(h, 5)
heapq.heappush(h, 1)
heapq.heappush(h, 9)
heapq.heappush(h, 3)
print(heapq.heappop(h))   # 1
print(heapq.heappop(h))   # 3

nums: list[int] = [9, 2, 7, 1, 8, 3]
heapq.heapify(nums)
print(heapq.heappop(nums))   # 1
```

`nsmallest` and `nlargest` answer the common "top-k" question without
sorting the whole list:

```dragon
import heapq

print(heapq.nsmallest(3, [9, 2, 7, 1, 8, 3]))  # [1, 2, 3]
print(heapq.nlargest(2, [9, 2, 7, 1, 8, 3]))   # [9, 8]
```

**Differs from Python:** `list[int]` only - no tuple-priority entries,
no `key=` argument, and no support for other element types (all of which
require generics in Dragon).

## At a glance

| Module | Import | Key names | Operand type | Differs from Python |
|---|---|---|---|---|
| `itertools` | `import itertools` | `accumulate` `chain` `repeat` `islice` `take` `pairwise` | `list[int]` | eager lists, not lazy iterators; no `count`/`cycle`/`product` |
| `functools` | `from functools import reduce` | `reduce` `reduce_init` `reduce_f` | `int` / `float` | separate init / float variants; no `lru_cache`/`partial`/`wraps` |
| `operator` | `import operator` | `add` `mul` `lt` `contains` `concat` (+ `_f`/`_s`) | `int` / `float` / `str` | monomorphic per type; no `itemgetter`/`attrgetter` |
| `enum` | `from enum import Enum, IntEnum, StrEnum, auto` | `Enum` `IntEnum` `StrEnum` `auto` | class members | annotated members; `IntEnum` uses `.value` for arithmetic; native `enum` keyword too |
| `bisect` | `import bisect` | `bisect_left` `bisect_right` `insort` `index` `contains` | `list[int]` | `int` only; no `key=` |
| `heapq` | `import heapq` | `heappush` `heappop` `heapify` `nsmallest` `nlargest` | `list[int]` | `int` only; no tuple priorities or `key=` |

These modules trade Python's runtime polymorphism for code that compiles
to exactly the integer arithmetic and pointer moves you wrote, with no
boxing in the hot loop. When the workload turns to securing data rather
than shaping it, the next chapter covers
[Cryptography and Hashing](/docs/1407-stdlib-crypto).
