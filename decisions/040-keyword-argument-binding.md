# Decision 040: Keyword-Argument Binding for Declared Parameters

Approved. I spent a stupid afternoon chasing why `drs.envs(env='prod')` kept coming back with defaults - turns out keyword args at call sites were silently dropped for anything with declared parameters. `f(name='api', port=8080)` against `def f(name: str = 'dev', port: int = 80)` still ran with `dev / 80`. Kwargs went nowhere; defaults filled the slots. After this fix, named args bind to the matching parameter by name.

Localized codegen fix: ~100-300 LOC in `CallExpr.cpp` plus a per-function param name table. Dragon Script was the consumer that made this impossible to ignore.

---

## Context / Motivation

Python developers reaching for Dragon naturally write:

```dragon
def connect(host: str = 'localhost', port: int = 5432, ssl: bool = false) {
 ...
}

connect(port=9000, ssl=true) # expecting host='localhost' default, others overridden
```

In current Dragon, `port=9000, ssl=true` are silently discarded and the
function runs with all defaults. There is no diagnostic.

**Diagnosis** :

The call-site codegen has two paths:

1. **Vararg path** (`hasVarArgs == true`, lines 297-385). Functions declared
 with `*args` or `**kwargs` get their `node.kwArgs` packed into a tagged
 dict and passed as the kwargs param. Works correctly.

2. **Normal path** (lines 387-426). For functions WITHOUT `*args`/`**kwargs`,
 codegen iterates `node.args` (positional) and never reads `node.kwArgs`.
 After the loop, `fillDefaultArgs` fills any unpopulated slots with their
 default values. Keyword arguments at the call site are silently dropped.

This is teh bug.

### Why this matters

- **'s `drs.envs(env='prod', region='us-east')` API depends on this.**
 Without keyword binding, every drs user has to use positional construction
 of the envs container - order-dependent, brittle, unergonomic.
- **Class constructors with default fields are broken.** `Config(timeout=30)`
 silently ignores the timeout and uses the default. Subtle data corruption
 bug.
- **Python parity.** Keyword args are a important ergonomic feature in Python; public APIs assume them.
- **Surprise-free behavior.** Silent default-fill instead of binding is the
 worst-case failue mode - no error, wrong runtime behavior.

---

## Design

### Per-function parameter-name table

CodeGen already emits per-function metadata for:
- `funcParamDefaults: map<symbol, vector<Expr*>>` - default-value expressions
- `funcVarArgInfo: map<symbol, VarArgInfo>` - `*args`/`**kwargs` info

Add:
- `funcParamNames: map<symbol, vector<string>>` - declared param names in
 order, populated at function-emission time alongside the defaults.

Storage in `CodeGenImpl.h` near the existing maps. **Three emission sites**
populate it (mirroring `funcParamDefaults`):
- `src/codegen/ImplInit.cpp:1056` - regular function declarations
- `src/codegen/ImplInit.cpp:1209` - class constructors
- `src/codegen/ImplInit.cpp:1324` - class methods

All three sites must be hooked or `Config(timeout=30)` works for one kind of
callable and silently breaks for another.

### Call-site binding algorithm

In `src/codegen/CallExpr.cpp` non-vararg path (lines 387-426), before
`fillDefaultArgs`:

```cpp
auto paramNamesIt = impl_->funcParamNames.find(func->getName.str);
if (paramNamesIt != impl_->funcParamNames.end && !node.kwArgs.empty) {
 const auto& paramNames = paramNamesIt->second;
 // args currently holds positional values at indices 0..node.args.size-1
 // Resize args to paramNames.size with placeholders.
 args.resize(paramNames.size, nullptr);

 // Bind each kwarg to its named position.
 for (auto& [kwName, kwVal] : node.kwArgs) {
 auto it = std::find(paramNames.begin, paramNames.end, kwName);
 if (it == paramNames.end) {
 impl_->diagnose(node.loc, DiagSeverity::Error,
 "function '" + name + "' has no parameter named '" + kwName + "'");
 return;
 }
 size_t idx = std::distance(paramNames.begin, it);
 // args[idx] is non-null iff idx < node.args.size (positional fill above).
 if (args[idx] != nullptr) {
 impl_->diagnose(node.loc, DiagSeverity::Error,
 "argument for '" + kwName + "' given both positionally and as keyword");
 return;
 }
 kwVal->accept(*this);
 llvm::Value* arg = impl_->lastValue;
 args[idx] = impl_->coerceArg(arg, funcType->getParamType(idx));
 }
}
// fillDefaultArgs walks args, filling any null slot with the param's default.
impl_->fillDefaultArgs(func->getName.str, func, args, *this);
```

### `fillDefaultArgs` interaction

The current `fillDefaultArgs` helper (`CodeGenImpl.h:1690-1710`) iterates
`for (size_t i = args.size; i < numParams; ++i)` - it only fills slots
PAST the current end of `args`. After our `args.resize(paramNames.size,
nullptr)` + kwarg binding, `args.size == paramNames.size` and the loop
no-ops, leaving any kwarg-skipped slot (the gaps between positional args and
kwarg-filled positions) as `nullptr`.

**Fix:** change `fillDefaultArgs` to scan all indices `[0, paramNames.size)`
and fill any slot where `args[i] == nullptr`. ~5 LOC adjustment:

```cpp
for (size_t i = 0; i < numParams; ++i) {
 if (i < args.size && args[i] != nullptr) continue;
 if (i >= args.size) args.push_back(nullptr);
 // ... emit default value into args[i] ...
}
```

Either that or use a parallel `vector<bool> filled` bitset to track positional
fills. The nullptr-scan version is simpler and doesn't require changing the
call interface.

### Stdlib C-alias path

`symbolAliases` at `CallExpr.cpp:430` routes calls like `math.sqrt(x)` to a C
function (`libm` etc.). These functions have fixed positional signatures and
do **not** participate in kwarg binding. Today, kwargs on alias-routed calls
are silently dropped (same bug, different code path).

**Fix:** at the top of the alias branch, before any arg processing, emit:

```cpp
if (!node.kwArgs.empty) {
 impl_->diagnose(node.loc, DiagSeverity::Error,
 "function '" + name + "' (C alias) does not accept keyword arguments");
 return;
}
```

Two LOC. Closes the silent-drop bug for the alias path explicitly.

### Error cases

| Case | Error |
|---|---|
| Kwarg name doesn't match any parameter | `function 'f' has no parameter named 'foo'` |
| Same param given positionally AND by keyword | `argument for 'name' given both positionally and as keyword` |
| Required param (no default) not supplied by either positional or kwarg | (existing behavior) `fillDefaultArgs` emits "missing required argument" |
| Kwarg with same name appears twice in call | (Parser already catches this - verify) |

All errors are compile-time, raised at the call site with the source loc.

---

## Implementation Phases

### Phase 1: Emit `funcParamNames` metadata at all three sites

Hook into the existing function-emission flow in `src/codegen/ImplInit.cpp`
at **three sites** where param processing happens today:
- Line ~1056 - regular function declarations
- Line ~1209 - class constructors
- Line ~1324 - class methods

For each, record `paramNames` keyed by the resolved LLVM symbol (same key
scheme as `funcParamDefaults` - post-mangling).

**Scope:** ~50 LOC (one helper + three call sites).

### Phase 2: Bind kwargs in the non-vararg call path

The algorithm above in `src/codegen/CallExpr.cpp:387-426`, before
`fillDefaultArgs`. Includes the `fillDefaultArgs` adjustment (scan-for-nullptr
or bitset) so kwarg-skipped slots are correctly filled.

**Scope:** ~80 LOC (binding + two diagnostics) + 5 LOC `fillDefaultArgs`
adjustment.

### Phase 2.5: Diagnose kwargs on C-alias calls

At `CallExpr.cpp:430` (`symbolAliases` branch), error if `node.kwArgs` is
non-empty. Closes the silent-drop bug for alias-routed stdlib calls.

**Scope:** 2 LOC.

### Phase 3: Tests

`test/CodeGenCallsTest.cpp`:
- `KwargBindsToNamedParam` - `f(port=8080)` against
 `def f(name='x', port=80)` returns `name='x' port=8080`.
- `KwargUnknownName` - compile error.
- `KwargDuplicatePositionalAndKeyword` - compile error.
- `KwargMixedWithPositional` - `f('api', port=8080)` works.
- `KwargReorderingAcrossParams` - `f(port=8080, name='api')` binds correctly.
- `KwargWithClassConstructor` - `Config(timeout=30)` actually sets timeout.
- `KwargPropagatesToDefaultsForUnnamedParams` - `f(port=8080)` leaves `name`
 at its default.

**Scope:** ~150 LOC.

### Phase 4: Sweep existing stdlib for now-broken assumptions

Search for `def <name>(... default=...)` patterns in `stdlib/*.dr` that may
have been written defensively assuming kwargs don't work. None of those would
be runtime-broken by this change (call sites that previously got defaults will
now get the intended arg), but documentation may reference the limitation.

**Scope:** ~30 minutes grep + audit.

### Combined scope

~200 LOC across CallExpr.cpp + ImplInit.cpp + CodeGenImpl.h + tests (revised
from initial 260 estimate after review confirmed `fillDefaultArgs`
reuse is viable).

---

## Motto Check

| Decision | Speed (#1) | No workarounds (#2) | Parity (#3) |
|---|---|---|---|
| Bind kwargs at compile time | Compile-time name lookup → no runtime cost; emitted call is identical to a positional call | Yes - fixes the silent default-fill bug at its root in CallExpr.cpp | Yes - matches Python's keyword argument semantics |
| Error on unknown kwarg | Compile-time | Surfaces typos that today silently default | Matches Python's `TypeError: got an unexpected keyword argument 'foo'` |
| Error on duplicate positional+kwarg | Compile-time | Surfaces bugs at call sites | Matches Python's `TypeError: multiple values for argument 'name'` |

Zero runtime cost. Pure compile-time ergonomic + correctness fix.

---

## What this fixes

### Positive

- `f(name='api', port=8080)` works for every Dragon function with declared
 parameters, including class constructors.
- 's `drs.envs(env='prod', debug=true)` becomes ergonomic instead of
 positional.
- Closes a silent miscompile (kwargs dropped, defaults used) that has been
 in the codebase since Dragon supported defaults.
- Aligns with Python parity for keyword arguments - a core Pythonic feature.
- Better diagnostics: typo'd kwarg names surface at compile time rather than
 running with wrong values.

### Negative

- ~260 LOC of compiler change. Small.
- Any existing Dragon code that **relied** on kwargs being silently ignored
 (and the defaults filling in) would change behavior. Likely zero such code
 exists in stdlib (the bug is surprising; not something to rely on), but
 the test-suite run during Phase 3 will catch any regression.

### Neutral

- The `**kwargs` variadic path (for `def f(**kwargs)`) is unchanged. That
 path already correctly packs kwargs into a tagged dict.

---

## Open Questions

1. Should we also support positional-only and keyword-only parameter syntax
 (PEP 570: `def f(pos1, pos2, /, name='x', *, kw_only)`)?
 **Tentative answer:** Defer. The current syntax has no `/` or bare `*`
 markers in declarations. Add when a use case demands it.
2. Should default-value expressions see other parameters in scope (Python
 doesn't - defaults evaluate at definition time in a clean scope)?
 **Tentative answer:** Match Python. Defaults evaluate at definition time
 against module scope only.
3. Should the parser warn when a function declares duplicate parameter names?
 **Action item:** Verify Parser.cpp already catches this. If not, add a
 one-line check.
