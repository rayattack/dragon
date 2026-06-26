# Operators and Symbols

This appendix is the authoritative reference for every operator and
symbol Dragon recognizes. Each group below is a GFM table of the form
**Operator | Meaning | Example**, and a single **precedence table** near
the end fixes how they bind. Where Dragon's behavior differs from the
Python you might expect - chiefly list `+` and the set algebra operators
- that is called out honestly with the working alternative.

Operators are grouped by category. The categories follow the source
order in which Dragon's parser climbs from loosest- to tightest-binding,
so reading top to bottom is also a tour of precedence from low to high.

## Arithmetic

Standard arithmetic on `int` and `float`. `/` is **true division** and
always yields a `float`; `//` is floor division and `%` is the
remainder. `**` is exponentiation and binds tighter than unary minus on
its left but looser on its right (`-2 ** 2` is `-(2 ** 2)`).

| Operator | Meaning | Example |
|---|---|---|
| `+` | addition | `7 + 3` is `10` |
| `-` | subtraction / unary negation | `7 - 3` is `4`; `-x` |
| `*` | multiplication | `7 * 3` is `21` |
| `/` | true division (always `float`) | `7 / 2` is `3.5` |
| `//` | floor division | `7 // 3` is `2` |
| `%` | remainder (modulo) | `7 % 3` is `1` |
| `**` | exponentiation | `7 ** 3` is `343` |

`+` and `*` are also defined on `str`: `"foo" + "bar"` is `"foobar"` and
`"ab" * 3` is `"ababab"`. List repetition with `*` works too -
`[0] * 3` is `[0, 0, 0]`. See [Strings](/docs/0401-strings) and
[Collections](/docs/0501-lists).

## Comparison

All six comparisons return a `bool`. Dragon supports **chained
comparisons** exactly as Python does: `1 < a < 10` is evaluated as
`1 < a and a < 10`, with `a` read once.

| Operator | Meaning | Example |
|---|---|---|
| `==` | equal | `5 == 8` is `False` |
| `!=` | not equal | `5 != 8` is `True` |
| `<` | less than | `5 < 8` is `True` |
| `>` | greater than | `5 > 8` is `False` |
| `<=` | less than or equal | `5 <= 5` is `True` |
| `>=` | greater than or equal | `8 >= 5` is `True` |

## Logical

Short-circuiting boolean operators. `not` is a prefix unary operator and
binds looser than comparison, so `not a == b` parses as
`not (a == b)`.

| Operator | Meaning | Example |
|---|---|---|
| `and` | logical and (short-circuit) | `True and False` is `False` |
| `or` | logical or (short-circuit) | `True or False` is `True` |
| `not` | logical negation | `not True` is `False` |

## Bitwise

Bitwise operators act on `int` operands. `~` is the prefix one's
complement; the rest are infix.

| Operator | Meaning | Example |
|---|---|---|
| `&` | bitwise and | `12 & 10` is `8` |
| `\|` | bitwise or | `12 \| 10` is `14` |
| `^` | bitwise xor | `12 ^ 10` is `6` |
| `~` | bitwise not (one's complement) | `~12` is `-13` |
| `<<` | left shift | `12 << 2` is `48` |
| `>>` | right shift | `12 >> 1` is `6` |

## Membership and identity

Membership (`in`, `not in`) tests by **value** against any container.
Identity (`is`, `is not`) is primarily used to test against `None`.

| Operator | Meaning | Example |
|---|---|---|
| `in` | member of a container | `2 in [1, 2, 3]` is `True` |
| `not in` | not a member | `5 not in [1, 2, 3]` is `True` |
| `is` | identity test | `n is None` |
| `is not` | negated identity | `n is not None` |

## Assignment and augmented assignment

`=` is **reassignment** - the name on the left must already be declared
in scope (`x: T = ...` introduces it; bare `x = v` updates it). Each
augmented form is `x op= y` shorthand for `x = x op y`, including the
float case `y /= 4.0`. See [Variables](/docs/0201-variables) for the
declare-with-`:` / assign-with-`=` rule.

| Operator | Meaning | Example |
|---|---|---|
| `=` | assign (to an already-declared name) | `x = 5` |
| `+=` | add and assign | `x += 5` |
| `-=` | subtract and assign | `x -= 2` |
| `*=` | multiply and assign | `x *= 3` |
| `/=` | true-divide and assign | `y /= 4.0` |
| `//=` | floor-divide and assign | `x //= 4` |
| `%=` | modulo and assign | `x %= 5` |
| `**=` | exponentiate and assign | `x **= 2` |
| `&=` | bitwise-and and assign | `x &= 6` |
| `\|=` | bitwise-or and assign | `x \|= 1` |
| `^=` | bitwise-xor and assign | `x ^= 3` |
| `<<=` | left-shift and assign | `x <<= 1` |
| `>>=` | right-shift and assign | `x >>= 1` |

## Structural operators and symbols

These are the operators that build expressions and bindings rather than
compute values.

| Operator | Meaning | Example |
|---|---|---|
| `:=` | walrus - assign within an expression | `if (n := len(xs)) > 3 { ... }` |
| `->` | return-type arrow in a `def` | `def add(x: int, y: int) -> int { ... }` |
| `x if c else y` | ternary conditional | `m: int = a if a > b else b` |
| `.` | attribute / method access | `user.name`, `xs.append(4)` |
| `[i]` | subscript (index) | `xs[1]`, `s[0]` |
| `[a:b]` | slice (stop exclusive) | `s[0:5]`, `xs[1:3]` |
| `[a:b:c]` | slice with step | `s[::2]` |

The walrus `:=` is one of Dragon's **implicit binding forms**: it
declares `n` with an inferred type, so it needs no annotation. Slices
accept any of the three components independently - `xs[1:]`, `xs[:3]`,
and `xs[::2]` are all valid.

## Template sigils

Inside a `template { ... }` block the braces are literal text; two
sigils punch into Dragon code and back. `!{ }` breaks **out** of content
into a Dragon expression, and `:{ }` breaks **back** into content from
inside that code. They are not general-purpose operators - they exist
only within a template block. See [Templates](/docs/1201-templates).

| Sigil | Meaning | Example |
|---|---|---|
| `!{ expr }` | interpolate a Dragon expression into content | `template {Hello !{name}}` |
| `:{ content }` | return to content mode from inside `!{ }` code | `!{ for x in xs :{ <li>!{x}</li> } }` |

## Operator precedence

From **loosest** binding (evaluated last) to **tightest** binding
(evaluated first). Operators on the same row share a precedence level.
Except for `**`, all binary operators are left-associative; `**` is
right-associative, and the unary prefixes (`-`, `+`, `~`, `not`) are
right-associative.

| Precedence | Operators | Associativity |
|---|---|---|
| 1 (loosest) | `x if c else y` (ternary) | right |
| 2 | `or` | left |
| 3 | `and` | left |
| 4 | `not x` (prefix) | right |
| 5 | `==` `!=` `<` `>` `<=` `>=` `in` `not in` `is` `is not` | chained |
| 6 | `\|` (bitwise or) | left |
| 7 | `^` (bitwise xor) | left |
| 8 | `&` (bitwise and) | left |
| 9 | `<<` `>>` | left |
| 10 | `+` `-` (binary) | left |
| 11 | `*` `/` `//` `%` | left |
| 12 | `-` `+` `~` (unary prefix) | right |
| 13 (tightest) | `**` | right |
| 14 | call `()` , subscript `[]`, attribute `.` | left |

The walrus `:=` and the assignment operators (`=`, `+=`, ...) are
statement-level, not part of the expression precedence ladder; a walrus
must be parenthesized to appear inside a larger expression.

## Known gaps

Two operators that Python defines are **not yet wired** in Dragon. They
fail at compile time with a `DRAGON SCALE ERROR`, and each has a working
method-based alternative.

| Wanted | Status | Use instead |
|---|---|---|
| `list + list` (concatenation) | not wired - `unsupported operand types for +: 'list[int]' and 'list[int]'` | `a.extend(b)` |
| `set \| set` (union) | not wired | `a.union(b)` |
| `set & set` (intersection) | not wired | `a.intersection(b)` |
| `set - set` (difference) | not wired | `a.difference(b)` |

The set operators report their operands as `list[int]` in the error
message because Dragon's type system currently represents a `set` as a
list internally; the method forms (`.union`, `.intersection`,
`.difference`) are fully supported and return a proper `set`:

```dragon
a: list[int] = [1, 2]
a.extend([3, 4])           # [1, 2, 3, 4]  - not  a + [3, 4]

s1: set[int] = {1, 2, 3}
s2: set[int] = {3, 4, 5}
print(s1.union(s2))        # {1, 2, 3, 4, 5}
print(s1.intersection(s2)) # {3}
print(s1.difference(s2))   # {1, 2}
```

See [Collections](/docs/0501-lists) for the full set of list, set, and
dict methods, and [Appendix A: Keywords](/docs/2001-appendix-keywords) for the
word-operators (`and`, `or`, `not`, `in`, `is`) as reserved keywords.
