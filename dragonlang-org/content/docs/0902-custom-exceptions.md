# Custom Exceptions

The built-in tree from [Exceptions](/docs/0901-exceptions) covers the common
failures, but a real library wants its *own* error types - so callers can catch
"any failure from this module" with one clause, and carry structured detail
(a status code, the offending value) alongside the message. In Dragon a custom
exception is just a class that subclasses `Exception`, and it slots into the
hierarchy exactly where you put it.

## Defining one

Subclass `Exception` (or any type in the tree). The constructor is the nameless
`def()` - Dragon's `.dr` constructor - and it stores the message:

```dragon
class ValidationError(Exception) {
    def(message: str) {
        self.message = message
    }
}

def validate_age(age: int) -> None {
    if age < 0 {
        raise ValidationError("age cannot be negative")
    }
}

try {
    validate_age(-5)
} except ValidationError as e {
    print("invalid: " + str(e))   # invalid: age cannot be negative
}
```

Your type slots into the hierarchy at the type you extend: subclass `Exception` and
`except Exception` catches it; subclass `OSError` and `except OSError` catches it.
Internally each user exception is assigned a type code starting at 1000, kept
distinct from the built-in tree, with subtype matching that walks your class's
parent chain - but none of that surfaces in the code you write.

## A hierarchy of your own

The standard library does this for database drivers, and you can too: one root
type, specific subtypes beneath it, so callers choose how broadly to catch.

```dragon
class AppError(Exception) {
    def(message: str) {
        self.message = message
    }
}

class NotFoundError(AppError) {
    def(message: str) {
        self.message = message
    }
}

class PermissionDenied(AppError) {
    def(message: str) {
        self.message = message
    }
}

def lookup(id: int) -> None {
    raise NotFoundError("user " + str(id) + " does not exist")
}

try {
    lookup(42)
} except AppError as e {            # catches any AppError subtype
    print("app error: " + e.message)   # app error: user 42 does not exist
}
```

A caller that wants to distinguish cases writes `except NotFoundError` before
`except AppError`; a caller that just wants "any failure from this library" writes
the single `except AppError`. The base type is the contract.

## `e.message` vs `str(e)`

Notice the handler above reads `e.message`, the field the constructor stored,
rather than `str(e)`. Both work, but they differ in what they carry:

- `str(e)` always returns a printable representation and never fails - reach for it
  when you just want *something* to log, for any exception type.
- Reading a **field** (`e.message`, or a `e.status` you defined) carries your exact
  stored data through to the handler, even when a subtype is caught through a base
  clause. That's the whole reason a custom exception that holds its data in fields
  is worth the few extra lines.

What Dragon does *not* capture is a traceback - there's no stack-frame list, so a
`traceback.format_exc()`-style render of where the error came from isn't available.

## Wrapping: `raise X from Y`

To translate a low-level failure into a domain-specific one, raise a new exception
inside the handler. The `from` clause records that the new error was *caused by* the
original:

```dragon
class ConfigError(Exception) {
    def(message: str) {
        self.message = message
    }
}

def load() -> None {
    try {
        const port: int = int("not-a-number")
    } except ValueError as e {
        raise ConfigError("config file is malformed") from e
    }
}

try {
    load()
} except ConfigError as e {
    print("startup failed: " + str(e))   # startup failed: config file is malformed
}
```

This is how you build a clean, layered error API where callers see *your* exception
type instead of the plumbing underneath. You can also wrap without `from` - a plain
`raise ConfigError(...)` inside the handler - when you don't need to record the
cause.

## At a glance

| You want to... | Write |
|----------------|-------|
| Define an error type | `class MyError(Exception) { def(message: str) { self.message = message } }` |
| Raise it | `raise MyError("...")` |
| Carry structured detail | store fields in the constructor; read `e.message`, `e.status`, … |
| Build an error family | one root subclass, specific subtypes under it |
| Catch the whole family | `except RootError as e { ... }` |
| Translate a failure | `raise MyError("...") from e` |

Custom exceptions give your modules a clean error contract. The last piece of this
part is the `with` statement that guarantees cleanup runs:
[Context Managers](/docs/0903-context-managers).
