# Ownership: del, own, and dub

[The Memory Model](/docs/1601-memory-model) explained Dragon's deal: automatic
reference counting with frees at deterministic points you can predict from the
source. This chapter is about three keywords that let you *name* those points
and have the compiler hold you to them: `del` ends a value's life early, `own`
transfers a value's ownership, and `dub` copies a value so a second owner can
exist. All three resolve entirely at compile time. There are no finalizers, no
destructor methods, no runtime flags; every way to get one of them wrong is a
compile error naming a line, never a change in runtime behavior.

They are also strictly opt-in. A program that uses none of them compiles
exactly as before. The keywords are annotations you add where you care, and
strictness appears only where you wrote them.

One rule sits underneath all three: **every heap value has exactly one owner.**
Everything else that names the value is a borrow - free, implicit, and not
allowed to outlive the owner. When one owner is not enough, you say which
escape hatch you mean: `own` (transfer it, free) or `dub` (copy it, costed).
The compiler never guesses between them.

## The five states

At every point in a function, each local binding is in exactly one of five
states. This is the whole machine; everything else in the chapter is the rules
for moving between them.

| State | Meaning |
|---|---|
| `Owned` | this binding is the value's single owner |
| `Owned*` | still the single owner, but the value has escaped somewhere; `del` and `own`-transfer refuse |
| `Borrowed` | names a value owned elsewhere; free, may not outlive the owner |
| `Moved` | ownership was transferred out; the name is dead |
| `Deleted` | released by `del`; the name is dead |

A quick tour of all five:

```dragon
def states_tour(p: str) {
    a: str = read_all(p)   # a: Owned     - fresh call result, 'a' is the sole owner
    b: str = a             # b: Borrowed  - names a value owned elsewhere
                           # a: Owned*    - a fact is recorded: "aliased as 'b' at line 3"

    c: str = read_all(p)   # c: Owned
    fire work(own c)       # c: Moved     - ownership crossed into the green thread

    d: str = read_all(p)   # d: Owned
    del d                  # d: Deleted   - released right here, the name is dead
}
```

What creates which state is intuitive once you ask "did this expression mint a
fresh value, or hand me a view of an existing one?" Fresh values are owned;
views are borrowed:

```dragon
def creation(xs: list[str], d: dict[str, str], r: Router) {
    a: str = "hi" * 3             # fresh result (concat)   -> Owned
    b: SSLContext = SSLContext()  # constructor             -> Owned
    c: dict[str, str] = dub d     # explicit copy           -> Owned
    e: str = xs[0]                # element read            -> Borrowed
    f: str = d["k"]               # dict read               -> Borrowed
    g: str = r.host               # field read              -> Borrowed
    h: str = a                    # alias of a local        -> Borrowed (and a becomes Owned*)

    # xs, d, and r themselves, plain parameters, are Borrowed:
    # the caller still owns them
}
```

Reading a dead name is a compile error, and `Moved` and `Deleted` behave
identically; they differ only in what the diagnostic says:

```dragon
job: Job = make_job()
fire run(own job)
print(job.id)          # error E1: 'job' was moved into 'fire run' at line 2

buf: str = read_all(p)
del buf
n: int = len(buf)      # error E2: 'buf' was deleted at line 2
```

## del - the proven early free

`del x` releases a value at the exact line you write it instead of at scope
exit. It compiles only when the compiler can prove `x` is the value's sole
owner at that point: nothing stored it, nothing captured it, nothing else
names it. When the proof holds, the free is safe by construction - there is no
other reference left to dangle.

```dragon
def digest_file(path: str) -> str {
    buf: str = read_all(path)   # 200MB, would otherwise live to scope exit
    d: str = sha256(buf)
    del buf                     # proven unique -> freed HERE
    # ... the rest of the function runs without holding 200MB
    return d
}
```

Note what made that legal: a **plain call argument does not change a binding's
state**. `sha256(buf)` borrows `buf` for the duration of the call and hands it
back. If plain calls consumed their arguments, `del` would be unusable.

When the proof fails, the error names the escape site, and this is where `del`
earns its keep as a diagnostic tool:

```dragon
cache: dict[str, str] = {}

buf: str = read_all(p)
cache[p] = buf
del buf            # error E5: 'buf' escaped into 'cache[p]' at line 4
```

You believed `buf` was yours alone; the compiler just showed you the alias you
forgot. Sprinkle `del` at the points where you believe lifetimes end, and every
refusal is a leak-shaped escape you did not know about - a leak map generated
by the type checker.

The full set of refusals:

```dragon
first: str = xs[0]     # Borrowed
del first              # error E4: 'first' is not the owner (it borrows xs[0]);
                       # only the sole owner can be deleted or moved

x: str = read_all(p)
probe: Callable[[], int] = lambda () -> int { return len(x) }
del x                  # error E7: 'x' is captured by a closure at line 2

del self._ctx          # error E3: a field's lifetime is dynamic;
                       # own fields release when the owner dies (see below)
```

That last one matters: `del` works on local bindings only, and module-level
`del` of a binding is refused too. A field's lifetime is dynamic, which is
precisely the ambiguous case that must be an error rather than a guess.
(`del d["key"]`, deleting a container *entry*, is a different operation and
is unaffected.)

Two more properties worth knowing:

- Same shape as Python's `del`, strictly stronger meaning. Python unbinds the
  name and the object quietly lives on if anything else references it. Dragon
  refuses to compile the ambiguous case instead of silently doing less than
  the reader expects.
- In debug builds, `del` lowers to `assert(rc == 1); free`. The compiler
  proved the count must be one; if the runtime disagrees, some code path
  leaked a reference, and the program aborts at the exact `del` line instead
  of leaking silently. `del` doubles as an executable assertion of the
  compiler's own ownership model. In release builds it is byte-for-byte the
  release scope exit would have performed, just earlier.

## own on fields - resources that die with their owner

Declaring a field `own` makes the instance the field's sole owner: the value
cannot be stored as anyone else's owned reference, and when the instance dies,
the compiler-generated cleanup releases the field. No destructor method, no
close-path to forget, no user code runs at death.

The motivating example is a TLS context, which holds tens of kilobytes of
engine state in a raw handle:

```dragon
class SSLContext {
    own _ctx: TlsCtx       # sole owner: released exactly once, when the context dies

    def(protocol: int = PROTOCOL_TLS_CLIENT) {
        self._ctx = tls_ctx_new(protocol)   # a fresh value transfers its +1 into the field
    }
}
```

Without `own`, this class needs a `_closed` flag, an idempotent `close()`
method, and the discipline to call it on every path - and the failure mode is
silent (the handle leaks). With `own`, the release is generated, and the bug
class is unwritable.

Owners compose. When an object holding `own` fields dies, the fields release
depth-first:

```dragon
class Router {
    own _storage_lock: Lock
    own _ssl_context: SSLContext
}
# router dies -> generated cleanup, depth-first:
#   the Lock's OS primitive is destroyed
#   the SSLContext is released -> ITS cleanup frees the TLS engine handle
# a deterministic chain of custody, with no finalizer anywhere in it
```

Reads and reassignment behave the way the one-owner rule predicts:

```dragon
h: TlsCtx = self._ctx        # h: Borrowed, like any field read; may not outlive self
self._ctx = tls_ctx_new(1)   # releases the PREVIOUS handle first, then adopts the new one
```

And because each `own` field has exactly one owner, `own` fields form a tree.
A tree cannot be a cycle, so the entire own-subgraph is excluded from cycle-GC
tracking - a small, free performance win that falls out of the rule.

One rule here is not optional. A **raw resource type** - `Lock`, a TLS engine
handle, the subprocess and database handles - is not an RC-managed object; no
reference count will ever destroy one. Whoever holds one either owns it
statically or it leaks. So a field holding one *must* be `own`:

```dragon
class Router {
    _storage_lock: Lock        # error E15: Lock is a resource type; a Lock field
}                              # must be declared own (a non-own Lock has no
                               # owner to destroy it)
```

Locals stay unannotated and free (the scope owns them and releases them at
scope exit), and containers of raw resources are refused:

```dragon
locks: list[Lock] = []         # error E16: a list cannot hold raw Lock values;
                               # wrap the resource in a class with an own field

pool: list[SSLContext] = []    # compiles: an RC class wrapping an own handle is
                               # an ordinary object; release flows through its dealloc
```

The principle behind the split: annotations stay optional where the runtime
stays correct without them (a missing `own` on a `dict` field only costs a
proof), and become mandatory where it cannot (a missing `own` on a `Lock`
field costs the Lock).

## own on parameters - the two ends of a move

Storing into an `own` field takes sole ownership, so a borrow cannot be
stored. That gives a method three legal spellings, and this is where `own`
appears in a `def`:

```dragon
class Config {
    own _data: dict[str, str]

    def set_bad(d: dict[str, str]) {        # d: Borrowed (plain parameter)
        self._data = d       # error E8: an own field takes sole ownership;
    }                        # move it (own d) or copy it (dub d) - a borrow cannot be stored

    def set_moved(own d: dict[str, str]) {  # d: Owned (the caller moved it in)
        self._data = d       # ok: d -> Moved; the field adopts the value, no copy, no incref
    }

    def set_copied(d: dict[str, str]) {
        self._data = dub d   # ok: a fresh copy is minted on this line and stored
    }

    def() {
        self._data = {}      # ok: a fresh value transfers its +1 directly
    }
}
```

A move has two ends, and **both ends must say `own`**:

```dragon
cfg: Config = Config()
headers: dict[str, str] = {"Accept": "application/json"}

cfg.set_moved(own headers)   # the move is visible at the call site
print(headers)               # error E1: 'headers' was moved into 'set_moved' at line 4
```

Why the declaration must carry it: the body of `set_moved` is compiled once,
to one meaning. `self._data = d` is legal only when `d` starts `Owned`. If the
call site alone decided ownership, the same body would be legal for some
callers and illegal for others, and whether `d` gets released at scope exit
would depend on a runtime flag. One def, one state, no flags.

Why the call site must carry it too: without the mark, calls that consume look
identical to calls that borrow, and this compiles into a mystery:

```dragon
cfg.set_moved(headers)       # looks like every call you have ever written...
print(headers)               # ...error: moved? where? by what?
```

Consumption must be visible where it happens. (Rust requires `&mut` at both
the signature and the call site for the same reason; Dragon requires `own` at
both ends.)

Mismatches are compile errors, not coercions:

```dragon
cfg.set_moved(headers)       # error E13: set_moved takes ownership of its argument;
                             # write set_moved(own headers) to move it, or
                             # set_moved(dub headers) to keep yours and pass a copy

def show(d: dict[str, str]) -> None { print(len(d)) }
show(own headers)            # error E14: show borrows its argument; own has no meaning here
```

One exemption keeps the ceremony honest: a nameless fresh value needs no
keyword, because there is no binding to poison and no later line that could
misread it:

```dragon
cfg.set_moved({"Accept": "text/html"})   # ok: a fresh temp transfers directly
```

## Moves and control flow

A move must happen on every path or on none. There is no runtime flag
recording "was it moved?", so the compiler refuses programs whose answer
depends on the branch taken:

```dragon
def branchy(cond: bool) {
    x: Job = make_job()
    if cond {
        fire run(own x)     # x: Moved on this branch only
    }                        # error E9 at the join: 'x' is moved on the branch at
}                            # line 4 but not on the branch at line 3; consume it
                             # on every path or on none

def branchy_fixed(cond: bool) {
    x: Job = make_job()
    if cond {
        fire run(own x)
    } else {
        del x                # both paths end ownership; the join agrees
    }
}
```

Note that the error fires at the join even if you never touch `x` again.
It has to: on the branch that did not move `x`, someone must still release
it, and deciding that at runtime would need exactly the drop flag this
design forbids. This is the one place `del` is *required* rather than
optional - `own` opened a consume-obligation on every path, and `del` is
how a path with nothing to move into closes it.

Loops follow the same logic through the back edge: a binding defined outside
the loop cannot be consumed inside it, because iteration two would use a dead
name:

```dragon
def loopy(paths: list[str]) {
    buf: str = read_all(paths[0])
    for p in paths {
        fire consume(own buf)   # error E10: 'buf' is moved on iteration 1;
    }                            # iteration 2 would use a dead name
}

def loopy_fixed(paths: list[str]) {
    for p in paths {
        buf: str = read_all(p)   # defined INSIDE the loop: re-created each iteration
        fire consume(own buf)   # fine - each iteration moves its own binding
    }
}
```

## dub - the priced copy

`dub` is the one way to mint a second independent owner, and its cost sits on
the line you wrote. Searching a program for `dub` shows you every copy it
makes.

```dragon
base: dict[str, str] = {"Accept": "application/json"}
mine: dict[str, str] = dub base   # deep copy: a new dict with dubbed values

n: int = 42
n2: int = dub n        # int: trivial copy

tag: str = "release"
tag2: str = dub tag    # str: semantically an independent copy; since the payload
                       # is immutable and indistinguishable from a copy, the
                       # compiler shares it - free where cost provably cannot exist

rows: list[dict[str, str]] = load()
snapshot: list[dict[str, str]] = dub rows   # dubable by recursion:
                                            # list -> dict -> str, all dubable
```

A class is dubable if and only if every field is dubable, and things that
cannot meaningfully be copied refuse:

```dragon
lk: Lock = Lock()
lk2: Lock = dub lk               # error E11: Lock is not dubable

ctx: SSLContext = SSLContext()
c2: SSLContext = dub ctx         # error E11: SSLContext is not dubable:
                                 # field '_ctx' is a raw engine handle;
                                 # a TLS engine cannot be copied
```

No trait, no marker interface: non-dubability of handle holders falls out of
the composition rule. You cannot copy a TLS engine by copying its bytes, so
the class that owns one is not copyable either.

And there is one place `dub` is *required* rather than optional. Mutating a
container while iterating it looks correct and is silently wrong:

```dragon
names: list[str] = ["a", "tmp1", "tmp2", "b"]
for name in names {
    if name.startswith("tmp") {
        names.remove(name)     # error E17: 'names' is mutated while it is being
    }                          # iterated; iterate a snapshot (for name in dub names)
}                              # or apply the changes after the loop
```

Without the error, removing `tmp1` shifts `tmp2` into the slot the loop just
visited, the loop steps past it, and the program prints `['a', 'tmp2', 'b']` -
no crash, no warning, one survivor that should be gone. (Python has the same
famous footgun; Dragon refuses to inherit it.) The fix is the priced snapshot,
taken once before the loop starts:

```dragon
for name in dub names {        # snapshot iteration: the copy is visible and priced
    if name.startswith("tmp") {
        names.remove(name)     # mutating the original is now well-defined
    }
}
print(names)                   # ['a', 'b']
```

Collecting the changes in the body and applying them after the loop compiles
too, and mutating a *different* container inside the loop was always fine -
only the binding being iterated is protected.

## Identity resources: the socket handle

Some values are not data at all. A kernel socket is an *identity*: there is
exactly one of it, the outside world can see it, and closing it is
irreversible. Copy a dict and the two copies drift apart harmlessly; "copy" a
socket wrapper and you have minted a second claim on one object - the copy
reads the original's bytes, and closing either kills both. Identities cannot
be dubbed, only owned.

The standard library encodes this with a two-layer shape, and it is the same
shape every serious ecosystem converged on (Rust's `OwnedFd` over `RawFd`,
C++'s deleted-copy RAII wrappers):

```dragon
# socket.SocketHandle - the one claim on a kernel socket
h: SocketHandle = SocketHandle.adopt_raw(fd)   # the ONE door in from a raw fd
h.fd()                          # the descriptor, while the claim is live
h.close()                       # closes the kernel object and POISONS the
h.close()                       # handle: idempotent, a second close no-ops
h.fd()                          # -1 after close - loud, never a recycled fd
```

The raw layer still exists - `TcpStream.fd` is a bare `int`, exactly like
Rust's `RawFd` - because runtimes and FFI need it. Safety lives at the owned
layer: anything that keeps a connection holds the handle as an `own` field,
so the ordinary field rules do the enforcement with no new machinery:

```dragon
class FdReader(ConnReader) {
    own _sock: SocketHandle          # sole owner of the claim

    def(own sock: SocketHandle) {    # taking one is visible at every call site
        self._sock = sock
    }
}

reader: FdReader = FdReader(own h)   # the move is spelled, and checked
r2: FdReader = FdReader(h)           # error: constructor takes ownership of
                                     # its argument; move it with 'own h'
copy: FdReader = dub reader          # error E11: not dubable - the class
                                     # holds a claim that cannot be copied
```

Three consequences fall out of composition, none of them special-cased:

- **No forgery.** A second wrapper over the same claim requires moving the
  handle out of the first, which the first will not do while it lives.
  Building one from a raw integer requires walking through `adopt_raw` - the
  visible, greppable door, Dragon's spelling of Rust's `from_raw_fd`.
- **No double-close.** Only the handle closes the descriptor, there is one
  handle, and its close is poison-idempotent. A stale caller touching a
  closed handle gets `-1` and a failed send - defined and loggable - never a
  write into whichever stranger's connection inherited the recycled fd.
- **Handoffs are honest.** When an HTTP handler takes a connection past the
  end of its response - a WebSocket upgrade, a live event stream - it calls
  `res.cede(fn)`: the router sends the headers, hands the live reader to
  `fn`, and never touches that connection again. The callback owns it from
  its first line, which is idiomatically `defer conn.close()`, so every way
  out of the stream puts the claim down exactly once.

```dragon
def stream(conn: ConnReader) -> None {
    defer conn.close()               # the new owner's first act: schedule
    while true {                     # the last one
        if conn.send(next_event()) <= 0 {
            return                   # client gone; defer closes on the way out
        }
    }
}
res.cede(stream)                     # formally give up the connection
```

## The fire boundary

The payoff for all of this discipline is [green threads](/docs/1101-green-threads)
without data races and without atomic reference counts. Every heap value
crossing into a green thread must arrive through one of four doors:

```dragon
req: Request = parse_request(conn)      # Owned
counts: dict[str, int] = load_counts()  # Owned

fire handle(req)              # error E12: 'req' crosses a thread boundary;
                               # move it (own req), copy it (dub req),
                               # or make it a locked type

fire handle(own req)          # door 1: moved - the thread becomes the sole owner
fire tally(dub counts)        # door 2: copied - the thread gets its own dict
fire banner(GREETING)         # door 3: an immortal const string literal
fire record(router)           # door 4: an internally-locked type (see
                               #         Synchronization for Lock and friends)
```

Door 1 is the important one for performance: a moved value is single-threaded
*by proof*, so its reference count never needs to become atomic and the hot
path pays zero synchronization. Sharing mutable state without a lock is not a
race you debug at 3am; it is error E12 at the fire line. See
[Synchronization](/docs/1104-synchronization) for the locked-type door.

The same `own`/`dub` argument modes work at [`defer`](/docs/1105-defer), the
scope-exit twin of `fire`. `defer archive(own log)` moves the binding at the
defer STATEMENT (every later use is a use-after-move error, same as any move),
and the deferred call receives the +1 when the scope ends. A plain borrowed
argument is PINNED instead: reads stay legal, but an `own` move or `del` of it
while the defer is pending refuses at compile time - the defer holds the
pointer it snapshotted, and nulling the name cannot disarm it.

## What the compiler actually emits

The entire feature lowers to three effects, all of which you asked for
explicitly:

- `del x` in a release build is the exact release scope exit would have
  performed, just earlier, and the scope-exit release is elided. In a debug
  build it is `assert(rc == 1); free` - the standing tripwire that turns a
  silent leak into a loud abort at a named line.
- A binding that ends its scope `Moved` or `Deleted` emits no scope-exit
  release. This is decided at compile time; no drop flags exist.
- `dub` is a deep-copy call (or an incref for immutable payloads).

Nothing else changes. A program with no `del`, no `own`, and no `dub`
compiles byte-identical to what it compiled before the feature existed.

## Where the keywords are mandatory

Most of this chapter is opt-in: a program with no `del`, no `own`, and no
`dub` compiles unchanged, because reference counting keeps it correct and the
keywords only add proofs and earlier frees. There are exactly three places
where the compiler insists, and each exists because the implicit path there
is not merely slower - it is wrong:

1. **`own` on a raw resource field** (E15, and E16 for containers). A `Lock`
   or an engine handle is not an RC-managed object; without a statically
   known owner it leaks, every time. A field holding one must be `own`.
2. **`del` to close a conditional move** (E9). If one branch moves a value
   and a sibling branch does not, releasing it at scope exit would need a
   runtime flag recording which branch ran. Dragon has no such flags, so
   every path must consume the value - and on a path with nothing to move
   into, `del` is how you consume it.
3. **`dub` to iterate a container the loop body mutates** (E17). Iterating
   and mutating the same container silently skips or corrupts; the snapshot
   has to be real, and `for x in dub xs` is the priced, visible way to take
   it.

One line each: `own` is mandatory where a value would otherwise have no
owner; `del` is mandatory where a conditional move would otherwise need a
drop flag; `dub` is mandatory where iteration would otherwise observe its own
mutations. Everywhere else, all three are yours to reach for - `del` to cut
peak memory and to assert a lifetime you believe in, `own` to give resources
a deterministic chain of custody and to hand values across the fire boundary
without sharing, `dub` to make every copy in the program greppable.

## Error reference

| Error | Meaning |
|---|---|
| E1 | use of a name that was moved (`'x' was moved into <sink> at line N`) |
| E2 | use of a name that was deleted |
| E3 | `del` of a field (fields release when their owner dies) |
| E4 | `del`/move of a borrow (only the sole owner can be deleted or moved) |
| E5 | `del`/move of a value that escaped (`'x' escaped into '<sink>' at line N`) |
| E7 | `del`/move of a closure-captured binding |
| E8 | storing a borrow into an `own` field (move it or dub it) |
| E9 | consumed (moved or deleted) on one branch but not on all; close every path or none |
| E10 | consumed inside a loop but defined outside it |
| E11 | `dub` of a non-dubable type (names the first offending field) |
| E12 | unproven value crossing a fire boundary |
| E13 | plain call to an `own` parameter (write `f(own x)` or `f(dub x)`) |
| E14 | `own` at the call site of a borrowing parameter |
| E15 | a field of raw resource type (Lock, handles) without `own` |
| E16 | a raw resource as a container element (wrap it in a class) |
| E17 | mutating a container while iterating it (iterate `dub xs` or apply after) |

One line to hold the whole chapter: `Owned` is yours, an escape makes it
`Owned*` and the compiler tells you where, `own` moves it (visibly, at both
ends), `dub` copies it, `del` ends it early - and every way to get it wrong is
a line-numbered compile error, never a runtime surprise.

