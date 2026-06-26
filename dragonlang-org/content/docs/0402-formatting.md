# F-strings and the Format Mini-Language

Building a string out of values is the single most common thing text code does,
and Dragon's answer is the f-string - the same one Python 3.6 introduced, and
the form you should reach for by default. Prefix a string with `f` and any
`{expression}` inside it is evaluated and interpolated in place. There is no
`+`-concatenation ceremony, no `str()` wrapping, no positional `%` placeholders
to keep in sync.

```dragon
const w: str = "world"
print(f"hello {w}")        # hello world
print(f"sum is {2 + 3}")   # sum is 5
```

Anything that's an expression can go in the braces - arithmetic, a method call,
an attribute access:

```dragon
const name: str = "Dragon"
print(f"{name.upper()} has {len(name)} letters")   # DRAGON has 6 letters
```

## The quote rule

A `{...}` slot can hold a string literal, but it has to use the *other* quote
style, or it will close the f-string early. Inside a double-quoted f-string, use
single-quoted dict keys - or dot-access, which sidesteps the issue entirely:

```dragon
const ages: dict[str, int] = {"Ada": 36}
print(f"{ages['Ada']}")   # 36   - single-quoted key inside a double-quoted f-string
print(f"{ages.Ada}")      # 36   - or dot-access for identifier keys
```

## The format mini-language

After a `:` inside the braces comes a **format specifier** that controls width,
alignment, sign, base, grouping, and precision. Dragon implements Python's full
mini-language. The grammar is:

```text
{value:[[fill]align][sign][#][0][width][grouping][.precision][type]}
```

Every piece is optional; you use only the parts you need.

### Width and alignment

A bare number sets the minimum field width. Numbers right-align by default,
strings left-align. `<`, `>`, and `^` force left, right, and center; a character
before the align symbol is the fill:

```dragon
const x: int = 42
print(f"[{x:<6}]")    # [42    ]
print(f"[{x:>6}]")    # [    42]
print(f"[{x:^6}]")    # [  42  ]
print(f"[{x:*^6}]")   # [**42**]   - '*' fill, centered

const s: str = "hi"
print(f"[{s:>10}]")   # [        hi]
print(f"[{s:^10}]")   # [    hi    ]
```

### Sign

`+` shows a sign on positive numbers too; a space reserves a column for it:

```dragon
const pi: float = 3.14159
print(f"{pi:+.2f}")   # +3.14
```

### Precision and float types

`.N` sets the number of fractional digits for `f`, or significant digits for `g`.
The type letter picks the presentation: `f` fixed-point, `e`/`E` scientific,
`g`/`G` the shortest of the two, `%` percentage (scaled by 100):

```dragon
const pi: float = 3.14159
print(f"{pi:.2f}")     # 3.14
print(f"{pi:10.2f}")   # "      3.14"   - width 10, right-aligned
print(f"{pi:010.2f}")  # 0000003.14     - zero-padded
print(f"{pi:.3g}")     # 3.14

const r: float = 0.25
print(f"{r:.1%}")      # 25.0%
```

### Integer types and grouping

For integers, the type letter sets the base - `d` decimal, `b` binary, `o`
octal, `x`/`X` hex - and `#` adds the `0x`/`0o`/`0b` prefix. `,` and `_` group
digits for readability:

```dragon
const n: int = 255
print(f"{n:05d}")    # 00255   - zero-pad to width 5
print(f"{n:08b}")    # 11111111
print(f"{n:x}")      # ff
print(f"{n:#x}")     # 0xff
print(f"{n:#o}")     # 0o377

const big: int = 1234567
print(f"{big:,}")    # 1,234,567
print(f"{big:_d}")   # 1_234_567
```

### Strings in the mini-language

A string value accepts fill, alignment, and width - the padding shown above - but
not numeric pieces like precision or sign, which have no meaning for text.

## At a glance

| Spec | Effect | Example → output |
|------|--------|------------------|
| `{x}` | interpolate | `f"{2+3}"` → `5` |
| `{x:.2f}` | 2 fractional digits | `3.14` |
| `{x:8.2f}` | width 8, right-aligned | `"    3.14"` |
| `{x:05d}` | zero-pad to width 5 | `00042` |
| `{x:,}` | thousands grouping | `1,234,567` |
| `{x:#x}` | hex with prefix | `0xff` |
| `{x:.1%}` | percentage | `25.0%` |
| `{s:^10}` | center in width 10 | `"    hi    "` |
| `{s:*>6}` | `*`-fill, right-align | `"****hi"` |

F-strings are the idiomatic way to format in Dragon - prefer them over manual
concatenation everywhere. Next, the raw byte view of text:
[Bytes and Encoding](/docs/0403-bytes).
