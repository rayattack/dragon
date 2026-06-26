# Operators

Operators are the part of a language you think you already know - `+` adds, `<`
compares, `and` is `and`. And mostly that intuition carries over to Dragon
intact. But a static, compiled language makes a few choices a Python programmer
should see coming: integers are fixed-width and wrap instead of growing without
bound, `/` always produces a float, and a handful of Python's spellings either
mean something different or aren't operators at all. This chapter walks every
operator and flags those divergences as they come up. The terse lookup table
lives in [Appendix B](/docs/2002-appendix-operators); this is the guided tour.

## Arithmetic

The seven arithmetic operators behave as you'd expect, with two things to note:

```dragon
const a: int = 17
const b: int = 5

print(a + b)       # 22
print(a - b)       # 12
print(a * b)       # 85
print(a / b)       # 3.4   - true division ALWAYS yields a float
print(a // b)      # 3     - floor division
print(a % b)       # 2     - modulo
print(a ** b)      # 1419857 - exponentiation
print(-a)          # -17
```

The first surprise is `/`. In Dragon it is **true division and always returns a
`float`**, even when the operands divide evenly - `6 / 2` is `3.0`, not `3`.
Storing that result in an `int` is a compile error; reach for `//` when you want
an integer quotient.

The second is overflow. A Dragon `int` is a **64-bit signed integer**, and it
**wraps silently** - there is no `OverflowError`, and no automatic promotion to a
big integer the way Python 3 does:

```dragon
const max64: int = 9223372036854775807
print(max64 + 1)   # -9223372036854775808   (wraps, no error)
```

This is the native-machine-integer behavior of C, Go, and Rust (in release mode),
and it is the price of `int` being a bare CPU register with no boxing. When you
genuinely need values beyond 64 bits, that's a different type, not a free upgrade.

### Floor division and modulo on negatives

`//` and `%` follow Python's semantics exactly, which differ from C: the result
of `//` floors toward negative infinity, and `%` takes the **sign of the
divisor**:

```dragon
print(-7 // 3)     # -3   (floors toward -inf, not toward zero)
print(-7 % 3)      # 2    (sign of the divisor)
print(7 % -3)      # -2
```

### Exponentiation

`**` is right-associative and binds tighter than a unary minus on its left:

```dragon
print(2 ** 3 ** 2)     # 512   = 2 ** (3 ** 2), not (2 ** 3) ** 2
print(-2 ** 2)         # -4    = -(2 ** 2)
print((-2) ** 2)       # 4

const root: float = 2 ** 0.5
print(root)            # 1.4142135623730951   (float exponent → float result)
```

## Comparison - including chains

The six comparison operators return `bool`. Dragon also supports Python's
**chained comparisons**, where `a < b < c` means `(a < b) and (b < c)` with `b`
evaluated once:

```dragon
const x: int = 5
print(x == 5)          # True
print(x != 8)          # True
print(1 < x < 10)      # True
print(1 < x < 4)       # False
print(0 < x <= 5)      # True
```

## Boolean logic

`and`, `or`, and `not` short-circuit - `or` stops at the first truthy operand,
`and` at the first falsy one - so the right-hand side may never be evaluated:

```dragon
print(True and False)   # False
print(False or True)    # True
print(not True)         # False
```

`not` binds *looser* than comparison, so `not 5 == 5` parses as `not (5 == 5)`,
which is `False`.

## Bitwise

The bitwise operators work on `int`: `&` and, `|` or, `^` xor, `~` complement,
`<<` left shift, `>>` arithmetic right shift.

```dragon
const a: int = 0b1100    # 12
const b: int = 0b1010    # 10
print(a & b)             # 8    (1000)
print(a | b)             # 14   (1110)
print(a ^ b)             # 6    (0110)
print(~a)                # -13  (~n == -(n + 1))
print(a << 2)            # 48
print(a >> 1)            # 6
```

Precedence among them is the C ordering: `&` tighter than `^` tighter than `|`,
all looser than the arithmetic operators - so `6 | 8 & 3` is `6 | (8 & 3)` = `6`.

## Membership and identity

`in` and `not in` test membership - in a list or set (by value), in a string (as
a substring), or in a dict (by key):

```dragon
const nums: list[int] = [1, 2, 3, 4, 5]
print(3 in nums)            # True
print(9 not in nums)        # True
print("rag" in "Dragon")    # True   (substring)

const d: dict[str, int] = {"x": 1, "y": 2}
print("x" in d)             # True   (key membership)
```

`is` and `is not` are used for one thing: checking against `none` (Dragon accepts
both `none` and `None` for the null literal). They are how you test an optional
value, hand in hand with the narrowing covered in
[Union, Optional, and Narrowing](/docs/0702-union-and-narrowing):

```dragon
def lookup(k: str) -> int | None {
    if k == "x" { return 42 }
    return none
}

const v: int | None = lookup("x")
print(v is not none)        # True
```

## The walrus `:=`

The walrus operator binds a name *and* yields its value in the same expression -
ideal for a sentinel loop or a compute-once-then-test `if`:

```dragon
const words: list[str] = ["alpha", "beta", "stop", "gamma"]
i: int = 0
while (w := words[i]) != "stop" {
    print(w)
    i += 1
}
print(f"stopped at: {w}")
```

```text
alpha
beta
stopped at: stop
```

## Augmented assignment

Every binary arithmetic and bitwise operator has an augmented form - `+= -= *=
/= //= %= **= &= |= ^= <<= >>=` - that updates a variable in place:

```dragon
x: int = 20
x += 5         # 25
x *= 2         # 50
x //= 3        # 16
x &= 0b1100    # 0
print(x)       # 0

s: str = "Dragon"
s += " lang"
print(s)       # Dragon lang
```

## Ternary

The conditional expression is `value_if_true if condition else value_if_false`.
It is right-associative, so it nests cleanly into a chain of cases:

```dragon
const score: int = 75
const grade: str = "A" if score >= 90 else "B" if score >= 80 else "C" if score >= 70 else "F"
print(grade)   # C
```

## Two things that aren't operators

A Python programmer reaches for two spellings that Dragon handles differently:

- **`@` is not matrix multiplication.** In Dragon `@` is exclusively the
  decorator sigil (see [Decorators](/docs/0802-decorators)). There is no
  matmul operator.
- **Combining containers is covered with the containers.** Joining lists,
  and set algebra (union, intersection, difference), are presented in
  [Lists](/docs/0501-lists) and [Sets and Tuples](/docs/0503-sets-and-tuples),
  alongside the methods that do the same work. Set *subset* comparisons
  (`<`, `<=`, `>`, `>=`) do work as operators and are shown there.

## At a glance

| Category | Operators |
|----------|-----------|
| Arithmetic | `+ - * / // % **` (unary `-` `+`) - `/` always float; `int` wraps at 64 bits |
| Comparison | `== != < > <= >=`, chainable (`a < b < c`) |
| Boolean | `and` `or` `not` (short-circuit) |
| Bitwise | `& \| ^ ~ << >>` |
| Membership | `in` / `not in` |
| Identity | `is` / `is not` (for `none`) |
| Bind-in-expression | `:=` (walrus) |
| Augmented | `+= -= *= /= //= %= **= &= \|= ^= <<= >>=` |
| Conditional | `x if cond else y` |

With operators in hand, the rest of Part 2 covers the statements they live inside
- see [Control Flow](/docs/0205-control-flow).
