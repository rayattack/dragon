# Synchronization

The moment two [OS threads](/docs/1103-os-threads) touch the same data, you need to
coordinate them - or a read-modify-write on one thread interleaves with the other
and updates vanish. The `threading` module provides the usual primitives, and every
lock here is also a **context manager**, so `with` acquires it on entry and releases
it on exit even if the body raises.

## `Lock` - the workhorse

`Lock` guards a critical section: only one thread holds it at a time. It needs the
import (`from threading import Lock`), and the idiomatic pattern wraps the shared
state in a class with a field holding the `Lock` that guards it - keeping the data
and its lock together makes the invariant obvious:

```dragon
from threading import Lock, Thread

class Counter {
    value: int = 0
    lock: Lock = Lock()

    def bump() -> None {
        with self.lock {
            self.value = self.value + 1   # only one thread in here at a time
        }
    }
}

c: Counter = Counter()

def work() -> None {
    i: int = 0
    while i < 1000 {
        c.bump()
        i = i + 1
    }
}

a: Thread = Thread(target=work)
b: Thread = Thread(target=work)
a.start()
b.start()
a.join()
b.join()
print(c.value)                            # 2000 - every bump is accounted for
```

Both threads run `work()`, each bumping a thousand times. Without the lock the two
read-modify-write sequences would interleave and lose updates; with it, the total
is always exactly `2000`. If the guarded state is a module global rather than a
field, mark the write with `global` (a bare assignment to a module global from
inside a function is a compile error, not a silent local):

```dragon
from threading import Lock

lock: Lock = Lock()
counter: int = 0

def bump() -> None {
    global counter
    with lock {
        counter = counter + 1
    }
}
```

### Acquiring without `with`

Most code should use `with` - it can't leak the lock. When you need finer control -
a non-blocking attempt, or acquiring in one place and releasing in another - call the
methods directly. `acquire()` blocks until the lock is free; `acquire(blocking=False)`
tries **once** and returns whether it got the lock, so you can do other work instead
of waiting:

```dragon
from threading import Lock

lock: Lock = Lock()

if lock.acquire(blocking=False) {     # try once, never wait
    # got it - do the guarded work, then hand it back
    lock.release()
} else {
    # someone else holds it; skip the work or retry later
}
```

`acquire(blocking=True, timeout=T)` waits up to `T` seconds and returns `False` if it
still couldn't get the lock. `with lock { ... }` is exactly `acquire()` on entry and
`release()` on exit, made exception-safe.

## The other primitives

Each mirrors its Python counterpart and is imported from `threading`. They all
**acquire the same way** - `acquire()` to block, `acquire(blocking=False)` to try once,
`acquire(blocking=True, timeout=T)` to wait at most `T` seconds (returns `False` if it
couldn't be taken in time) - so the shape you learned for `Lock` carries over unchanged:

| Primitive | Construct | Key methods | Use it for |
|-----------|-----------|-------------|------------|
| `Lock` | `Lock()` | `acquire(blocking=…, timeout=…)` / `release()` / `with lock { ... }` | mutual exclusion (one holder) |
| `RWLock` | `RWLock()` | `acquire(write=…, blocking=…, timeout=…)` / `release()` | many readers **or** one writer |
| `Semaphore` | `Semaphore(n)` | `acquire(blocking=…, timeout=…)` / `release()` | bound concurrency to `n` permits |
| `Barrier` | `Barrier(n)` | `wait()` | rendezvous: all `n` threads meet before any proceeds |
| `Condition` | `Condition()` | `acquire()` / `wait()` / `notify()` / `notify_all()` / `release()` | wait for a state change |
| `Event` | `Event()` | `set()` / `clear()` / `is_set()` / `wait()` | a one-shot/broadcast flag |

An `RWLock` lets **many readers** share access **or** give **one writer** exclusive
access. `acquire()` takes the shared read lock; `acquire(write=True)` takes the
exclusive write lock - one verb, one `write` flag:

```dragon
from threading import RWLock

rw: RWLock = RWLock()

rw.acquire()                 # shared read lock - other readers may join
# ... read the shared state ...
rw.release()

rw.acquire(write=True)       # exclusive write lock - blocks all other access
# ... mutate the shared state ...
rw.release()
```

A `Semaphore` caps how many threads run a section at once - acquire a permit before,
release it after:

```dragon
from threading import Semaphore

sem: Semaphore = Semaphore(2)        # at most 2 at a time
sem.acquire()
# ... bounded work ...
sem.release()
```

An `Event` is a flag threads can wait on - one thread sets it, others proceed:

```dragon
from threading import Event

ready: Event = Event()
print(ready.is_set())    # False
ready.set()
print(ready.is_set())    # True
```

`RWLock`, `Semaphore`, and `Condition` are also context managers - `with rwlock { ... }`
acquires and releases for you, the same as `Lock` (a `with` on an `RWLock` takes the
exclusive write lock).

## Thread-safe collections

When the shared state is a whole container, a lock around every access is tedious.
The `collections.concurrent` module ships two ready-made thread-safe containers -
import them from there:

```dragon
from collections.concurrent import ConcurrentList, ConcurrentDict

cl: ConcurrentList = ConcurrentList()
cl.append(10)
cl.append(20)
print(cl.len())       # 2
print(cl.get(0))      # 10

cd: ConcurrentDict = ConcurrentDict()
cd.set("hits", 1)
print(cd.get("hits")) # 1
```

These are deliberately **monomorphic and minimal**: `ConcurrentList` holds `int`
elements and `ConcurrentDict` maps `str` keys to `int` values, with method-style
access (`append`/`get`/`set`/`len`/`pop`/…) rather than `[]` syntax - every
operation takes the internal lock so concurrent callers stay consistent. For
container shapes they don't cover, guard an ordinary `list`/`dict` with a `Lock` as
in the `Counter` example.

## At a glance

| You want to... | Write |
|----------------|-------|
| Mutual exclusion | `from threading import Lock`; `with lock { ... }` or `acquire()` / `release()` |
| Many readers / one writer | `RWLock()` - `acquire()` / `acquire(write=True)` |
| Cap concurrency to N | `Semaphore(n)` - `acquire()` / `release()` |
| Rendezvous N threads | `Barrier(n)` - `wait()` |
| Wait for a state change | `Condition()` - `wait()` / `notify()` |
| A broadcast flag | `Event()` - `set()` / `wait()` / `is_set()` |
| A thread-safe int list / str→int map | `from collections.concurrent import ConcurrentList, ConcurrentDict` |

That completes the concurrency model - three colorless tiers and the primitives to
coordinate them. Next, a different kind of building block:
[Templates](/docs/1201-templates), Dragon's typed, auto-escaping approach to
generating HTML, SQL, and other structured text.
