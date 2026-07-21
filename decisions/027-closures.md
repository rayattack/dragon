# Decision 027: Closures and Variable Capture

**Status:** Implemented (Phases 0-4 landed; .1 nonlocal extension also shipped)

The CORS middleware bug is what finally forced this one. I had `origin` in an outer scope and a lambda trying to close over it - compiler said "not in scope," I said "yeah that's the problem." Callback-driven APIs (HTTP handlers, event loops, iterators) don't work without capture, and I'm not shipping a language that makes people hand-roll env structs like it's 1998.

## Summary

Lambdas, `fire { }` blocks, and `thread { }` blocks can capture variables from enclosing scopes. Capturing lambdas get an extra trailing `i8*` parameter pointing at a heap-allocated environment struct. Non-capturing lambdas stay bare function pointers with zero overhead.

---

## Implementation status

| Phase | Status | Where it lives |
|---|---|---|
| 0. AST + Sema free-variable analysis | done Landed | `LambdaExpr`/`FireExpr`/`ThreadStmt`/`FunctionDecl::capturedVars` (`include/dragon/AST.h:347/373/495/647`); `Sema::visit` populates via `ctx.capturedNames` (`src/Sema.cpp:368, 799`). Nested `def` capture also wired. |
| 1. Runtime support | done Landed | `DRAGON_TAG_CLOSURE` (`lib/Runtime/runtime_internal.h:87`), `struct DragonClosure` (`runtime_internal.h:330`), `dragon_closure_create` / `dragon_closure_dealloc` (`lib/Runtime/runtime_builtins.cpp:905, 916`). Dealloc hooked into the generic `dragon_decref` switch (`runtime_core.cpp:108`). |
| 2. CodeGen - capturing lambdas | done Landed | `VarKind::Closure` flows through assignment (`src/codegen/Assign.cpp:945, 1110, 1835`) and the indirect-call path (`src/codegen/CallExpr.cpp:610`); env packing + per-site dealloc fns generated in `Functions.cpp` / `Expressions.cpp`. |
| 3. `fire` / `thread` capture | done Landed | Env relayed through the spawn args array; atomic refops reused from /. |
| 4. Edge cases (self-capture, type-checker compatibility, transitive capture) | done Landed | Covered by Sema's transitive walk and CodeGen's closure-vs-bare-fn dispatch. |
| 5. Optimization (escape analysis, inline scalar captures, IIFE elision) | ⏳ Partial | Phase 1 (`17910dc`) replaced anonymous `i8*` envs with **per-closure named env structs** and per-callsite spawn trampolines, which gets most of the static-type win. Escape analysis / stack-allocated envs / IIFE elision still deferred. |

### Beyond-scope extension: .1 (`nonlocal` mutable capture)

The original ADR deferred mutable capture; it has since landed. Cells are allocated by `dragon_cell_alloc` and read/written through `dragon_cell_get` / `dragon_cell_set` (declared in `src/codegen/ImplInit.cpp:949-960`). Sema tracks the `nonlocal`-declared subset of captures in `mutatedCapturedVars` on `LambdaExpr`/`FireExpr`/`ThreadStmt`/`FunctionDecl` (`include/dragon/AST.h:348/374/496/648`), and the codegen path in `src/codegen/Functions.cpp:37-93` and `src/codegen/Expressions.cpp:108` routes those names through cell loads/stores. GC participates in cells via `runtime_core.cpp:423`.

The env struct layout is unchanged - a cell is just another `i64` (pointer) in the env, exactly as the original "future path to `nonlocal`" closing note predicted.

---

## Context / Motivation

Right now our lambdas compile as standalone `InternalLinkage` LLVM functions with no capture mechanism. Code like this fails:

```dragon
def cors(origin: str = "*") -> None {
 self.AFTER("/*", fn(req: Request, res: Response, ctx: Context) -> None {
 res.set_header("access-control-allow-origin", origin) # ERROR: 'origin' not in scope
 })
}
```

Closures are essentail for callback-driven APIs (HTTP servers, event handlers, iterators). Any developer coming from Python, JavaScript, or Rust expects them.

**The constraint:** Dragon must match Rust/C++ speed. No heap allocation for non-capturing lambdas. No universal fat-pointer calling convention change.

---

## Options Considered

### Option A: Hidden environment parameter (chosen)

Capturing lambdas get one extra `i8*` param appended to their signature. A heap-allocated environment struct holds captured values. The call site packs the env and passes it.

**Pros:** Non-capturing lambdas unchanged (zero overhead). No calling convention breakage. Works with existing `fire`/`thread` spawn infrastructure (env fits in the args array). Simple GC story - env is a refcounted `DragonObjectHeader`.
**Cons:** Indirect calls must check `VarKind::Closure` to know whether to append the env.

### Option B: LLVM trampolines

Use `llvm.init.trampoline` to generate a thunk that binds the env into a bare function pointer.

**Pros:** Callers see a normal function pointer - no dispatch changes needed.
**Cons:** Requires writable+executable memory (W^X violation). Platform-fragile. Incompatable with `fire`/`thread` spawn infrastructure.

### Option C: Universal closure pair / fat pointer

Change all function values from bare `i64` to a `{fn_ptr, env_ptr}` pair.

**Pros:** Uniform representation.
**Cons:** Breaks every existing callback site (HTTP server handlers, `fire fn(args)`, indirect calls). Doubles the size of function values. Penalizes non-capturing lambdas.

---

## Decision

**Option A: Hidden environment parameter.**

### Capture Semantics

**By-value capture.** At the point the lambda expression is evaluated, each captured variable's current value (an `i64`) is copied into the environment struct. This avoids aliasing issues with `fire`/`thread` where the enclosing scope may exit before the closure runs.

Mutable capture (`nonlocal` semantics via heap-boxed cells) was deferred to a future extension. Assigning to a captured variable in a lambda body is a Sema error (until .1).

### Data Structures

**DragonClosure** (heap-allocated, refcounted, `TAG_CLOSURE`):
```
{ DragonObjectHeader header, void* fn_ptr, void* env_ptr }
```

**Environment struct** (heap-allocated, refcounted):
```
{ DragonObjectHeader header, void(*dealloc_fn)(void*), i64 captures[N] }
```

A per-closure-site LLVM dealloc function (generated by CodeGen) handles decref of heap-typed captures in the env.

When the compiler knows a lambda doesn't capture, it uses the bare function pointer directly - no `DragonClosure` wrapper.

### VarKind::Closure

A new `VarKind::Closure` entry. Marked as a heap kind. The indirect call path checks this to extract `fn_ptr` + `env_ptr` from the closure struct and append the env to the call args.

---

## Implementation Phases

### Phase 0: AST + Sema - Free Variable Analysis

**Files:** `include/dragon/AST.h`, `src/Sema.cpp`

1. Add `capturedVars` field (vector of `{name}`) to `LambdaExpr`, `FireExpr`, `ThreadStmt`.
2. In `Sema::visit(LambdaExpr&)`: after visiting the body, collect all `NameExpr` references that resolve in an ancestor scope (not the lambda's own scope, not module scope). Store in `node.capturedVars`.
3. Same for `FireExpr` and `ThreadStmt` block forms.
4. Emit error on assignment to a captured variable.
5. Nested lambdas: transitive capture propagation - if inner captures `x` from middle, middle must also capture `x` from outer.

### Phase 1: Runtime Support

**Files:** `lib/Runtime/runtime.cpp`

1. `TAG_CLOSURE` constant (next available tag).
2. `DragonClosure` struct and `dragon_closure_create(fn, env)`, `dragon_closure_get_fn/get_env`.
3. `dragon_env_create(n)`, `dragon_env_set(env, i, val)`, `dragon_env_get(env, i)`.
4. Dealloc for closure (decrefs env) and for env (calls stored dealloc function which decrefs heap captures).

### Phase 2: CodeGen - Capturing Lambdas

**Files:** `src/CodeGen.cpp`

1. Non-capturing path: unchanged (if `capturedVars` empty, existing code runs).
2. Capturing path:
 - At call site: `dragon_env_create(N)`, populate via `dragon_env_set`, incref heap captures.
 - Lambda function gets extra trailing `i8*` param.
 - Inside lambda body: load captures from env via `dragon_env_get`, register in scope as `borrowed`.
 - Result: `dragon_closure_create(fn, env)` cast to i64.
3. Modified indirect call path: if `VarKind::Closure`, extract fn/env from closure, append env to call args.
4. Scope cleanup: decref closures on scope exit.
5. Generate per-closure-site dealloc function for the env (knows which slots are heap-typed).

### Phase 3: Fire and Thread Capture

**Files:** `src/CodeGen.cpp`

1. `fire { block }`: create env, anonymous function takes `i8*` param, env packed as `args[0]`. Atomic incref/decref for cross-thread safety.
2. `thread { block }`: same pattern.
3. `fire fn(args)` where `fn` is a closure: extract fn_ptr + env, pass env through spawn infrastructure.

### Phase 4: Edge Cases and Hardening

1. Capturing `self` in methods - naturally works (self is a parameter, `VarKind::ClassInstance`, gets incref'd in env).
2. TypeChecker: closure compatible with bare function pointer of matching arity.
3. Nested lambda transitive capture (if not fully handled in Phase 0).

### Phase 5: Optimization (Future)

1. Stack-allocated envs for non-escaping lambdas (escape analysis).
2. Inline scalar captures as extra hidden params (skip heap alloc for 1-2 scalar captures).
3. Elide closure wrapper when lambda is immediately called (IIFE).

---

## Runtime cost

- **Non-capturing lambdas:** completely unchanged. Zero overhead.
- **Capturing lambdas:** one heap allocation (env struct) + one heap allocation (closure wrapper) per lambda evaluation. One extra pointer dereference on indirect call.
- **GC:** env and closure participate in refcounting. Per-closure-site dealloc ensures correct cleanup. Cross-thread captures use atomic refops (existing pattern from).
- **Breaking changes:** none. Existing code compiles identically. New `VarKind::Closure` only applies to capturing lambdas.
- **Future path to `nonlocal`:** mutable capture via heap-boxed cells can be added later without changing the env struct layout (the cell pointer is just another i64 in the env). .1 shipped this.
