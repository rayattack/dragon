# Type Annotations

Most "gradually typed" languages treat annotations as decoration - a hint the
runtime ignores, a linter's suggestion you can silence. Dragon takes the opposite
view. In Dragon **types are mandatory and they are the program** - in both `.dr`
and `.py` source, no exceptions. Every annotation you write becomes a concrete
LLVM type the compiler commits to: `int` is an `i64` in a register, `float` is an
`f64` in an xmm register, `list[int]` is a contiguous `int64_t[]`. There is no
boxing, no tag, no dictionary of attributes hiding behind a value. The annotation
isn't advice - it's the storage layout.

That single decision is why Dragon hits C speed. A `list[float]` is a real
`double[]` the vectorizer can see through, not a list of pointers to heap floats.
The type checker isn't a safety net bolted on top; it's the thing that lets the
code generator emit fast machine code at all.

This part goes past the [data types](/docs/0202-data-types) tour into the
annotation *system*. This chapter covers the foundation - the core annotations,
the declaration rule, type aliases, and how annotations become storage. The
chapters that follow take it further:
[Union, Optional, and Narrowing](/docs/0702-union-and-narrowing),
[Any](/docs/0703-any), [Callable, TypedDict, and intc](/docs/0704-callable-and-typeddict),
and [Generics](/docs/0705-generics).

## The core types, as annotations

You've met these already. As annotations they are just names (and, for containers,
names with bracketed parameters):

```dragon
count:   int              = 42
ratio:   float            = 3.14
ok:      bool             = True
label:   str              = "Dragon"
ints:    list[int]        = [1, 2, 3]
prices:  dict[str, float] = {"pen": 1.5}
pair:    tuple[int, str]  = (1, "x")
tags:    set[str]         = {"a", "b", "c"}
```

Each compiles to a different native representation. The annotation on the left
fully determines the machine layout on the right: `list[int]` and `list[float]`
are *not* the same type with a different label - they are different storage,
different runtime entry points, different code.

## `:` declares, `=` reassigns

This is the rule that catches more typos than any other, and it is identical in
`.dr` and `.py`. A name enters a scope exactly once, through a **declaration**
with a colon and a type; a bare `=` is *always* a reassignment of a name that
already exists:

```dragon
total: int = 0      # declares `total`, type fixed as int
total = total + 5   # reassigns - `total` already exists
```

If the name isn't already in scope, a bare `=` is an error, not a new variable:

```text
error: 'result' is not declared; introduce it with
       'result: <type> = ...' (bare '=' only reassigns an existing variable)
```

In Python, a misspelled `reuslt = compute()` silently creates a new variable and
the bug surfaces a hundred lines later. In Dragon it's a compile error at the typo.

### Type is fixed at declaration

Once `total: int` is declared, `total` is an `int` for the rest of that scope.
Assigning an incompatible type is rejected - `x: int = 5` then `x = "hello"` is a
compile error. This isn't pedantry: `x` is an `i64` slot in a register, with no
room for a string pointer, and the compiler will not silently re-type it behind
your back. If you genuinely need a slot that holds either, you declare a
[union](/docs/0702-union-and-narrowing) and pay for it explicitly. You also can't
declare the same name twice in one scope (`x: int` twice is a redeclaration error
- reassign with `x = ...`). Sibling `{ }` blocks reusing a name are *not*
redeclarations; each block is its own scope.

## Type aliases

When a type expression is long or carries domain meaning, give it a name with
`type`. An alias is purely a compile-time synonym - it has no runtime cost and no
new representation; it *is* the type it names:

```dragon
type UserId = int

uid: UserId = 42
print(uid)            # 42
```

`UserId` and `int` are interchangeable; the alias just makes intent legible at the
declaration and the signature. Aliases shine for the longer composite types you'll
meet in the next chapters - a `type Handler = Callable[[Request], Response]` reads
far better than spelling the callable out at every use.

## Annotations drive monomorphized storage

This is the payoff for mandatory types, made concrete. Three lists, three
annotations, three completely different memory layouts:

```dragon
ints:   list[int]   = [1, 2, 3]      # int64_t[]  - packed integers
floats: list[float] = [1.5, 2.5]     # double[]   - packed doubles
strs:   list[str]   = ["a", "b"]     # void*[]    - refcounted pointers
```

`list[int]` stores raw `i64`s; `list[float]` stores raw `double`s, so the loop
vectorizes; `list[str]` stores refcount-aware pointers. The code generator picks
the storage and the runtime entry points from the element type in your annotation.
Nothing is boxed; nothing carries a per-element tag. Dicts work the same way -
`dict[str, int]` keeps `i64` values inline, and only `dict[str, Any]` boxes,
because only then is the value type unknown. You tell the truth about your data,
and the truth compiles to fast code.

## `.py` mode: the same rules, looser surface

The `.py` adoption mode relaxes *syntax*, not *semantics*. It is not "optional
typing." A `.py` file still requires typed function signatures, typed module-level
variables (`count: int = 0`, not `count = 0`), and `:`-to-declare / `=`-to-reassign
- a bare `=` on a new name is still an error, in a function body or at module
level:

```python
def compute(a: int, b: int) -> int:
    total: int = a + b      # declares - needs the annotation
    total = total * 2       # reassigns - bare = is fine
    return total

count: int = 0              # module global - annotation required
```

What `.py` mode buys you is the Python *surface* - indentation, `def __init__(self)`,
`global x` - so you can compile an already-typed Python file as-is. The annotation
discipline is the same language underneath; only the punctuation changes.

## At a glance

| You want to express... | Annotation |
|------------------------|------------|
| 64-bit integer / IEEE-754 double | `int` / `float` |
| Boolean / Unicode text / raw bytes | `bool` / `str` / `bytes` |
| Homogeneous list | `list[int]`, `list[str]`, `list[float]` |
| Hash map / record / set | `dict[str, int]` / `tuple[int, str]` / `set[str]` |
| A named synonym | `type UserId = int` |
| Declare a variable | `x: T = value` |
| Reassign one | `x = value` |

The type you write is the layout you get. Next: the types that hold *more than
one* possibility - [Union, Optional, and Narrowing](/docs/0702-union-and-narrowing).
