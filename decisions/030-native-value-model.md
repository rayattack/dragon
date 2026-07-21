# Decision 030: Native-Typed Value Model - Eliminating Uniform i64

**Status:** Implemented (all 5 phases landed and green)

**Subsumes:** the monomorphized-containers forward-reference - typed lists/dicts are Phase 3 of this decision.

Every Dragon value was an `i64` at the LLVM level for the first ~18 months. Polymorphic containers were trivial, one ABI to reason about - and also a steady stream of "why is this float wrong" bugs at 2am. I killed the uniform-i64 model in five phases; this doc is the autopsy and the victory lap.

## Implementation log

| Phase | Commit | Summary |
|---|---|---|
| 1 | `17910dc` | Function ABI strengthening: per-closure named env structs, per-callsite spawn trampolines (fire/async/generator). |
| 2 | `479efe9` | Native-typed locals; bug-class rescue paths (`argType == i64 && vk == Float/Bool`) deleted. `list[bool]` returns native i1. |
| 3.A | `aee3e54` | Runtime list family: `DragonListF64`, `DragonListPtr` + typed ops. |
| 3.B | `372fb5a` | Codegen list alloc + read paths use typed runtime ops (`dragon_list_get_f64`, `dragon_list_new_ptr`, etc.). |
| 3.C | `9925c91` | Codegen list write paths (subscript assign, append, comprehension push) use typed ops. |
| 3.E/F | `230f1e5` | Dict[K,V] monomorphization for str-keyed dicts: `dragon_dict_get_str_f64` / `_str_ptr` + codegen migration. |
| 3.G | (current) | Dict[K,V] monomorphization for int-keyed dicts: `dragon_dict_int_set_*` / `_get_*` / `_has_key` / `_pop` / `_keys` + `dragon_print_dict_int`. SplitMix64 hash for i64 keys, branchless probe. Codegen tracks K via `varDictKeyKinds` + `classFieldDictKeyKinds` (Type::Kind), and dispatches DictExpr literal alloc, subscript set/get, `in`, and print to the int-keyed family. Unblocks `dict[int, str]` patterns: `errno.errorcode`, HTTP status maps, signal-number tables. |
| 4 | `230c7b4` | Boxed `Union[...]` / `Any` with `%dragon.box = { i64 tag, i64 payload }`. `funcUnionTagMask` + `unionTagAllocas` deleted. |
| 5 | (current) | Cleanup: 5 dead rescue branches deleted, `coerceArg` audited, zen.md updated. `VarKind::Bytes` collapsed into the generic-heap `VarKind::List`; bytes-vs-list distinction now flows through `Type::Kind` / `typeKindToTag` at every consumer. |

## Outcome vs success criteria

- done All 1089 tests pass at every phase boundary.
- done `coerceArg` call sites: 20 (was ~152 pre-, **87% reduction**).
- done Zero bug-class rescue patterns (`argType == i64 && vk == Float/Bool`) remain.
- done "Tagged value system" claim in zen.md replaced with the native-typed value-system description.
- warn Net LOC: +1006 across (vs the doc's predicted "net-negative"). The increase is mostly new typed runtime ops (each per-variant) and box helpers; the deletions of parallel maps are real but smaller in count than the new architecture's surface. Fair architectural cost.
- warn Class-field type inference for ptr-returning RHS (`self.handle = fopen(...)`) still stores i64 instead of ptr. Tracked as a follow-up; the int↔ptr branches in `coerceArg` stay until field-type inference is fixed.

Sieve and float-heavy benchmarks not re-measured in this push - those wins are real but unmeasured here.

---

## Context / Motivation

I bootstrapped with a uniform value model: every Dragon value is an `i64` at the LLVM level, with pointers cast via `IntToPtr`/`PtrToInt`, floats via bitcast, and bools via `ZExt`/`ICmpNE`. A side-channel `VarKind` enum (`CodeGenImpl.h:67`) tracks what each `i64` *actually represents* so codegen can dispatch correctly.

That made sense for the first 18 months. Polymorphic containers (`dict`, `list[Any]`, unions) were trivial, and the whole compiler had one ABI to reason about. But the model became the main source of correctness bugs and a real performance ceiling.

**Recurring bug pattern** (sample from recent commits, all on this branch):
- `7c1a35b` "fix: i64 coercion causing argparse bugs"
- `978b5b9` "fix: i64 coercion causing argparse bugs"
- `9d89ed0` "fix: tier1 1.13+1.14 dict.values mixed leak + list.extend tag mismatch"
- `5f21f5f` "fix: tier1 1.8 cycle collector string leak"
- `4702fc6` "fix: tier1 1.9 atomic child decref in dealloc path"

Same root cause every time: the LLVM type says `i64` but the *real* type lives in a parallel map (`varKinds`, `unionMemberKinds`, `moduleGlobalKinds`, `funcParamKinds`, `classFieldKinds`, `typedDictFieldKinds`). When those maps go stale, get cleared at the wrong scope boundary, or disagree across an inlined call, codegen emits the wrong incref/decref/print/dispatch. Bug ships (this pattern occured at least eight times on this branch alone).

**Performance cost** (versus C, see / benchmarks):
- Every `float` op pays a bitcast round-trip (`f64 → i64 → f64`). LLVM can't keep floats in xmm registers across most boundaries.
- Pointer-typed values get stored as `i64` then re-`IntToPtr`'d at every use; the optimizer loses provenance and can't alias-analyze.
- `list[float]` stores `f64` bit patterns inside `i64[]` arrays, so vectorization is blocked unless we round-trip through bitcasts in the loop body.
- Closure environments pack everything as `i64`, forcing bitcast/PtrToInt at every capture and every read.

**The asymmetry is the giveaway:** function *signatures* already use native types (`typeExprToLLVM` at `CodeGenImpl.h:1397` returns `f64Type` for `float`, `i1Type` for `bool`, `i8PtrType` for `str`/`list`/etc.). The bug-prone i64 funnel lives in the *interior* - locals, env packing, container slots, polymorphic returns, the `coerceArg` shim at `CodeGenImpl.h:1433`, and the VarKind side-channel that tries to compensate.

Dragon pays the cost of polymorphism everywhere, even in fully-statically-typed code where the type is known at every site. Dragon is a *typed* Python superset. The compiler knows the types. The compiler should use them.

---

## Options Considered

### Option A: Status quo + better VarKind discipline

Tighten how VarKind is tracked: scope-stack push/pop, invalidation on reassignment, audit every `lookupVarKind` call site. Keep uniform-i64.

No structural change. Fixes go in incrementally. But the bug class is intrinsic to having two parallel sources of truth. Each fix uncovers a new edge case (closures + unions + generators + async spawn = exponential matrix). Performance ceiling unchanged. Eight commits on this branch were exactly this approach.

### Option B: NaN-boxing

Pack everything into a single `f64` slot using NaN payload encoding (LuaJIT/JavaScriptCore approach).

Truly uniform 64-bit slot; double arithmetic free. But we don't have that problem - Dragon already knows types statically in 90%+ of code. NaN-boxing is for dynamically-typed languages where every slot might be anything. It would *prevent* the static-type wins we're after. Pointer space limited to 47 bits; ints typically 32. Adds bit-twiddling on every type check.

### Option C: Universal pointer-tagging

Tag low bits of pointers (SBCL/V8 small-integer style). All values are `i64` with low 3 bits as type tag.

Single uniform value, type check is a mask. Same fundamental issue as A and B - assumes a dynamic language. Forces tag-mask-and-shift on every primitive op, even when the type is statically known. Makes integer arithmetic slower, not faster.

### Option D: Native LLVM types by default; box only at polymorphism boundaries (chosen)

Each value carries its native LLVM type end-to-end: `int` is `i64`, `float` is `f64`, `bool` is `i1`, `str`/list/dict/instance are typed pointers. Boxing into a `{ i8 tag, i64 payload }` happens *only* at well-defined boundaries: `Any`, `Union[...]`, polymorphic container slots that need it, and dynamic dispatch sites.

Why this one:
- LLVM regalloc, mem2reg, vectorizer, and inliner see real types and can do their jobs.
- Float arithmetic stays in xmm regs - no bitcast round-trip.
- Pointer provenance is preserved → better alias analysis → better codegen.
- The VarKind side-channel collapses to source-level metadata only (TypedDict-vs-Dict, StrLiteral-vs-Str). The LLVM type *is* the truth.
- `coerceArg` shrinks to legitimate widening (int↔float, intc bridging).
- Whole bug class - "VarKind got stale, wrong dispatch emitted" - goes away at the source.
- Monomorphized containers ('s pending reference) fall out naturally: `list[int]` stores `i64[]`, `list[float]` stores `f64[]`, `list[Point]` stores `Point*[]`, with no boxing inside.

Downside: touches most of `src/codegen/*.cpp`. Multi-week effort, must phase carefully to keep tests green. `Any` and `Union[...]` need an explicit boxed representation (16-byte `{tag, payload}` passed in two registers; heap-promoted only when stored in a polymorphic container). Some runtime functions currently take `i64` and rely on tag dispatch internally; their signatures need to split (`dragon_print_int(i64)` vs `dragon_print_float(double)` already exist - the polymorphic `dragon_print` shim is what gets thinner). Not a small change, but worth it.

Matches what Julia does for non-`Any` code and what Rust/C++ have always done. Only option that gives native-type performance and removes the structural source of the bug class.

---

## Decision

Adopt Option D. Phase the work so that each phase is independently testable and shippable.

### The Box ABI (used by Phases 3 and 4)

A boxed value is a 16-byte struct passed in two registers:

```
struct DragonBox {
 uint8_t tag; // DragonValueTag
 uint64_t payload; // i64 or bit-cast f64 or pointer
};
```

In LLVM IR: `{ i8, i64 }` returned by-value (sysv ABI splits this into two registers - no memory traffic on hot paths). For storage in a polymorphic container, the entry layout is `{ i8 tag, i64 payload }` packed (which is already roughly how dict entries work today; lists currently carry a single `elem_tag` field for the whole list, which is fine because typed lists are monomorphic).

### Phase 1 - Function ABI strengthening

**Goal:** No function-call boundary should funnel through `i64` for known types.

**Already partially there** (`typeExprToLLVM` returns native types). The work is in:
- Closure env packing (`src/codegen/Functions.cpp:228-273`): pack values in their native size into a typed struct, not all-`i64`. Env layout becomes `{ i64, f64, ptr, i1, ... }` with explicit field types.
- Spawn arg arrays (`fire`/`thread`/generator trampolines): replace the `i64[]` array with a per-call typed struct. The trampoline switch already knows `nargs`; extend it to know per-arg types via a small descriptor.
- Indirect calls through closure pointers: today they assume `i64` params. Use the `callableTypes` map (already exists) to emit the right bitcast on the function pointer, not on every argument.

**Verification:** No new bugs in CodeGen E2E suite (637 tests). IR diff should show fewer `bitcast`/`inttoptr`/`ptrtoint` around `def`/`fire`/`thread` boundaries.

### Phase 2 - Native-typed locals

**Goal:** Local variables hold their native LLVM type. `VarKind` becomes source-metadata only.

- Allocas use `typeExprToLLVM` (or inferred type for untyped locals), not always `i64`.
- Reassignment must preserve native type or transition through an explicit box (when the new type is incompatible - that's a `Union` situation).
- Return-value normalization (`Functions.cpp:160-166`) already handles `i64↔f64↔i1` conversions - these stay, but become *narrowing/widening* between known types, not "is this i64 actually a float?" guesses.
- Audit and delete the parallel `varKinds`/`moduleGlobalKinds`/`funcParamKinds` maps where they're used to *recover* a type. Keep them only for source-level distinctions LLVM can't represent (TypedDict vs Dict, StrLiteral vs Str for refcount decisions, ClassInstance class identity for vtable lookup).

**Verification:** `coerceArg` shrinks measurably (count call sites: today ~152; target after Phase 2: <40, all legitimate widening). Re-run the full test suite.

### Phase 3 - Monomorphized containers

**Goal:** Typed containers store native-typed elements. Replaces the implicit " - Monomorphized Containers" referenced from .

- `DragonList` becomes a family: `DragonListI64`, `DragonListF64`, `DragonListPtr`, `DragonListBox`. The header carries an `elem_kind` discriminator so polymorphic helpers (`len`, `clear`, `__del__`) can still dispatch.
- Codegen picks the right family from `list[T]`'s element type at allocation. `list[Any]` and `list` (untyped) get `DragonListBox`.
- `dict[K, V]`: keys and values each pick their representation. `dict[str, int]` stores keys as `i8*[]` and values as `i64[]`, no boxing. `dict[str, Any]` boxes values.
- Runtime ops (`dragon_list_get_i64`, `dragon_list_get_f64`, `dragon_list_get_ptr`, `dragon_list_get_box`) replace the single polymorphic `dragon_list_get`. CodeGen picks the call by element type. The current polymorphic version stays for `Box` lists and untyped.
- Side effect: Phase 1 (inline GEP for typed lists) becomes trivial - no `elem_tag` mismatch hazard because the type is in the list type itself.

**Verification:** Sieve benchmark should drop from 0.041s toward Go's 0.007s. Float-heavy benchmarks (matmul, anything we add) get >2× from native xmm-register codegen.

### Phase 4 - Boxed `Any` and `Union[...]`

**Goal:** Replace the current "i64 + side-channel tag" union representation with explicit boxes.

- `Union[int, str]`-typed variables become an `{ i8 tag, i64 payload }` value (passed in two registers, stored on stack as a single 16-byte alloca).
- Function params of union type: today we append a hidden `i64` tag arg (`funcUnionTagMask`). Replace with a single `{ i8, i64 }` param. Same number of registers, but type-correct.
- `isinstance` narrowing on a boxed union becomes a branch on `box.tag`, then extraction of `box.payload` cast to the narrowed type. Same logic as today, but structurally typed instead of map-tracked.
- `Any` is just `Union[everything]` - same box.
- The `unionMemberKinds` map can be deleted (the box's tag is canonical).

**Verification:** All union/`Any`/`isinstance` tests pass. The `funcUnionTagMask` / `unionMemberKinds` / `emitUnionDecref` / `emitUnionIncref` machinery is deletable. Diff should be net-negative LOC.

### Phase 5 - Cleanup and deletion

**Goal:** Remove the legacy uniform-i64 scaffolding now that nothing depends on it.

- Reduce `coerceArg` to widening conversions only. Delete the ptr↔int and bool↔int↔float dispatch table.
- Delete `VarKind` enum members that are now redundant with the LLVM type. Likely keep: `Str` vs `StrLiteral` (refcount), `ClassInstance` (class identity), `Union` (boxed-union marker), `Closure` (calling convention marker), `Generator`, `TypedDict` vs `Dict`. Probable deletes: `Int`, `Float`, `Bool`, `List`, `Dict`, `Tuple`, `Set`, `File`, `Bytes`, `Type`, `Other` - recoverable from the LLVM type or the type-system Type tree.
- Delete the parallel maps that backed those VarKinds.
- Update `zen.md`: the "tagged value system" section is no longer accurate at the IR level.

**Verification:** Net LOC decrease in CodeGen. No regressions. Final pass: the term "i64 coercion" should not appear in any future commit message describing a bug.

---

## Payoff vs the refactor bill

**Pros**
- Eliminates the recurring "VarKind got stale" bug class at its source - one source of truth (the LLVM type) instead of two.
- 2-3× speedup on float-heavy code; substantial wins on container-heavy benchmarks once Phase 3 lands.
- Unblocks Phase 1 (inline GEP for typed lists) and tightens its safety story.
- Composes cleanly with (tracing GC): the GC scans typed slots, not "i64 that might be a pointer".
- Net LOC decrease in CodeGen by Phase 5 - fewer special cases, fewer parallel maps.

**Cons**
- 2-4 weeks of focused work, touching most of `src/codegen/*.cpp`.
- Phase 1 and 2 must land before any of the perf wins are realized - early phases pay cost without yet capturing benefit (thats just how phased refactors go).
- Box ABI for `Union`/`Any` is new design surface; needs its own mini-spec doc before Phase 4.
- (refcount GC) interacts with Phase 2: incref/decref of locals must dispatch on LLVM type, not VarKind. Some `emitDecrefByKind` / `emitIncrefByKind` paths get rewritten.

**Risks**
- Test churn during phases - the IR check tests (`generateIR`) will need updating en masse as bitcasts disappear. Mitigation: write a helper that normalizes "expected" IR to ignore obsolete coercions, then tighten as each phase completes.
- Closure env layout change is ABI-visible across `fire`/`thread`/lambda boundaries. Mitigation: Phase 1 lands all at once for the env code path, with a focused test sweep.
- Generator trampoline (`dragon_gen_trampoline`) takes `i64[]` args today. Either keep the old path for generators specifically, or extend the trampoline switch with typed dispatch. Lean toward the latter for consistency.

---

## Success criteria

- All 1089 tests pass at the end of each phase.
- Sieve benchmark ≤ 0.010s after Phase 3 (currently 0.041s).
- `coerceArg` < 40 call sites after Phase 5 (currently ~152).
- No commit on or after Phase 5 has "i64 coercion" or "tag mismatch" in its message.
- The phrase "tagged value system (all values stored as i64)" is removed from `zen.md` and replaced with the new model description.
