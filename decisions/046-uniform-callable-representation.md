# Decision 046: Uniform Callable Value Representation

Proposed. design + implementation spec in this doc; Phase 0 interim foundation already landed (see Implementation status). A `Callable[...]` value today has two incompatible runtime shapes and it's biting us:

1. a **bare LLVM function pointer** - a non-capturing lambda, a top-level function used as a value, or
 a `: ptr` FFI function pointer; and
2. a **`DragonClosure { [16 x i8] hdr; ptr fn_ptr; ptr env }`** - a capturing lambda / nested `def`.

The call site chooses bare-vs-closure dispatch **statically**, from `VarKind::Closure` vs
`varIsPtrCallable` (`src/CodeGenImpl.h`, dispatch in `src/codegen/CallExpr.cpp`). That static guess is
**unsound** whenever a Callable value's representation isn't knowable at the call site - a closure
passed to a `Callable` parameter, a function with mixed bare-fn/closure returns, a closure stored in a
`Callable` field/list. The pre-existing fallback (read `type_tag` at offset 8 and compare to
`DRAGON_TAG_CLOSURE`) is a **probabilistic** discriminator: for a bare fn it reads a `.text` byte that
is `== 10` with probability ~1/256 → wrong path → crash (this exact false-positive broke
`CodeGenE2E.LambdaPassedDirectlyAsArgument` during the work).

**Decision:** every `Callable[...]` value is a `DragonClosure`. Bare fn →
`dragon_closure_create(fn, /*env=*/null)`. Indirect call always unwraps `fn_ptr` + `env`, branches on
`env == null` (bare ABI) vs non-null (closure ABI). Drop the `.text` tag guess. One `VarKind::Closure`
everywhere so refcount/storage/return paths are uniform.

`: ptr` stays the **raw escape hatch** - an unwrapped C function pointer for FFI, never wrapped, always
bare-dispatched. It is the one deliberate exception, and the only place a raw code pointer is callable.

**#1 is preserved:** *direct* calls (`foo(args)` where `foo` resolves to a known function)
are unchanged - compile-time resolved, called directly, no wrapper, no env. They are the overwhelming
majority of calls. Only *first-class* Callable values pay anything, and even that drops to **zero
allocation** via a per-function immortal null-env wrapper (Phase 2).

---

## Context / Motivation

 shipped closures with a **hidden environment parameter**: a capturing lambda gets a trailing
`i8*` env; a non-capturing one stays a bare fn pointer (zero overhead). That was the right call for
*producing* closures. The gap is in *consuming* a Callable value whose origin the consumer can't see:

```dragon
def make -> Callable[[], int] {
 x: int = 42
 def get -> int { return x } # a closure (env holds x)
 return get
}
def use(f: Callable[[], int]) -> int { # f could be a closure OR a bare fn
 return f # which dispatch?
}
print(use(make)) # closure in -> must use the env ABI
print(use(some_bare_fn)) # bare fn in -> must use the bare ABI
```

`use` is compiled **once**; `f`'s representation is a runtime property. Static dispatch can't be right
for both callers. The same ambiguity appears in:

- **mixed-return** - `def pick(c: bool) -> Callable[[], int] { if c { return cl } return bare }`;
- **Callable fields / list elements** - `route.handler` may hold a bare fn today and a closure tomorrow;
- **returning a bare fn from a `-> Callable` function** - the value escapes as a bare pointer into a
 context that may closure-dispatch it.

The interim fix made the *common* cases robust enought (closure-as-arg, bare-fn-as-arg) by
runtime-tag-checking **and** wrapping bare fns passed to Callable parameters. But the `.text` tag-read
is still reached on the un-wrapped paths (returns / fields / lists), and those values also have an
**ambiguous VarKind**, so their refcount can't be managed consistently (a returned bare-vs-closure
union has no single `storeWithRCOverwrite` kind). Both problems have one root cuase - *two
representations* - and one root fix: *one representation*.

### Constraint restated (the three commandments)

1. **Speed is king.** Direct calls must stay identical (zero cost). Indirect calls may add at most one
 predictable branch. Wrapping a bare fn as a value must reach **zero allocation** (Phase 2).
2. **No workarounds.** The `.text` tag-guess is a workaround; this ADR deletes it. No reshaping `.dr`
 to dodge the ambiguity.
3. **Python parity.** First-class functions, higher-order functions, and callbacks are expected to
 "just work" - this ADR makes them work for *every* value flow, not a subset.

---

## Options Considered

### Option A: Uniform `DragonClosure` for every `Callable` value (chosen)

Every value of type `Callable[...]` is a `DragonClosure`. A bare fn used as a value is wrapped with
`env = null`. Indirect dispatch always unwraps and branches on `env`. `VarKind::Closure` is the single
kind for all Callable storage. `: ptr` is the unwrapped exception.

**Pros:**
- **Sound** - dispatch is a property of the value, not a compile-time guess. Mixed-return,
 closure-in-arg/field/list/return all correct.
- **Uniform refcount** - one `VarKind::Closure`; existing `storeWithRCOverwrite` / scope-cleanup /
 return-incref paths (Phase 2) handle every Callable value identically. No ambiguous-kind leak.
- **Deletes the `.text` workaround** entirely (#2).
- **Direct calls untouched** (#1). Indirect calls: one `icmp env, null` branch.
- Reuses 's `DragonClosure` + GC integration verbatim; reuses 's spawn-args marshalling.

**Cons:**
- A bare fn *used as a value* must be wrapped → an allocation, unless cached (Phase 2 removes it).
- Touches every Callable *producer* site (the places a fn-name becomes a value) - but these are few
 and centralizable (see spec).

### Option B: Two representations + runtime `type_tag` discrimination (the interim; rejected as permanent)

Keep bare fns and closures distinct; at each dispatch read `type_tag@8` to pick the path.

**Rejected:** reading offset 8 of a bare fn pointer is a `.text` byte - a **probabilistic**
discriminator (`== TAG_CLOSURE` ~1/256 → wrong unwrap → crash; observed breaking a real test). And it
does nothing for the **refcount ambiguity** of bare-vs-closure values in fields/returns. It is a
workaround, not a representation.

### Option C: Fat-pointer Callable - pass `{ fn, env }` by value everywhere

Make `Callable[...]` a two-word value type rather than `i8*`.

**Rejected:** breaks 's invariant that `Callable` lowers to a single `i8*` (every container,
union box, spawn-args slot, and FFI boundary assumes 8-byte ptr-shaped Callables). It changes the
calling/value convention for *all* Callables (huge churn) and still needs a heap env for capture
lifetime. The cost lands on every Callable, not just first-class uses.

### Option D: Wrap bare fns only at the argument boundary

Wrap only bare-fn *arguments* to Callable params.

**Rejected as complete:** leaves returns / var-assigns / fields / list-elements on the fragile path.
It is exactly Phase 0 of Option A - a foundation, not the destination.

---

## Decision

Adopt **Option A**. Concretely:

1. **One representation.** Any value typed `Callable[...]` (param, local, field, list/dict element,
 return) is a `DragonClosure`. A bare top-level fn / non-capturing lambda used *as a value* is
 wrapped via `dragon_closure_create(bitcast(fn, i8*), null)`. Capturing lambdas / nested defs already
 produce a `DragonClosure` - unchanged.

2. **One dispatch.** The indirect call through a Callable value unwraps `fn_ptr` + `env` and branches:
 `env == null` → `call fn(args)` (user signature); `env != null` → `call fn(args, env)` (user
 signature + trailing `i8*`). No `type_tag` read. (The result is a single `phi` over the two call
 sites - see `src/codegen/CallExpr.cpp` env-branch, already written in Phase 0.)

3. **One kind.** `Callable[...]` params/locals/fields/elements are `VarKind::Closure`. `trackPtrParam`
 (`src/CodeGenImpl.h:264`) marks a `Callable[...]` param `Closure` (not `varIsPtrCallable`); class
 field registration (`src/codegen/Classes.cpp`) and `AnnAssign` (`src/codegen/Assign.cpp`) do the
 same for fields/locals. Refcount then flows through the existing `VarKind::Closure` paths .

4. **`: ptr` is the exception.** A `: ptr`-typed value is a raw C function pointer - never wrapped,
 always bare-dispatched (its dispatch has no `callableTypes` entry; the Phase-0 `valIsTypedCallable`
 gate already distinguishes it). This is the single, documented escape hatch for FFI.

5. **Delete the `.text` tag-guess.** Once all Callable values are `DragonClosure`, the offset-8
 `type_tag == 10` discrimination in `CallExpr.cpp` and `CallMethods.cpp` (field dispatch) is replaced
 by an unconditional unwrap + env-branch. The probabilistic read is removed.

---

## Implementable Spec

### Runtime (no change)
`struct DragonClosure { [16 x i8] hdr; void* fn_ptr; void* env }` and
`dragon_closure_create(void* fn, void* env)` already exist (`lib/Runtime/runtime_internal.h`,
`lib/Runtime/runtime_builtins.cpp:1190`). A `null` env is already representable. `dragon_decref` already
frees a `DragonClosure` (and its env, if any) via the tag-10 branch - so wrapped bare fns free
correctly with **no new runtime code**.

### Codegen - the producer sites (where a fn becomes a Callable value)

Wrap iff the **target type is `Callable[...]`** and the value is a bare fn (`llvm::isa<llvm::Function>`
or a `varIsPtrCallable` value). Drive the wrap off the *target*, never the producer, so `: ptr`
targets stay raw.

| Producer site | File | Action |
|---|---|---|
| **Arg → Callable param** | `src/codegen/CallExpr.cpp` (direct-call arg loop) | done Phase 0: `funcCallableParam[symbol][i]` + `isa<Function>` → wrap; freed via `argTemps(VarKind::Closure)`. |
| **Return a bare fn from `-> Callable`** | `src/codegen/Statements.cpp` (`ReturnStmt`) | Phase 1: if the current fn's return type is `Callable[...]` and `retVal isa Function` → wrap. Track `currentFnReturnsCallable` (save/restore around nested fns). |
| **Assign a bare fn to a `Callable` local/global** | `src/codegen/Assign.cpp` (`AnnAssign`/`Assign`) | Phase 1: target annotation `Callable[...]` + RHS `isa Function` → wrap. |
| **Store a bare fn in a `Callable` field** | `src/codegen/Assign.cpp` (attr-target store) | Phase 1: `classFieldCallableType` field + `isa Function` → wrap. |
| **Bare fn into a `list[Callable]`/`dict[_,Callable]`** | `src/codegen/Literals.cpp` / append paths | Phase 1: element type `Callable[...]` + `isa Function` → wrap. |

### Codegen - the dispatch sites (unconditional unwrap + env-branch)

| Dispatch site | File | Action |
|---|---|---|
| **Indirect call via var/param** | `src/codegen/CallExpr.cpp` | done Phase 0 has the env-branch on the closure path. Phase 1: drop the `type_tag` gate - a `valIsTypedCallable` value is *always* a `DragonClosure`, so unwrap unconditionally. Keep the `: ptr` (`!valIsTypedCallable`) plain-bare path. |
| **Callable field call** (`obj.h(...)`) | `src/codegen/CallMethods.cpp:~1714` | Phase 1: same - replace the `type_tag` read with unconditional unwrap + env-branch (field values are now always `DragonClosure`). |
| **`fire f` / spawn-arg Callable** | `src/codegen/Concurrency.cpp` | Already marshal `DragonClosure` ptrs through the args struct . No change beyond uniform kind. |

### Kind marking

- `trackPtrParam` (`src/CodeGenImpl.h:264`): `CallableTypeExpr` param → `VarKind::Closure` +
 `callableTypes[name]` (today it sets `varIsPtrCallable`).
- `typeExprToKind(CallableTypeExpr)` → `VarKind::Closure` (centralizes field/local/element kind).
- The interim helpers `funcReturnsClosure` and the `funcReturnsClosure`-driven `AnnAssign` Closure
 branch become **subsumed** (every `-> Callable` value is a closure now) and can be removed once
 Phase 1 is complete.

### Phase 2 - zero-allocation wrapping (speed)

A top-level function's null-env wrapper is **value-identical every time** (same `fn_ptr`, `env = null`).
Emit **one immortal `DragonClosure` global per top-level fn used as a value**, created once at module
init and marked immortal (`DRAGON_IMMORTAL_REFCOUNT`, so refops are no-ops - see the immortal-guard fix
in the audit). "Wrapping a bare fn" then becomes *loading that global* - **zero allocation,
zero per-use refcount traffic**. Capturing closures still allocate per instance (their env differs),
exactly as specifies. This restores #1 fully for first-class top-level functions.

(Phase 2 also composes with Phase 5 escape analysis: a non-escaping capturing closure can keep a
stack env; an escaping one heaps it. Orthogonal to the null-env wrapper cache.)

---

## Implementation status

| Phase | Status | Where |
|---|---|---|
| 0. Env-branch dispatch + arg-wrap + leak-free free + `: ptr` gate | done Landed | `CallExpr.cpp` (indirect dispatch + direct-call arg loop), `ImplInit.cpp`/`CodeGenImpl.h` (`funcCallableParam`), `dragon_closure_create(fn,null)` wrap, freed via `argTemps`. Proven: closure-as-arg / bare-fn-as-arg / closure-with-args green + leak-free (2M churn → flat RSS). |
| 1. Wrap at all producers + unconditional unwrap (delete `.text` guess) + uniform `VarKind::Closure` | ⏳ Proposed | Returns, var/field/list producers; dispatch de-gate; `trackPtrParam`/`typeExprToKind` kind change; retire `funcReturnsClosure` + `type_tag` reads. |
| 2. Per-fn immortal null-env wrapper global (zero-alloc) | ⏳ Proposed | Module-init wrapper globals; "wrap" = load global. |

---

## Test plan

`.dr` E2E under `test/dr/` (extends `test/dr/test_closure_escape.dr`), GoogleTest IR checks for the
dispatch shape:

1. **Mixed-return both directions** - `pick(true)` (closure) and `pick(false)` (bare fn) each called;
 distinct results; run many times (no `.text` flake) and under ASan.
2. **Bare fn through every producer** - returned from `-> Callable`, assigned to a `Callable` local,
 stored in a `Callable` field, put in a `list[Callable]`, then called. All correct.
3. **Closure through the same producers** - symmetric.
4. **`: ptr` unaffected** - `apply(f: ptr, x)` with a bare fn / lambda stays bare-dispatched (the
 `LambdaPassedDirectlyAsArgument` / `FunctionPassedAsPtrParameter` tests stay green).
5. **Leak/no-double-free** - churn each producer 1M× under `/usr/bin/time -v` (flat RSS) + LSan zero
 leaks. Phase 2: confirm zero per-use allocation for top-level-fn values (RSS flat without the
 per-call wrapper).
6. **Full suite** must stay 100% green at each phase (CallExpr/CallMethods dispatch is shared).
7. **IR check** (`DRAGON_DUMP_IR=1`): a Callable-value call emits the unwrap + env-branch, never a
 `type_tag` load; a direct call emits a plain `call @fn` (unchanged).

---

## Root cause closed

- **Mixed-return, closure-as-arg, closure-in-field/list/return all correct** - the residual flagged in
 the live audit is closed at root.
- **The `.text` tag-guess is deleted** - no probabilistic dispatch anywhere (#2).
- **Refcounting is uniform** - one `VarKind::Closure` for all Callable storage; no ambiguous-kind leak.
- **Direct calls unchanged; indirect calls +1 branch; first-class top-level fns reach zero allocation**
 (Phase 2) - #1 intact.
- **`: ptr` is now explicitly the sole raw-fn-pointer path** - documents the FFI boundary precisely.
- **`funcReturnsClosure` + `funcCallableParam` interim machinery is retired** once Phase 1 lands - they
 were scaffolding for the partial; the uniform rep makes them unnecessary.
- **Migration risk** is concentrated in the producer sites + the two dispatch de-gates; the phased plan
 keeps the suite green throughout, and Phase 0 already validated the dispatch + wrap + free mechanics.
