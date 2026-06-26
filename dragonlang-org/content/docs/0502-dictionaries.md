# Dictionaries

A dictionary maps keys to values - the second indispensable container after the
list. Dragon's dicts preserve insertion order (like Python 3.7+), support the
familiar `d[key]` access and `.get`/`.items`/`.keys`/`.values` methods, and add a
dot-access convenience for string keys. They also make one deliberate choice
Python doesn't: a dict is **monomorphic in its key type**, which is what lets the
lookup compile down to a tight, type-specialized probe.

## Creating and typing

A dict is braces, typed `dict[K, V]`:

```dragon
const ages: dict[str, int] = {"Ada": 36, "Grace": 85}
const empty: dict[str, int] = {}
```

## Reading values

There are three ways to read. The bracket form works for any key; **dot-access**
is a Dragon convenience for string keys that are valid identifiers; and `get`
returns a default instead of failing on a missing key:

```dragon
const ages: dict[str, int] = {"Ada": 36, "Grace": 85}
print(ages["Ada"])          # 36  - bracket access
print(ages.Ada)             # 36  - dot-access (string keys)
print(ages.get("Bob", 0))   # 0   - default for a missing key
```

Inside an f-string, remember the quote rule from
[F-strings](/docs/0402-formatting): use a single-quoted key or dot-access, since
a double quote would close the string - `f"{ages['Ada']}"` or `f"{ages.Ada}"`.

## Adding, updating, membership

Assigning to a key adds or updates it; `in` tests for a key; `len` counts pairs:

```dragon
ages: dict[str, int] = {"Ada": 36}
ages["Grace"] = 85          # add
ages["Ada"] = 37            # update
print("Ada" in ages)        # True
print(len(ages))            # 2
print(ages)                 # {'Ada': 37, 'Grace': 85}
```

## Removing

`del` drops a key; `pop` removes it and returns its value (with an optional
default); `popitem` removes and returns the last-inserted pair:

```dragon
d: dict[str, int] = {"x": 1, "y": 2, "z": 3}
del d["y"]
print(len(d))               # 2
print("y" in d)             # False
print(d.pop("x"))           # 1
print(d.pop("missing", -1)) # -1  - default for absent key
```

## Get-or-insert with `setdefault`

`setdefault` reads a key, inserting a default when it is missing - the standard
Python idiom for accumulating into a dict without a separate "is it there yet?"
check. If the key exists it returns the stored value untouched; if it does not,
it inserts the default **and returns it**:

```dragon
counts: dict[str, int] = {}
for word in ["a", "b", "a"] {
    counts[word] = counts.setdefault(word, 0) + 1
}
print(counts)               # {'a': 2, 'b': 1}
```

For a heap value the inserted default and the returned value are the **same
object**, exactly as in Python - so you can insert-and-mutate in one step. This
is the canonical "group into buckets" pattern:

```dragon
groups: dict[str, list[int]] = {}
for n in [1, 2, 3, 4] {
    const parity: str = "even" if n % 2 == 0 else "odd"
    groups.setdefault(parity, []).append(n)   # bucket list created on first hit
}
print(groups)               # {'odd': [1, 3], 'even': [2, 4]}
```

The default is **required** in Dragon. Python's one-argument `d.setdefault(k)`
inserts `None`; a Dragon `dict[str, int]` cannot hold `None` (its value type is
`int`, not `int | None`), so the one-argument form inserts the value type's
zero-value (`0` for `int`, `""` for `str`, `[]` for a list) rather than smuggling
in a dynamic `None`. When you want absence to be representable, type the value as
`dict[str, int | None]` and pass the default explicitly. This is the same honest-
types stance described in [Any](/docs/0703-any): the surface mirrors Python, the
type system stays truthful.

## Merging and copying

`update` merges another dict in place (overwriting collisions); `copy` returns an
independent shallow copy:

```dragon
config: dict[str, int] = {"timeout": 30}
config.update({"timeout": 60, "retries": 3})
print(config)               # {'timeout': 60, 'retries': 3}
```

## Iterating

Iterating a dict yields its **keys**, in insertion order. `.items()` yields
key/value pairs you can unpack; `.keys()` and `.values()` return lists:

```dragon
const scores: dict[str, int] = {"a": 1, "b": 2, "c": 3}
for k in scores {
    print(k, scores[k])
}
for k, v in scores.items() {
    print(f"{k} = {v}")
}
const keys: list[str] = scores.keys()
const vals: list[int] = scores.values()
print(keys)                 # ['a', 'b', 'c']
print(vals)                 # [1, 2, 3]
```

## One key type per dict

This is the deliberate divergence from Python. A Dragon dict is monomorphic in
its key type: a `dict[str, V]` holds string keys, a `dict[int, V]` holds integer
keys, and you cannot mix the two - there is no dict that holds both `1` and
`"1"`. The payoff is that the compiler emits a key-type-specialized hash and
probe (integer keys hash and compare as native `i64`, never boxed), so lookups
are as fast as the data allows. A `dict[int, V]` works exactly like the `dict[str,
V]` shown here - same methods, same iteration - just with integer keys.

## Comprehensions

A dict comprehension builds a mapping in one expression:

```dragon
const lengths: dict[str, int] = {w: len(w) for w in ["a", "bb", "ccc"]}
print(lengths)              # {'a': 1, 'bb': 2, 'ccc': 3}
const squares: dict[int, int] = {n: n * n for n in [1, 2, 3]}
print(squares)              # {1: 1, 2: 4, 3: 9}
```

The full comprehension story - list, dict, and set - is in
[Comprehensions](/docs/0504-comprehensions).

## At a glance

| You want to... | Write |
|----------------|-------|
| A dict | `d: dict[str, int] = {"a": 1}` |
| Read a value | `d["a"]`, `d.a`, `d.get("a", 0)` |
| Add / update | `d["b"] = 2` |
| Get or insert a default | `d.setdefault("a", 0)` |
| Membership / size | `"a" in d`, `len(d)` |
| Remove | `del d["a"]`, `d.pop("a")`, `d.popitem()` |
| Merge | `d.update(other)` |
| Iterate keys | `for k in d { ... }` |
| Iterate pairs | `for k, v in d.items() { ... }` |
| Keys / values | `d.keys()`, `d.values()` |

Next, the two remaining built-in containers - [Sets and Tuples](/docs/0503-sets-and-tuples).
