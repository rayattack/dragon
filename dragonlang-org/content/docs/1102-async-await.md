# Colorless `async`/`await`

[Green threads](/docs/1101-green-threads) gave you `fire` and `Task[T].join()`.
There's a second way to wait on a task - `await` - and it comes with the single
most important property of Dragon's concurrency model: **it never colors your
functions.** `await` can appear in *any* function, and using it imposes no
obligation on the callers above.

## `await` does what `join` does

`await` blocks until a task's value is ready and returns it - the same as `join()`
- but it reads better at a call site that does I/O:

```dragon
async def fetch_data(url: str) -> str {
    # ... network I/O, runs on a green thread ...
    return "Data"
}

def show() -> None {
    result: str = await fetch_data("https://example.com")
    print(result)               # Data
}

show()
```

Notice what `show()` is **not**: it is not `async`. This is **the Dragon Rule** -
`await` is allowed in *any* function, not just `async` ones.

## Why that matters: no function coloring

In Python, one `async def` at the bottom forces every caller above it to become
`async` too, all the way up to `asyncio.run`. That's the *function coloring*
problem: a leaf change ripples through dozens of signatures, and you end up
maintaining two of everything - `read` and `async_read`, `Lock` and `AsyncLock`.

Dragon refuses that trade. The `await` keyword stays - so a reader can *see* where
a function might pause - but it carries no obligation upward. The caller of
`show()` doesn't know or care that any I/O happened. There is one `Lock`, one
`read`, one of everything; concurrency is a property of *how you call*, not a virus
in the type signature.

## What `async def` actually does

It changes the **return type**. Calling an `async def f() -> T` gives you a
`Task[T]`, not a `T` - the body runs on a green thread. `await` is how you unwrap
it:

```dragon
async def fetch(url: str) -> str {
    return "..."
}

data: str = await fetch("https://x.com")   # await -> str (run and wait)
task: Task[str] = fetch("https://x.com")   # no await -> Task[str] (run, hold the handle)
later: str = await task                    # unwrap when you're ready
```

`await` only works on a `Task`. Awaiting a plain value, or the result of an
ordinary synchronous function, is a compile error - the compiler tells you to drop
the `await` because there's nothing to wait for.

## `fire` vs `async def`

Both produce a `Task[T]`; they differ in intent:

| | `fire fn()` | `async def` + `await` |
|---|---|---|
| Reads as | "run this in parallel" | "this does I/O, pause me until it's done" |
| Typical use | CPU fan-out, background work | sequential network / disk calls |
| Collect with | `t.join()` | `await` (inline) |

Reach for `fire` when you're launching parallel work; reach for `await` when you're
writing a straight-line sequence that happens to touch the network or disk. They're
the same machinery - a `Task[T]` on a green thread - wearing two names that read
right in two different situations.

## At a glance

| You want to... | Write |
|----------------|-------|
| A function that returns a `Task[T]` | `async def f(...) -> T { ... }` |
| Run it and wait inline | `x: T = await f(...)` |
| Run it, hold the handle | `t: Task[T] = f(...)` then `await t` |
| Use `await` in a plain function | just do it - no `async` needed on the caller |

`await` and `join` wait on green threads. When you need *real* hardware
parallelism instead, drop to the OS-thread tiers:
[Scoped and Manual OS Threads](/docs/1103-os-threads).
