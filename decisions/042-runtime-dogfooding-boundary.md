# Decision 042: The C++/Dragon Runtime Boundary - Dogfooding Audit & Migration Policy

Approved. Phase 1 landed and verified; Phases 2-4 deferred to their own ADRs. Zen says if it can live in Dragon, it should; C++ only when Dragon genuinely can't express it. I wanted to know whether I'm actually at that line or just telling myself I am. Audit: ~11.3k lines of `lib/Runtime/*.cpp` vs ~18.8k lines of `stdlib/*.dr`.

Honest answer: distribution isn't optimal, but the gap is smaller than it looks. Three tiers:

1. **Irreducible primitives** (~6-7k lines) that are correctly C++ and must stay.
2. **Pure logic that leaked into C++** - dogfoodable today with no new mechanism, but only
 a thin slice of it is *safe* to move without a speed regression.
3. **Large targets** (string methods, builtins) that are expressible in `.dr` but **blocked
 by two missing language mechanisms** - and that would *violate #1 (speed)* if
 moved naively against today's codegen.

Records the audit, migration policy, and what has to land before big moves:
mutable byte/string builder, prelude/auto-import, optionally C-struct-layout FFI.
Explicitly rejects "leave everything in C++" and "rewrite everything in `.dr` now."

---

## Context / Motivation

### The capability is real and free - so "could it be `.dr`?" is almost never about the FFI

A Dragon value already *is* its C ABI type, so a bound `extern "C"` call is a
direct native call with no marshalling thunk. The mechanism is proven in-tree:

- `stdlib/threading.dr` binds raw `pthread_mutex_*` and `malloc`/`free` directly.
- `stdlib/glob.dr` binds `opendir`/`closedir`/`access`/`getcwd`.
- `stdlib/io.dr` wraps C stdio; `stdlib/math.dr` wraps libm with nothing but `extern "C" def`.

So the question for any chunk of C++ is **never** "is the FFI capable?" It is two other
questions:

1. **Is the C function's signature trivial-core** (`int`/`float`/`bool`/`intc`/`ptr`), or
 does it need a **C struct Dragon cannot lay out** (`z_stream`, `struct stat`,
 `sockaddr_in`)? The latter requires a C++ shim *by construction*.
2. **Would the `.dr` version be as fast?** (#1.) Today, tight per-element byte
 loops and allocation-heavy code in `.dr` are *not* yet at C speed (see "Speed reality").

### The speed reality - the anchor for "don't move naively"

Dragon's own benchmarks: Dragon **beats Go** on fibonacci (0.81s vs 1.44s) and string work
(0.014s vs 0.040s) - arithmetic and control flow are at or above target. But it is **~6×
slower on the sieve and ~13× slower on object creation**, and the scrypt BlockMix inner loop
(pure-`.dr` byte shuffling) currently runs **~30× a C implementation**.

That is the emperical line. Code shaped like *"loop over every byte of a buffer, allocate an
object per piece"* - which is precisely what string methods, byte-split, and the escape/hash
helpers are - is the shape `.dr` does **not** yet emit at C speed. For that code, keeping it
in C++ is the #1-correct call, not laziness.

---

## The Audit - three tiers

### Tier 1 - Irreducible primitives. Must stay C++. (~6-7k lines, correctly placed)

| Area | Files | Why it cannot be `.dr` |
|---|---|---|
| Refcount / GC mark·traverse·clear | `runtime_core.cpp` | The intrinsics codegen emits *into*; circular. |
| Exceptions (setjmp/longjmp, TLS stacks) | `runtime_exception.cpp` | Raw `setjmp`/`longjmp`, per-vthread stacks. |
| Union/`Any` box | `runtime_box.cpp` | The `{i64,i64}` layout codegen depends on. |
| Closures, cells, env, generators, class descriptors | `runtime_builtins.cpp` | Codegen/reflection support objects. |
| Green-thread scheduler + epoll/kqueue | `runtime_concurrency.cpp` | minicoro, raw I/O reactor. |
| Object **storage** (list/dict/set/tuple/str/bytes internal arrays) | `runtime_list/dict/collections/string.cpp` | Refcount headers + native arrays. |
| **Struct-marshalling shims** | `runtime_zlib/zstd.cpp` (`z_stream`), `runtime_platform.cpp` (`struct stat`, `sockaddr_in`), PCRE2 / llhttp / mbedTLS / ed25519 wrappers | Manipulate C structs Dragon cannot lay out. `runtime_zlib.cpp` is *exactly* a `--cc-source` shim - `deflateInit2` + a realloc grow-loop over a `z_stream`. |

These are the irreducible native core the FFI guide itself points external authors at.

### Tier 2 - Pure logic that leaked into C++. Dogfoodable today; only a slice is *safe*.

- **`dragon_normpath` / `dragon_relpath`** (`runtime_platform.cpp`) - pure path-string
 algorithms that `stdlib/os/path.dr:76-81` forwards to *verbatim* (`def normpath(p) { return
 dragon_normpath(p) }`). Short, infrequently-called strings → **zero speed risk. Cleanest
 move; harvest now.**
- **`dragon_template_escape_html/sql/url`, `dragon_str_fnv1a`** - also pure logic, **but** on
 render-/query-hot paths and a prepared-statement hashtable-bucket lookup. `fnv1a`
 additionally must hash byte-for-byte identically to `CodeGen::sqlCanonicalHash`. Moving
 these risks #1 and adds a codegen-parity coupling. **Leave until the byte-loop
 speed gap closes.**
- **`fileio` whole-file read/write-bytes, set *algebra*** (`union`/`intersection`/`difference`
 layered on `add`/`contains`/iterate) - expressible in `.dr`, but low value: stable, not
 hot, ~50 lines of settled C. **Leave; not worth the churn.**

### Tier 3 - Large targets, blocked by a language gap. Do not move yet.

- **String methods** - `upper`, `lower`, `split`, `join`, `replace`, `strip`, `center`,
 `ljust`, `zfill`, `partition`, `isalpha`/`isdigit`/…, `startswith`/`endswith`: ~50
 functions, the bulk of `runtime_string.cpp` (1633 lines). Expressible via `dragon_str_cp_at`
 / `dragon_utf8_*`, **but** a `.dr` `split` doing a function call *per code point* plus an
 allocation per piece is far slower than the C++ version's raw `data[i]` indexing + `memcmp`
 (see `dragon_bytes_split`). **Gap:** no fast mutable string/byte builder is exposed to
 `.dr`. (`dragon_str_append_inplace` exists at `runtime_string.cpp:421` but is an internal
 amortized-`cap` helper, not a first-class `bytearray`.)
- **Builtins** - `sorted`, `sum`, `min`, `max`, `zip`, `enumerate`, `divmod`, `abs`, `pow`,
 `bin`, `hex`, `oct`, `chr`, `ord`. Many are trivial `.dr` one-liners - **but there is no
 prelude/auto-import.** They are hardwired in `src/codegen/CallBuiltins.cpp` to emit direct
 `dragon_*` calls (`sorted` @732, `zip` @721, `enumerate` @705, `divmod` @1169). Making them
 `.dr` requires an auto-imported prelude module first. (And `sorted` specifically needs a
 real `.dr` Timsort to beat `std::sort` - a naive sort loses on #1.)

### The one architectural lever worth naming - the struct-field-accessor explosion

The single largest source of *C++ bloat caused by a language gap* is the proliferation of
struct-field accessors in `runtime_platform.cpp`: `dragon_stat_size/mode/mtime/ino/dev/atime/
ctime/isdir/isfile/islink`, the parallel `dragon_lstat_*` set, `dragon_uname_*`,
`dragon_timespec_*`, `dragon_http_parsed_*`. **All of it exists solely because `.dr` cannot
read a field off a C struct at a known offset.** A C-struct-layout FFI (`@repr(C)` structs /
typed offset reads on a `ptr`) would let a large swath of `runtime_platform.cpp` collapse into
`os.dr`/`socket.dr` - and is independently a prerequisite for clean *external* bindings
(`stat`, `sockaddr`, `curl` option structs).

---

## Options Considered

### Option A - Status quo: declare the boundary "good enough," move nothing

- **Pro:** zero work; nothing regresses.
- **Con:** knowingly leaves pure logic (`normpath`/`relpath`) in C++ in direct contradiction
 of, and gives up the long-term path to shrinking the C++ surface. Fails the audit's
 own premise.

### Option B - Aggressive dogfood now: rewrite string methods + builtins in `.dr` immediately

- **Pro:** maximal dogfooding purity (#3); smaller C++ tree today.
- **Con:** **violates #1.** Against today's codegen, byte-loop + allocation-heavy
 `.dr` runs ~6-30× the C version (scrypt). Shipping that trades the top-priority
 commandment for the third. It is the "easy purity win" #2 forbids.

### Option C - Selective harvest now + gate the large migrations behind enabling primitives (**chosen**)

Harvest only the slice that is *both* pure logic *and* free of speed risk; freeze the rest in
C++ until the two mechanisms that would let `.dr` match C speed exist; treat each mechanism as
its own scoped decision. This is the dogfooding policy applied *correctly*: when a
language gap blocks an at-speed implementation, **extend the language** (a builder primitive, a
prelude) rather than either retreating permanently to C++ *or* shipping a slow `.dr` version.

- **Pro:** respects the commandment order (speed → optimal → parity); removes a real leak now;
 turns the remaining migration into well-scoped language work instead of a slow rewrite.
- **Con:** the big C++ blocks (`runtime_string.cpp`) stay until the builder/prelude land -
 i.e., this defers, rather than achieves, full dogfooding. Accepted: deferral at C speed beats
 arrival at 1/10th C speed.

---

## Decision

Adopt **Option C**. Concretely, in priority order:

1. **Harvest now (zero speed risk):** move `dragon_normpath` / `dragon_relpath` into
 `stdlib/os/path.dr` as native `.dr` and delete the C++ originals + their `extern` forwards.
 Proves the pattern end-to-end and removes the clearest violation.
2. **Gate the large migrations behind two enabling mechanisms**, each shipped under its own
 ADR before the corresponding C++ is touched:
 - **(a) A mutable byte/string builder primitive** (`bytearray`-class: amortized-growth
 buffer with raw-span append, exposed to `.dr`) - the prerequisite for moving string
 methods and byte ops at C speed.
 - **(b) A prelude / auto-import mechanism** - the prerequisite for moving pure-logic
 builtins (`divmod`, `abs`, `bin`/`hex`/`oct`, `zip`, `enumerate`, `sum`/`all`/`any`,
 `min`/`max`) out of `CallBuiltins.cpp` into a `.dr` prelude. `sorted` waits on a `.dr`
 Timsort that benchmarks ≥ the current `std::sort` path.
3. **Optionally pursue C-struct-layout FFI** as a third ADR - it collapses the
 `runtime_platform.cpp` accessor explosion *and* improves external bindings; larger blast
 radius, schedule independently.
4. **Never move:** `runtime_core`, exceptions, scheduler, box, object storage, and the genuine
 library shims (zlib/zstd/PCRE2/llhttp/mbedTLS/ed25519).
5. **Leave for now:** `template_escape_*`, `fnv1a`, `fileio`, set algebra - revisit *only*
 after 2(a) lands and a benchmark shows the `.dr` version is not slower.

**Gating rule for every future migration:** a chunk of C++ moves to `.dr` only when a
benchmark shows the `.dr` version is **not slower** than the C++ original. No move regresses
performance to gain purity.

---

## Implementation Phases

### Phase 1 - Harvest `normpath`/`relpath` (this ADR) done
- Reimplemented `normpath`/`relpath` natively in `stdlib/os/path.dr` (with a private
 `_segments` helper and an `extern "C" def getcwd` for the cwd absolutization relpath needs).
- Deleted `dragon_normpath`/`dragon_relpath` from `lib/Runtime/runtime_platform.cpp`
 (1393 → 1231 lines) and their stub mirrors from `reference/runtime-api.h`.
- `stdlib/os/os.dr`'s flat `path_normpath`/`path_relpath` now delegate to the `os.path`
 natives via `from os.path import normpath as _normpath, relpath as _relpath` - no
 duplication, no remaining `extern` to the deleted symbols.
- Behavioral parity verified two ways: a side-by-side harness confirmed the `.dr` port
 matches the old C++ output on 24 edge cases (incl. cwd-dependent relative paths) *before*
 deletion; `test/dr/test_ospath.dr` (20 cases, auto-registered as `dr_test_ospath`) now pins
 it. Full `ctest` suite (36/36) green after the runtime rebuild.

### Phase 2 - Builder primitive (separate ADR)
- Design + ship the mutable byte/string builder; then migrate string/byte methods *one family
 at a time*, each gated by a benchmark vs. the C++ original.

### Phase 3 - Prelude / auto-import (separate ADR)
- Design + ship the prelude module mechanism; migrate the trivial-arithmetic builtins; land a
 `.dr` Timsort before retiring `dragon_sorted`.

### Phase 4 - C-struct-layout FFI (separate ADR, optional)
- Collapse the `stat`/`uname`/`timespec`/`http_parsed` accessor families into `.dr`.

---

## Motto Check

1. **Speed is king.** The whole decision is structured *around* #1: the only thing moved
 immediately (`normpath`/`relpath`) is provably not on a hot path; everything byte-loop- or
 allocation-shaped is frozen in C++ until a primitive lets `.dr` match C speed, enforced by a
 no-regression benchmark gate. We explicitly reject the option (B) that would trade speed for
 purity.
2. **Efficiency over quick wins.** We refuse both the "leave it" non-effort and the "rewrite it
 slow now" quick purity win, in favor of the optimal long-term path: extend the language so
 the migration arrives *at speed*.
3. **Python API parity.** The targets are exactly the surfaces where parity is most visible
 (string methods, builtins, `os.path`); doing them in `.dr` behind the right primitives makes
 parity *easier* to extend and audit than C++ does - but only when #1 is satisfied first.

---

## Where this leaves dogfooding

### Positive
- A written, principled boundary: every reader can see *why* a given function is C++ vs `.dr`,
 and the no-regression gate keeps future migrations honest.
- One real violation removed immediately; the rest converted from "should rewrite someday"
 into two concretely-scoped language features.
- The enabling primitives (builder, prelude, struct FFI) each carry value *beyond* this
 migration - user `bytearray`, user-defined preludes, and external struct bindings.

### Negative
- Full dogfooding is **deferred**, not achieved: `runtime_string.cpp` and the builtin shims
 remain C++ until Phases 2-3 land. This ADR consciously accepts that.
- Adds three follow-on ADRs to the backlog before the large C++ blocks can shrink.

### Neutral
- The C++ line count barely moves after Phase 1; the real reduction is back-loaded behind the
 enabling work. The value of Phase 1 is the *policy*, not the diff size.

## Open Questions

- **Builder shape:** does the byte/string builder expose a raw mutable span to `.dr` (fastest,
 but a new unsafe-ish surface), or only safe append/extend ops? Resolve in the Phase 2 ADR.
- **Prelude visibility:** is the prelude a fixed stdlib module auto-imported by the driver, or
 user-overridable? Parity leans fixed; extensibility leans overridable. Phase 3 ADR.
- **`sorted` threshold:** how close must a `.dr` Timsort get to `std::sort` before we retire the
 C++ path - exact parity, or within a fixed margin on representative inputs?
- **Float formatting:** `dragon_float_to_str` currently delegates to `snprintf("%g")`
 (`runtime_string.cpp`), which is *not* Python's shortest-round-trip `repr`. Out of scope here,
 but flagged: a parity-correct float formatter is its own task and would be a Ryu/Grisu-class
 primitive, not a dogfooding candidate.
