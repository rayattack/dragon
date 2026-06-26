# The Memory Model

Every language makes a deal with you about memory. C hands you `malloc`
and `free` and trusts you never to slip - one mistake is a leak, a
double-free, or a security bug. Java and Go take the keys away entirely
and run a garbage collector that pauses your program at times it chooses,
not times you choose. Rust threads a third needle with a borrow checker
that proves at compile time when a value can be freed, at the cost of a
type system you have to argue with.

Dragon's deal is closer to C++'s `shared_ptr` or Swift's ARC than to any
of those: **automatic reference counting, with frees that happen at
deterministic points you can predict from the source** - plus a cycle
collector so the one weakness of pure refcounting never bites you. You
never write `free`. You never wait for a collector to decide. And on the
hot path there is no stop-the-world pause at all.

This chapter is about the *observable* model - what you can rely on as a
Dragon programmer. The exact runtime mechanics are an implementation
detail that improves release to release; the guarantees below do not.

## Values versus references

The first thing to know is what even *has* a memory cost. Dragon's
scalar types - `int`, `float`, `bool` - are not heap objects. They flow
in machine registers, are copied by value, and cost nothing to "free"
because there is nothing to free. Assigning one `int` to another copies
the number.

The heap types - `str`, `list`, `dict`, `set`, `tuple`, class instances,
closures - are *references*. A variable holds a handle to the object, and
assigning it copies the handle, not the object. Both names then see the
same thing, exactly as in Python:

```dragon
a: list[int] = [1, 2, 3]
b: list[int] = a          # b and a refer to the same list
b.append(4)
print(a)                  # [1, 2, 3, 4] - a sees it too
print(len(a) == len(b))   # True
```

This is reference semantics, and it is why the memory model matters: when
two names share an object, *which* of them is responsible for freeing it?
The answer is neither and both - the object frees itself when the last
reference goes away.

## Reference counting, deterministically

Each heap object carries a count of how many references point at it. The
count goes up when you bind a new name to the object and down when a name
stops referring to it - goes out of scope, is reassigned, or is the
temporary result of an expression that has been consumed. When the count
reaches zero, the memory is reclaimed **immediately**, on that line, not
"eventually."

That immediacy is the property worth internalizing. Because
[every block is its own scope](/docs/0201-variables), a heap value
declared inside a block is freed at the closing brace of that block - the
moment it can no longer be reached. There is no nursery to fill, no
heap-occupancy threshold to cross, no collector thread to wake. The free
is part of the straight-line control flow, as predictable as the `return`
next to it.

You can *see* the determinism through a context manager, whose `__exit__`
is Dragon's hook for "this resource's scope has ended":

```dragon
class Scope {
    def(name: str) {
        self.name = name
        print(f"enter {self.name}")
    }
    def __enter__() -> Scope {
        return self
    }
    def __exit__() -> None {
        print(f"free {self.name}")
    }
}

def work() -> None {
    with Scope("inner") as s {
        print("working")
    }
    print("after block")
}

work()
```

```
enter inner
working
free inner
after block
```

`free inner` prints *before* `after block`, every run, deterministically.
That ordering is the whole memory model in miniature: cleanup is bound to
scope, and scope is something you read straight off the page. (See
[Context Managers](/docs/0903-context-managers) for the full `with`
story.)

## Cleanup survives exceptions

A reference-counted value that goes out of scope because an exception is
unwinding the stack is still freed - the runtime walks the owned locals
of each frame it unwinds and releases them on the way out. So an error
path does not leak:

```dragon
def risky(crash: bool) -> None {
    data: list[int] = [1, 2, 3]   # heap-allocated here
    if crash {
        raise ValueError("boom")  # `data` is still freed as the stack unwinds
    }
    print(len(data))
}

def main() -> None {
    try {
        risky(true)
    } except ValueError as e {
        print(f"caught: {str(e)}")
    }
}

main()
```

```
caught: boom
```

Whether `risky` returns normally or raises, `data` is released. You get
the convenience of exceptions without the leak that a manual `free` at
the bottom of the function would have skipped on the error path. (More on
exceptions in [Exceptions](/docs/0901-exceptions).)

## The one weakness, and the fix

Reference counting has a single, well-known blind spot: **cycles**. If
object A holds a reference to B and B holds one back to A, neither
count ever reaches zero, even after every outside reference is gone. Pure
refcounting would leak them forever.

```dragon
class Node {
    def() {
        self.next = none
    }
}

def make_cycle() -> None {
    a: Node = Node()
    b: Node = Node()
    a.next = b
    b.next = a          # a <-> b: a reference cycle
    # both go out of scope here with non-zero counts
}

make_cycle()
print("no leak")
```

```
no leak
```

Dragon closes the gap with a **cycle collector** that periodically finds
and reclaims unreachable cycles - the same idea CPython uses behind its
own refcounting. It is a backstop, not the primary mechanism: the vast
majority of objects are acyclic and die the instant their count hits
zero, with no collector involvement at all. The cycle collector only
earns its keep on the genuine cycles, and it runs incrementally rather
than as a generational stop-the-world sweep. There is no pause where your
whole program freezes while a collector traces the live heap.

## When you want the counting gone

Reference counting is not free - each share and release is a small
bookkeeping cost, and it is the dominant overhead on allocation-churning
workloads (see [Performance](/docs/1603-performance)). For a short-lived
program that allocates and exits - a build script, a one-shot CLI filter,
a micro-benchmark - you can turn refcount emission off entirely:

```bash
dragon build tool.dr --gc=none
```

With `--gc=none` the program never spends a cycle on counts; it simply
lets the operating system reclaim everything when the process exits. This
is a deliberate, narrow tool: **do not ship a long-running service built
this way**, because without the counting nothing is ever freed mid-run
and memory grows without bound. The default, `--gc=rc`, is what you want
for everything that runs longer than a blink.

## What this buys you

The deal, restated as guarantees you can build on:

- **No manual frees, ever.** You never write `free`; you cannot
  double-free or use-after-free a Dragon value through the safe surface.
- **Deterministic lifetimes.** A heap value dies at a point you can read
  off the source - block exit, reassignment, last use - not whenever a
  collector wakes.
- **No hot-path pauses.** Counting is incremental and local; there is no
  global stop-the-world phase. The cycle collector is a periodic backstop
  for the rare cyclic garbage, not a tax on every allocation.
- **Leak-free error paths.** Exceptions unwind and free owned locals as
  they go.

It is the predictability of manual memory management without the
foot-guns, and the safety of a garbage collector without the pauses -
which is exactly what the first commandment, *speed is king*, demands of
the runtime. The next chapter, [How Dragon Compiles](/docs/1602-how-dragon-compiles),
shows the other half of that story: how the compiler arranges for these
values to flow at machine speed in the first place.
