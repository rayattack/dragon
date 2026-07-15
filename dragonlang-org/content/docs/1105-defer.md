# Defer: Scope-Exit Calls

`fire` and `defer` are two halves of one coin:

- `fire f(x)` runs f NOW, on ANOTHER green thread.
- `defer f(x)` runs f LATER, on THIS thread, when the current scope ends.

`defer` schedules a direct call to run when the block it appears in exits, on
every exit path: falling off the end, `return`, `break`, `continue`, and an
exception unwinding through. Whatever way control leaves the block, the call
runs.

```dragon
def load_index(path: str) -> Index {
    f: File = open(path, "rb")
    defer f.close()                    # runs on return, raise, every exit

    hdr: bytes = f.read(16)
    if hdr[0:5] != b"ODB01" {
        raise CorruptionError("bad magic")     # close still runs
    }
    return parse_index(f)                      # close runs after the return
}                                              # value is built
```

The operand must BE a call: a function call or a method call. A bare name,
arithmetic, or anything value-shaped is a parse error, because a deferred
call's return value is always discarded and the grammar simply prevents
wanting it.

## Block-scoped, not function-scoped

A defer lives and dies with the BLOCK that declared it, like every other
binding. The Zen line is literal here: a scope that ends frees what it held.

```dragon
def process(items: list[str]) {
    if len(items) > 100 {
        defer log_bulk_mode()      # runs at this closing brace,
        reserve_capacity(items)    # not at the end of process()
    }
    for item in items {
        defer count_one()          # runs at the end of EVERY iteration
        handle(item)
    }
}
```

This is deliberately not Go's function-scoped defer. Block scoping is what
makes defer-in-a-loop well defined (it runs each iteration, nothing
accumulates, nothing allocates) and it is what makes the whole feature
compile to plain static code: every defer site, count, and callee is known
at compile time, so the calls are emitted directly into each exit edge.
There is no runtime defer stack to push onto and no hidden allocation. If
you cannot explain the lowering, you cannot trust the speed; this one is
"the compiler writes the call at every exit for you."

Within one scope, deferred calls run LIFO (reverse declaration order):
acquisition order in, release order out. And they run BEFORE the scope's own
memory cleanup, so anything a defer borrowed is still alive when it runs.

## The one gotcha: arguments are snapshots

You did nothing wrong if this surprises you. Ordinary, correct-looking code
triggers it, and the mechanism is invisible from the source.

```dragon
def report(log: list[str]) {
    i: int = 1
    defer record(log, i)    # captures 1 NOW
    i = 9
}                           # prints "1" at scope exit, not "9"
```

Arguments (and the receiver of a method defer) evaluate AT THE DEFER
STATEMENT, into compiler-managed slots. Only the CALL runs at scope exit.
One rule, no exceptions, because ownership requires it: a move has to be
checkable at a source line, not at an exit edge.

When you want the latest value at exit, defer a call that READS the state at
call time instead of passing the value in:

```dragon
def collect(log: list[str]) {
    parts: list[str] = ["a", "b"]
    defer join_into(log, parts)    # parts is a borrow; the call reads it at exit
    parts.append("c")
}                                  # join_into sees ["a", "b", "c"]
```

The snapshot also protects you: rebinding a name after deferring it is safe,
because the snapshot holds its own reference to the old value.

```dragon
s: str = read_banner()
defer show(s)          # holds the banner
s = read_footer()      # rebind is fine; show still gets the banner
```

## Ownership: the half Go does not have

Pair `defer` with `own` and "we promise not to touch it again" stops being a
comment and becomes a compile error. `defer f(own x)` moves `x` at the
statement; the deferred call receives the +1 at exit. Every later use of `x`
in the scope is rejected exactly like any other use-after-move:

```dragon
def flush_and_ship(batch: list[Order]) {
    log: WriteAheadFile = open_wal()
    defer archive(own log)     # moved HERE: any log.append(...) below this
    ...                        # line is a compile error, not a race
}
```

`defer f(dub x)` copies at the statement, exactly like `fire`. The source
stays yours; the defer holds the priced snapshot.

A plain borrowed argument (or method receiver) is PINNED for the life of the
defer: you can keep reading and mutating it, but you cannot `own`-move or
`del` it while a defer still holds the pointer it snapshotted. The checker
explains why at the line that tries:

```dragon
r: Reader = open_reader()
defer r.close()
hand_off(own r)     # error: 'r' is pinned by a pending defer (the deferred
                    # 'close()' at line 2); it cannot be moved before the
                    # defer runs at scope exit
```

Without the pin, `close()` would run on a connection somebody else now owns.
A resource with more than one possible fate writes the fate on each exit
path instead, or defers a wrapper that decides at exit.

## Exceptions

Defers run during unwind; that is half the point. A raise that leaves the
block runs its defers before the handler sees the exception, whether the
handler is in the same function or ten frames up:

```dragon
def risky(log: list[str]) {
    defer record(log, "cleanup")
    raise ValueError("boom")
}

try {
    risky(log)
} except ValueError {
    record(log, "caught")      # log is ["cleanup", "caught"]
}
```

One boundary to know about: when no handler exists anywhere and the process
is about to exit on an uncaught exception, pending defers are skipped along
with everything else; the OS reclaims the process. Defers are a scope-exit
tool, not a process-shutdown hook.

## What defer is NOT

- **Not `with`.** Paired enter/exit protocols keep `with`. `defer` covers
  the exits that have no enter, or an ownership-transferring fate. A defer
  registered inside a `with` body belongs to the body's scope and runs
  before `__exit__`.
- **Not `try`/`except`.** Rollback-only-on-failure is conditional on the
  exception and stays `except`. `defer` is for actions unconditional on HOW
  the scope ends.
- **Not a conditional registration trick.** The Go idiom
  `if cond { defer unlock() }` registering for FUNCTION exit is
  inexpressible on purpose; under block scoping that defer fires at the
  `if`'s closing brace. Register at the scope you mean and decide in the
  callee: `defer maybe_unlock(locked)`.
- **Not free at the last line.** A defer whose scope ends one statement
  later is a direct call in a costume; write the direct call. `defer` earns
  its keep when there is distance between registration and the exits:
  multiple paths, raises, or real code after it.

## v1 limits (all loud compile errors, never silent)

- No `defer` at module top level (no scope ends before process exit).
- The callee must resolve to a direct function or method; computed and
  closure callees are not supported yet.
- No keyword arguments in the deferred call yet; pass positionally.
- Arguments whose static type is `Any` or a union are not supported yet;
  annotate the concrete type.
- No `defer fire f(x)` fusion; spawning at scope exit composes manually with
  a wrapper that fires inside.

## At a glance

| You want to... | Write |
|----------------|-------|
| Run cleanup on every exit path | `defer f.close()` |
| Hand a resource to its scope-exit owner | `defer archive(own log)` |
| Snapshot a value, keep mutating the source | `defer send(dub items)` |
| See the latest state at exit | defer a call that reads a borrow: `defer report(stats)` |
| Cleanup per loop iteration | put the `defer` in the loop body |
| Paired enter/exit protocol | not defer - use [`with`](/docs/0903-context-managers) |

That completes the concurrency model - three colorless tiers, the primitives
to coordinate them, and the scope-exit calls that clean up after all of it.
Next, a different kind of building block: [Templates](/docs/1201-templates),
Dragon's typed, auto-escaping approach to generating HTML, SQL, and other
structured text.

