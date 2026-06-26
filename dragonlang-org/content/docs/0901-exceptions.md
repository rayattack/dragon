# Exceptions

Every language has to answer one question: when something goes wrong deep inside a
call, how does the failure get back to the code that can do something about it? Go
threads an `error` value through every return. Rust wraps it in a `Result<T, E>`
and makes you unwrap it. Python - and Dragon - use **exceptions**: a separate
channel that unwinds the stack until a handler catches it, leaving the happy path
uncluttered.

Dragon's syntax is Python's, with brace blocks: `try` / `except` / `else` /
`finally`, `raise`, and a built-in exception hierarchy. Under the hood there's no
zero-cost landing-pad machinery and no `Result` monad - a `raise` lowers to
`setjmp`/`longjmp`, so the *throwing* path pays the cost and the
*normal* path is free. You write the same code you'd write in Python; it compiles
to a long jump.

## `try` / `except`

A `try` block guards a region. If a `raise` fires anywhere inside it - directly or
down a chain of calls - control jumps to the matching `except` clause:

```dragon
def parse_port(s: str) -> int {
    try {
        return int(s)
    } except ValueError {
        return 8080
    }
}

print(parse_port("9090"))       # 9090
print(parse_port("not-a-port")) # 8080
```

`int("not-a-port")` raises `ValueError`; the `except ValueError` clause catches it.
A clause only fires for the type it names (or a subtype); anything it doesn't match
keeps unwinding to an outer handler.

## Binding the exception: `except E as e`

To inspect the error, bind it with `as`. `str(e)` returns the message it was raised
with - the form that works for every exception type:

```dragon
try {
    raise ValueError("temperature out of range")
} except ValueError as e {
    print("caught: " + str(e))   # caught: temperature out of range
}
```

## Multiple `except` clauses

Stack clauses to handle different failures differently; they're tried top to
bottom, first match wins. Put **specific** types first and **broad** ones last - a
leading `except Exception` would swallow everything:

```dragon
def handle(s: str) -> None {
    try {
        classify(s)
    } except ValueError as e {
        print("value: " + str(e))
    } except TypeError as e {
        print("type: " + str(e))
    } except Exception as e {
        print("fallback: " + str(e))
    }
}
```

To handle several types in one clause, group them in a tuple - the same
`except (ValueError, KeyError) as e` form Python uses:

```dragon
def classify(s: str) -> None {
    if s == "v" { raise ValueError("bad value") } else { raise KeyError("bad key") }
}

try { classify("v") } except (ValueError, KeyError) as e {
    print("caught: " + str(e))   # caught: bad value
}
```

## The exception hierarchy

Dragon ships Python's exception tree, and matching is **range-based on the
hierarchy**, not exact-name: catching a type also catches every type beneath it. So
`except Exception` is the universal "catch anything recoverable" net, and
`except ArithmeticError` catches `ZeroDivisionError`, `OverflowError`, and
`FloatingPointError` alike. The tree, abridged:

```text
BaseException
└── Exception
    ├── ArithmeticError → ZeroDivisionError, OverflowError, FloatingPointError
    ├── LookupError → IndexError, KeyError
    ├── OSError → FileNotFoundError, PermissionError, ConnectionError → ...
    ├── RuntimeError → NotImplementedError, RecursionError
    ├── TypeError
    └── ValueError → UnicodeError
```

A specific type is caught by its base:

```dragon
try {
    raise ConnectionRefusedError("port 5432 closed")
} except OSError as e {
    print("an OS-level error: " + str(e))   # an OS-level error: port 5432 closed
}
```

## `raise` and re-raising

`raise E("message")` throws; the argument is what `str(e)` returns in a handler and
what an uncaught exception prints before the program exits with a nonzero status.
A **bare** `raise` inside an `except` block re-throws the current exception -
log at one level, let an outer handler decide:

```dragon
try {
    try {
        raise ValueError("disk full")
    } except ValueError as e {
        print("logging: " + str(e))
        raise                            # re-throw to the outer handler
    }
} except ValueError as e {
    print("recovered: " + str(e))
}
# logging: disk full
# recovered: disk full
```

Translating a low-level failure into a domain-specific one - `raise MyError(...) from e`
- is covered with [Custom Exceptions](/docs/0902-custom-exceptions).

## `finally` and `else`

A `finally` block runs no matter how the `try` exits - normal completion, caught
exception, or an exception still propagating. It's where cleanup goes. An `else`
block runs only if the `try` completed *without* raising - the success
continuation, kept out of the `except`'s reach:

```dragon
def parse(s: str) -> None {
    try {
        const n: int = int(s)
    } except ValueError {
        print("not a number: " + s)
    } else {
        print("parsed ok: " + s)
    } finally {
        print("done")
    }
}

parse("42")     # parsed ok: 42 / done
parse("abc")    # not a number: abc / done
```

For tying cleanup to a *resource* rather than a code block, the more focused tool
is the `with` statement - see [Context Managers](/docs/0903-context-managers).

## Built-in runtime errors are catchable

Runtime checks raise like everything else, through the same path your own `raise`
uses - so `try`/`except` catches them. Integer division by zero raises
`ZeroDivisionError`, an out-of-range index raises `IndexError`, a missing key via
`d[k]` raises `KeyError`:

```dragon
try {
    const x: int = 10 // 0
} except ZeroDivisionError as e {
    print("caught: " + str(e))   # caught: ZeroDivisionError: integer division by zero
}
```

The one error that still **aborts** the process - printing to stderr and exiting,
*bypassing* `try`/`except` - is a failed `assert`:

```dragon
assert False, "boom"     # prints "AssertionError: boom", exits 1 - NOT catchable
```

That's by design: `assert` is for invariants that must hold, not recoverable
conditions. If you want a catchable failure, `raise AssertionError(...)` explicitly.

## Running out of memory: `MemoryError`

Most Dragon code allocates without ever saying so. Every one of these asks the
runtime for heap memory behind the scenes - you never write `malloc`, Dragon does
it for you:

```dragon
s: str = a + b               # concat allocates a new string
c: str = chr(code)           # a one-character string
u: str = name.upper()        # the upper-cased copy
blob: str = json.dumps(rec)  # built in a growing buffer
```

So you do not control *every* place memory gets requested - the allocation is
hidden inside ordinary, correct-looking code. When the system cannot satisfy one
of those hidden requests - a caller sends a body larger than you planned for, the
process hits a container memory cap, a long-running server fills up - the operation
raises **`MemoryError`**, through the same path as every other exception. It is
catchable, so the same loop that might exhaust memory can degrade instead of dying:

```dragon
def build_response(parts: list[str]) -> str {
    try {
        out: str = ""
        for p in parts {
            out = out + p       # any of these concats may run out of memory
        }
        return out
    } except MemoryError {
        return "<response too large>"   # recover instead of crashing
    }
}
```

This matters most in a long-running server: one request that hits the memory
ceiling raises an exception the handler catches, and the process keeps serving
everything else. Contrast a runtime that instead used the failed allocation as if
it had succeeded: it would write to a null pointer and the OS would kill the
process with a segfault - taking down every in-flight request with it, with no
traceback and nothing `try`/`except` could do, because a segfault happens *below*
the exception machinery. Dragon checks the allocation and raises, so out-of-memory
is a recoverable event in your code, not a process death.

## Guarding still beats catching

Even though these errors are catchable, guarding the operation is often the better
discipline - it's faster than throwing and makes the failure case explicit. Use
`dict.get(key, default)` and the `in` operator when "absent" is a normal outcome,
and check `if b != 0` before dividing. Reach for `try`/`except` when the failure is
genuinely *exceptional* - corrupt input, a violated invariant, an error that should
propagate several frames up - and for a return value (a default, a sentinel, an
`int | None`) when "not found" is a normal, expected result.

## At a glance

| You want to... | Write |
|----------------|-------|
| Guard a region | `try { ... } except E { ... }` |
| Inspect the error | `except E as e { print(str(e)) }` |
| Handle several types | `except (A, B) as e { ... }` or a clause per type |
| Catch anything recoverable | `except Exception as e { ... }` |
| Throw | `raise ValueError("message")` |
| Re-throw in a handler | bare `raise` |
| Always-run cleanup | `finally { ... }` |
| Success-only continuation | `else { ... }` |
| Avoid a missing key | `d.get(key, default)` / `if key in d` |

Exceptions are colorless - no `throws` annotation, no function "color," just like
Dragon's [functions](/docs/0301-functions) need no `async` marker. Next, defining
your own error types: [Custom Exceptions](/docs/0902-custom-exceptions).
