# Decision 029: Generational Tracing GC - Replacing Reference Counting

**Status:** Proposed

**Supersedes:** Decision 018 (Reference Counting GC)

RC got us here - 1000+ tests, full stdlib, closures, concurrency. But the objects benchmark still screams at 23× C and every pointer write pays incref/decref. I'm not ripping RC out tomorrow; this is the proposal for when we're ready to stop pretending refcounting is free.

---

## Context / Motivation

We use CPython-style reference counting as the primary memory management strategy. Every heap object carries a 16-byte header with a refcount, and every pointer write emits incref/decref. A cycle collector (trial deletion algorithm) handles reference cycles.

That was the right call for bootstrapping. It got us to 1000+ passing tests, a full stdlib, generators, closures, and concurrency. The cost is still real on object-heavy workloads though (obviously).

**Benchmark evidence :**

| Benchmark | C | Go | Dragon | Dragon/C | Bottleneck |
|-----------|------|------|--------|----------|------------|
| Sieve | 0.011s | 0.029s | **0.011s** | 1.0× | resolved by (was an i64-funnel issue, not RC) |
| Objects | 0.004s | 0.016s | 0.094s | 23× | **RC + malloc/free + GC tracking** - this proposal targets this |
| Fibonacci | 1.036s | 2.249s | 1.273s | 1.2× | pure recursion, no heap |
| Strings | 0.006s | 0.162s | 0.024s | 4× | refcounted-string allocation |

**Pre- numbers** had sieve at 0.032s and objects at 0.059s on a different machine. I thought sieve was a refcount issue; turned out to be i64-funnel + function-call subscript dispatch (Phase 3 fixed it). Easy to misdiagnose these things seperately. Easy to misdiagnose these things seperately.

After, the case for tracing GC got narrower but cleaner. Object-heavy workloads (the 23× C gap on objects) were the real RC pain. Sieve, strings, and fibonacci didn't need this proposal to close their gaps.

**What RC costs us today:**

1. **Per-write overhead**: Every pointer store emits `dragon_incref` + `dragon_decref` - two function calls minimum. In the objects benchmark, that's 2M incref + 2M decref for 1M objects.
2. **Scope-exit cleanup**: `emitScopeCleanupFor` emits a decref call for every heap-typed local at every scope exit (function return, loop break, block end).
3. **Atomic variants**: Multi-threaded code uses `dragon_incref_atomic`/`dragon_decref_atomic` with `__atomic_fetch_add`/`__atomic_sub_fetch` - far more expensive than non-atomic.
4. **GC tracking**: Every container calls `dragon_gc_track` at creation and `dragon_gc_untrack` at destruction, maintaining a global tracked-objects array.
5. **Cycle collector**: `dragon_gc_collect` runs trial deletion (copy refs → subtract internal → BFS reachable → clear unreachable) every 700 allocations. O(tracked_objects) per invocation.
6. **Object header bloat**: 16 bytes per object (refcount 8B + type_tag 1B + gc_flags 1B + class_id 2B + gc_track_idx 4B).
7. **String special-casing**: Strings have their own `dragon_incref_str`/`dragon_decref_str` that navigate from data pointer to header via `offsetof` + heap validation check.

**What tracing GC gets rid of:**

| Operation | Current (RC) | Tracing GC |
|-----------|-------------|------------|
| Pointer store | incref new + decref old | store (1 instruction) + write barrier (~3 instructions) |
| Scope exit | N decref calls per heap local | nothing |
| Object creation | malloc + memset + init header + gc_track | bump allocator (1 instruction) |
| Object death | decref → dealloc → gc_untrack → type-dispatch free | bulk sweep during collection |
| Cycle handling | trial deletion every 700 allocs | natural - tracing finds all reachable objects |
| Thread safety | atomic incref/decref | no per-object atomics needed |

---

## Decision

Replace reference counting with a **generational tracing garbage collector** using a nursery bump allocator and a mark-sweep old generation. `__del__` becomes non-deterministic; context managers (`with` statements) are the recommended pattern for deterministic cleanup.

---

## Architecture

### Memory Layout

**Nursery (young generation):**
- Single contiguous memory region (default 4MB, tunable)
- Bump-pointer allocation: `alloc(size)` = `nursery_ptr += size; return old_ptr`
- One instruction to allocate (compare + pointer increment)
- When full → trigger minor GC

**Old generation:**
- Standard malloc-based allocation
- Objects promoted from nursery after surviving one minor GC
- Mark-sweep collected during major GC (triggered when old gen grows past threshold)

**Object header (new):**

```c
typedef struct {
 uint32_t gc_word; // mark bit (1) + forwarding/age bits (31)
 uint8_t type_tag; // DRAGON_TAG_LIST, etc.
 uint8_t flags; // GEN_YOUNG=0, GEN_OLD=1, FINALIZE=2
 uint16_t class_id; // for user-defined classes
} DragonObjectHeader; // Total: 8 bytes (down from 16)
```

Changes from current header:
- **Removed**: `refcount` (8 bytes) - no longer needed
- **Removed**: `gc_track_idx` (4 bytes) - no tracked-objects array
- **Changed**: `gc_flags` → `flags` (generation + finalization bits)
- **Added**: `gc_word` (mark bit for tracing, forwarding pointer for copying)
- **Net savings**: 8 bytes per object (16 → 8)

### Write Barrier

When an old-generation object stores a pointer to a young-generation object, the GC must know about it (otherwise the minor GC could collect a young object that's reachable only via an old object).

**Card-marking write barrier** (same as Java HotSpot):

```c
// Emitted by codegen after every pointer store into a heap object
static inline void dragon_write_barrier(void* obj) {
 // Card table: 1 byte per 512-byte region of old gen
 // Mark the card containing obj as dirty
 card_table[(uintptr_t)obj >> 9] = 1;
}
```

Cost: ~3 instructions (shift, index, store). Only emitted for stores into heap objects (not stack locals, not primitive stores).

The write barrier is NOT needed for:
- Stores to stack-allocated locals (they're roots, always scanned)
- Stores of primitive values (int, float, bool - not pointers)
- Stores into nursery objects (young→young is fine, both scanned together)
- Object construction (new object is in nursery, all fields are young)

CodeGen emits the barrier only when storing a heap-typed value into a heap-typed container or class field.

### Minor GC (Nursery Collection)

**Algorithm: Semi-space copying collector**

1. **Stop the world** (pause all threads)
2. **Scan roots**: stack frames (all allocas with heap VarKind), globals, thread-local state
3. **Scan remembered set**: old-gen objects with dirty cards (may point to nursery)
4. **Copy live objects**: live nursery objects are copied to old gen (malloc) and forwarding pointers installed
5. **Update references**: all pointers to moved objects are updated via forwarding pointers
6. **Reset nursery**: `nursery_ptr = nursery_start` (one instruction - the entire nursery is free)

**Expected pause time**: < 1ms for 4MB nursery. Most objects die young; only survivors are copied.

### Major GC (Old Generation Collection)

**Algorithm: Mark-sweep**

1. **Mark phase**: starting from roots + nursery refs, traverse all reachable old-gen objects via `gc_word` mark bit
2. **Sweep phase**: walk old gen, free unmarked objects (call type-specific destructors)
3. **Finalization**: objects with `__del__` are enqueued to a finalization queue, `__del__` called after sweep completes

Triggered when old gen size exceeds threshold (default: 2x size after last major GC).

### Finalization (`__del__`)

- `__del__` is **non-deterministic**: called during GC, not at a predictable point
- Objects with `__del__` are added to a finalization queue during sweep
- `__del__` methods are called after the sweep phase completes (so the object is still valid)
- If `__del__` resurrects the object (stores `self` somewhere reachable), it survives until next GC
- **Recommended pattern**: use `with` statements (context managers) for deterministic cleanup
- Dragon documentation will state: "`__del__` is called when the garbage collector reclaims the object. For deterministic resource cleanup, use context managers."

---

## Implementation Plan

### Phase 0: Nursery Bump Allocator

**Runtime changes (`lib/Runtime/runtime_gc.cpp` - new file):**
- `dragon_gc_init`: allocate nursery region (4MB mmap), initialize bump pointer
- `dragon_gc_alloc(size_t size)`: bump-allocate from nursery, trigger minor GC when full
- `dragon_gc_minor`: stub (just resets nursery for now - no copying yet)
- Replace `malloc` calls in `dragon_list_new_tagged`, `dragon_dict_new`, `dragon_string_alloc_raw`, etc. with `dragon_gc_alloc`

**CodeGen changes:**
- `dragon_gc_alloc` declaration in ImplInit.cpp
- Object creation calls `dragon_gc_alloc` instead of emitting `malloc` size calculations

**What to delete:**
- Nothing yet - keep RC infrastructure, just route allocations through nursery

**Tests:** Existing tests pass with nursery allocation (objects still refcounted, just allocated differently)

### Phase 1: Remove Incref/Decref from CodeGen

**CodeGen changes (`src/codegen/*.cpp`, `src/CodeGenImpl.h`):**

Delete all incref/decref emission:
- `emitIncrefByKind` → empty body (guarded by `gcMode`)
- `emitDecrefByKind` → empty body
- `emitScopeCleanupFor` → empty body
- `emitUnionDecref` → empty body
- `emitAtomicIncref` → empty body
- `storeWithRCOverwrite` → plain store (no old-value decref, no new-value incref)
- String concat intermediate decref (Expressions.cpp:264-278) → remove
- List repeat source decref (Expressions.cpp) → remove
- Delete stmt decref (Statements.cpp) → remove
- Return incref (Functions.cpp) → remove
- `for-in` iterator incref/decref → remove

All changes are guarded by a new `GCMode::Tracing` enum value so `--gc=rc` remains available for comparison.

**Runtime changes:**
- `dragon_incref`, `dragon_decref` → no-op when tracing GC active
- `dragon_incref_str`, `dragon_decref_str` → no-op when tracing GC active
- Atomic variants → no-op

**What to delete (eventually):**
- All `dragon_incref*` / `dragon_decref*` function bodies (keep declarations as no-ops during transition)
- `DRAGON_IMMORTAL_REFCOUNT` sentinel and all immortal checks

**Tests:** Existing tests pass with incref/decref disabled (objects leak, but tests are short-lived - GC not yet collecting)

### Phase 2: Write Barrier

**Runtime changes:**
- Card table allocation: `uint8_t card_table[heap_size >> 9]` (one byte per 512 bytes of heap)
- `dragon_write_barrier(void* container)`: mark card dirty
- Remembered set scan: iterate dirty cards to find old→young pointers

**CodeGen changes:**
- After every store of a heap-typed value into a class field or container element, emit:
 ```
 call void @dragon_write_barrier(ptr %container)
 ```
- Skip barrier for: primitive stores, nursery-to-nursery stores (optimization: check if both ptrs are in nursery range), stack locals

**What to delete:** Nothing

### Phase 3: Minor GC (Copying Collector)

**Runtime changes (`lib/Runtime/runtime_gc.cpp`):**
- Root scanning: walk the stack frame chain (requires stack map - see below)
- Copying: allocate in old gen, install forwarding pointer, update all references
- Nursery reset after collection

**Stack maps:**
This is the hard part (no way around it). LLVM can generate stack maps via the `@llvm.experimental.gc.statepoint` intrinsic or the simpler shadow-stack approach:

**Shadow stack (recommended for Dragon):**
- Maintain a thread-local linked list of GC root frames
- Each function pushes a frame on entry listing its heap-typed locals
- Minor GC walks the shadow stack to find all roots
- Cost: ~5 instructions per function call (push frame, pop frame)
- Much simpler than LLVM statepoints, sufficient for Dragon's needs

```c
typedef struct GCFrame {
 struct GCFrame* prev;
 int32_t num_roots;
 void** roots[]; // pointers to stack slots holding heap ptrs
} GCFrame;

__thread GCFrame* __gc_shadow_stack = NULL;
```

CodeGen emits frame push/pop around function bodies that contain heap locals.

**What to delete:**
- `dragon_gc_track` / `dragon_gc_untrack` - no longer needed (tracing finds live objects)
- `gc_tracked` array, `gc_tracked_size`, `gc_tracked_cap` - global tracking state
- `gc_alloc_counter` / `gc_threshold` - allocation-count trigger replaced by nursery-full trigger

### Phase 4: Major GC (Mark-Sweep)

**Runtime changes:**
- `dragon_gc_major`: mark phase (traverse from roots + nursery), sweep phase (free unmarked old-gen objects)
- Finalization queue for `__del__` methods
- Per-type traverse functions are **reused** from current implementation (`dragon_list_traverse`, `dragon_dict_traverse`, etc.) - these already exist in `runtime_core.cpp`

**What to delete:**
- `dragon_gc_collect` - the trial deletion cycle collector (entire function, lines 398-470 of runtime_core.cpp)
- `gc_visit_subtract`, `gc_visit_reachable` - trial deletion helpers
- `GCHashEntry`, `gc_ht_lookup`, `gc_ht_insert` - trial deletion hash table
- `__gc_refs`, `__gc_ht`, `__gc_ht_mask` - TLS state for trial deletion
- All `dragon_*_clear_refs` functions (lines 262-352) - cycle-breaking clear functions
- `dragon_clear_refs` dispatch function

### Phase 5: Object Header Shrink

**Runtime changes:**
- New 8-byte header struct (see Architecture section above)
- Update `dragon_obj_init` for new layout
- Update all code that reads `refcount` (should be none after Phase 1)
- Update `type_tag` / `gc_flags` offsets in all runtime code

**CodeGen changes:**
- Update `headerOffset` constant (currently **3** for class instances - refcount i64 + packed type_tag/gc_flags/class_id i64 + vtable pointer - → changes to 1 i64 gc-word, plus a retained vtable pointer for polymorphic classes; so a class-instance header shrinks 24B → 8B non-polymorphic / 16B polymorphic, vs the 16B → 8B of the runtime `DragonObjectHeader` struct used by containers/strings)
- Update all GEP offsets for class fields, vtable pointer
- Update struct type definitions for classes

**Impact:** 8 bytes saved per object. For 1M objects = 8MB less memory. Also improves cache utilization.

### Phase 6: Escape Analysis Integration

Once tracing GC is in place, escape analysis (Part 2) becomes much simpler:

- Non-escaping objects are stack-allocated (`alloca`) - no GC header, no nursery allocation, no write barrier
- Shadow stack frame excludes these objects (they're not GC roots - they're stack memory)
- Composes with monomorphized containers (Phase 3 - already implemented). Typed list reads + escape-analyzed temporaries should compound for matmul-style float-heavy code.

This phase is documented in and will be implemented after the GC transition.

---

## Complete Deletion Inventory

Everything below is **deleted** by the end of Phase 5:

### Runtime functions to delete:
- `dragon_incref(void*)` - refcount increment
- `dragon_decref(void*)` - refcount decrement + dealloc dispatch
- `dragon_incref_atomic(void*)` - atomic refcount increment
- `dragon_decref_atomic(void*)` - atomic refcount decrement
- `dragon_incref_str(const char*)` - string refcount increment (navigates header)
- `dragon_decref_str(const char*)` - string refcount decrement
- `dragon_incref_str_atomic(const char*)` - atomic string incref
- `dragon_decref_str_atomic(const char*)` - atomic string decref
- `dragon_make_immortal(void*)` - set immortal refcount sentinel
- `dragon_is_immortal_obj(void*)` - check immortal sentinel
- `dragon_gc_track(void*)` - add to tracked-objects array
- `dragon_gc_untrack(void*)` - remove from tracked-objects array
- `dragon_gc_collect` - trial deletion cycle collector (lines 398-470 of runtime_core.cpp)
- `dragon_gc_set_threshold(int64_t)` - set cycle collector trigger threshold
- `dragon_list_clear_refs(void*)` - cycle-break: null out list elements
- `dragon_dict_clear_refs(void*)` - cycle-break: null out dict values
- `dragon_tuple_clear_refs(void*)` - cycle-break: null out tuple elements
- `dragon_set_clear_refs(void*)` - cycle-break: null out set elements
- `dragon_clear_refs(void*)` - cycle-break dispatch by type_tag
- `gc_visit_subtract(void*, void*)` - trial deletion: subtract internal refs
- `gc_visit_reachable(void*, void*)` - trial deletion: BFS reachable marking
- `gc_ht_lookup` / `gc_ht_insert` - trial deletion hash table
- `dragon_incref_tagged(int64_t, uint8_t)` - inline tagged incref
- `dragon_decref_tagged(int64_t, uint8_t)` - inline tagged decref

### Runtime state to delete:
- `gc_tracked` (void**) - tracked-objects array
- `gc_tracked_size`, `gc_tracked_cap` - tracking array bookkeeping
- `gc_alloc_counter`, `gc_threshold` - allocation-count trigger
- `gc_collecting` (int) - reentrancy guard for cycle collector
- `__gc_refs`, `__gc_ht`, `__gc_ht_mask` - TLS state for trial deletion
- `DRAGON_IMMORTAL_REFCOUNT` - sentinel constant
- `GC_FLAG_TRACKED`, `GC_FLAG_REACHABLE` - flag bits (replaced by new gc_word)

### Runtime functions to keep (repurposed):
- `dragon_dealloc(void*)` - still needed for sweeping dead objects (called by major GC)
- `dragon_list_traverse`, `dragon_dict_traverse`, `dragon_tuple_traverse`, `dragon_set_traverse`, `dragon_traverse` - reused by mark phase
- `__class_dealloc_table`, `__class_traverse_table` - reused for user-defined class cleanup and traversal
- `dragon_obj_init` - updated for new header layout

### CodeGen functions to delete/gut:
- `emitIncrefByKind` - all incref emission (CodeGenImpl.h:796)
- `emitDecrefByKind` - all decref emission (CodeGenImpl.h:809)
- `emitScopeCleanupFor` - scope-exit decref loop (CodeGenImpl.h:647)
- `emitScopeCleanup` - wrapper (CodeGenImpl.h:683)
- `emitAllScopeCleanup` - return-path cleanup (CodeGenImpl.h:693)
- `emitScopeCleanupToDepth` - break/continue cleanup (CodeGenImpl.h:706)
- `emitUnionDecref` - union tag-dispatched decref (CodeGenImpl.h:824)
- `emitAtomicIncref` - fire/async spawn incref (CodeGenImpl.h:761)
- `storeWithRCOverwrite` - assignment with decref-old/incref-new (CodeGenImpl.h:880+)
- String concat intermediate decref (Expressions.cpp:264-278)
- List repeat source decref (Expressions.cpp)
- Delete stmt decref (Statements.cpp)
- Return incref (Functions.cpp)

### CodeGen declarations to delete (ImplInit.cpp):
- `dragon_incref` - 22 declarations/uses across codegen files
- `dragon_decref` - same
- `dragon_incref_str` / `dragon_decref_str`
- `dragon_incref_atomic` / `dragon_decref_atomic`
- `dragon_incref_str_atomic` / `dragon_decref_str_atomic`
- `dragon_make_immortal` / `dragon_is_immortal_obj`
- `dragon_gc_track` / `dragon_gc_untrack`
- `dragon_gc_collect`
- `dragon_gc_set_threshold`

### Header fields to delete:
- `DragonObjectHeader.refcount` (i64, 8 bytes) - no longer needed
- `DragonObjectHeader.gc_track_idx` (i32, 4 bytes) - no tracked-objects array

---

## Options Considered

1. **Stay with RC** - Lowest effort but leaves the performance ceiling permanently. Every new feature that touches heap objects inherits the overhead. Not viable if we want "crazy speed."

2. **Upgrade RC → ARC (Swift-style)** - Compiler eliminates 50-70% of redundant incref/decref pairs. Medium effort, incremental improvement. Still pays refcount cost on every non-elided store, still needs the cycle collector, still has the 16-byte header. A local maximum.

3. **Generational tracing GC** - This proposal. Eliminates per-write overhead entirely. Bulk-reclaims dead objects. Natural cycle collection. Pairs well with escape analysis and monomorphized containers. Go, Java, C#, JS all do this at scale (definately proven at this point).

4. **Ownership/borrowing (Rust model)** - Zero runtime cost. But requires a borrow checker, changes the language feel, and is incompatible with Python semantics. Not viable for a Python superset.

5. **Region/arena allocation** - Great for specific patterns (web servers, compiler passes) but not a general-purpose GC replacement. Could be added later on top of tracing GC.

## What changes at runtime

- **`__del__` becomes non-deterministic** - documented behavior change. Context managers recommended for deterministic cleanup. Same as Java, Go, C#, JS.
- **Pointer stores become cheap** - one store + optional write barrier (~4 instructions total) instead of incref+decref (~20+ instructions including function call overhead)
- **Object creation becomes cheap** - bump allocation (1 instruction) instead of malloc+memset+init+track
- **Object header shrinks** - 16 → 8 bytes (8 bytes saved per object)
- **Cycle collector deleted** - ~200 lines of complex trial-deletion code removed
- **All incref/decref code deleted** - ~100 lines of runtime code + ~200 lines of codegen emission code
- **GC pauses introduced** - minor GC < 1ms, major GC potentially longer but infrequent. For Dragon's target workloads (CLI tools, servers, scripts), probably fine (we'll see once it's wired up).
- **`--gc=rc` flag retained during transition** - allows A/B comparison and rollback
- **Opens the door for future optimizations** - escape analysis, monomorphized containers, and SIMD autovectorization all benefit from removing per-object refcount overhead

---

## Expected Performance Impact

| Benchmark | RC (pre-) | RC + Mono Containers (current) | + Tracing GC (proj.) | + Escape Analysis (proj.) |
|-----------|---------------|-------------------------------|---------------------|--------------------------|
| Sieve | 0.032s | **0.011s** ok | ~0.011s | ~0.008s |
| Objects | 0.059s | 0.094s* | ~0.020s | ~0.005-0.008s |
| Fibonacci | 0.772s | 1.273s* | ~1.273s | ~1.273s |
| Strings | 0.014s | 0.024s* | ~0.010s | ~0.010s |

\* Variance vs the pre- numbers in non-target benchmarks is mostly machine-load noise; runs were not on identical hardware. Sieve's 3× drop is unambiguous because the bottleneck (i64-funneled subscripts) is structurally gone.

The objects benchmark saw the largest projected improvement because it's dominated by malloc/free/incref/decref - all eliminated by nursery allocation + tracing.

---

## References

- [The Garbage Collection Handbook](https://gchandbook.org) - Jones, Hosking, Moss (2011)
- [Go GC design](https://tip.golang.org/doc/gc-guide) - concurrent mark-sweep with write barrier
- [Java G1 GC](https://docs.oracle.com/en/java/javase/17/gctuning) - generational, region-based
- [LuaJIT GC](https://wiki.luajit.org/New-Garbage-Collector) - incremental tri-color mark-sweep
- [V8 Orinoco GC](https://v8.dev/blog/trash-talk) - generational, concurrent marking
- [CPython nogil (PEP 703)](https://peps.python.org/pep-0703) - removing the GIL required rethinking refcounting; biased refcounting + deferred RC as intermediate step
