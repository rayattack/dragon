# Iterators and Generators

A `for` loop in Dragon is not a built-in that only works on lists and dicts -
it speaks a small protocol, and anything that satisfies the protocol drops
into a `for` loop. There are two ways to produce something iterable: write a
**generator** (a function that pauses on `yield`), or implement the
**iteration protocol** (`__iter__` / `__next__`) on a class. This chapter
covers both, and why a generator is just the first spelled with less ceremony.

## Generators

A function that contains `yield` is a *generator*. Calling it does not run the
body - it returns a lazy object that produces values one at a time, each
`yield` pausing the function until the next value is pulled:

```dragon
def count_up(limit: int) {
    i: int = 0
    while i < limit {
        yield i
        i = i + 1
    }
}

for v in count_up(4) {
    print(v)                    # 0 1 2 3, one per line
}
```

State persists across yields, which makes generators a clean way to express
sequences without materializing a list. The Fibonacci numbers, lazily:

```dragon
def fib(n: int) {
    a: int = 0
    b: int = 1
    count: int = 0
    while count < n {
        yield a
        next: int = a + b
        a = b
        b = next
        count = count + 1
    }
}

for x in fib(8) {
    print(x)                    # 0 1 1 2 3 5 8 13
}
```

Generators yield any type - drive one off a `list` parameter and transform as
you go:

```dragon
def shout(words: list[str]) {
    for w in words {
        yield f"{w}!"
    }
}

for w in shout(["one", "two", "three"]) {
    print(w)                    # one! two! three!
}
```

Generators are designed to be consumed by `for`. That is the supported
mechanism: there is **no `next()` builtin**, and a generator object can only
be bound to an `Any`-typed variable (`g: Any = count_up(4)`), so the idiomatic
form is to iterate the call directly - `for v in count_up(4)`. You can `yield`
the result of an expression - `for w in text.split(" ") { yield w.upper() }`
yields each transformed value - just as you can `yield` a parameter or a
literal.

## The iteration protocol

`for` is not magic - it speaks a protocol. Any object with an `__iter__`
returning an iterator and a `__next__` that produces values (raising
`StopIteration` when exhausted) is iterable. Implement the two methods and
your own class drops into a `for` loop like any built-in container:

```dragon
class Countdown {
    n: int
    def(start: int) {
        self.n = start
    }
    def __iter__() -> Countdown {
        return self
    }
    def __next__() -> int {
        if self.n <= 0 {
            raise StopIteration()
        }
        v: int = self.n
        self.n = self.n - 1
        return v
    }
}

for x in Countdown(3) {
    print(x)                    # 3 2 1
}
```

This is the same protocol generators implement under the hood - a generator
is just a compiler-synthesized class that satisfies it. Reach for an explicit
`__iter__`/`__next__` class when iteration needs object state or multiple
independent passes; reach for a generator when a single linear sweep with
local variables says it more directly.

## At a glance

| You want to... | Write |
|----------------|-------|
| A lazy sequence | a generator: `def g() { yield ... }` |
| Yield a transformed value | `yield f"{w}!"` (any expression) |
| Consume a generator | `for v in g() { ... }` (no `next()`) |
| Hold a generator object | `g: Any = count_up(4)` (only `Any`-typed) |
| Make a class iterable | `__iter__` returns the iterator, `__next__` yields values |
| Signal exhaustion | `raise StopIteration()` from `__next__` |

That closes out functions and the values built on them. The next part changes
gears entirely: [concurrency](/docs/1101-green-threads) - green threads, real OS
threads, and an `await` that never colors your functions.
