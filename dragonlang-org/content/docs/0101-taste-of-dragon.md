# A Taste of Dragon

You have the `dragon` binary on your `PATH` (if not, see
[Installation](/docs/0003-installation)). This chapter is the thirty-four-thousand-foot tour - it captures five small programs that show what the language actually feels like to write, and what you get for free that you don't get elsewhere. Every one of them compiles to a standalone native binary.

We move fast here and explain little; the rest of the book fills in the *why*.
Think of this as the trailer, not the manual.

## Run something

Open an editor, create `greet.dr`, and write a typed function:

```dragon
def greet(name: str) -> str {
    return f"Hello, {name}!"
}

print(greet("Dragon"))
```

Run it:

```
$ dragon run greet.dr
Hello, Dragon!
```

That's the whole program - its **top-level statements** are the program body.
There is no `main`, no boilerplate, no `if __name__ == "__main__"`. The file you
hand to `dragon` *is* the entry point.

`dragon run` compiled `greet.dr` to a native binary and executed it. When you
want the artifact itself - to ship it, to time it - build it:

```
$ dragon build greet.dr -o greet
$ ./greet
Hello, Dragon!
$ time ./greet
Hello, Dragon!

real    0m0.001s
```

No interpreter, no VM, no JIT warm-up. A Dragon program starts in about a
millisecond and runs at the speed of compiled C. The types you wrote (`name: str`, `-> str`) were checked at compile time and then *erased* - at runtime
there is no boxing, no tag-checking, no dictionary lookup hiding behind that
function call.

## A web server, whole

Here is an entire HTTP server. Not a snippet of one - the whole thing:

```dragon
from http.server import Router, Request, Response, Context

app: Router = Router(8000, "127.0.0.1")

app.GET("/", lambda (req: Request, res: Response, ctx: Context) -> None {
    res.text("Hello from Dragon!")
})

app.GET("/hi/:name", lambda (req: Request, res: Response, ctx: Context) -> None {
    res.text(f"Hello, {req.params['name']}!")
})

app.listen()
```

Build it and talk to it:

```
$ dragon build server.dr -o server && ./server &
$ curl http://127.0.0.1:8000/
Hello from Dragon!
$ curl http://127.0.0.1:8000/hi/Ada
Hello, Ada!
```

The router, the HTTP/1.1 parser, path parameters, sessions, gzip, static files,
even the WebSocket codec - the **entire web stack ships in the standard library,
written in Dragon**. There is no third-party framework to assemble and no C
server hiding under the hood; the only native code is the socket syscalls. When
your service misbehaves, the call stack stays inside the language you wrote it
in. This site, dragonlang.org, runs on exactly this `http.server`.

The full story - routing, forms, a SQLite-backed leaderboard with the
cross-site-scripting hole closed - is in
[Building a Web Application](/docs/1701-web-application).

## Concurrency without colors

Most languages make you pick a side: cheap tasks with a viral `async`/`await`
that splits every function into two colors, or real OS threads that are heavy
and few. Dragon refuses the trade.

Start work with `fire`. It spawns a **green thread** - an M:N-scheduled
coroutine cheap enough to launch by the thousand - and hands you a `Task[T]`:

```dragon
def slow_square(n: int) -> int {
    return n * n
}

a: Task[int] = fire slow_square(3)
b: Task[int] = fire slow_square(4)
c: Task[int] = fire slow_square(5)

print(a.join() + b.join() + c.join())   # 50
```

All three run concurrently; `join()` collects each result, already typed.

The part Python can't do: `**await` works in any function.** It does not
"infect" callers, and it never changes a signature.

```dragon
async def fetch(url: str) -> str {
    return f"data from {url}"
}

# show() is NOT async - yet it can await. No coloring, no ripple upward.
def show() -> None {
    result: str = await fetch("example.com")
    print(result)
}

show()
```

In Python, one `async def` at the bottom forces every caller above it to become
`async` too, all the way up to `asyncio.run`, and you end up maintaining two of
everything (`read` and `async_read`, `Lock` and `AsyncLock`). Dragon keeps the
`await` keyword - so a reader can *see* where a function might pause - but it
carries no obligation upward. There is no green/red function split. Full
treatment in [Concurrency](/docs/1101-green-threads).

## Real threads, shared safely

When you want true hardware parallelism, a `thread { }` block becomes a real OS
thread that **joins automatically** at the end of its scope - RAII for threads,
so you cannot forget:

```dragon
def heavy(label: str) -> None {
    total: int = 0
    i: int = 0
    while i < 1000000 {
        total = total + i
        i = i + 1
    }
    print(f"{label} sum={total}")
}

thread { heavy("A") }
thread { heavy("B") }
print("both threads joined")
```

Both threads run in parallel and have finished before that last line prints.

When threads share mutable state, reach for a `Lock` - it is a context manager,
so `with` releases it even if the body raises:

```dragon
from threading import Lock, Thread

class Counter {
    value: int = 0
    own lock: Lock = Lock()    # own: the Counter owns (and destroys) its Lock

    def bump() -> None {
        with self.lock {
            self.value = self.value + 1     # one thread in here at a time
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

t1: Thread = Thread(target=work)
t2: Thread = Thread(target=work)
t1.start()
t2.start()
t1.join()
t2.join()
print(c.value)   # 2000 - no lost updates
```

Two threads, a thousand increments each, exactly `2000` at the end. Drop the
lock and you'd see fewer - the lock is doing real work. Green threads (`fire`),
scoped OS threads (`thread { }`), and manual `Thread`s all share one scheduler
and the same synchronization primitives.

## What you just saw

In five short programs: a typed function compiled to native code, a complete web
server from the standard library, parallel work with no function coloring, and
shared state across real OS threads - locked correctly. No framework downloads,
no VM, no runtime daemon. Just `dragon build` and a binary.

That's the pitch. The rest of the book earns it. The next chapter introduces the
`dragon` command-line tool - `run`, `build`, `check`, and friends - and from
there we start at the beginning: variables, types, and control flow.