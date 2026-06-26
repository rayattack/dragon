# Building a Concurrent Application

The [Concurrency](/docs/1101-green-threads) chapter laid out Dragon's three
tiers - `fire` green threads, scoped `thread { }` OS threads, and the
manual `Thread` class - and the rule they share: a suspension point is
always *visible* at the callsite, but it never changes a function's
signature. That chapter teaches the model. This one spends it.

The shape we're after is the one most concurrent programs actually want:
**fan out a batch of independent jobs, let them run at the same time,
then collect and combine the results.** A web handler firing three
backend calls; a build tool checksumming a dozen files; a report that
queries four tables. In Go you'd reach for goroutines and a `sync.WaitGroup`;
in Rust, `tokio::join!` and a coloring decision that ripples up every
caller; in Python, `asyncio.gather` and a function tree painted `async`
all the way to `main`. Dragon's answer is smaller, and it stays the same
whether the job is CPU-bound or I/O-bound: `fire` it, hold the `Task[int]`
it hands back, and `await` it when you need the value.

We'll build one real program piece by piece, compile and run each piece,
then add a CPU-bound verification pass on a real OS thread to show where
tier 2 fits. We will **not** re-derive how the scheduler works - that's
[Concurrency](/docs/1101-green-threads)'s job. Here we wire the parts into
something that does work.

## The smallest fan-out that works

Start with the canonical pattern at its smallest. A `def` does the work;
`fire` launches two copies of it concurrently and hands back a `Task[int]`
for each; `await` unwraps each handle into its `int`.

```dragon
def square(n: int) -> int {
    return n * n
}

t1: Task[int] = fire square(6)
t2: Task[int] = fire square(7)
a: int = await t1
b: int = await t2
print(a + b)            # 85

total: int = 0
for i in range(4) {
    ti: Task[int] = fire square(i)
    total = total + await ti
}
print(total)            # 14
```

This compiles and prints `85` then `14`. Three things in it are
load-bearing and worth saying out loud, because they're the rules every
later example obeys:

- **`fire f()` yields a `Task[T]`, and the binding annotation is
  mandatory.** You write `t1: Task[int] = fire square(6)`, never
  `t1 = fire square(6)`. Dragon's `:`-declares / `=`-reassigns rule has
  no exception for tasks: a task carries exactly one typed result, so you
  state the type at the declaration. This is the same "typing is never
  optional" stance you've seen since [Type Annotations](/docs/0701-type-annotations).
- **`fire` returns *immediately*.** Both `square(6)` and `square(7)` are
  in flight before the `await t1` line runs. The two green threads share
  a small pool of OS threads - spawning them is cheap (~64 KB each), not
  the ~1 MB an OS thread costs.
- **`await t` blocks-and-yields.** It is the same value as `t.join()`,
  but when the awaiter is itself a green thread, `await` parks it and
  lets the scheduler run something else instead of pinning an OS thread.
  In top-level code like this it simply waits for the result.

The second loop shows the idiomatic accumulate-as-you-go form: fire a
task, await it, fold the result into `total`. Note `total` is declared
once with `total: int = 0` and then *reassigned* with bare `=` inside the
loop - the loop body is its own block scope (see
[Variables](/docs/0201-variables)), and the bare `=` mutates the outer
`total` rather than shadowing it.

## Holding many tasks at once

Awaiting each task the instant you fire it throws away the parallelism -
you'd be running them one at a time. The win comes from firing the
*whole batch first*, then collecting. Hold the handles in a
`list[Task[int]]`:

```dragon
# A pure-CPU "job": sum of squares up to n (stands in for real work).
def crunch(job_id: int, n: int) -> int {
    total: int = 0
    for i in range(n) {
        total = total + i * i
    }
    return total
}

# Fan out four independent jobs. Each fire returns immediately with a
# Task[int]; the four green threads run concurrently on the scheduler.
tasks: list[Task[int]] = []
const sizes: list[int] = [100, 200, 300, 400]
for jid in range(len(sizes)) {
    t: Task[int] = fire crunch(jid, sizes[jid])
    tasks.append(t)
}

# Collect: await each handle. await == .join(), but it yields the
# current green thread to the scheduler instead of blocking the OS thread.
grand: int = 0
for t in tasks {
    grand = grand + await t
}
print(grand)            # 33183500
```

That prints `33183500`. The two loops are the entire pattern: the first
loop is *fan-out* (every `fire` starts work and the work overlaps), the
second is *collect* (every `await` drains one result). `Task[int]` is a
real, fully-typed element type - `list[Task[int]]` works exactly like any
other typed list, and `.append(t)` and `for t in tasks` behave the way
you'd expect from [Collections](/docs/0501-lists).

A detail that matters for correctness: **the order of results follows the
task list, not finish order.** The second loop awaits `tasks[0]` first
regardless of whether `crunch(0, 100)` or `crunch(3, 400)` completed
first. If you need results keyed to their inputs, the list index already
gives you that - `tasks[k]` is the handle for `sizes[k]`.

## `await` works inside any function - no coloring

The most important thing the [Concurrency](/docs/1101-green-threads) chapter
calls "the Dragon Rule" pays off when you factor the fan-out into a
reusable helper. In Python, the moment a function `await`s, it must be
`async def`, and every caller must be `async def`, and so on up to the
entry point - the coloring virus. In Dragon, `await` is just a callsite
marker. A plain `def` may `await`, and its callers are none the wiser:

```dragon
def fetch_price(symbol: str) -> int {
    # stand-in for an I/O fetch; returns a deterministic "price"
    total: int = 0
    for c in symbol {
        total = total + ord(c)
    }
    return total
}

# The Dragon Rule: await works in a plain def - no async coloring.
def total_basket(symbols: list[str]) -> int {
    tasks: list[Task[int]] = []
    for s in symbols {
        t: Task[int] = fire fetch_price(s)
        tasks.append(t)
    }
    sum: int = 0
    for t in tasks {
        sum = sum + await t
    }
    return sum
}

const basket: list[str] = ["AAPL", "MSFT", "GOOG"]
print(total_basket(basket))      # 900
```

This prints `900`. `total_basket` fires its work, awaits inside its own
body, and returns a plain `int`. The top-level caller treats it as an
ordinary function - `print(total_basket(basket))` - with no `await`, no
`async`, no special handling. The concurrency is an implementation detail
of `total_basket`, sealed behind a normal signature. That is the whole
point of the colorless design: you can make a function concurrent without
forcing a rewrite on everything that calls it.

> If you `await` something that is *not* a `Task`, that's a compile
> error, not a silent pass-through. `await` is only meaningful on a task
> handle, and the type checker enforces it. See the error catalogue in
> [Concurrency](/docs/1101-green-threads).

## A worked application: a parallel checksum runner

Now the real thing. We want to checksum a set of data chunks of varying
sizes, run the chunks concurrently, and report a combined digest. Each
chunk is independent, which is exactly the workload `fire` was built for.

```dragon
# checksum.dr - fan out independent checksum jobs, aggregate the results.

# One unit of work: a deterministic rolling checksum over a chunk.
def checksum(chunk_id: int, size: int) -> int {
    h: int = 2166136261
    for i in range(size) {
        h = (h * 16777619 + (i + chunk_id)) % 4294967296
    }
    return h
}

const chunks: list[int] = [500, 750, 1000, 1250, 1500, 1750]

# Fan out: one green thread per chunk. fire returns a Task[int] now;
# the work runs on the M:N scheduler.
tasks: list[Task[int]] = []
for cid in range(len(chunks)) {
    t: Task[int] = fire checksum(cid, chunks[cid])
    tasks.append(t)
}

# Collect: await each handle in turn. Order of results follows the
# task list, regardless of which green thread finished first.
combined: int = 0
done: int = 0
for t in tasks {
    combined = (combined + await t) % 4294967296
    done = done + 1
}

print(f"jobs={done} combined={combined}")
```

Run it and you get a single deterministic line:

```bash
$ dragon run checksum.dr
jobs=6 combined=1649988601
```

Six chunks, six green threads, one aggregated digest. The structure is
the same fan-out/collect skeleton from before, dressed in realistic work:
`checksum` is the unit of work, the first loop fires all six jobs, the
second folds their results into `combined` with a running modulus. Swap
`checksum` for a real `open(p).text()` (see
[Files and the Filesystem](/docs/1402-stdlib-io)) or a database query (see
[Databases](/docs/1301-databases)) and nothing about the concurrency
plumbing changes - that's the payoff of keeping the work behind a plain
`def`.

### Checking whether a task is still running

Sometimes you want to poll rather than block. A `Task[T]` exposes
`.is_alive()` (a `bool`) alongside `.join()`:

```dragon
def work(n: int) -> int {
    return n * n
}

# .join() is the explicit form; await is the same value but yields.
t: Task[int] = fire work(9)
r: int = t.join()
print(r)                # 81

# is_alive() reports whether the green thread is still running.
t2: Task[int] = fire work(5)
v: int = await t2
print(t2.is_alive())    # False
```

This prints `81` then `False`. After you've collected a task's result
(via `await` or `.join()`), `is_alive()` reports `False` - the green
thread has finished. `.join()` and `await` are interchangeable for the
*value*; the difference is only in scheduling behavior (`await` yields a
green-thread awaiter; `.join()` is the plain blocking name). Use `await`
in concurrent code, reach for `.is_alive()` when you genuinely want to
ask "are you done?" without waiting.

## Tier 2: a CPU-bound block on a real OS thread

Green threads are M:N-scheduled coroutines: thousands of them share a few
OS threads, and they shine when work *waits* (I/O) so the scheduler can
overlap it. But a tight CPU loop that never yields doesn't benefit from
green-threading it - it benefits from running on its *own hardware
thread*, in parallel with the rest. That's tier 2: `thread { }`, a real
OS thread that **auto-joins at the end of its enclosing scope**.

We can bolt a verification pass onto the checksum runner. While the
green-thread results aggregate, a `thread { }` block sums the chunk sizes
on a separate OS thread, and the RAII join guarantees its result is ready
before we print:

```dragon
def checksum(chunk_id: int, size: int) -> int {
    h: int = 2166136261
    for i in range(size) {
        h = (h * 16777619 + (i + chunk_id)) % 4294967296
    }
    return h
}

const chunks: list[int] = [500, 750, 1000, 1250, 1500, 1750]
tasks: list[Task[int]] = []
for cid in range(len(chunks)) {
    t: Task[int] = fire checksum(cid, chunks[cid])
    tasks.append(t)
}
combined: int = 0
for t in tasks {
    combined = (combined + await t) % 4294967296
}

# While the green-thread results are aggregated above, a CPU-bound
# verification pass runs on a real OS thread and joins before we print.
verify: list[int] = []
thread {
    acc: int = 0
    for c in chunks {
        acc = acc + c
    }
    verify.append(acc)
}
# <- the OS thread has joined here, guaranteed.

print(f"combined={combined} bytes={verify[0]}")
```

This prints:

```bash
$ dragon run checksum.dr
combined=1649988601 bytes=6750
```

The closing brace of the `thread { }` block is a synchronization point:
control does not move past it until the OS thread is done. That's why
reading `verify[0]` on the next line is safe - by the time `print` runs,
the thread has joined and `verify` holds its result. No explicit
`join()`, no `WaitGroup`, no forgotten handle. It's RAII for hardware
threads, the same way a `with` block is RAII for a file (see
[Files and the Filesystem](/docs/1402-stdlib-io)).

Two practical rules for tier 2:

- **Reach for `thread { }` when the work is CPU-bound and you want true
  parallelism** - image processing, hashing, a number-crunching pass.
  For waiting work (network, disk, a DB round-trip), `fire` is better:
  the scheduler can overlap thousands of waiting green threads on a
  handful of OS threads, where one OS thread per job would exhaust the
  machine.
- **The block auto-joins; you cannot get a return value out of it
  directly.** Write results into a binding the enclosing scope can read
  after the join - here, appending to the `verify` list. If you need a
  typed return value and explicit lifecycle control, that's tier 3, the
  manual `Thread` class.

## Guarding shared state with a `Lock`

The fan-out/collect pattern above never touches the same variable from
two threads at once - each task returns its own value and the *collecting*
loop is single-threaded. That's the cleanest design, and you should
prefer it. But when two threads genuinely must mutate the same state,
serialize the critical section with a `Lock`. Unlike `Task`, a `Lock`
requires an explicit import (Python parity - `from threading import Lock`):

```dragon
from threading import Lock

lock: Lock = Lock()
lock.acquire()
n: int = 41
n = n + 1
lock.release()
print(n)            # 42

with lock {
    n = n + 1
}
print(n)            # 43
```

This prints `42` then `43`. `Lock` follows the Python `threading.Lock`
surface: `acquire()` blocks until the lock is free, `release()` hands it
back, and a `with lock { }` block acquires on entry and releases on exit
even if the block raises. The `with` form is the one to reach for - it
can't leak a held lock. One caution from the runtime design: **don't hold
a `Lock` across an `await` or a blocking I/O call** in a green thread -
parking the thread while holding the lock can deadlock the others waiting
on it. Keep critical sections short and lock-free of suspension points.

## When to use which

| You want… | Use | Returns | Joins |
|---|---|---|---|
| Many cheap, possibly-waiting jobs, overlapped | `fire f()` | `Task[T]` | `await t` / `t.join()` |
| The value of a fired job | `await t` | `T` | yields if awaiter is a green thread |
| To poll a fired job | `t.is_alive()` | `bool` | - |
| A one-shot CPU-bound block, parallel | `thread { ... }` | nothing (write to outer binding) | automatically, at scope exit |
| A long-lived thread with manual lifecycle | `Thread(target=fn)` | `int` from `.join()` | `t.start()` / `t.join()` |
| To serialize a shared mutation | `from threading import Lock` | - | `with lock { }` |

The decision tree is short. Default to `fire` and the fan-out/collect
pattern - it covers the overwhelming majority of concurrent work and
costs almost nothing per job. Drop to `thread { }` when a block is pure
CPU and you want a second core working on it now, with a guaranteed join.
Reach for the manual `Thread` class (covered in
[Concurrency](/docs/1101-green-threads)) only when you need a thread to
outlive the scope that spawned it. And add a `Lock` only when shared
mutable state is unavoidable - most of the time, returning a value from
each task and combining them in one place is simpler *and* faster.

Every example in this chapter was compiled and run with the real
compiler; the outputs shown are the outputs observed. The patterns are
small on purpose - concurrency in Dragon is meant to be something you
reach for without ceremony, not a subsystem you architect around.

