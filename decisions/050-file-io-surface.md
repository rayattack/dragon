# 050 - One Obvious File I/O Surface: `open` / `make` / `push`

Approved. We had four ways to read a file: `open` builtin (raw `dragon_file_*` handle, five methods), `io.File`, one-shot `io` helpers (`read_text`/`write_text`/…), and `pathlib.Path` read/write. Builtin `open` and `io.File` weren't even the same type; errors told you to import `File` for more. Python has this mess too. I don't need it here. Zen: one obvious way.

## Options considered

1. **`Path` + `open` split** - Python's modern blessing: `Path` methods
 for whole-file, `open` for streams.
2. **`io` free functions + `open`** - briefest whole-file one-liners,
 but a Dragon-only surface (no Python parity) and no bytes variants.
3. **`open` only** - one construct, whole-file reads cost a `with`
 block.
4. **Reader / Writer split, mode-free** - three verbs (`open` / `make` /
 `push`) returning distinct `Reader` / `Writer` types, no mode strings.

## Decision

**Option 4.** File I/O is a reader/writer dichotomy reached via
`from io import open, make, push`:

- `open(path) -> Reader` - read an existing file.
- `make(path) -> Writer` - create or replace it (truncating).
- `push(path) -> Writer` - append to it.

The verb *is* the mode; there are no `"r"`/`"w"`/`"a"` mode strings.
Reading from a `Writer` (or writing to a `Reader`) is a **compile error**,
not a runtime `UnsupportedOperation` - the split moves the mode mismatch
to compile time ("you should know if it is broken when you compile").

`Reader`: `text` / `bytes` / `lines` (whole-file, **self-closing**,
so `open(p).text` is a leak-free one-liner with no `with`), `line` /
`take(n)` (streaming), `for line in r`, idempotent `close`. `Writer`:
`write(str)` (UTF-8) and `write(bytes)` (verbatim) as one overload,
`line` / `lines`, `flush`, `close`. Both are pure Dragon classes
in `stdlib/io.dr` over the `extern "C"` stdio shims; `open`/`make`/`push`
are ordinary Dragon functions, not builtins.

**Deleted** (one surface, no plumbing left behind): the `open` builtin
and its raw `dragon_file_*` handle + codegen path, the `io.File` class and
`UnsupportedOperation`, the `io` one-gulp helpers, and `pathlib.Path`'s
read/write methods (`Path` is path manipulation only now).

A `Writer` has no leak-free one-liner: Dragon has no finalizer, and
`write` cannot self-close (the caller may write more), so writers are
`with`-scoped or explicitly closed. Whole-file readers self-close because
they consume the file. Random-access file I/O (`seek`/`tell`/`r+`) went
with `File`; `BytesIO` / `StringIO` remain for in-memory seekable stremas.

## Evolution

The revision of this ADR blessed **`open` in a `with` block**
as the one way and kept `io.File`, the `io` one-gulp helpers, and
`pathlib`'s read/write methods as documented "plumbing." Desgin review
 rejected the leftover plumbing as exactly the overlap the ADR
set out to remove, and replaced the single `open` with the `open` /
`make` / `push` reader/writer split. That revision also subsumed the
"deeper unification" this ADR had deferred: `open` is now a pure-Dragon
function returning a real `Reader` class, so there is no separate raw
`FILE*` handle and no `dragon_file_open` builtin lowering to maintain.

## Surface after the migration

- `910-stdlib-io.md` and all book/site examples lead with
 `open` / `make` / `push`; there is no "Plumbing" section.
- Six stdlib modules, `pathlib`, the `dragon-egg` CLI, the `dragonlang-org`
 app, and the test suite were migrated; the dead `dragon_file_write` /
 `dragon_file_readlines` runtime functions were removed.
- `open`/`make`/`push` require `from io import ...` (no builtin, no
 prelude). Consistent with every other stdlib module.
- No random-access file I/O until a deliberate future addition; in-memory
 needs are served by `BytesIO` / `StringIO`.
