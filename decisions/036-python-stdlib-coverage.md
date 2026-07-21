# Decision 036: Speed-First Stdlib - Curated Core, Eggs for the Tail

**Status:** Approved

**#3 update:** Commandment #3 is now *Familiarity must earn its place* (zen.md), not Python API parity. Reinforces the stance below: the Python index is a *map, never a target* - adopt a module's surface only where it suits static typing, drop the Python-shaped parts that need dynamism.

**Builds on:** D022 (Package Manager / eggs), D026 (vtables + devirtualization), D030 (native-typed values)

**Supersedes:** the original D036 framing that walked the entire Python 3.12 index as a
*coverage mandate*. That framing inherited Python's idea of what a stdlib is. This version
re-roots in **commandment #1 - speed is king** - and treats the Python index as a *map*, never a target.
**Absorbs:** the former slot-021 *Python Stdlib Dogfooding* content. Its module-feasibility matrix is fully
superseded by the Tiers below; surviving content merged into
[Dogfooding Rationale, Layering & Rollout](#dogfooding-rationale-layering--rollout-absorbed-from-former-d021).
The freed **021** slot was recycled for *No Runtime Type Introspection* (now D021).

I spent an embarassing amount of time with the Python 3.12 module index open in one tab and zen.md in the other. I'm not shipping 280 modules because Python did in 2003 when pip didn't exist. Go's curated core + eggs is the model; Python's table of contents is a map, not a todo list.

---

## Summary

Dragon's stdlib follows **Go's curated-core model, not Python's batteries-included breadth.**
A module earns a place in `stdlib/` only when **both** are true:

1. **It can be made C-fast (faster-than-Rust) in `.dr`** - no reflection tax, no forced dynamic dispatch, no arbitrary-precision-by-default, nothing that defeats devirtualization or native types.
2. **Dragon's actual domain needs it in core** - servers, CLI tooling, the `dragonlang-org` registry, systems/network work. (Go's stdlib footprint, basically.)

Everything else goes to **eggs **. Dragon ships with a package manager from day one, so it never needs Python's 2003 "everything in core or you can't get it" hack. That reason **does not apply here.**

**Parity (commandment #3) names modules; it never decides whether they belong in core.** We match Python's API surface *where a module exists*, but Python's table of contents has no vote. The walk below re-buckets every module by **Tier**, and a
**[Speed-Retrogression Watchlist](#speed-retrogression-watchlist)** flags everything that fights commandment #1: **Drop**, **→Egg**, **Block-until-language**, or **Keep-contained**.

---

## Context / Motivation

### Why Python's stdlib is huge - and why that reason is dead for Dragon

Python's ~280 modules are a historical artifact. "Batteries included" was coined when there was no pip/PyPI: if it wasn't in core, you couldn't get it. So `email`, `xml.dom`, `ftplib`, `telnetlib`, `cgi`, `aifc`, `nntplib` all landed in core.

Python now regrets this. **PEP 594 ("dead batteries") removed ~20 modules in 3.13** - `aifc`, `audioop`, `cgi`, `crypt`, `nntplib`, `telnetlib`, `imp`, `distutils`, and more. The stdlib is *"where modules go to die"* because one release train can't keep pace with an ecosystem. **If Dragon chases the full 3.12 index, it implements modules CPython already deleted** - commandment #2 violation (effort on the wrong thing).

Dragon launches *with* eggs . Default answer for the long tail: **"that's an egg,"** not **"add it to stdlib."** (Obvious in retrospect, but worth stating explicitely.)

### What Go and Zig teach

| | Python | Go | Zig |
|---|---|---|---|
| Size | ~280 modules | ~150 packages, curated | smaller, primitives-first |
| Philosophy | batteries (historical) | "build a production server on stdlib alone, stable forever" | "raw materials + explicit allocators; you build up" |
| Crypto / TLS | yes | **first-class** | **first-class (`std.crypto`)** |
| HTTP | basic client/server | **production-grade `net/http`** | evolving |
| Long tail (email/xml/ftp) | in core | **pushed to ecosystem** | **pushed to ecosystem** |

Dragon's dogfood - `http`, `ssl`, `socket`, `urllib`, `json`, `crypto`, `database`, `argparse`, `logging`, `subprocess` - **is the Go stdlib core, almost line for line.** Not a coincidence; it's the domain. Dragon's stdlib should be Go-shaped: small, curated, production-grade, permanently fast, everything else in eggs.

### Why speed has veto power over a module

Dragon's optimizations - ** devirtualization**, ** native-typed values**, container monomorphization - assume a *static* world. A module that demands runtime reflection, ABC/MI dynamic dispatch, or monkeypatchable bindings doesn't just make *itself* slow; it forces the runtime to carry metadata and indirection that taxes **programs that never import it**. That's the real danger. Watchlist below names each one.

---

## The Speed Test (the one question per module)

> **"Can this be C-fast in `.dr`, *and* does Dragon's domain need it in core?"**

```
 ┌─ slow-by-nature, but self-contained? → Tier 2 (egg)
 needs it in core? ── yes ──┤
 │ └─ forces WHOLE-PROGRAM slow machinery? → Watchlist
 │ (reflection / ABC-MI / monkeypatch) (Drop · Block · Contain)
 └─ no ─────────────────────────────────────────────────────── → Tier 2 (egg) or Tier 3 (never)
```

**Self-contained slowness is fine - as an egg.** `decimal` is slow, but only importers pay. **Whole-program slowness is not fine anywhere.** `inspect`/`pickle`-style reflection forces live type metadata, which bloats every binary and defeats devirtualization. Those don't get an egg pass by default - blocked behind a language feature that pays for itself, or a *static* analogue, or a drop.

---

## Tiers

| Tier | Meaning | Speed bar |
|------|---------|-----------|
| **T0** | **Domain core.** Servers/CLI/registry/systems need it. | **Must be benchmarked against Go's stdlib. Fastest emitted path is mandatory.** |
| **T1** | **Ergonomics, on demand.** Build when dogfood trips over the gap. | Must stay zero-overhead (no boxing/reflection creeping in). |
| **T2** | **Egg.** Useful but not core; *or* slow-by-nature (opt-in cost). | Lives in the registry, not `stdlib/`. |
| **T3** | **Never.** Deprecated upstream, CPython-internal, or replaced by a Dragon equivalent. | - |

**Status legend (work lifecycle, orthogonal to Tier):**

| Mark | Meaning |
|------|---------|
| `[x]` | Implemented in `stdlib/`, working today |
| `[~]` | Partial - exists but missing significant API surface |
| `[ ]` | Not implemented |
| `[!]` | Blocked on a language feature |
| `[-]` | Out of scope (T3) |
| slow | **Speed-retrogression candidate** - see Watchlist |

---

## Speed-Retrogression Watchlist

Each entry **fights commandment #1**. Two classes:

- **(A) Whole-program tax** - forces reflection / runtime type metadata / dynamic dispatch / monkeypatchable bindings into the runtime, defeating + for *every* program. **No egg pass.** Verdict: Drop, Block-until-language (only if the feature pays for itself statically), or replace with a compile-time analogue.
- **(B) Self-contained slowness** - slow by nature, but only importers pay. **Verdict: →Egg.** They retrogress speed only when parked in *core* pretending to be peers of fast primitives.

| Module | Class | Why it fights #1 | Shipped? | Verdict |
|--------|:----:|------------------|:--------:|---------|
| `inspect` | A | Runtime reflection over all symbols → live type metadata; kills devirtualization & dead-code stripping | no | **Block-until-language** (static compile-time reflection only, - never a runtime mirror) |
| `types` | A | Same runtime-type-introspection tax | no | **Block-until-language** (compile-time only) |
| `pickle` | A | Reflection-based (de)serialization + runtime type registry; security footgun | no | **Drop** - use `json`/`drs`. Egg only if a *typed*, codegen-driven serializer is designed |
| `copy` / `deepcopy` | A | Runtime graph reflection | already dropped | **Drop** (containers have `.copy`; static clone-or-refer model) |
| `abc` | A | ABCs force dynamic dispatch + defeat monomorphization | no `[!]` | **Block-until-language** - only if can devirtualize ABC calls to direct calls in the common case |
| `collections.abc` | A | Same - ABCs over containers | no `[!]` | **Block-until-language** (with the above bar) |
| `numbers` | A | Abstract numeric tower (ABCs) over native `int`/`float` | no `[!]` | **Drop** - native types already are the tower; the abstraction is pure overhead |
| `unittest.mock` | A | Monkeypatching → mutable dispatch tables → defeats inlining/devirt | no | **→Egg** (test-only; never let it touch core dispatch) |
| `ctypes` | A | Runtime FFI marshalling vs. compile-time `extern "C"`, which is strictly faster | no | **Drop** - compile-time FFI already wins; `[-]` |
| `dataclasses` (runtime extras: `field`, `replace`, kwargs-construction) | A | Needs kwargs-construction reflection | no `[!]` | **Compile-time analogue only** - `@dataclass` that *emits plain fields* is fast; runtime-reflection extras are not |
| `typing` (shim) | A | Encourages line-for-line Python porting; zero speed value | no | **Drop** - Dragon types are built in; `[-]` |
| `multiprocessing` | A | Fork model + IPC marshalling; orthogonal to green-thread story | no | **→Egg** (if ever); core story is `fire`/`thread`/`Thread` |
| `decimal` | B | Arbitrary-precision base-10, immutability-heavy; orders of magnitude slower than `f64` | no | **→Egg** |
| slow `fractions` | B | Arbitrary-precision rationals; not a peer of native numerics | **`[x]` shipped** | **→Egg** - graduate out of core (preserve the work; don't silently delete) |
| slow `difflib` | B | Ratcliff-Obershelp is ~O(n²); not domain-core | **`[x]` shipped** | **→Egg** (scope + speed) |
| slow `reprlib` | B | Niche recursive-repr guard; little domain value | **`[x]` shipped** | **→Egg** |
| slow `colorsys` | - | Trivial but niche (UI-toolkit-only); scope issue, not speed | **`[x]` shipped** | **→Egg** (scope, not speed) |
| `email` (full) / `xml.dom` / `xml.sax` | B | Large dynamic parsers | no | **→Egg** (keep only minimal multipart parser in core for upload endpoint - see T0) |
| `gettext` / `locale` | B | i18n tables + libc `setlocale`; rarely on hot path | no | **→Egg** |
| `pdb` / `trace` / `tracemalloc` / `faulthandler` | A/C | Runtime hooks / allocation tracking add ambient overhead | mostly `[-]` | **Keep out of core** (debugger work is its own track) |

**Root rationale for Class-(A) reflection blocks:** Dragon has **no runtime type-introspection layer** - the static type system *is* the type system, so `inspect`/`types`/`pickle`/`type` are redundant by construction. That doctrine is ** (No Runtime Type Introspection)**; this Watchlist is its stdlib consequence.

**Net removals from core to act on now:** graduate the four shipped slow modules (`fractions`, `difflib`, `reprlib`, `colorsys`) to eggs; confirm `copy`/`ctypes`/`typing`/`numbers` stay permanently out (`[-]`); no future PR adds a *runtime*-reflection `inspect`/`types`/`pickle`.

---

## Tier 0 - Domain Core (must be fastest; benchmarked vs Go)

What Dragon is *for*. Several gate the registry (`dragonlang-org` + `dragon egg publish`); all must hit commandment #1 and should carry microbenchmarks against Go.

**Networking / web / registry**
- `[x]` `socket` - raw epoll/kqueue/WSAPoll, no libuv
- `[x]` `ssl` - TLS via embedded mbedTLS (ADR 038); non-blocking socket+TLS I/O wired
- `[x]` `http` - status codes + base
- `[x]` `http.client` - HTTP/1.1 client (HTTPS via `ssl`)
- `[x]` `http.server` - heaven-style server ; keep-alive/chunked/reassembly
- `[x]` `http.cookies`
- `[x]` `urllib.parse` - URL/query parsing
- `[x]` `urllib.request` - convenience over `http.client`
- `[x]` `ipaddress` - per-IP rate limiting / abuse detection
- `[x]` **multipart parser** (minimal subset of `email.parser`) - `stdlib/http/multipart.dr`, wired into `server.dr` (`Request.form`/`files`); binary-safe, 10 tests. **Core keeps only the multipart slice; full `email` is an egg.**
- `[x]` `subprocess` - POSIX fork+pipe+dup2+execvp (`runtime_subprocess.cpp`); `run`/`check_output`/`call`/`Popen.communicate`, binary-safe. `dragon egg install --git`-ready. (Windows = ; `communicate(timeout=)` + non-blocking pipe drain = T1 follow-up.)
- `[x]` `getpass` - POSIX termios no-echo (`runtime_getpass.cpp`); ECHO restored on every exit path

**Data formats (fast parsers servers/CLIs live on)**
- `[x]` `json` - must be the fastest path; benchmark vs Go `encoding/json`
- `[x]` `csv`
- `[x]` `configparser`
- `[x]` `tomllib` - Cargo-style config
- `[x]` `drscript` (Dragon-specific) - `.drs` parser
- `[x]` `struct` - typed pack/unpack (binary protocols)
- `[x]` `base64`, `[x]` `binascii`

**Crypto (first-class, like Go and Zig)**
- `[x]` `hashlib`, `[x]` `hmac`, `[x]` `secrets`
- `[x]` `crypto` (Dragon-specific) - SHA-2 family, scrypt, RSA/ECDSA, AES-GCM, ChaCha20-Poly1305, Ed25519 (all KAT-verified)
- `[x]` `argon2id` (Dragon-specific) - registry password hashing (OWASP's #1; Python stdlib lacks it). Own BLAKE2b + Argon2 core in `runtime_argon2id.cpp`, **no external lib, no mbedTLS coupling** (self-contained secure-zero). Verified byte-exact vs `libargon2` + RFC 9106 §5.3 vector.
- `[x]` `totp` (Dragon-specific) - pure-Dragon RFC 6238/4226 over `hmac`; RFC 4226 §6.2 HOTP KAT green
- `[x]` `ed25519` exposure (Dragon-specific) - in `crypto.dr`, runtime RFC 8032 KAT-verified; package-signing-ready

**Compression / archive (tarball pipeline)**
- `[x]` `gzip`, `[x]` `tarfile`, `[x]` `zstandard` (Dragon-specific)
- `[x]` `zipfile` - pure-Dragon PKWARE (STORED + DEFLATED) over raw RFC-1951 deflate runtime path; CRC-verified round-trip, binary-safe
- `[ ]` `zlib` - libz FFI (underpins the above)

**OS / runtime / process**
- `[x]` `os`, `[x]` `os.path`, `[x]` `io`, `[x]` `time`, `[x]` `sys`
- `[x]` `pathlib` (`[!]` `@property` ergonomics pending), `[x]` `glob`, `[x]` `fnmatch`, `[x]` `tempfile`, `[x]` `stat`, `[x]` `shutil`
- `[x]` `errno`, `[x]` `syslog`
- `[x]` `argparse` - CLI front door
- `[x]` `logging` (`[ ]` `logging.handlers` / `.config` are T1)
- `[ ]` `os.cpu_count` - `sysconf` FFI the scheduler already calls
- `[ ]` `resource` - `getrusage` RSS + CPU-time introspection
- `[ ]` `platform` - `uname` wrapper

**Concurrency (Dragon's three-tier model)**
- `[x]` `threading`
- `[x]` `collections.concurrent` (Dragon-specific) - lock-free containers
- `[~]` `concurrent.futures` - partially covered; gap analysis needed
- `[ ]` `queue` - thread-safe queue
- `[ ]` `select` / `selectors` - thin shim over the reactor

**Database (interface + drivers, Go's `database/sql` model)**
- `[x]` `sqlite` (`sqlite3` in Python)
- `[x]` `database` (Dragon-specific) - `postgres` / `mysql` / `sqlite`
- `[x]` `postgres` driver - Extended Query Protocol + SCRAM-SHA-256 + result decoding; registry-ready (Phase 2). TLS is, not a stdlib gap.

**Core data structures + text (hot-path primitives)**
- `[x]` `collections` (`[~]` OrderedDict/defaultdict need `__missing__`), `[x]` `heapq`, `[x]` `bisect`, `[x]` `graphlib`
- `[x]` `string`, `[x]` `re` (PCRE2, JIT), `[x]` `textwrap`, `[x]` `html` (escapes/entities), `[x]` `shlex`
- `[x]` `math`, `[x]` `random`, `[x]` `statistics`
- `[x]` `datetime` (`[!]` `@property`), `[x]` `calendar`, `[x]` `uuid`, `[x]` `mimetypes`
- `[x]` `operator`, `[x]` `enum` (class-based + keyword, ADR 043), `[x]` `warnings`

**Testing**
- `[x]` `unittest` - `.dr` test framework (stdlib self-tests + behavioral `test/dr/*.dr`)

---

## Tier 1 - Ergonomics, On Demand

Build when real `.dr` code (or the dogfood) trips over the gap. Must stay zero-overhead. Driven by need, not Python's index.

- `[x]` `functools` (monomorphic `reduce`; full generics blocked on generics)
- `[x]` `itertools` (monomorphic int variants)
- `[ ]` `dataclasses` - compile-time `@dataclass` decorator that emits plain fields only (runtime extras on Watchlist)
- `[ ]` `contextlib` - `[!]` needs `__exit__` exception args + suppression
- `[~]` `traceback` - `[!]` needs stack-frame capture + exported exc-name accessor
- `[ ]` `logging.handlers`, `[ ]` `logging.config`
- `[ ]` `urllib.error`, `[ ]` `http.cookiejar`
- `[ ]` `html.parser`, `[ ]` `xml.etree.ElementTree` (SAX/DOM; keep narrow)
- `[ ]` `pkgutil`, `[ ]` `runpy` - over `ModuleResolver`
- `[~]` `importlib` - finish core API (D035) *(superseded: D035 now says no importlib; treat as dropped)*
- `[ ]` `doctest`, `[ ]` `timeit` - pure-Dragon
- `[ ]` `zoneinfo` - IANA TZ (pure-Dragon + tzdata)
- `[ ]` `signal` - `[!]` enum-blocked; thin FFI
- `[ ]` `array` - `[!]` typed buffer (boundary work - see)
- `[~]` `gc` - flesh out controls Dragon's GC already exposes
- `[ ]` `cmd`, `[ ]` `netrc`, `[ ]` `fileinput`, `[ ]` `filecmp`, `[ ]` `linecache`, `[ ]` `sysconfig`
- `[ ]` Unix shims (on demand): `pwd`, `grp`, `termios`, `tty`, `pty`, `fcntl`, `mmap`
- `[ ]` `webbrowser`, `[ ]` `zipapp`
- `[ ]` `unicodedata` - large tables; FFI candidate
- `[ ]` `codecs` - basic encodings
- `[ ]` `ast` - expose Dragon's AST with a Python-compatible API (compile-time, not runtime)
- `[ ]` `pydoc` - complements `dragon egg doc`

---

## Tier 2 - Eggs (out of core)

Leave `stdlib/` for the registry. Not domain-core, or slow-by-nature (importer pays).
**Bulk of Python's index goes here.**

- **Slow-by-nature (B):** `decimal`, `cmath` (needs complex type), `fractions` slow (graduate), `difflib` slow (graduate), `reprlib` slow (graduate), `colorsys` slow (graduate)
- **Internet data / markup (large, dynamic):** full `email` package, `email.mime`, full `xml` (`xml.dom`, `xml.sax`), `quopri`, `markdown` (Dragon-specific; graduate from `dragonlang-org/src/markdown.dr`)
- **Internet protocols:** `urllib.robotparser`, `urllib.response`, `ftplib`-likes
- **i18n:** `gettext`, `locale`
- **Persistence:** `dbm`; `pickle` only if a *typed* serializer is designed (see Watchlist)
- **Compression (lower priority):** `bz2`, `lzma`
- **Test/mocking:** `unittest.mock`
- **Concurrency (if ever):** `multiprocessing`, `sched`, `contextvars`
- **Numeric:** `numbers` is a Drop, not an egg (see Watchlist)

---

## Tier 3 - Never (out of scope, `[-]`)

Deprecated upstream, CPython-internal, or replaced by a Dragon equivalent.

- **Replaced by Dragon:** `asyncio` (→ `fire`/`await`), `tkinter`+submodules (→ UI), `turtle` (→), `wsgiref`/`cgi`/`cgitb` (→ `http.server`), `venv`/`ensurepip`/`distutils` (→ `dragon egg`), `readline`/`rlcompleter` (→ Dragon shell), `_thread` (→ `threading`), `posix` (→ `os`), `site` (→ `ModuleResolver`), `getopt` (→ `argparse`), `builtins`/`__main__`/`__future__` (implicit / Driver), `typing` shim (built-in types), `ctypes` (→ compile-time FFI)
- **Deprecated / removed upstream:** `stringprep`, `binhex`, `imp`, `crypt`, `nntplib`, `telnetlib`, `aifc`, `audioop`, `sunau`, `wave`, `chunk`, `imghdr`, `sndhdr`, `ossaudiodev`, `mailbox`, `plistlib`, `xmlrpc.*`
- **CPython-internal:** `marshal`, `copyreg`, `shelve`, `symtable`, `token`, `keyword`, `tokenize`, `tabnanny`, `pyclbr`, `py_compile`, `compileall`, `dis`, `pickletools`, `bdb`, `trace`, `tracemalloc`, `sys.monitoring`, `modulefinder`, `zipimport`, categories 30/34-38, Windows-specific `winreg`/`winsound`/`msvcrt`/`winapi` (defer to)
- **Reflection-tax Drops (see Watchlist):** `copy`/`deepcopy`, `numbers`

---

## Phased Rollout (by Tier)

### Now - finish T0 + act on the Watchlist
1. Close registry-gating T0 gaps: **multipart parser**, `subprocess`, `getpass`, `argon2id`, `totp`, `ed25519` exposure, `postgres` hardening, `zipfile`, `zlib`.
2. **Graduate the four shipped slow modules to eggs** (`fractions`, `difflib`, `reprlib`, `colorsys`).
3. Add Go-comparison microbenchmarks to T0 hot paths (`json`, `socket`, `http`, `re`, `crypto`).

### Next - T1 on demand
Dogfood-driven: `dataclasses` (compile-time decorator), `contextlib`/`traceback` (when language gaps close), `logging.handlers`, `html.parser`, `xml.etree` (narrow), `zoneinfo`, `signal`, `array`.

### Ongoing - eggs
Long-tail (T2) modules ship as eggs as the community needs them. Core does **not** grow to meet them.

### Permanently out
Tier 3 + Watchlist Drops. Don't revisit without a language change that makes them *fast*.

---

## Dragon-Specific Modules (no Python counterpart)

- `[x]` `crypto`, `[x]` `drscript`, `[x]` `template`, `[x]` `collections.concurrent`
- `[ ]` `argon2id`, `[ ]` `totp`, `[ ]` `ed25519` exposure, `[x]` `zstandard`, `[x]`/`[ ]` `database`/`postgres`
- `[ ]` `markdown` (egg; graduate from `dragonlang-org/src/markdown.dr`)

Same dogfood treatment - `.dr` first, FFI only when unavoidable.

---

## Dogfooding Rationale, Layering & Rollout (absorbed from former)

Absorbs **Decision 021 (*Python Stdlib Dogfooding*)**. Module-feasibility matrix superseded by Tiers above. The **C++/Dragon boundary** is **D042**, not here.

### Why the stdlib is written in `.dr` (dogfooding)

Policy (zen.md): *anything that can be done in Dragon must be done in Dragon; drop to C++/LLVM only when genuinely unexpressible.* Two payoffs: (1) **dogfooding** - real `.dr` stdlib code surfaces compiler bugs faster than synthetic tests; (2) **readability** - users can read stdlib modules, unlike C runtime code.

### The four layers

```
┌─────────────────────────────────────────────────────────────────┐
│ Layer 3: Pure Dragon Stdlib (.dr, zero C deps) │
│ json, datetime, pathlib, collections, csv, logging, uuid, … │
├─────────────────────────────────────────────────────────────────┤
│ Layer 2: Dragon + FFI (.dr calling C via extern "C") │
│ os, threading, io, socket, crypto, re, sqlite … │
├─────────────────────────────────────────────────────────────────┤
│ Layer 1: Core Runtime (libdragon_runtime.a - C, per) │
│ container storage, refcount/GC, setjmp/longjmp, scheduler … │
├─────────────────────────────────────────────────────────────────┤
│ Layer 0: OS / libc (system, never touched) │
└─────────────────────────────────────────────────────────────────┘
```

Dogfooding work lives in Layers 2-3. Layer 1 is the irreducible native core; **what stays C++ and why is **, not restated here.

### Working language features (the dogfooding baseline)

Shipped: classes + single inheritance + `super`; `@staticmethod`/`@classmethod`; dunder suite; try/except/finally; `extern "C"` + `ptr`; f-strings; `*args`/`**kwargs`; lambdas; `Union`/`Optional`; tuples/lists/dicts/sets; generators; context managers; cross-module imports; refcount + cycle-collector GC; green threads + OS threads.

### Phased rollout (complete)

- **Phase 0** - ~17 no-language-change modules - **done**.
- **Phase 1** - `@property` descriptors - **done**.
- **Phase 2** - `enum` (import-gated intrinsic, ADR 043) - **done**.
- **Phase 3** - heavy hitters (`datetime`, `pathlib`, `logging`, `argparse`, `subprocess`, `http`, `urllib`) - **done**.

Live module status is the Tier tables above; rollout retained as historical arc.

---

## What this policy commits us to

- **Commandment #1 is the gate, not parity.** Speed Test is the one question.
- **Stdlib stays small and Go-shaped.** No drift toward Python's 280-module breadth.
- **Reflection/ABC/monkeypatch modules blocked at the door.** They tax every program, not just importers.
- **Four shipped modules flagged for graduation to eggs** (`fractions`, `difflib`, `reprlib`, `colorsys`). Work moves to registry, not deleted.
- **Honest non-goals.** Tier 3 + Watchlist Drops document what Dragon will *never* carry in core.
- **Living document.** Tiers and status marks change per PR (and occassionally get out of date between PRs, that's fine).

---

## Open Questions

1. **`dataclasses` static decorator scope.** How much API can a compile-time decorator cover before it needs kwargs-construction reflection?
2. **ABC devirtualization bar.** Can devirtualize ABC/`collections.abc` calls to direct calls in the common case? If yes, ABCs move from Block to T1.
3. **Typed serializer instead of `pickle`.** Worth designing as a Dragon-specific egg?
4. **Egg-graduation timing.** Pre-release - no back-compat owed. Four slow modules should go straight to eggs before 1.0. Confirm `dragon egg install` resolves `from fractions import Fraction` cleanly.

---

## Done log

(Newest at top.)

- - **Absorbed former Decision 021 (Stdlib Dogfooding).** Feasibility matrix superseded by Tier system; dogfooding rationale, four-layer model, working-feature baseline, phased rollout merged above. D021 number recycled for *No Runtime Type Introspection*. No code shipped.

- - **Tier-0 follow-on: argon2id + subprocess hardening.** argon2id shipped (Dragon-specific; OWASP's #1, absent from Python stdlib): own BLAKE2b + Argon2 core in `runtime_argon2id.cpp`, no external lib, no mbedTLS coupling. Verified vs `libargon2.so.1` + RFC 9106 §5.3. subprocess T1: `communicate` deadlock fixed via concurrent `poll(2)` pump + `communicate(timeout=)`/`run(timeout=)`. 71/71 `.dr` suites green.

- - **Tier-0 registry wave.** Recon found multipart, ed25519, postgres already shipped. Shipped totp, zipfile, getpass, subprocess in parallel. 70 `.dr` E2E suites pass. Deferred argon2id for focused KAT pass.

- - Parity wave . Shipped `shlex`, `stat`, `colorsys`, `mimetypes`, `shutil`, `difflib`, `http.cookies`, monomorphic `functools`/`itertools`. `enum` as import-gated intrinsic (ADR 043). Fixed tuple-unpack coercion and for-loop element kind bugs. Dropped `copy`/`deepcopy`.

- - Batch 1: `operator`, `errno`, `warnings`, `reprlib`, `textwrap`. Fixed Phase 3.G int-keyed dict monomorphization.

- - Phase A.3: `gzip`, `zstandard`, `tarfile`. Phase A.2: `urllib.parse`, `http.client`, `urllib.request`. Phase A.1: `binascii`, `base64`, `hmac`, `secrets`, `ipaddress`.

- - created, initial walk of Python 3.12 index.

- - **Re-scoped.** Speed-first curated core (Go model) + eggs for tail. Added Speed Test, Tiers, Watchlist. Flagged four modules for egg graduation. Policy re-scope, no code.
