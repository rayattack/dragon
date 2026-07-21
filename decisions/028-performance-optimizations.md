# Decision 028: Performance Optimizations - Closing the Gap with Go/Rust

**Status:** Implemented. Part 1 (inline list access + list repetition) landed as a side-effect of Phase 3. Part 2 (escape analysis + stack allocation) shipped; the objects benchmark now ties the C tier (0.004s).

The benchmark spreadsheet kept taunting me. Sieve was ~6× off Go, objects were 23× off C - two different bugs, two different fixes. This ADR is me writing down what actually closed each gap so I don't re-litigate it at 1am next time.

---

## Context / Motivation

> **Update :** Original benchmark numbers were taken before . Re-measured below. Sieve (Part 1's target) is now resolved. Objects (Part 2's target) was the open item until Part 2 landed.

**Current benchmarks :**

| Benchmark | C | C++ | Rust | Go | Java | Python | Dragon | Status |
|---|---|---|---|---|---|---|---|---|
| Fibonacci (n=42) | 1.024s | 0.992s | 1.290s | 2.364s | 1.911s | - | **1.294s** | ties Rust, ~26% off C |
| Sieve (n=1M) | 0.015s | 0.018s | 0.008s | 0.025s | 0.230s | 0.245s | **0.010s** | **ties/beats C** |
| Strings (10k) | 0.003s | 0.006s | 0.004s | 0.043s | 0.037s | 0.032s | **0.010s** | ~3× C, beats Go 4× |
| Objects (1M) | 0.002s | 0.003s | 0.003s | 0.004s | 0.050s | 0.545s | **0.002s** | **ties C, beats Rust/Go** |

> Sub-10ms rows (sieve/strings/objects) are mostly process startup + runtime init at this resolution. Treat them as "C tier" / "off C tier," not precise ratios. Objects fell from the baseline's 0.094s to 0.002s after Part 2 shipped.

**Prior benchmarks :**

| Benchmark | C | C++ | Rust | Go | Java | Python | Dragon | Status |
|---|---|---|---|---|---|---|---|---|
| Fibonacci (n=42) | 1.036s | 0.999s | 1.421s | 2.249s | 2.127s | - | **1.273s** | beats Rust/Go/Java |
| Sieve (n=1M) | 0.011s | 0.015s | 0.021s | 0.029s | 0.309s | 0.274s | **0.011s** | **ties C** |
| Strings (10k) | 0.006s | 0.008s | 0.006s | 0.162s | 0.263s | 0.080s | **0.024s** | 4× C, beats Go 7× |
| Objects (1M) | 0.004s | 0.005s | 0.008s | 0.016s | 0.276s | 0.752s | **0.094s** | 23× C - open gap |

**Original benchmarks :**

> Faster than Go on fibonacci (0.81s vs 1.44s) and strings (0.014s vs 0.040s). Slower than Go on sieve (0.041s vs 0.007s, ~6×) and object creation (0.053s vs 0.004s, ~13×). Consistently faster than Java and Python.

Two distinct gaps, two distinct causes:

1. **Sieve** *(now resolved)*: every `list[i]` went through `dragon_list_get` as a function call. Phase 3 monomorphized containers and Attributes.cpp now emits inline-GEP for typed lists with native-stride loads (i8 for `list[bool]`, i64 for `list[int]`, f64 for `list[float]`, ptr for `list[<heap>]`).
2. **Objects** *(was open, now fixed)*: every `Point(x, y)` did `malloc(40)` + `memset(40)` + GC header init + `gc_track` + `__init__` call, then `decref` + `free` at scope exit. Go stack-allocates non-escaping objects (zero malloc cost). didn't touch this. It's allocation/GC pattern, not a value-model issue.

Both need real compiler passes. Runtime hacks wont cut it (I tried).

> **Failed quick-fix attempt :** Three optimizations were tried and reverted:
> - `dragon_list_filled(N, val, tag)` - missing incref for heap-typed fill values
> - `[val] * N` codegen - wrong tag for non-string pointer types (would corrupt memory)
> - Skip `gc_track` for primitive-only classes - under-analyzed edge cases with `__del__` and inheritance
>
> Lesson: anything that touches GC needs to be designed properly, not patched in at the last minute.

---

## Part 1: Inline List Access for Typed Lists - IMPLEMENTED

> **Status :** Done as a side-effect of Phase 3. Sieve dropped from 0.041s to 0.011s, matching C. Keeping the description below for history; the actual implementation evolved with 's monomorphized containers.

### Problem (historical)

Dragon's `list[int]` originally stored elements as `i64` in a `DragonList` struct. Post-, the family is:

```c
struct DragonList { header; void* data; size; capacity; elem_tag; elem_size; }; // I64 variant
struct DragonListF64 { header; double* data; size; capacity; elem_tag; elem_size; }; // list[float]
struct DragonListPtr { header; void** data; size; capacity; elem_tag; elem_size; }; // list[<heap>]
```

Every `is_prime[i]` originally compiled to:
```llvm
%val = call i64 @dragon_list_get(i8* %list, i64 %i)
```

Function call → null check → bounds check → load. In the sieve inner loop that overhead definately dominated.

### Solution as shipped: Direct GEP for Typed Lists

When the compiler knows the list type, codegen emits direct memory access at the element's native LLVM type (per Phase 3.B). No function call, no i64-bashing.

**Phase 1: Known-layout direct access (always bounds-checked)** - IMPLEMENTED in `src/codegen/Attributes.cpp` `visit(SubscriptExpr)`. Native stride per element kind:
- `list[bool]` → i8 stride (1-byte packing)
- `list[int]` → i64 stride
- `list[float]` → f64 stride
- `list[<heap>]` → ptr stride

The call to `dragon_list_get` only fires on the OOB path (unreachable after the bounds check anyway).

**Phase 2: Bounds check elimination** - partially handled by LLVM's SCEV pass once Phase 1 removed the function-call barrier. I didn't pursue tighter range propagation (e.g. `j <= limit < len(list)`) as a separate pass.

**Phase 3: List repetition (`[val] * N`)** - IMPLEMENTED. `dragon_list_repeat(src, count)` exists in the runtime with proper refcount semantics; TypeChecker accepts `list * int`. Verified working post-.

### Impact (achieved)

Sieve dropped from 0.041s → **0.011s** (matches C, beats Rust/Go). Target of "0.008-0.012s on par with Go/C" met.

### Effort

- Phase 1: Medium (change SubscriptExpr codegen for known-list-typed variables)
- Phase 2: Low (LLVM handles most of this once Phase 1 removes the function call barrier)
- Phase 3: Medium (runtime + codegen + TypeChecker, but must handle all elem_tag/incref cases)

---

## Part 2: Escape Analysis + Stack Allocation for Objects

> **Status : IMPLEMENTED.** Shipped as two composable optimizations - **Option A** (the safe `gc_track` skip, = the proposed Phase 4) and **Option B Phase 1** (escape analysis → stack allocation, = the proposed Phases 1-3). Objects benchmark dropped **0.094s → 0.004s** (best-of-3), tying the C tier and beating Go (0.016s) and Rust (0.008s). Verified: 1015 codegen + 58 interop tests green, correct output, IR-level confirmation that every escape vector stays on the heap. Phase numbering below is kept for history; here's what actually shipped:
>
> **What shipped, mapped to the proposal:**
>
> - **Option A = Phase 4 (safe `gc_track` skip).** A class is *acyclic* when it (and its full MRO) has no heap-type fields, no `__del__`, and can't otherwise root a cycle. Such instances can't form reference cycles, so `_new` omits the `dragon_gc_track` call and decref hits the lock-free fast path. This is the proper version of the reverted hack: walks the MRO, gated on class metadata, not patched inline. Code: `src/codegen/Classes.cpp` (`classIsAcyclic`) + the gated `dragon_gc_track` call. Tests: `AcyclicClassNotTracked` / `CyclicCapableClassTracked` (`test/CodeGenClassesTest.cpp`).
>
> - **Option B Phase 1 = Phases 1-3 (escape analysis + stack alloc + header elision).** A compile-time escape pass (`src/codegen/EscapeAnalysis.cpp`: `computeStackAllocSites`, `analyzeBlockForStackAlloc`, `exprEscapes`, `stmtEscapes`, `nodeMentionsName`) finds locals that provably never escape their declaring block. The construction fork (`src/codegen/CallExpr.cpp`) then builds the instance in an entry-block `alloca` + memset + immortal refcount + a direct `__init__` call. No `malloc`/`_new`/`gc_track`, no decref/free at scope exit. LLVM's SROA scalarizes the instance away and folds the benchmark loop. Phases 1 (detection), 2 (stack alloc), and 3 (header elision via SROA) collapsed into one unit.
>
> **Design choices that stuck:**
>
> 1. **Acyclic detection walks the full MRO**, not just the immediate parent (the reverted hack's bug). A single heap-type field, `__del__`, or unknown base anywhere in the chain makes the class cyclic-capable → tracked.
> 2. **The escape predicate is conservative: default-escapes.** Anything the analysis doesn't positively prove non-escaping stays on the heap. `nodeMentionsName` is an exhaustive fallback so an unhandled AST shape never produces a false "no-escape" verdict. Wrong verdict = dangling stack pointer, so the failure mode is "miss an optimization," never "miscompile."
> 3. **No new `VarKind`.** Stack instances reuse `VarKind::ClassInstance`; "this binding is stack-allocated" is tracked out-of-band via a `Scope.stackAllocated` set (`src/CodeGenImpl.h`) consulted at block-exit cleanup to skip the decref/free. A stack instance *is* a class instance for every type/field/dispatch decision. Only storage class and cleanup differ. A new `VarKind` would have forced every existing `ClassInstance` switch arm to grow a parallel case for no semantic gain.
> 4. **Eligibility gates (Phase 1 restrictions, all in `stackEligibleClasses`, `src/codegen/Classes.cpp`):** scalar-only fields (no heap-owning fields yet - see deferred B Phase 2), a single ctor that doesn't escape `self`, and no per-instance field defaults. Anything outside the gate falls to the heap path (still gets Option A if acyclic).
>
> **Deferred follow-on (broadens which patterns benefit; won't move the benchmark number):**
> - **B Phase 2 - heap-owning fields:** emit a per-class `__dragon_drop_fields_T(self)` (decref children, no free) called at block-exit cleanup, lifting the scalar-only gate.
> - **B Phase 3 - cross-block + method calls:** promote instances used across nested blocks (cleanup at the correct scope depth via `emitScopeCleanupToDepth`); allow non-escaping method calls once per-method `self`-escape summaries exist.
> - **B Phase 4 - interprocedural:** per-function escape summaries so `helper(p)` keeps `p` on the stack when `helper` provably doesn't retain it.
> - **Option C - dead-memset elision:** skip zero-init when the ctor writes every field. Marginal (LLVM already fuses `malloc`+`memset`→`calloc`).

### Problem

Every `Point(x, y)` allocates on the heap:
```
malloc(40) → memset(40) → init header → gc_track → __init__ → ... → decref → free
```

40 bytes per object: 16 (GC header) + 8 (vtable ptr) + 8 (x) + 8 (y).

In the benchmark loop, `p` never escapes. Created, fields read, destroyed at scope exit. Classic escape-analysis win.

### Proposed Solution: Compile-time Escape Analysis

**Phase 1: Non-escaping object detection**

An object "escapes" if any of these are true:
- Stored in a global variable
- Stored in a heap container (list, dict, class field)
- Passed to a function that isn't inlined
- Returned from the current function
- Stored via pointer through unknown alias

For the simple case: `p = Point(x, y)` where `p` is only used for field reads/writes within the same scope and never passed anywhere - **provably non-escaping**.

**Phase 2: Stack allocation for non-escaping objects**

Replace `malloc` with `alloca` in the `_new` function (or better: inline the entire construction):

```llvm
; Instead of call @Point_new(i64 %x, i64 %y)
%p = alloca %Point ; stack-allocated, zero malloc cost
; Init fields directly (skip memset - alloca is undefined, but we write all fields)
%x_ptr = getelementptr %Point, %Point* %p, i32 0, i32 3 ; field offset
store i64 %x, i64* %x_ptr
%y_ptr = getelementptr %Point, %Point* %p, i32 0, i32 4
store i64 %y, i64* %y_ptr
; No gc_track (stack-allocated = no GC involvement)
; No decref at scope exit (stack cleanup is automatic)
```

This eliminates: malloc, memset, header init, gc_track, decref, free. All six.

**Phase 3: GC header elision**

Stack-allocated objects don't need:
- Refcount (not refcounted - lifetime is lexical)
- Type tag (known at compile time)
- GC tracking (not on heap)
- Vtable pointer (class known at compile time → direct dispatch)

So the stack allocation can be just the **fields**: 16 bytes for Point instead of 40.

**Phase 4: Selective gc_track skip (the safe version)**

For **heap-allocated** objects that provably cannot form cycles:
- Class has no heap-type fields (checked including parent chain)
- Class has no `__del__` dunder
- Class does not implement `__traverse__` or `__clear__`

These objects still need refcounting (incref/decref) but can skip `dragon_gc_track`/`dragon_gc_untrack`, which avoids the cycle collector overhead.

This is the optimization that was attempted and reverted. The proper version must:
1. Walk the full MRO (not just immediate parent) for heap field detection
2. Check for `__del__` which can execute arbitrary code
3. Be implemented as a separate LLVM pass or a flag on the class metadata, not inline in `emitNewBody`
4. Have solid test coverage: inheritance chains, context managers, generators, classes with mixed field types

### Impact

Expected: objects benchmark drops from 0.053s to ~0.004-0.008s (on par with Go/Rust). Actual (post-ship): 0.004s best-of-3, ties C tier.

### Effort

- Phase 1: Large (escape analysis is a significant compiler pass)
- Phase 2: Medium (once escape analysis exists, stack alloc is straightforward)
- Phase 3: Small (just skip header fields for stack-allocated objects)
- Phase 4: Medium (safe gc_track skip requires careful analysis - see reverted attempt)

---

## Part 3: List Repetition (`[val] * N`) - Correct Design

### Problem

`[True] * 1000000` originally didn't compile (TypeChecker rejects `list * int`). The Python idiom is common for initializing large arrays.

### Proposed Solution

**Runtime function: `dragon_list_repeat(list_ptr, count) → new_list_ptr`**

Takes an existing list and repeats its contents N times into a new list. Handles:
- Multi-element repetition: `[1, 2, 3] * 4` → 12 elements
- Heap element incref: each copy of a string/list/dict element gets incref'd
- Correct elem_tag propagation from source list

```c
DragonList* dragon_list_repeat(DragonList* src, int64_t count) {
 int64_t total = src->size * count;
 DragonList* result = dragon_list_new_tagged(total, src->elem_tag);
 result->size = total;
 for (int64_t c = 0; c < count; c++) {
 for (int64_t i = 0; i < src->size; i++) {
 int64_t val = src->data[i];
 result->data[c * src->size + i] = val;
 // Incref heap elements
 if (val && (src->elem_tag == TAG_STR))
 dragon_incref_str((const char*)(uintptr_t)val);
 else if (val && (src->elem_tag >= TAG_LIST))
 dragon_incref((void*)(uintptr_t)val);
 }
 }
 return result;
}
```

**Codegen**: `[expr1, expr2, ...] * N`:
1. Evaluate the list literal normally (produces a list with N elements)
2. Call `dragon_list_repeat(list, count)`
3. Decref the temporary source list

**TypeChecker**: Allow `list * int → list` and `int * list → list`.

**Special case**: `[primitive_literal] * N` can be optimized to a single-allocation fill loop since there's no incref needed and no source list to build.

### Impact

Correct semantics, proper refcounting, no memory leaks.

---

## Options Considered

1. **Status quo** - Dragon is already faster than Java/Python/Go on most benchmarks. Accept the sieve/objects gap.
2. **Quick hacks** - Tried and reverted. GC-interacting optimizations are too fragile without proper analysis passes.
3. **Proper compiler passes** - This proposal. Higher effort but correct and maintainable.
4. **Switch to tracing GC** - Would eliminate refcount overhead but add pause times. Not aligned with Dragon's deterministic-cleanup design.

## Decision

Original implementation order (impact-per-effort):
1. done **Part 3** (list repetition) - DONE
2. done **Part 1 Phase 1** (inline list access) - DONE via Phase 3.B with native stride per element kind
3. done **Part 2 Phase 4** (safe gc_track skip) - DONE as Option A (acyclic skip-tracking; full-MRO check)
4. done **Part 2 Phases 1-3** (escape analysis + stack alloc) - DONE as Option B Phase 1; closed the 23× C gap on the objects benchmark (0.094s → 0.004s)

## What actually landed

- done Dragon matches C on collection-heavy benchmarks (sieve, 0.011s vs 0.011s)
- done Dragon now matches the C tier on object-heavy benchmarks (objects, 0.004s - ties C, beats Go 0.016s and Rust 0.008s)
- done The escape-analysis pass (`src/codegen/EscapeAnalysis.cpp`) is in the codegen now. Future optimizations (closure capture elision, temporary elimination) can build on it.
- warn Escape analysis is memory-safety-critical: a wrong "no-escape" verdict = a dangling stack pointer. The pass is deliberately conservative (default-escapes, `nodeMentionsName` exhaustive fallback). Any future broadening (B Phases 2-4) must preserve that bias and re-verify the negative cases (return / append / pass / alias / method / `fire` / heap-field) stay on the heap.
- done List repetition syntax reaches Python parity (`[0] * N`)
