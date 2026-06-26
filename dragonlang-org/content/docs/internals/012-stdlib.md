# Dragon Standard Library

> **Version:** 0.2.0
> **Directories:** `stdlib/`, `src/ModuleResolver.cpp`, `src/StdlibRegistry.cpp`
> **Last updated:** 2026-06-22

---

## 1. Overview

Dragon's standard library is **self-hosted**: it is written in Dragon (`.dr` files under `stdlib/`), not as a C-name-substitution table. This follows the project's dogfooding policy - anything that can be expressed in Dragon must be written in Dragon, and C/LLVM is reserved for primitives a high-level language genuinely cannot reach (raw syscalls, FFI shims, bundled-library entry points).

Each stdlib module is an ordinary Dragon module compiled by the same pipeline as user code (Lexer -> Parser -> Sema -> TypeChecker -> CodeGen). A module reaches the platform by declaring the C functions it needs with `extern "C"` and calling them at typed Dragon boundaries. The thin layer wrapped this way is libc/libm and the statically linked bundled libraries (SQLite, PCRE2, llhttp, mbedTLS, minicoro); everything user-visible above that line - parsing, formatting, data structures, classes, error handling - is Dragon code.

As of this writing there are roughly **100 stdlib modules** (`find stdlib -name '*.dr' | wc -l` reports 101), spanning text and data, OS and I/O, time and math, collections, cryptography, networking and HTTP, databases, concurrency, and testing.

> Historical note: an earlier design mapped a small subset of Python module symbols to C names through a lookup table (`StdlibRegistry`). That table still exists, but only as a **legacy fallback** (see Section 3). The standard library is now the `.dr` module set described here.

---

## 2. Anatomy of a Self-Hosted Module

A module is Dragon source. Where it touches the platform, it declares an `extern "C"` signature with honest Dragon types, then builds the Pythonic surface on top in pure Dragon.

`stdlib/math.dr` is the clearest example. Constants are module-level globals, and each libm entry point is a typed `extern "C"` declaration:

```python
# math.dr - wraps libm via extern "C"

const pi: float = 3.14159265358979323846
const e: float  = 2.71828182845904523536
const tau: float = 6.28318530717958647692

extern "C" def sqrt(x: float) -> float
extern "C" def pow(x: float, y: float) -> float
extern "C" def sin(x: float) -> float
extern "C" def cos(x: float) -> float
# ... and the rest of libm
```

Calling `math.sqrt(4.0)` compiles to a direct, native `f64` call into libm - no boxing, no name table. The `intc` type (C's `int`, an `i32`) is used at C boundaries that take or return a C `int`, so the ABI stays correct.

Modules range from "thin wrapper" to "almost entirely Dragon":

- **`stdlib/re.dr`** compiles patterns through the bundled **PCRE2 10.44** static library. It declares the PCRE2 C API (`pcre2_compile_8`, `pcre2_match_8`, ...) with `extern "C"`, then exposes a Dragon `Pattern` class plus `match` / `search` / `sub` / `findall` / `split` convenience functions. No external dependency.
- **`stdlib/sqlite.dr`** wraps the bundled **SQLite3** amalgamation (`sqlite3_open`, `sqlite3_prepare_v2`, `sqlite3_step`, ...) and presents a typed connection/cursor surface in Dragon.
- **`stdlib/threading.dr`** wraps POSIX pthreads and presents `Lock`, `RWLock`, `Semaphore`, `Barrier`, `Condition`, and `Event` classes, all usable as context managers (`with` statement).
- **`stdlib/json.dr`** is mostly Dragon: structural traversal and value building happen in Dragon, with `extern "C"` reserved for string<->number conversion and a byte-level scanner that traverses the raw buffer with zero per-character allocation. The module also co-locates the `template[JSON]` content type (spec-32).
- **`stdlib/datetime.dr`** is almost pure Dragon: it provides `timedelta`, `date`, `time`, and `datetime` with arithmetic, comparison, and ISO-8601 formatting. Timestamp decomposition uses Howard Hinnant's days-from-civil algorithm written directly in Dragon, with no C calls beyond what `time` already exposes.
- **`stdlib/ssl.dr`** mirrors CPython 3.12's `ssl` surface (`SSLContext`, `wrap_socket`, `create_default_context`, the `CERT_*` / `PROTOCOL_*` constants, the `SSLError` hierarchy) over an mbedTLS shim, with a modern-only TLS policy enforced in the C shim.
- **`stdlib/unittest.dr`** is a Python-parity test framework written in Dragon, powered by Dragon's own reflection (`dir`/`getattr`, spec-33) and deep `Any` equality (spec-39).

The Pythonic shape (names, signatures, semantics) is preserved only where it expresses cleanly in a statically-typed, compiled model; type annotations are mandatory throughout, and there is no dynamic fallback.

---

## 3. Module Resolution

When the compiler sees `import math` or `from os import listdir`, it resolves the module against the filesystem first and falls back to the legacy registry only if no file is found.

### 3.1 File resolution (the primary path)

The **`ModuleResolver`** (`src/ModuleResolver.cpp`) resolves an import to a source file:

1. The directory of the file being compiled (local modules).
2. Additional search paths, including the directory named by the **`DRAGON_STDLIB_DIR`** environment variable / CMake define - this is where the `stdlib/` tree is installed.
3. Site-packages (installed eggs/packages).

Dotted imports map to paths: `from os.path import join` resolves to `stdlib/os/path.dr`. The strict rule is **flat file XOR package directory, never both**; a package root is `name/name.dr` (for example `stdlib/collections/collections.dr`, `stdlib/http/http.dr`, `stdlib/urllib/urllib.dr`).

All resolved modules compile into a **single LLVM module** (no separate compilation units). CodeGen forward-declares every dependency's functions, classes, and globals, emits their bodies, and then emits the entry file's top-level code inside the synthesized C `main`. Each module's symbols are name-mangled per module (for example `os__listdir`), and a `from`-import records a local alias so calls resolve to the correct mangled symbol.

Every module resolved this way is tracked in **`fileResolvedModules`** (`CodeGenImpl.h`). That set is the priority signal: a file-resolved module never consults the registry.

### 3.2 The `StdlibRegistry` fallback

`StdlibRegistry` (`include/dragon/StdlibRegistry.h`, `src/StdlibRegistry.cpp`) is a singleton lookup table that maps a handful of module symbols directly to C names/expressions and the headers they need. Historically it *was* the standard library mechanism; today it is a **fallback only**.

In `CodeGen::visit(ImportStmt&)` (`src/codegen/Statements.cpp`) the guard is explicit:

```cpp
for (auto& alias : node.names) {
    // Skip StdlibRegistry for modules resolved as .dr/.py files
    if (impl_->fileResolvedModules.count(alias.name)) continue;
    registry.resolveImport(alias.name, alias.asName,
                           impl_->symbolAliases, dummy);
}
```

So for any module that exists as a `.dr` file - which is the entire `stdlib/` tree - the registry is bypassed entirely. It remains in the codebase to catch the residual case where an import names something not backed by a source file, but it is no longer how the standard library is implemented or extended. New stdlib functionality is added by writing Dragon modules, not by adding registry entries.

---

## 4. Module Survey

This is a representative survey grouped by area, not an exhaustive listing. All paths are under `stdlib/`.

### Text and data
`string`, `re` (PCRE2), `json`, `csv`, `configparser`, `tomllib`, `template`, `textwrap`, `difflib`, `shlex`, `struct`, `binascii`, `base64`, `quopri`, `html`, `reprlib`, `fnmatch`, `glob`.

### OS and I/O
`os` (package: `os/os.dr`, `os/path.dr`), `sys`, `io`, `pathlib`, `shutil`, `tempfile`, `stat`, `glob`, `fileinput`, `filecmp`, `linecache`, `subprocess`, `signal`, `errno`, `platform`, `getpass`, `syslog`, `gettext`, `argparse`.

### Compression and archives
`gzip`, `zipfile`, `tarfile`, `zstandard` (package).

### Date, time, math, and numerics
`datetime`, `time`, `calendar`, `sched`, `timeit`, `math`, `statistics`, `fractions`, `bisect`, `heapq`, `colorsys`, `operator`.

### Collections and functional
`collections` (package: container datatypes, plus concurrent collections such as `SyncList`/`SyncDict`), `itertools`, `functools`, `enum`, `graphlib`.

### Cryptography and hashing
`hashlib`, `hmac`, `secrets`, `crypto`, `argon2id`, `totp`, `uuid`.

### Networking and HTTP
`socket`, `ssl` (mbedTLS), `ipaddress`, `urllib` (package: `parse`, `request`, `response`, `error`, `robotparser`), `http` (package: `client`, `server`, `connection`, `cookies`, `cookiejar`, `sessions`, `message`, `multipart`, `websocket`), `mimetypes`, `netrc`.

### Databases
`sqlite` (bundled SQLite3) and the `database` package (`base`, `sqlite`, `mysql`, `postgres`) for a uniform driver-style API.

### Concurrency
`threading` (pthread-backed `Lock`/`RWLock`/`Semaphore`/`Barrier`/`Condition`/`Event`). Green threads, scoped OS threads, and `Task[T]` are language-level features (see the concurrency docs), not a module.

### Diagnostics and testing
`unittest` (Dragon-native, Python-parity test framework), `logging`, `warnings`, `random`.

### UI and tooling
`ui` (package, with a `desktop` submodule), `drs` (manifest support).

---

## 5. How an Import Compiles End-to-End

Take `from math import sqrt`:

```python
from math import sqrt
x: float = sqrt(4.0)
print(x)
```

1. **Resolve.** `ModuleResolver` finds `stdlib/math.dr` on a `DRAGON_STDLIB_DIR` search path and records `math` in `fileResolvedModules`.
2. **Compile the module.** `math.dr` is parsed, type-checked, and forward-declared alongside the entry file in the single LLVM module. Its `extern "C" def sqrt(x: float) -> float` becomes an external declaration bound to libm's `sqrt`.
3. **Bind the name.** The `from`-import records that the bare name `sqrt` in the importing module maps to math's `sqrt`. Because `sqrt` is `extern "C"`, it keeps its C symbol name rather than a per-module mangle.
4. **Emit the call.** `sqrt(4.0)` lowers to a direct, native `call double @sqrt(double 4.0)` - no name-table substitution, no box.
5. **Link.** The final object links against libm (`-lm`); bundled-library modules link against their static archives via the CMake-provided paths.

For a module like `re` or `sqlite`, the same flow applies, except the `extern "C"` declarations bind to the bundled static library rather than libc, and the Dragon class surface (`Pattern`, connection/cursor) is emitted from the module's own `.dr` body.

---

## 6. Properties and Guarantees

- **Types are honest end-to-end.** Stdlib functions carry full Dragon signatures, so calls are type-checked like any other code, values flow at their native LLVM types, and monomorphized containers stay allocation-free. There is no untyped name-substitution layer between the call site and the type checker.
- **Memory is managed.** Dragon uses reference counting plus a cycle collector; stdlib values participate in the same ownership model as user values. Heap locals are freed at block exit. Earlier `getcwd`-style "leaks memory" caveats no longer apply.
- **No dynamic module objects.** Consistent with Dragon's static model, `import math` does not create a runtime module object; `type(math)` / `dir(math)` over a module are not supported. Modules are namespaces of typed declarations resolved at compile time.
- **Bundled, not external.** SQLite, PCRE2, llhttp, mbedTLS, and minicoro are statically linked, so modules that depend on them have no external runtime dependency.
- **Extending the stdlib means writing Dragon.** A new module is a new `.dr` file under `stdlib/`; a language or runtime gap that blocks a Dragon implementation is fixed by extending the language/runtime, not by retreating to C.

---

## Previous Document

[011 - Type Hint Enforcement](011-type-hints.md)

## Next Document

[013 - Dragon Diagnostic and Error Reporting System](013-diagnostics.md)
