# Decision 024: User-Defined Decorators

> **Status:** Approved (Phases 1-2 implemented; 3-4 outstanding).

I'm adding user-defined `@decorator` support with Python-style wrapping - same in `.py` and `.dr`. Built-ins (`@staticmethod`, `@classmethod`) stay compiler flags; this covers everything else. Most code should use `with lock {}` for multi-step patterns, but occassionally a decorator is cleaner.

Dragon already parses `@decorator` in both modes. Lexer emits `AT`, parser collects decorators on `FunctionDecl`/`ClassDecl`, Sema resolves names. But only `@staticmethod` and `@classmethod` do anything - everything else is **silently ignored** at codegen. I found that out the hard way when `@log_calls` looked fine in the AST and did absolutely nothing at runtime. Python people expect `@decorator` to wrap functions; we want `@synchronized`, `@cache`, `@route("/path")`, `@deprecated`, etc.

### Why not merge .py and .dr modes?

The split is intentional:

| | .py mode | .dr mode |
|---|---|---|
| **Identity** | Python superset - valid Python runs as-is | Dragon's own language |
| **Static methods** | `@staticmethod` (Python way) | `static def` (Dragon way) |
| **Portability** | Can run with `python3`, lint with mypy | Dragon-only |

Adding Dragon keywords like `static` to `.py` breaks Python compat - that's the whole point of `.py` mode. Decorators are the rare feature that works in both without breaking anything, since `@expr` is already valid Python.

`.py` mode is a **migration path**: write Python, add types, compile with Dragon, rename to `.dr` when you want Dragon-only stuff (`const`, `static`, `template`). Merging modes kills step one.

---

## Options Considered

### Option A: Python-style function wrapping (chosen)

`@decorator` on `f` desugars to `f = decorator(f)`.

```python
# .py mode
@log_calls
def greet(name: str) -> str:
 return "hello " + name

# equivalent to: greet = log_calls(greet)
```

```
// .dr mode
@log_calls
def greet(name: str) -> str {
 return "hello " + name
}

// equivalent to: greet = log_calls(greet)
```

**Pros:** Python-compatible, composable, users define their own.
**Cons:** Decorator runs at definition time; function identity changes.

### Option B: Java-style annotations

Compile-time metadata only. Compiler pattern-matches known names. Users can't add new ones.

**Pros:** Simple, zero runtime cost.
**Cons:** Not extensible, not Python-compatible. Already what `@staticmethod`/`@classmethod` do.

### Option C: Hybrid

Registry of magic decorators + wrapping for unknowns. Confusing two-tier system.

---

## Decision

**Option A.** `@decorator` means `f = decorator(f)`.

Built-ins stay special-cased - they affect calling convention and can't be expressed as runtime wrapping. Everything else wraps.

### Semantics

1. **Function decorators**: `@dec def f(...)` → `f = dec(f)`. Multiple decorators bottom-up: `@a @b def f` → `f = a(b(f))`.

2. **Decorator arguments**: `@dec(arg)` is a call that returns a decorator. `@dec(arg) def f` → `f = dec(arg)(f)`. Falls out naturally from parsing decorators as full expressions.

3. **Class decorators**: DROPPED . `@dec class C` → `C = dec(C)` would need the class as a runtime value, which and bar. Class decorators aren't part of Dragon. Compile-time transforms like `@dataclass` are dedicated synthesis, not runtime `dec(C)`.

4. **Stacking**: Unlimited, bottom-up.

5. **Scope**: Decorator expressions evaluate in the enclosing scope (Sema already visits them before the function body scope).

### What a decorator function looks like

```python
def log_calls(f: function) -> function:
 def wrapper(*args) -> any:
 print("calling " + f.__name__)
 result = f(*args)
 print("done")
 return result
 return wrapper
```

---

## Implementation Plan

### Current state (already done)

- Lexer: `AT` token
- Parser: `parseDecorators` collects `vector<Expr>` on FunctionDecl/ClassDecl
- AST: `decorators` field on both node types
- Sema: decorators visited for name resolution, `property` registered as builtin
- CodeGen: `@staticmethod`/`@classmethod` handled via flags

### Phase 1: Function decorator application (CodeGen)

After emitting a decorated function's LLVM IR:

1. Get the function pointer as an i64 tagged value
2. For each decorator (bottom-up), emit: `result = decorator_fn(fn_ptr)`
3. Store the result as the new binding for the function name
4. Module-level: overwrite the global variable
5. Local: overwrite the alloca
6. Methods: overwrite the method table slot

Functions must be passable as i64 values (already true - function pointers cast via `PtrToInt`). Decorators are normal Dragon functions with signature `(i64) -> i64`.

**Files touched**: `src/CodeGen.cpp`

### Phase 2: Class decorators (CodeGen) - DROPPED

Would need class-as-value (path). rejected, bars runtime class objects. `@dataclass`/`NamedTuple` are synthesis passes, not `C = dec(C)`.

### Phase 3: TypeChecker integration

- Validate decorator expression is callable
- Validate decorator accepts the decorated type
- Infer return type of decorator application
- Optional warn on signature changes

**Files touched**: `src/TypeChecker.cpp`

### Phase 4: Standard library decorators

- `stdlib/functools.dr`: `@cache`, `@lru_cache`
- `stdlib/concurrent.dr`: `@synchronized`
- `stdlib/http/`: `@route`, `@middleware`

---

## After we ship this

### Positive
- Familiar decorator patterns for Python migrants
- Stdlib can use decorator APIs (HTTP routing, caching)
- Both modes benefit; `.py` stays Python-compatible
- `@dec(arg)` works for free - already parsed as CallExpr

### Negative
- Function identity changes after decoration (stack traces show wrapper)
- Runtime indirection cost
- `@staticmethod`/`@classmethod` stay special - two systems
- Class decorators deferred (asymmetry until we decided to drop them entirely)

### Risks
- CodeGen complexity in a ~9500-line file; module-level, local, method contexts
- Refcounting: wrapper must incref original, decref if replaced
- Generators: decorator wraps outer function, not the body

---

## Estimated Effort

| Phase | Scope | Risk |
|-------|-------|------|
| Phase 1 (function decorators) | Medium | Medium - core CodeGen change |
| Phase 2 (class decorators) | DROPPED | needed class-as-value, rejected |
| Phase 3 (type checking) | Small | Low |
| Phase 4 (stdlib decorators) | Small per decorator | Low |
