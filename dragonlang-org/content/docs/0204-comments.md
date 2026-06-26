# Comments

Comments don't change the behavior of a program. They tell readers
something the code itself can't.

Dragon supports two comment styles, both familiar.

## Line comments

A `#` starts a comment that runs to the end of the line:

```dragon
# Total seconds in an 80-year lifespan.
const lifespan: int = 80 * 365 * 24 * 3600
```

Use line comments for one-line explanations, attribution, or to
temporarily disable a single line of code while debugging.

## Block comments

There is no separate block-comment syntax. Multiple `#` lines in a row
serve the same purpose:

```dragon
# Refcount + cycle collector. The fast path is non-atomic; the slow
# path is taken when an object has been observed by more than one OS
# thread (the GC_FLAG_SHARED bit is set on the header).
def incref(obj: ptr) -> None {
    ...
}
```

This is the same convention as Python. Editors handle it well - most
will toggle a comment block with a single keystroke.

## Doc comments

A `"""triple-quoted string"""` placed as the first statement of a
function, class, or module is treated as a docstring. Tools like
documentation generators and IDE hover-tips read docstrings; runtime
code can read them via `__doc__`:

```dragon
def fibonacci(n: int) -> int {
    """Return the n'th Fibonacci number, 0-indexed.

    Uses the closed-form Binet formula. Accurate up to n = 70 or so;
    floating-point error grows after that.
    """
    ...
}
```

Use docstrings for the public interface of a function or class - what
it does, what its parameters mean, what it returns, what it raises.
Use line or block comments for *why* a piece of code is the way it is
- hidden constraints, workarounds, design choices a reader wouldn't
infer from the code alone.

## What not to comment

Code with well-named identifiers and a clear structure rarely needs
comments. A comment that only restates what the code already says is
worse than no comment, because it takes time to read and rots out of
sync with the code.

```dragon
# bad: restates the code
i = i + 1                    # increment i

# bad: explains an obvious operation
total = a + b                # add a and b

# good: hidden constraint a reader couldn't see
# Dragon's int is 64-bit; values above 2^53 lose precision when round-
# tripped through float. Sum order matters here.
total = sum_descending(values)
```

If you're tempted to write a comment because the code is confusing,
the better fix is usually to clean up the code instead.

The next section covers control flow.
