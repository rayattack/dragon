# Decision 020: GC Bug Fixes - Reference Counting Hardening

> **Status:** Approved. Critical - memory leaks, double-frees, crashes under concurrency.

018 got reference counting scaffolding in place (object headers, scope-exit decref, string RC, class instance RC, atomic RC for threads, cycle collector). I did a deep audit with valgrind and too much coffee and found **~25-35% of heap allocations still leak** - missing RC on temporaries, container ops, edge cases. Cycle collector finds garbage but doesn't free it yet. This doc tracks teh fixes, prioritized.

Two fix commits landed between audits (`e6d8d7d`, `f3ee633`):
- C3: `dragon_str_index` now returns a proper `dragon_string_alloc`'d pointer
- `dragon_dict_set_tagged` overwrite decref
- `dragon_list_set` overwrite decref, `dragon_set_copy` incref
- `dragon_list_extend` incref, `dragon_set_clear/remove` decref

What's left is 6 priority tiers. Each fix is discrete and testable on its own. No new language features - just correctness.

---

## Phase 1: Cycle Collector Completion (CRITICAL)

### 1a. Free unreachable objects after `dragon_clear_refs`
**Location:** `runtime.cpp:2696-2704`

`dragon_gc_collect` calls `dragon_clear_refs` on unreachable objects (breaks internal references), but never calls `dragon_dealloc` to actually free them. Objects sit as zombies in `gc_tracked[]`, re-scanned every cycle.

**Fix:** After `dragon_clear_refs`, call `dragon_dealloc` on each unreachable object. Reverse iteration (already in place) since `dragon_dealloc` calls `dragon_gc_untrack` which does swap-with-last.

### 1b. Fix CLASS path in `dragon_clear_refs`
**Location:** `runtime.cpp:2599-2603`

`dragon_clear_refs` for CLASS type calls `__class_dealloc_table[cid]` which **frees the object**, then the gc_collect cleanup loop dereferences freed memory. Need a separate `__class_clear_table` that zeros refs without freeing.

**Fix:** Add `__class_clear_table` parallel to dealloc/traverse tables. Generate per-class `__clear__` functions that decref fields but don't free `self`. Use clear table in `dragon_clear_refs`, dealloc table only in `dragon_dealloc`.

---

## Phase 2: Container Incref/Decref Gaps (CRITICAL)

### 2a. `dragon_list_clear` - decref elements before zeroing
**Location:** `runtime.cpp:892-894`

Currently `list->size = 0` with no decref. Must iterate and decref all heap-typed elements (using `elem_tag`) before zeroing size.

### 2b. `dragon_list_slice` - incref shared elements
**Location:** `runtime.cpp:1709-1721`

Elements copied to slice without incref. Both source and slice share references → double-free on destroy. Must incref each element based on `elem_tag`.

### 2c. `dragon_dict_values` - incref values + propagate tag
**Location:** `runtime.cpp:2257-2265`

Values appended without incref; new list has no `elem_tag`. Must incref each value based on entry tag and set appropriate `elem_tag` on the result list.

### 2d. `dragon_set_add` - incref stored elements
**Location:** `runtime.cpp:2012-2023`

Stores without incref but remove/clear/destroy decref. Asymmetric RC. Must incref on add. Cascades to fix `set_union/intersection/difference/symmetric_difference` which all call `set_add`.

### 2e. `dragon_list_insert` - incref inserted element
**Location:** `runtime.cpp:832-847`

Raw store without incref. Same class as 2d.

### 2f. `dragon_tuple_set` / `dragon_dict_items` - incref on store
**Location:** `runtime.cpp:1910-1914, 2274-2276`

Tuples used by `dict_items` store keys/values without incref. Low severity (write-once) but still wrong.

---

## Phase 3: Fire/Async RC Balance (HIGH)

### 3a. Balance atomic incref with decref in spawned function
**Location:** `CodeGen.cpp:7220-7232`

`fire fn(args)` atomically increfs heap args, but the spawned function marks params as borrowed (never decrefs). Every `fire fn(str_arg)` permanently leaks one refcount.

**Fix:** When emitting the body of a fire'd/async function, mark heap-typed params as owned (not borrowed) so they are decref'd at scope exit. Or emit explicit `dragon_decref_atomic` calls at function epilogue for fire targets.

### 3b. Handle lambdas and imported functions in fire
**Location:** `CodeGen.cpp:7224-7227`

`funcParamKinds` only populated for forward-declared functions. Firing a lambda or imported function skips incref entirely. Need fallback kind inference.

---

## Phase 4: Temporary Value Cleanup (HIGH)

### 4a. ExprStmt heap result decref
**Location:** `CodeGen.cpp:7330-7333`

`visit(ExprStmt)` discards result without cleanup. After `node.expr->accept(*this)`, determine VarKind of result; if heap-kinded, emit decref on `lastValue`.

### 4b. String method chain intermediate decref
**Location:** `CodeGen.cpp:4664-4676`

`s.upper.strip` - intermediate from `upper` leaked. When a method call's receiver is itself a call expression (temporary), save and decref it after the outer call completes.

### 4c. F-string / template concat intermediate decref
**Location:** `CodeGen.cpp:2398-2408, 2585-2595`

In the concat loop, decref `result` before overwriting with the next concat. Also decref conversion results (`dragon_int_to_str`, etc.) after they are consumed by concat.

### 4d. Binary string concat chain intermediate decref
**Location:** `CodeGen.cpp:2884-2889`

For `a + b + c`, the LHS of the outer concat is itself a concat result (temporary). Track and decref after the outer concat.

---

## Phase 5: Thread Safety (MEDIUM)

### 5a. Protect `gc_tracked` with mutex
**Location:** `runtime.cpp:155-158`

Add a mutex (or spinlock) around `dragon_gc_track`, `dragon_gc_untrack`, and `dragon_gc_collect`. Make `gc_alloc_counter` atomic.

### 5b. Atomic child decref in dealloc
**Location:** `runtime.cpp:91-116`

When `dragon_dealloc` is called from `dragon_decref_atomic`, child decrefs must also be atomic. Add a thread-local flag or parameter to propagate "atomic context" into destroy functions.

---

## Phase 6: Scope & Generator Cleanup (MEDIUM)

### 6a. Comprehension scope cleanup (12 sites)
**Location:** `CodeGen.cpp` - ListCompExpr, DictCompExpr, SetCompExpr

Add `emitScopeCleanup` before every `popScope` in comprehension emission. Currently low impact (loop vars are Int), but prevents future leaks.

### 6b. Match arm scope cleanup
**Location:** `CodeGen.cpp:9610`

Add `emitScopeCleanup` before `popScope` in match case arms.

### 6c. Generator destroy retained values
**Location:** `runtime.cpp:4894-4903`

Add `arg_tags` array to `DragonGenArgs`. In `dragon_generator_destroy`, decref heap-typed args. Also decref `yielded_value` if heap-typed (needs a `yielded_tag` field on `DragonGenerator`).

---

## What to expect after the fixes

- **Leak rate drops from ~30% to <5%** after Phases 1-4
- **Crash risk eliminated** for cycle collector (Phase 1) and container ops (Phase 2)
- **Thread safety** addressed in Phase 5 (currently only matters for `thread {}` and `Thread`)
- Phase 6 is preventive - fixes latent bugs that don't yet manifest in typical code
- No language syntax changes, no new AST nodes, no parser changes
- All fixes are independently testable via existing E2E test infrastructure

---

## Verified Correct (No Changes Needed)

| Component | Status |
|-----------|--------|
| DragonObjectHeader layout (16 bytes) | Correct |
| dragon_incref / dragon_decref (non-atomic + atomic, memory orderings) | Correct |
| storeWithRCOverwrite (self-assignment guard) | Correct |
| emitScopeCleanup for functions, for-loops, try/except handlers | Correct |
| Break/continue scope cleanup before jump | Correct |
| Return value incref before scope cleanup | Correct |
| Class _new refcount init, __dealloc__/__traverse__ generation | Correct |
| dragon_gc_track in class _new | Correct |
| fire/thread atomic incref for forward-declared functions | Correct |
| dragon_list_destroy / dict_destroy / tuple_destroy / set_destroy | Correct |
| dragon_list_copy / list_extend / dict_copy / set_copy incref | Correct |
| dragon_dict_clear decref, dragon_dict_set_tagged overwrite decref | Correct |
| AugAssign += string via storeWithRCOverwrite | Correct |
| dragon_str_index returns dragon_string_alloc'd pointer | Correct |
| Borrowed parameter convention (coherent, matches CPython) | Correct |
