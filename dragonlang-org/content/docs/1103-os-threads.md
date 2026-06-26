# Scoped and Manual OS Threads

[Green threads](/docs/1101-green-threads) are the right default - cheap, numerous,
and great at I/O. But some work wants a *real* OS thread: a CPU-bound computation
that should run on its own hardware core, or a long-lived worker whose lifetime you
manage by hand. Dragon's other two tiers cover exactly those: `thread { }` for a
scoped thread that joins itself, and the `Thread` class for one you control.

## Scoped OS threads: `thread { }`

When you need true hardware parallelism for a CPU-bound chunk of work, use a
`thread { }` block:

```dragon
thread {
    compute_heavy_task()
    write_results_to_disk()
}
# the thread has joined here - guaranteed
```

`thread` is a contextual keyword. The block becomes a real OS thread, and Dragon
inserts the join **automatically** at the end of the enclosing scope. You cannot
forget to join it - when control leaves the scope, the thread is done. This is RAII
for hardware threads.

Launch several and let them all join together at the scope boundary:

```dragon
thread { process_batch_a() }
thread { process_batch_b() }
# both have joined before execution continues past this point
```

You can even `await` inside a `thread { }` block, mixing tiers freely - a green
thread running on top of the scoped OS thread:

```dragon
async def stage() -> None {
    print("runs on a green thread")
}

thread {
    await stage()
}
```

## Manual threads: the `Thread` class

For a thread whose lifetime you control - a long-running daemon, a worker you start
and join at different points - use the `Thread` class from the `threading` module.
Its API matches Python's `threading.Thread`:

```dragon
from threading import Thread

def worker() -> None {
    print("working")
}

t: Thread = Thread(target=worker)
t.start()
# ... do other things ...
if t.is_alive() {
    print("still running")
}
t.join()
```

`Thread` gives you three methods: `start()` launches it, `is_alive()` reports
whether it's still running, and `join()` waits for it to finish. Note that
`Thread.join()` returns a status code (an `int`), **not a value** - if you want a
result back from concurrent work, that's exactly what the green-thread `Task[T]` is
for. OS threads are for *running* work in parallel; green-thread tasks are for
*collecting a typed result*.

## Choosing a tier

```text
Need a typed result back?           → green thread: t: Task[T] = fire work()
A CPU burst, no result, no fuss?    → thread { ... }   (auto-joins at scope exit)
A worker you start/stop yourself?   → Thread(target=fn)
```

## At a glance

| You want to... | Write |
|----------------|-------|
| A CPU burst that auto-joins | `thread { ... }` |
| Several parallel bursts | multiple `thread { }` blocks in a scope |
| A thread you control | `t: Thread = Thread(target=fn)` |
| Start / check / wait | `t.start()` / `t.is_alive()` / `t.join()` |

Real OS threads share memory, which means coordinating their access to shared data.
That's the last chapter of this part: [Synchronization](/docs/1104-synchronization)
- locks, semaphores, and the rest.
