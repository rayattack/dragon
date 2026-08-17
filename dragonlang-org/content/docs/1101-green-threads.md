# Green Threads and Tasks

Most languages make you choose a side. Either you get cheap, numerous tasks but a
viral `async`/`await` that splits your functions into two colors, or you get real
OS threads that are heavy and few. Dragon gives you **three tiers** that share one
rule: *a suspension point is always visible, but it never changes a function's
signature.*

| Tier | Syntax | Backed by | Best for |
|------|--------|-----------|----------|
| Green thread | `fire fn()` / `fire { }` | M:N scheduler | thousands of concurrent tasks, I/O |
| Scoped OS thread | `thread { }` | one OS thread | a CPU-bound burst with no ceremony |
| Manual OS thread | `Thread(target=fn)` | one OS thread | daemons, custom lifecycles |

This chapter covers the first and default tier - green threads. The
[OS-thread tiers](/docs/1103-os-threads) and
[synchronization](/docs/1104-synchronization) follow.

## Green threads with `fire`

`fire` starts a green thread. You can fire a function call:

```dragon
def log_event(kind: str, user_id: int) -> None {
    print(f"{kind}: {user_id}")
}

user_id: int = 7
fire log_event("user_login", user_id)
```

That's "fire and forget" - the work runs concurrently and you never look back. You
can also fire a **block**:

```dragon
def cleanup_temp_files() -> None { print("cleaning up") }
def send_notification(msg: str) -> None { print(msg) }

fire {
    cleanup_temp_files()
    send_notification("done")
}
```

Green threads are not OS threads. The runtime schedules many of them onto a small
pool of OS threads (an "M:N" scheduler), so each costs about 64 KB instead of the
~1 MB an OS thread needs - spawning ten thousand is normal. And when a green thread
does I/O - a socket read, a database query - the runtime quietly parks it and runs
another, instead of blocking the underlying OS thread.

## What may cross into a green thread

A green thread runs concurrently with the code that fired it, so every heap
value that crosses the `fire` boundary must arrive in a way that cannot race.
There are five doors, and anything else is a compile error at the fire line:

```dragon
# doc: no-check
req: Request = parse_request(conn)      # this function still owns req
counts: dict[str, int] = load_counts()

fire mutate(req)              # error E12: 'req' crosses a thread boundary;
                              # 'mutate' writes its argument, so move it
                              # (own req), copy it (dub req), or lock it

fire handle(own req)          # door 1: moved - the green thread becomes the
                              #         sole owner; req is dead on this side
fire tally(dub counts)        # door 2: copied - the green thread gets its own
fire banner(GREETING)         # door 3: an immortal const string literal
fire record(router)           # door 4: an internally-locked type
fire read_only(counts)        # door 5: shared read-only - 'read_only' provably
                              #         never writes counts, so N threads share
                              #         it at zero copy
```

Door 1 is the default to reach for: a moved value belongs to exactly one
thread, proven at compile time, so there is no lock to take and no data race
to debug. Use door 2 when this side still needs the value, and door 4 (a
class whose storage is guarded by an `own` Lock field, like the HTTP
`Router`) when threads genuinely must share mutable state.

Door 5 is the one you never have to ask for. When the fired function provably
only *reads* the argument - no `append`, no `x[i] = ...`, no `x.field = ...` -
the value is shared across the threads at zero copy. `fire` already marks it so
its reference count is updated atomically, and a value nobody writes cannot be
raced. This is the worker-pool shape: fire one task per job over the same shared
data, then join them all. The rule reads cleanly in reverse: **a plain
`fire f(x)` that compiles is proof that `f` only reads `x`.** The day you need to
write it, the compiler makes you say so - `dub x` for a private copy, `own x` to
hand it over - so a `dub` at a fire site is always a visible admission that a
copy was needed.

```dragon
def total_price(items: list[Item]) -> int { ... }   # reads items, never writes

orders: list[Item] = load_orders()
tasks: list[Task[int]] = []
w: int = 0
while w < 8 {
    t: Task[int] = fire total_price(orders)   # door 5: every task reads `orders`
    tasks.append(t)
    w = w + 1
}
grand: int = 0
for t in tasks { grand = grand + t.join() }
```

The full rules live in [Ownership](/docs/1604-ownership); the lock types in
[Synchronization](/docs/1104-synchronization).

## Task handles: `Task[T]`

When you want a result back, bind the `fire` to a variable. `fire` returns a
**`Task[T]`**: a lightweight, zero-cost handle to the running green thread, where
`T` is the type the work will eventually produce.

```dragon
def compute(n: int) -> int {
    return n * n
}

t: Task[int] = fire compute(9)
# ... do other work while compute() runs ...
result: int = t.join()           # 81
print(result)
```

Two rules to remember:

- **The annotation is required.** Write `t: Task[int] = fire compute(9)`, not
  `t = fire compute(9)` - the same `:` declares / `=` reassigns rule as everywhere
  else. (You may write `t: Task = fire compute(9)` and let Dragon pin the inner
  type, but spelling out `Task[int]` reads better.)
- **A task carries exactly one typed value.** `t.join()` blocks until the work
  finishes and hands you that value, already typed as `T`.

A `Task` has just two methods:

| Method | Returns | Meaning |
|--------|---------|---------|
| `t.join()` | `T` | Wait for the result and return it |
| `t.is_alive()` | `bool` | `true` while the task is still running |

## Task lifetime: the scope frees what it holds

A bound `Task[T]` is an ordinary scope-owned value, just like a `str` or a `list`.
You never free it by hand, and you cannot leak one by forgetting to collect it:

```dragon
class Data { }
class Response { }
class Request {
    id: int
    wants_detail: bool
    def(id: int, wants_detail: bool) {
        self.id = id
        self.wants_detail = wants_detail
    }
}

def load(id: int) -> Data { return Data() }
def full(d: Data) -> Response { return Response() }
def fast(req: Request) -> Response { return Response() }

def maybe(req: Request) -> Response {
    t: Task[Data] = fire load(req.id)
    if req.wants_detail {
        return full(await t)        # collected: await hands you the value
    }
    return fast(req)                # abandoned: the scope frees t on the way out
}
```

The rule is the same on every path:

- **`await t` / `t.join()` *consume* the task** - they collect the value and the
  task is done. After that the handle is spent: don't `await`/`join` the same
  task again, and don't read `t.is_alive()` on it. Capture the value once and
  reuse it (`r = await t`).
- **An abandoned task is freed by its scope.** If you bind a task and never collect
  it - on any branch - the scope reclaims it when it ends. A fired task that runs
  to completion is never leaked just because you skipped the `await`.
- **Returning, storing, or passing a task hands off ownership.** `return t`, a
  `self.t = t` field store, or `f(t)` gives the task to the receiver, which then
  collects (or abandons) it. The producing scope no longer owns it.

In short: collect a task once and use its value, or abandon it freely - either way
nothing leaks, and the abandon path is the scope's job, not yours.

## Parallel fan-out

The classic use is starting several tasks, then collecting them - they run at the
same time, and the `join()` calls gather the results:

```dragon
def compute(n: int) -> int {
    return n * n
}

a: Task[int] = fire compute(3)
b: Task[int] = fire compute(4)
total: int = a.join() + b.join()    # 9 + 16 = 25
print(total)
```

Both `compute` calls run concurrently; neither `join` returns until its task is
done. Scale that to a list of tasks and you have a worker pool: fire one task per
job, then join them all.

## At a glance

| You want to... | Write |
|----------------|-------|
| Fire-and-forget | `fire fn(args)` or `fire { ... }` |
| A task you'll collect | `t: Task[int] = fire compute(9)` |
| Wait for the result | `t.join()` (or `await t` - see next chapter) |
| Check if it's still running | `t.is_alive()` |
| Parallel fan-out | several `fire`s, then join each |

`join()` is the explicit way to wait. The next chapter introduces the other way -
`await` - and the colorless model that lets it appear in *any* function:
[Colorless async/await](/docs/1102-async-await).

One more thing before you go: `fire` has a twin. `fire f(x)` runs f NOW on
another green thread; `defer f(x)` runs f LATER on this thread, when the
current scope ends - same call shape, same `own`/`dub` argument modes, opposite
half of the coin. Cleanup that must run on every exit path lives there:
[Defer: Scope-Exit Calls](/docs/1105-defer).
