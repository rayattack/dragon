# Context Managers

Some things must be undone. A file must be closed, a lock released, a connection
returned to the pool - and it has to happen *whether the work succeeds or throws*.
You could write a [`finally`](/docs/0901-exceptions) for every one, but that ties
the cleanup to a code block instead of to the resource itself. The `with` statement
ties cleanup to the **resource**: you say what "enter" and "exit" mean once, on the
type, and every `with` block gets guaranteed teardown for free.

## The `with` statement

A type becomes a context manager by implementing two dunder methods: `__enter__`,
which runs on the way in and returns the value bound by `as`, and `__exit__`, which
runs on the way out. Both use the implicit-`self` method form.

```dragon
class Section {
    name: str = ""
    def(name: str) {
        self.name = name
    }
    def __enter__() -> Section {
        print(f"--- begin {self.name} ---")
        return self
    }
    def __exit__() -> None {
        print(f"--- end {self.name} ---")
    }
}

with Section("setup") as s {
    print(f"doing {s.name} work")
}
```

```text
--- begin setup ---
doing setup work
--- end setup ---
```

`__enter__` runs first; whatever it returns is bound to `s`. The block runs. Then
`__exit__` runs - no matter what.

## Cleanup runs even when the body raises

That "no matter what" is the whole point. If the block raises, `__exit__` still
runs before the exception continues on its way - exactly like a `finally` tied to
the resource:

```dragon
class Resource {
    def() {}
    def __enter__() -> Resource {
        print("acquire")
        return self
    }
    def __exit__() -> None {
        print("release")
    }
}

try {
    with Resource() as r {
        print("working")
        raise ValueError("boom")
    }
} except ValueError as e {
    print("handled: " + str(e))
}
```

```text
acquire
working
release
handled: boom
```

The resource is released on the way out, and the exception still propagates to the
outer handler. This is the guarantee `with` exists to provide: the teardown is
attached to the resource, so you can't forget it and an early `raise` can't skip it.

## The current limitation

Dragon's `__exit__` today takes **no arguments** and returns nothing: it's a
guaranteed-cleanup hook, not yet an exception *handler*. It runs on both the normal
and the throwing path, but it does not receive the exception and **cannot suppress
it** - a `raise` inside the block always continues propagating after `__exit__`
runs (catch it with an outer `try`/`except`, as above).

The richer form - an `__exit__` that inspects the in-flight exception and can
swallow it - is the planned design (a native
`__exit__(exc: Exception | None) -> bool`, where a truthy return suppresses). It is
not wired yet; until it lands, treat `__exit__` as cleanup-only and handle
exceptions with `try`/`except` around the `with`.

## Where you'll meet them

The standard library uses this protocol for the obvious resources - a file opened
with `with open(...) as f` closes itself at block exit, and a database
[`transaction()`](/docs/1301-databases) commits or is abandoned at the end of its
block. Writing your own is the same two methods shown here.

## At a glance

| You want to... | Write |
|----------------|-------|
| Make a type usable with `with` | `__enter__() -> T` and `__exit__() -> None` |
| Bind the entered value | `with R() as r { ... }` - `r` is what `__enter__` returns |
| Guarantee cleanup | put it in `__exit__` - it runs on success *and* on `raise` |
| Handle an error from the block | wrap the `with` in `try`/`except` (no suppression in `__exit__` yet) |

That closes Part 9. With exceptions, custom error types, and guaranteed cleanup in
hand, the next part returns to data - [Modules and Packages](/docs/1001-modules) and
how a Dragon program is organized across files.
