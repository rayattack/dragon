# Decision 041: Public Native-Extension FFI ABI

Done. C FFI already works (`extern "C"`, `ptr`/`intc`, `-l`/`-L`/`-I` in `Driver.cpp`). I proved it wiring `threading.dr` against raw pthread. What's missing is a written ABI an external contributor can actually rely on when wrapping libcurl, SQLite, OpenCV, etc., plus two marshalling helpers and a sane C++ shim build path. Telling people to manually `c++ -c shim.cpp && ar rcs … && dragon build … -lstdc++` is the workaround I refuse to ship.

This decision:

1. **Publishes the public native-extension ABI** as a stability commitment: the
 Dragon↔C type mapping, the `str`/`bytes` representation rules, the `dragon_*`
 runtime symbols external code may link against, refcount/ownership rules across the
 boundary, and an explicit list of what is **not** guaranteed (opaque struct layouts,
 the closure calling convention, refcount intrinsics).
2. **Adds two marshalling primitives** so the documented path is complete rather than a
 workaround: `dragon_str_to_utf8_bytes(str) -> bytes` (a NUL-terminated UTF-8 buffer
 wrapped in a GC-managed `bytes`) and `dragon_bytes_data(bytes) -> ptr` (the raw
 `uint8_t*`). It also fixes `str.encode` (`dragon_str_encode`), which silently
 truncated non-ASCII text.
3. **Adds C++-shim build support** to `dragon build`: a `--cc-source <file>` flag that
 compiles a C/C++ source against the same target and links it in, switching the link
 driver from `cc` to `c++` when any C++ shim is present.

It explicitly does **not** add a package/manifest layer (deferred to 's `.drs`).

---

## Context / Motivation

### The capability is already there - and proven

`stdlib/threading.dr` binds **raw libc and pthread** directly, with no runtime shim:

```python
extern "C" def malloc(size: int) -> ptr
extern "C" def free(p: ptr)
extern "C" def pthread_mutex_init(mtx: ptr, attr: ptr) -> intc
extern "C" def pthread_mutex_lock(mtx: ptr) -> intc
```

This is exactly the pattern an external contributor would use against any installed C
library. The mechanism works today:

- `extern "C" from "curl" { def curl_easy_init -> ptr }` forward-declares the symbol at
 its native LLVM type and auto-appends `-lcurl` to the link line (`CodeGen.cpp:846`).
- `dragon build app.dr -I/usr/include -L/usr/lib -lcurl` covers include/lib paths.

### So what's missing?

Three things, none of them teh core capability:

1. **No published ABI contract.** The mapping that makes this work - `str` is a
 `char*`, `int` is `int64_t`, `intc` is `int`, `ptr` is `void*` - lives only in
 codegen and `lib/Runtime/runtime_internal.h` (not shipped). An external author has to
 reverse-engineer it, and nothing stops a future refactor from silently breaking them.
 **The moment we tell external developers "do this," the ABI becomes a commitment.**
 That commitment must be written down and scoped.

2. **The `str` story has a sharp edge.** A Dragon `str` value is `&DragonString::data[0]`
 (header-behind-the-pointer; `runtime_internal.h:143`), but `kind` is **1**
 (latin1, NUL-terminated - a valid C string) or **4** (UCS-4 - *not* a byte string).
 "Pass a `str` where C wants `const char*`" is only correct for ASCII. General text
 needs an explicit UTF-8 conversion, and today there is no blessed one-call accessor
 for it (the same gap exists for `bytes` → `uint8_t*`).

3. **C++ libraries have no supported build path.** OpenCV, Qt, ICU, etc. expose C++
 classes, not a C ABI; name mangling means you cannot `extern "C" from "opencv_core"`
 directly. You must write a thin `extern "C"` shim - but `dragon build` can only link
 pre-built archives (`-l`), it cannot compile a shim, and it always links with `cc`,
 which does not pull in the C++ runtime or correctly stage C++ static
 initializers/exception machinery.

Dogfooding rule: extend the language/runtime, don't document a workaround.
Telling people to manually `c++ -c shim.cpp && ar rcs … && dragon build … -lstdc++`
is the workaround we refuse to ship. Close the gaps first, then write the guide.

---

## Options Considered

### Option A - Docs only

Write a guide for the C path that works today; leave the `str` edge, the `bytes`
accessor, and the C++ build to per-user folklore.

- **Pro:** zero code, ships immediately.
- **Con:** documents a half-truth. The `str`-as-`char*` shortcut is a latent UTF-8 bug;
 the C++ path is an unsupported, fragile dance. Violates "no workarounds." And without a
 written ABI contract, every doc example is a promise we haven't actually made - the next
 runtime refactor breaks external code with no warning.

### Option B - Publish the ABI + two primitives + C++ build support (**chosen**)

Make the documented path real and stable: pin the ABI as a contract, add the two
marshalling accessors so string/bytes FFI is correct (not just ASCII-correct), and teach
`dragon build` to compile+link a C++ shim with the right driver.

- **Pro:** the guide describes a complete, correct, supported path. The ABI contract lets
 us evolve internals safely (anything not in the contract is fair game). Small, localized
 changes (two ~3-line runtime functions, one CLI flag, one link-driver switch).
- **Con:** ABI publication is a commitment - we can no longer freely rename/repurpose the
 blessed `dragon_*` symbols. Mitigated by keeping the blessed set deliberately small.

### Option C - Option B + a `[native]` manifest section

Also wire native deps (libs, search paths, shim sources) into the `.drs` project manifest
, so `dragon build` picks them up without CLI flags.

- **Pro:** native deps become a first-class, per-project, reproducible concern.
- **Con:** depends on the `.drs` build-manifest integration, which is still Proposed
 . Couples this decision to an unbuilt one. **Deferred** - the ABI + primitives +
 build flag are independently shippable and unblock external contributors now; the
 manifest is a pure ergonomics layer on top and belongs in 's scope.

---

## Design

### 1. The public Dragon ↔ C type mapping (the contract)

This table is the stable surface. Per, Dragon values flow at their native LLVM types, which *are* teh C ABI types - there is no marshalling thunk:

| Dragon type | C type at the boundary | Direction notes |
|---|---|---|
| `int` | `int64_t` | exact |
| `float` | `double` | exact |
| `bool` | `bool` (`i1`, zero-extended in varargs) | exact |
| `intc` | `int` (`i32`; `i16` on 16-bit targets) | the C-`int` bridge - use for any C function taking `int`/`unsigned`-width args |
| `ptr` | `void*` | opaque handles; the universal escape hatch |
| `str` | `const char*` | **see §2** - points at the char buffer; ASCII-safe directly, general text via `dragon_str_to_utf8_bytes` |
| `bytes` | `(uint8_t* data, int64_t len)` | `dragon_bytes_data` + `dragon_bytes_len`; **see §3** |
| `Callable[[A,B],R]` | function pointer `R(*)(A,B)` | a non-capturing Dragon `def` lowers to a bare fn pointer; **see §5** |
| `list[T]`, `dict[K,V]`, instances | `void*` to the runtime object | layout is **not** public; manipulate only via blessed `dragon_*` calls |

`int`/`float`/`intc`/`ptr`/`bool` are the *guaranteed-trivial* core: any C function whose
signature uses only these can be bound with a bare `extern "C"` declaration and zero glue.

### 2. The `str` ABI and the `dragon_str_to_utf8_bytes` primitive

A `str` value is a pointer to `DragonString::data[]`, with the object header (refcount,
tags, `len`, `kind`) immediately *before* it (`dragon_string_from_data` recovers the
header by subtracting `offsetof`). Two storage kinds:

- **`kind == 1`** - pure ASCII, NUL-terminated. The pointer *is* a valid C string and is
 already valid UTF-8 (byte count == code-point count).
- **`kind == 4`** - UCS-4 (one 32-bit code point per slot), used whenever any code point
 is ≥ 0x80. The bytes are **not** UTF-8 and **not** a C string.

Therefore the contract is:

- **Fast path (ASCII only):** passing a `str` to a `const char*` parameter is valid and
 zero-copy *iff the contents are ASCII* (kind=1). The guide marks this ASCII-only.
- **General path:** `dragon_str_to_utf8_bytes(s: str) -> bytes` returns a fresh,
 **NUL-terminated UTF-8** `bytes` for any string (kind=1 borrow-copies; kind=4
 transcodes via the existing `dragon_str_to_utf8_alloc`). The buffer's lifetime is the
 `bytes` object's - a normal GC-managed Dragon value freed by ordinary scope cleanup -
 so there is no cache-invalidation or manual-free hazard. The caller hands
 `dragon_bytes_data(b)` (and `dragon_bytes_len(b)`) to C and keeps `b` in scope across
 the call. Round-trip back to a `str` uses `dragon_string_alloc(const char* src, int64_t len) -> str`.

> **Design note.** This supersedes an earlier sketch of `dragon_str_utf8(str) -> ptr` with
> a per-object UTF-8 cache (à la CPython's `PyUnicode_AsUTF8`). `DragonString` is sized to
> a fixed 32-byte header with no spare field, so a cache would need an out-of-band side
> table plus an invalidation story against the in-place-append (`cap`) fast path. Returning
> a `bytes` - whose lifetime is already managed - sidesteps all of that. Optimal over
> convenient (#2).

As a correctness corollary, `str.encode` (`dragon_str_encode`) is fixed to transcode
kind=4 strings to UTF-8 as well; it previously `strlen`'d the UCS-4 buffer, truncating at
the first embedded NUL (e.g. `"😀".encode` returned `b''`). ASCII strings are
byte-identical to before.

### 3. The `bytes` ABI and the `dragon_bytes_data` primitive

`DragonBytes` is `{ header; int64_t len; uint8_t* data; }`. The blessed accessors:

- `dragon_bytes_data(b: bytes) -> ptr` - the raw `uint8_t*` (new; see §"primitives").
- `dragon_bytes_len(b: bytes) -> int` - the byte length (exists).
- `dragon_bytes_new(data: ptr, len: int) -> bytes` - construct from a C buffer (exists;
 **copies**, so the source buffer's lifetime is the caller's problem only until return).

This replaces the current habit of (ab)using `dragon_bytes_decode` (whose verb implies
text decoding) to get at the pointer. `bytes` is the recommended vehicle for **all**
binary FFI and for non-ASCII string FFI (`dragon_str_encode(s) -> bytes`).

### 4. The blessed `dragon_*` symbol set (linkable from external `.dr`)

The runtime is statically linked into every Dragon binary, so external `.dr` code may
`extern "C"`-declare and call these. **Only this set is contractual**; everything else in
`runtime-api.h` is internal and may change:

- **Strings:** `dragon_string_alloc`, `dragon_str_to_utf8_bytes`, `dragon_str_encode`, `dragon_str_len`
- **Bytes:** `dragon_bytes_new`, `dragon_bytes_data`, `dragon_bytes_len`, `dragon_bytes_decode`
- **Refcount (for objects an extension stores beyond a call):** `dragon_incref`,
 `dragon_decref` (and the str-specialized `dragon_incref_str`/`dragon_decref_str`)
- **Errors:** `dragon_raise_exc(code: int, msg: str)` to surface a C-side failure as a
 Dragon exception.

`reference/runtime-api.h` gains a header banner marking this subset **stable**; the rest
**internal**.

### 5. Refcount & ownership rules across the boundary

The one place FFI bites. The contract:

- **Scalars/`ptr`** carry no ownership - passing them is free.
- **`str`/`bytes`/objects passed *into* C for the duration of one call:** borrowed. C must
 not retain the pointer past the call without an explicit `dragon_incref`.
- **`str`/`bytes` *returned from* a blessed constructor** (`dragon_string_alloc`,
 `dragon_bytes_new`) arrive with refcount = 1, owned by the Dragon side; normal scope
 cleanup frees them.
- **A C library's own allocations** (e.g. a `CURL*`) are owned by C; Dragon holds them as
 opaque `ptr` and must call the library's destructor (`curl_easy_cleanup`). Dragon's GC
 never touches a bare `ptr`.

### 6. C libraries - works today, no code change

```python
extern "C" from "curl" {
 def curl_easy_init -> ptr
 def curl_easy_setopt(h: ptr, opt: intc, val: str) -> intc # ASCII opt; UTF-8 via dragon_str_utf8
 def curl_easy_perform(h: ptr) -> intc
 def curl_easy_cleanup(h: ptr)
}
```
```bash
dragon build app.dr -L/usr/lib -I/usr/include -lcurl
```

### 7. C++ libraries - the shim + `--cc-source`

C++ surfaces need a thin `extern "C"` shim (one-time, written by the binding author):

```cpp
// cvshim.cpp
#include <opencv2/opencv.hpp>
extern "C" void* cvshim_imread(const char* path) { return new cv::Mat(cv::imread(path)); }
extern "C" int cvshim_rows(void* m) { return static_cast<cv::Mat*>(m)->rows; }
extern "C" void cvshim_free(void* m) { delete static_cast<cv::Mat*>(m); }
```
```python
extern "C" from "opencv_core" {
 def cvshim_imread(path: str) -> ptr
 def cvshim_rows(m: ptr) -> intc
 def cvshim_free(m: ptr)
}
```
```bash
dragon build app.dr --cc-source cvshim.cpp -lopencv_core -lopencv_imgcodecs
```

`--cc-source <file>` (repeatable):
1. Compiles `<file>` to a temp object with the **same target triple** as the program
 (`c++` for `.cpp`/`.cc`/`.cxx`/`.C`, `cc` otherwise), forwarding `-I`/`-O` flags.
2. Adds the object to the final link.
3. If any compiled source was C++, the final link driver switches `cc` → `c++` so the
 C++ runtime, static initializers, and exception tables link correctly. (Linking C++
 via bare `cc -lstdc++` works for trivial shims but breaks on static init / thread-local
 / exception edge cases - switching the driver is the root-cause fix, not `-lstdc++`.)

---

## Implementation Phases

### Phase 1 - Marshalling primitives done
- `dragon_str_to_utf8_bytes(const char* s) -> DragonBytes*` in `runtime_collections.cpp`:
 reuses `dragon_str_to_utf8_alloc` (the kind-aware encoder already used by `print`),
 returns a NUL-terminated UTF-8 `bytes`.
- `dragon_bytes_data(DragonBytes* b) -> uint8_t*` in `runtime_collections.cpp`: returns
 `b->data`.
- Fixed `dragon_str_encode` (str.encode) to transcode kind=4 → UTF-8 (was
 `strlen`-truncating).
- Mirrored in `reference/runtime-api.h` with the `[STABLE FFI - ADR 041]` markers and the
 stable/internal banner.

### Phase 2 - C++ shim build done
- `--cc-source <file>` parsing in `Driver.cpp` → `options.ccSources`; `-I` search paths
 forwarded as `includePaths`.
- In `CodeGen::linkExecutable`: a shared `runTool(argv)` helper (fork/execvp on POSIX,
 `_spawnvp` on Windows) compiles each `ccSource` to a temp `.o` (`c++` for
 `.cpp/.cc/.cxx/.C/...`, else `cc`), tracks `anyCxxShim`, appends the objects to the
 link, and selects `c++`/`g++` as the link driver when any C++ shim is present. Temp
 objects are anchored off the existing object-file temp path and cleaned up after link.

### Phase 3 - Tests done
- `test/dr/ffi_cxx/{app.dr,shim.cpp}`: a `std::string`-using C++ shim compiled via
 `--cc-source` (proving the `cc`→`c++` switch - bare `cc` would leave libstdc++ symbols
 undefined), bound via `extern "C"`, asserting `dragon_str_to_utf8_bytes("héllo")` → 6
 NUL-terminated UTF-8 bytes through the shim's `strlen`. Registered explicitly in
 `test/CMakeLists.txt` (the `test_*.dr` glob can't pass `--cc-source`).

### Phase 4 - The guide done
- `dragonlang-org/content/docs/920-ffi.md` - the book's FFI chapter (previously a "coming
 soon" stub) now covers: the two `extern "C"` forms, the type table, the `str`/`bytes`
 rules, ownership rules, worked C (zlib CRC-32) and C++ (OpenCV shim via `--cc-source`)
 examples, and a troubleshooting table. Registered in `SUMMARY.md` as "FFI: Calling C and
 C++". (No separate compiler-repo doc - the book is the single source of truth.)

---

## Motto Check

1. **Speed is king.** The core type mapping is zero-copy/zero-thunk - a bound C call is a
 direct native call. The only added cost is opt-in: `dragon_str_utf8` on a kind=4 string
 does one encode (cached), which is strictly less work than the alternative of forcing
 every string through a box. No path slows down for code that doesn't use FFI.
2. **Efficiency over quick wins.** We reject the docs-only workaround and the
 `cc -lstdc++` hack in favor of the root-cause fixes (a real UTF-8 accessor; the correct
 link driver). The ABI contract is the efficient long-term choice: it lets internals
 evolve freely behind a small frozen surface.
3. **Python API parity.** `dragon_str_utf8` mirrors `PyUnicode_AsUTF8`; the
 borrow/own/incref discipline mirrors CPython's C-API reference rules, which is the model
 Python C-extension authors already know.

---

## For anyone writing bindings

### Positive
- External contributors get a documented, correct, supported way to wrap C **and** C++
 libraries - the foundation for community bindings (numpy-class numerics via BLAS,
 image IO, database drivers) without touching Dragon's own tree.
- The ABI contract decouples internal refactors from external code: anything outside the
 blessed set stays free to change.
- String/bytes FFI becomes encoding-correct, not just ASCII-correct.

### Negative
- A real stability commitment: the blessed `dragon_*` symbols and the type mapping can no
 longer change casually. Kept manageable by freezing a deliberately small set.
- `--cc-source` makes `dragon build` a (minimal) C/C++ compile driver; we now depend on a
 working `c++` toolchain being present when C++ shims are used.

### Neutral
- The `.drs` `[native]` manifest section (Option C) remains future work under .
- `runtime-api.h` stays hand-maintained; the new banner makes the stable/internal split
 explicit but does not change how it is generated.

## Open Questions
- Should `dragon_str_utf8`'s cached buffer be invalidated on in-place string mutation, or
 do we rely on `str` immutability? (Dragon strings are immutable at the surface; the
 cache is safe, but the in-place-append fast path on `cap` must not alias a handed-out
 UTF-8 pointer - Phase 1 must verify.)
- Do we want a `dragon build --emit-bindings` helper later that scaffolds an `extern "C"`
 block from a C header? Out of scope here; note for a future ergonomics ADR.
