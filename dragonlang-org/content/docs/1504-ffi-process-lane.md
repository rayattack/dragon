# Calling Python, Go, and Rust

The previous chapters covered the in-process lane: `extern "C"` links native
code straight into your binary, and a call costs roughly nothing. This chapter
is the other lane. Dragon does not embed foreign runtimes - there is no
libpython in your binary, no GIL, no green-thread-hostile VM. Instead it passes
the baton across a process boundary, and the handoff is typed on both ends.

```dragon
from unittest import TestCase, main

extern "python" def score(batch: list[User]) -> list[Scored] from "ml/score.py"

class User {
    def(id: int, tag: str) {
        self.id = id
        self.tag = tag
    }
}

class Scored {
    def(id: int, score: int) {
        self.id = id
        self.score = score
    }
}

def handle(users: list[User]) -> list[Scored] {
    return score(users)   # a plain call; the transport parks the green thread
}
```

That one `extern` line is the whole Dragon side. The declaration is the
contract: typed at compile time like any extern, monomorphized like any
generic. `Any` appears nowhere.

Three tags are accepted: `"python"`, `"golang"`, and `"rust"`. (`"C"` is the
in-process lane from the previous chapters; `extern "go"` is rejected with a
hint, Dragon spells it `"golang"`.)

## What actually happens

The compiler lowers the declaration to an ordinary Dragon function that:

1. encodes the arguments as one JSON object keyed by parameter name, through
   the box-free `json.encode[T]` (`{"batch": [...]}`); every field is written
   at its static type, nothing is boxed. A `bytes` parameter rides after the
   body as a raw blob - no base64, ever,
2. spawns the child ONCE from an argv list (never a shell) and keeps it warm:
   the process, its imports, and its state survive across calls,
3. sends one length-prefixed frame per call on stdin, reads one reply frame,
4. decodes the reply body into the declared return type with `json.decode[T]`,
   box-free (a `bytes` return arrives as a raw blob).

The serializer here is not FFI-private plumbing: `encode[T]` / `decode[T]`
are the same public schema-directed pair you can call yourself on any class
- see [Data Formats](/docs/1404-stdlib-data). Your `User` crosses the
boundary at exactly the cost of encoding a `User`, nothing hidden on top.

A child that answers a call with an error keeps serving (you get a
`ForeignError` with its traceback; the process lives). A child that dies is
restarted once for the NEXT call - never mid-call, so a crash can't silently
re-run your side effects.

For `"python"` the interpreter is resolved the trusted way: the
`DRAGON_PYTHON` environment variable if set, otherwise absolute candidates
(`/usr/bin/python3`, `/usr/local/bin/python3`, `/bin/python3`). A bare name is
never searched on `$PATH` - a writable directory early in `$PATH` must not get
to decide what "python3" means. For `"golang"` and `"rust"` the `from` path IS
the compiled binary, resolved relative to the declaring `.dr` file.

There is no `async` and no callsite `await`. The pipe wait is a real
suspension point, but the transport parks the green thread there, so a plain
call cooperates and `fire score(users)` fans out like any other function.

## The child contract

Framed: read a 4-byte length, a JSON header, the body, any blobs; write one
reply frame; repeat until stdin closes (the baton protocol - the exact wire
format lives in the D052 decision doc). You do not have to write that loop,
because:

## dragon ffi sync

```
$ dragon ffi sync ml.dr
wrote ml/score_stub.py                  (types + serve loop; DO NOT EDIT, always regenerated)
wrote ml/score.py (skeleton - yours to edit)
```

The Dragon signature is an IDL you never had to write. `sync` reads the extern
declarations and emits the counterpart for each: for python a `_stub.py` with
dataclasses and a `serve()` loop, for golang a `_stub.go` with structs and a
`main()`, for rust a `_stub.rs` with serde structs. The stub is always
regenerated; the skeleton next to it is written once and never touched again.
Your side of the python file ends up this small:

```python
from score_stub import User, Scored, serve

def score(batch):
    return [Scored(id=u.id, score=model.predict(u)) for u in batch]

serve(score)
```

Two guards keep the two sides honest: `dragon check` (and every build) fails
when an existing stub's embedded signature no longer matches the declaration -
a renamed parameter would otherwise zero-fill silently on the foreign side -
and `dragon ffi sync ml.dr --check` verifies byte-exactly in CI without
writing. Stubless hand-written children are always allowed; the checks only
bind stubs that exist.

## Three failures, three catchable types

```dragon
from ffi import FFIError, SpawnError, ForeignError, SchemaError

try {
    scored: list[Scored] = score(users)
} except SpawnError as e {
    log(e.hint)        # the binary could not start; str(e) says why
} except ForeignError as e {
    log(e.traceback)   # the child's stderr, verbatim - the Python traceback
} except SchemaError as e {
    log(e.detail)      # exited clean but the output did not match list[Scored]
}
```

`except FFIError` catches all three. A child that crashes after printing
JSON-shaped garbage is a `ForeignError`, never a decoded value: no decode is
attempted after a non-zero exit, because a silent fallback is a silent lie.

## The price, stated plainly

Every call costs serialize + pipe + deserialize. Spawn and `import torch` are
paid ONCE - the sidecar stays warm. That is the honest price of "no
interpreter in my process", and it is the right price for batch-shaped calls:
score a thousand users, resize an image, parse a log shard. It is still the
wrong tool for a per-element call in a hot loop - for that, take the
`extern "C"` lane against the underlying native library instead. If it is
interpreted, it is not sacred; the boundary is where you pay, so cross it in
batches.

## Stepping down: ffi.runs

The extern sugar is the front door. When you need the transport without a
declaration - a one-off script, a tool whose signature you build at runtime -
the kernel underneath is public:

```dragon
from ffi import runs

const report: Summary = runs[Summary](["./analyze", "--fast"], payload)
```

`runs[T]` spawns the argv one-shot, feeds `input` bytes on stdin, and decodes
stdout into `T` with the same taxonomy on failure. The extern sugar compiles
into exactly this call; there is nothing the sugar can do that you cannot.

One rule worth restating here because sync enforces it: **one script or binary
per extern**. A stub owned by a different `.dr` file is never overwritten -
last-sync-wins would silently zero-fill the other caller's renamed arguments.

## Current limits

- Parameters and returns cross as scalars, `list` of scalars, classes, `list`
  of classes, `dict[str, <scalar>]`, `Optional` of those, and `bytes` (raw
  blobs). Still out: `dict` with non-`str` keys or class values, and `bytes`
  as a class *field* (as a parameter or return it is fine) - that one needs
  blob-hoisting through nested objects, which is frame-protocol work, not
  sugar.
- Calls on one sidecar are serialized in call order; worker pools (`pool N`)
  are deliberately undecided and not yet surface. `runs[T]` remains the
  one-shot whole-stream tier and never changes contract.
- The `[ffi]` manifest section (declare `python = ">=3.10"` in `dragon.drs`,
  verified at sync/deploy) is the remaining phase.
