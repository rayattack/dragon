# Decision 035: Compile-Time Imports and Static `importlib`

**Status:** Approved

Python's `import` is runtime - parse file, run top-level, cache in `sys.modules`, `importlib.import_module(name)` for computed names. Ours is the opposite: `ModuleResolver` walks the graph at build time, everything links to static symbols. People keep asking for `importlib`; this ADR is me explaining why I mostly said no, and what pattern to use instead.

---

## Revision : adopt Option A, not Option C

The compile-time import policy below stands unchanged: `import` is a build-time directive, there is no runtime `sys.modules`/module objects/source loader. What changed is how we serve `importlib`-shaped use cases: **Option A (pure compile-time, no `importlib`)** instead of Option C (`@expose` + a static registry).

Rationale: the headline use case, picking a callable by a runtime-computed string (plugin dispatch), already works *today* with a dispatch table: module-level `dict[str, Callable[...]]` filled at top level. Option C's registry would have added `reload`, runtime module mutation, loading code not in the build. Those were already non-goals. So `importlib` + `@expose` would buy almost nothing the dispatch-table pattern doesn't, while adding a codegen pass, a runtime registry, and a perfect-hash build step. Per the commandments (speed and architectural integrity over parity), not worth teh weight.

Fallout from picking Option A:
- `importlib.import_module` / `@expose` / a module-export registry are **not** implemented and not planned. Removed from the bug/defect tracker (they were never a defect - `import` works; this was an optional parity nicety).
- Documented pattern is the dispatch table; see `dragonlang-org/content/docs/1001-modules.md` ("Selecting a callable by name at runtime").
- Options B, C, D below and the implementation plan are kept for historical record only; path not taken.

---

## Context / Motivation

Python's `import` is runtime: first execution parses the file, runs top-level code, builds a module object, caches in `sys.modules`. `importlib.import_module(name)` exposes the same for computed names; `getattr(mod, name)` reads off the live module. That's what makes plugins, lazy imports, conditional imports, and reload work.

Dragon is the opposite. `ModuleResolver` walks the import graph at build time, every dependency compiles into one LLVM module, cross-module refs resolve to static symbols at link time (see D022). After D022 and the Phase 1 module-attribute work, `import x.y` and `controllers.health.health_check` already lower to static `Function*` refs with zero runtime indirection.

Documented gap with Python:

```python
# Python: works - name is computed at runtime
mod = importlib.import_module(f"plugins.{name}")
handler = getattr(mod, "handle")
handler(payload)

# Dragon today: no runtime importlib, no module objects
```

This decision settles: **what does Dragon's import model look like, and do we serve `importlib`-shaped use cases without sacrificing speed?**

Current state was unsatisfactory: `import` is already compile-time, but no policy was written down, `importlib` doesn't exist, and Python ports using runtime imports silently break. That violates priority (3) - no workarounds, no silent fallbacks (I should've written this down earlier, honestly).

Three forces:

1. **Speed (priority 1).** Whatever we do must not slow down the 99% of programs that never touch dynamic imports. Runtime module objects, per-call hash lookups, JIT loaders all violate this.
2. **Python parity (priority 2).** Plugin systems, computed-name dispatch, reflective tooling are real. Removing `importlib` outright forces every port to refactor.
3. **Architectural integrity (priority 3).** Dragon is compiled with C linking semantics. `importlib` can't be a half-runtime, half-compile-time mess with two paths.

---

## Options Considered

### Option A - Pure compile-time, no `importlib`

Document `import` as compile-time-only. Don't ship `importlib`. Plugins use compile-time registration tables (e.g. `@register("name")` building `dict[str, Callable]` at module init).

- done Zero runtime cost. Smallest binaries. Simple mental model.
- done Max static analyzability and DCE.
- no Hard parity break. Every Python port using `importlib.import_module(name)` needs a rewrite.
- no Dogfooding spirit: `importlib` is stdlib, and refusing to ship one means we're saying a Python idiom can't run on Dragon at all. User should see "we did it differently for speed," not "we don't support that."

### Option B - Always-emit full registry, every-module-by-default

Compiler emits a registry of every function/class/const in every module, sorted by canonical name. `importlib.import_module(name)` does perfect-hash lookup (~30 ns); `getattr(mod, attr)` does a second lookup. Module objects are read-only handles into `.rodata`.

- done Zero ceremony. Any function in any compiled module is `import_module`-able. Closest to Python ergonomics.
- done Per-call ~30 ns + native call (faster than CPython's interpreted dispatch).
- no **Tax on non-users.** 100-module project ships ~80 KB of `.rodata` even when `importlib` is never imported. Hello-world pays for a feature it doesn't use.
- no **DCE-blocked.** Linker can't strip any function symbol - any might be looked up dynamically. Release binaries grow; I-cache pressure increases.
- no Violates priority (1): non-users pay binary-size tax, DCE-blocking compounds as codebase grows.

### Option C - Conditional registry, opt-in via `@expose` *(originally chosen, superseded)*

Compiler emits a static module-export registry **if and only if** `importlib` is in the build graph. When emitted, only `@expose` symbols are included. `--expose-all` overrides for projects that want every symbol discoverable.

Runtime API matches Python's `importlib`: `import_module(name) -> Module | None`, `getattr(mod, name)`, `hasattr`, `dir`. Perfect-hashed at compile time, O(1), ~30 ns per call. After lookup, resolved symbol is a normal Dragon `Callable` / class - same path as static imports.

- done **Zero tax for non-users.** Hello-world stays small. Registry/runtime/importlib only linked if referenced.
- done **Full DCE on un-exposed symbols.** Release builds stay tight.
- done **O(1) lookup** (~30 ns). After resolution, native call cost.
- done Matches Python `importlib` API for lookup-only subset (~95%). `import_module`, `getattr`, `hasattr`, `dir` work.
- warn `@expose` per public-by-string function. Same cost as Rust `pub` or Java `public`.
- warn No `reload`, source-from-disk loading, runtime-defined modules. Explicit non-goals.

### Option D - Real runtime `importlib` with embedded JIT

Runtime module loader: Dragon parser/sema/codegen in the runtime, compiling `.dr` at runtime, live module objects with mutable `__dict__`s.

- done Full Python parity, including `reload`, runtime-defined classes.
- no Multi-month effort (compiler-in-runtime).
- no Violates priority (1): every `import_module` pays parse/compile/codegen, typically 10-100 ms per fresh module, not 30 ns.
- no Violates priority (3): parallel codegen path to keep in sync with build-time. Two compilers, two bug sets.
- no Breaks "C-speed compiled binary" identity.

---

## Decision

**Revised : adopt Option A.** The original decision below chose Option C; that path was superseded after the dispatch-table pattern proved sufficient.

Historical record of Option C policy (not implemented):

### Policy summary (Option C one-liner, not shipped)

> Dragon `import` is a **compile-time directive**, not a runtime statement. `importlib.import_module(name)` would do perfect-hash lookup over modules already linked into your binary. `@expose` opts a function into the runtime registry. Source-from-disk loading and `reload` not supported.

### Concrete rules (Option C, historical)

1. **`import` statements processed at compile time.** `ModuleResolver` builds the dependency graph; CodeGen emits all transitive modules into one LLVM module. No `sys.modules`, no runtime module table.

2. **Module-level top-level code** runs once at program startup, before `main`, in topological order. Static initialization (C++ global ctors), not "import execution." Pure constants folded at compile time when possible.

3. **`importlib` would have been a stdlib module** with `import_module`, `Module.__getattr__`, `Module.__dir__`, `Module.__name__`, and `NotImplementedError` stubs for `reload` etc.

4. **Registry emission gated on `importlib` in build graph.** No registry if absent.

5. **`@expose` decorator** opts symbols into the export table. Compile-time-only, no call overhead.

6. **`--expose-all` build flag** for plugin frameworks / REPL / strict Python-port compatibility.

7. **Registry layout** (`.rodata`):

 ```c
 struct dragon_module_export {
 const char* name;
 void* symbol;
 uint8_t kind; // FUNCTION | CLASS | CONST | SUBMODULE
 uint32_t type_signature;
 };
 struct dragon_compiled_module {
 const char* name;
 uint32_t n_exports;
 const struct dragon_module_export* exports;
 };
 extern const struct dragon_compiled_module dragon_module_registry[];
 extern const uint32_t dragon_module_count;
 ```

8. **Dynamic-name imports via `import_module`, not `import`.** `import x` is compile-time-known names only.

### What ships (Option A)

- `import` is compile-time only. Document it.
- No `importlib` module.
- Plugin dispatch: module-level `dict[str, Callable[...]]` populated at init. See `1001-modules.md`.

---

## What works now (and what doesn't)

### What works

| Pattern | Status |
|---|---|
| `import x.y` / `from x import y` / `import x.y as z` | done static, zero overhead |
| `from x import sub` (submodule) | done static, zero overhead |
| Dispatch table: `handlers[name]` for computed names | done static, zero overhead |
| Plugin systems with compile-time-known plugin set | done headline use case |
| Conditional imports (`try: import lxml except: import xml`) | done both compile in; pick at runtime via dispatch table or `if` |
| Top-level decorator side effects | done run at static init |

### What does not work (explicit non-goals)

| Pattern | Status / Workaround |
|---|---|
| `importlib.import_module(name)` | no not shipped. Use dispatch table. |
| `importlib.reload(mod)` | no no source at runtime. Rebuild. |
| Loading a `.dr` file not in the build | no not in the binary. Subprocess a separate `dragon` binary if you need runtime extensibility. |
| `mod.attr = X` (runtime module mutation) | no no module objects. Use module-level state. |
| `__import__("x", fromlist=...)` low-level | no no equivalent. |
| Lazy/deferred imports for startup speed | warn Not needed - imports are compile-time. Top-level init can be lazy via `lazy const X = ...` (separate Decision). |

### Costs (Option A)

- **Binary size:** no registry overhead. Programs stay as small as before.
- **Compile time:** unchanged.
- **Runtime:** dispatch table is a normal dict lookup. No perfect-hash machinery.

### Documentation

Document the compile-time import model:

> Dragon's `import` is a compile-time directive. No `sys.modules`, no module objects, no source loader. Plugin dispatch uses module-level dispatch tables - see and `1001-modules.md`. `importlib` is not supported.

---

## Implementation Plan (Option C, not taken - historical)

1. `@expose` decorator. Lex/parse, Sema records, no codegen at function.
2. Registry CodeGen pass (`src/codegen/ModuleRegistry.cpp`). Gated on `importGraph.contains("importlib")`.
3. Runtime primitives. `dragon_import_module`, etc. ~80 lines.
4. Stdlib `importlib.dr`. ~80 lines.
5. `--expose-all` flag.
6. Tests. Binary-size diff for hello-world.
7. Anti-tax verification.

None of the above was built. Option A requires documentation only.

---

## References

- compile-time import model documentation.
- (Standard Library).
- (Python Stdlib Coverage).
- (Package Manager).
- Phase 1 module-attribute work, - `ModuleType` registry, `import x.y.z` chains.
- Rust feature flags / C++ "you don't pay for what you don't use" - heritage for Option C's conditional emission (path not taken).
