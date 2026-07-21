# Decision 003: Memory Management Plan [Superseded]

Superseded. See [018-garbage-collector.md](018-garbage-collector.md). Critical blocker for any long-running program; I fixed it differently than this doc proposes.

This ADR's arena-first plan got rejected. Dragon ships reference counting now (hardened elsewhere). I'm leavin this file around for the leak audit and the old rationale - don't implement from here.

Everything leaked. 100% leak rate. Every heap alllocation in the generated C runtime was never freed. I confirmed it with an audit after a debugging session that made me question my life choices:

- **53 `malloc`/`calloc` call sites** in `CEmitterRuntime.cpp`
- **0 `free` calls** anywhere in the runtime
- **0 destructor** logic for any runtime type

Every `dragon_str_concat`, `dragon_str_upper`, `dragon_list_new`, `dragon_list_copy`, `dragon_list_sorted`, `dragon_dict_new`, and dozens of other functions allocate memory that is never reclaimed.

### Leak Categories

| Category | Allocation Sites | Example Functions |
|----------|-----------------|-------------------|
| String operations | ~35 | `dragon_str_concat`, `dragon_str_upper`, `dragon_str_lower`, `dragon_str_replace`, `dragon_str_split`, `dragon_str_join`, `dragon_str_strip`, all `dragon_str_*` methods |
| List operations | ~10 | `dragon_list_new`, `dragon_list_copy`, `dragon_list_slice`, `dragon_list_sorted`, `dragon_list_reversed`, `dragon_enumerate`, `dragon_zip`, `dragon_map`, `dragon_filter` |
| Dict operations | ~5 | `dragon_dict_new`, `dragon_dict_copy`, `dragon_dict_keys`, `dragon_dict_values` |
| Utilities | ~3 | `dragon_bin`, `dragon_hex`, `dragon_oct`, `dragon_chr`, `dragon_fstr_*` |

### Specific Broken Functions

1. **`dragon_list_clear`** - resets `list->size = 0` but does NOT free the backing `void** data` array or any contained elements
2. **`dragon_dict_clear`** - resets `dict->size = 0` but does NOT free keys (strings) or values
3. **`dragon_list_pop`** - returns the element but doesn't shrink or free anything
4. **`dragon_dict_pop`** - removes from logical view but doesn't free the key string
5. **All string methods** - every method that returns a new string mallocs and never frees

### Real-World Impacct

```python
# Leaks ~80 bytes per iteration
def process_names(names: list[str]) -> list[str] {
 result: list[str] = [] # malloc (list struct + backing array)
 for name in names {
 upper: str = name.upper # malloc (new string, never freed)
 result.append(upper) # may realloc backing array
 }
 return result
}

# Over 1M iterations: ~80MB leaked
for i in range(1000000) {
 names: list[str] = ["alice", "bob"] # malloc x2 (never freed)
 process_names(names) # malloc x3 (never freed)
}
```

---

## Design Options

### Option A: Arena Allocator (Recommended for Phase 1) [Done]

**Concept:** All allocations go to a per-function or per-scope arena. When the scope exits, the entire arena is freed in one call.

```c
// Generated C code:
void* dragon_arena_current = NULL;

static void* dragon_arena_alloc(size_t size) {
 // Bump allocator within current arena
 // Falls back to malloc if no arena active
}

static void dragon_arena_reset(void* arena) {
 // Free everything allocated since arena was created
}

// Example generated function:
int64_t fibonacci(int64_t n) {
 void* arena = dragon_arena_push; // Save arena state
 // ... all allocations go to arena ...
 int64_t result = /* computed */;
 dragon_arena_pop(arena); // Free all allocations in this scope
 return result;
}
```

**Good:**
- Simple to implement (~200 lines of C runtime)
- Very fast allocation (bump pointer)
- Very fast deallocation (one reset)
- No per-object overhead
- Works perfectly for functions that allocate temporaries

**Bad:**
- Cannot free individual objects (all-or-nothing) - acceptable for v0.2, and `--mem=rc` exists for Phase 2

**How it handles common concerns:**
- **Returned values** are copied out of the callee's arena into the caller's arena before reset. This is the same ownership-transfer pattern as Rust's move semantics - one memcpy per return, predictable and cheap.
- **Long-lived data structures** (globals, class fields) live on a module-level arena whose lifetime is the program. Function-scoped arenas nest inside it. The key rule: an arena's lifetime >= everything allocated in it. Multiple arena tiers solve this naturally.
- **Strings returned from functions**: `strdup` into the caller's arena before `dragon_arena_pop`. This is standard arena practice, not a limitation - it's just part of the implementation.

**Rough effort:** 1-2 weeks

### Option B: Reference Counting [Dropped]

**Concept:** Every heap object has a reference count. Increment on assignment, decrement on scope exit. Free when count reaches zero.

```c
typedef struct {
 int32_t refcount;
 // ... payload ...
} dragon_rc_header;

static void dragon_incref(void* obj) {
 ((dragon_rc_header*)obj - 1)->refcount++;
}

static void dragon_decref(void* obj) {
 dragon_rc_header* hdr = (dragon_rc_header*)obj - 1;
 if (-hdr->refcount == 0) {
 // type-specific destructor
 free(hdr);
 }
}
```

**Good:**
- Deterministic deallocation
- Works for all lifetime patterns (short and long)
- Familiar model (CPython uses this)

**Bad:**
- Per-object overhead (4 bytes minimum)
- Every assignment/scope-exit emits inc/dec instructions
- Cycles leak (need cycle collector or weak refs)
- Significantly more complex CEmitter changes
- Runtime performance overhead (~10-20% for ref-heavy code)

**Rough effort:** 3-4 weeks

### Option C: Boehm GC (Conservative Garbage Collector) [Dropped]

**Concept:** Link against `libgc` (Boehm-Demers-Weiser). Replace `malloc` with `GC_malloc`. No explicit free needed.

```c
#include <gc/gc.h>

static dragon_list_t* dragon_list_new {
 dragon_list_t* list = (dragon_list_t*)GC_MALLOC(sizeof(dragon_list_t));
 list->data = (void**)GC_MALLOC(8 * sizeof(void*));
 // ...
}
// No free needed anywhere
```

**Good:**
- Minimal code changes (replace `malloc` → `GC_MALLOC`)
- Handles cycles automatically
- Well-tested library (used by Mono, GCJ, etc.)
- Zero changes to CEmitter logic

**Bad:**
- External dependency (`libgc-dev`)
- Non-deterministic collection (not suitable for real-time)
- Conservative scanning may retain false positives
- Harder to debug memory issues
- ~5-10% throughput overhead

**Rough effort:** 1 week (mostly build system)

### Option D: Hybrid (Recommended Long-Term) [Deferred]

**Concept:** Combine arena allocation for function-local temporaries with reference counting for escaped values.

```c
// Compiler analysis determines:
// - "local" values → arena (fast, bulk-freed)
// - "escaped" values (returned, stored in globals) → refcounted

static const char* dragon_str_concat(const char* a, const char* b) {
 // If we know this is a temporary (used only in this scope):
 char* result = dragon_arena_alloc(len_a + len_b + 1);

 // If this escapes the scope:
 dragon_rc_string* result = dragon_rc_string_new(len_a + len_b + 1);

 // Decision made by CEmitter based on escape analysis
}
```

**Good:**
- Best performance (arenas for hot path, refcount only where needed)
- Correct for all patterns

**Bad:**
- Requires escape analysis in CEmitter
- Most complex to implement
- Escape analysis can be conservative (over-refcounts)

**Rough effort:** 6-8 weeks

---

## Recommended Plan (historical - superseded)

### Phase 1: Stop the Bleeding (Week 1-2) - Arena Allocator

**Goal:** No more unbounded leaks in function-scoped code.

1. **Add arena allocator to runtime** (`CEmitterRuntime.cpp`):
 - `dragon_arena_t` struct: base pointer, current offset, capacity
 - `dragon_arena_push` / `dragon_arena_pop` for nested scopes
 - `dragon_arena_alloc(size)` bump allocator
 - Thread-local arena stack for future threading support

2. **Update CEmitter function emission**:
 - At function entry: `void* _arena_save = dragon_arena_push;`
 - At function exit (before return): copy return value out, then `dragon_arena_pop(_arena_save);`
 - String return values: `strdup` the arena copy into permanent storage

3. **Update string operations**:
 - All `dragon_str_*` functions allocate from arena when one is active
 - Fallback to malloc when no arena (module-level code)

4. **Fix broken clear/pop functions**:
 - `dragon_list_clear`: free backing array elements, then reset
 - `dragon_dict_clear`: free key strings, then reset
 - `dragon_list_pop`: don't leak removed slot
 - `dragon_dict_pop`: free removed key

### Phase 2: Escaped Values (Week 3-4) - Reference Counting for Heap Objects

**Goal:** Global variables, class fields, and returned collections don't leak.

1. **Add refcount header to list/dict/class instances**:
 ```c
 typedef struct {
 int32_t refcount;
 int64_t size;
 int64_t capacity;
 void** data;
 } dragon_list_t;
 ```

2. **CEmitter emits incref/decref**:
 - On variable assignment to a refcounted type: incref new, decref old
 - On scope exit: decref all refcounted locals
 - On function return: caller takes ownership (no extra incref)

3. **Type-aware destructors**:
 - `dragon_list_destroy`: decref each element, free backing array, free struct
 - `dragon_dict_destroy`: free each key, decref each value, free struct
 - `dragon_instance_destroy`: decref each field, free struct

### Phase 3: Compile-Time Selection (Week 5)

**Goal:** User can choose memory strategy.

- `dragon build --mem=arena` (default): fast, good for scripts
- `dragon build --mem=rc`: reference counting, good for servers
- `dragon build --mem=leak`: current behavior, good for benchmarks/debugging

The CEmitter checks `CEmitterOptions::memoryStrategy` and emits different allocation/deallocation code.

### Phase 4: Escape Analysis (Future)

**Goal:** Automatically select arena vs refcount per-variable.

- CEmitter performs simple escape analysis: if a variable is never stored to a global, passed to a function, or returned, it's arena-eligible
- This makes `--mem=arena` correct for most programs without manual annotation

---

## Immediate Quick Fixes (Can Do Now)

Before the full arena/refcount plan, these targeted fixes reduce the worst leaks:

1. **Fix `dragon_list_clear`**: Free backing array elements
2. **Fix `dragon_dict_clear`**: Free key strings and value storage
3. **Add `dragon_str_free`**: Called by CEmitter when a string temporary goes out of scope
4. **Add `dragon_list_free`**: Called by CEmitter at end of function for local lists
5. **Add `dragon_dict_free`**: Same for dicts

This alone would fix ~60% of leaks in typical programs (function-local temporaries).

---

## Test Plan

| Test | Validates |
|------|-----------|
| Compile and run program under Valgrind | Zero leaks for arena-eligible code |
| Long-running loop with string ops | Memory usage stays bounded |
| Function returning a list | Returned list survives arena reset |
| Global variable reassignment | Old value freed, new value retained |
| Nested function calls | Arena nesting works correctly |
| Class instance with list field | Fields freed when instance freed |
| Benchmark: 1M string concats | No OOM, memory stays under 100MB |

---

## Risks

| Risk | Mitigation |
|------|------------|
| Arena breaks returned strings | Copy-on-return (strdup) |
| Refcount cycles | Detect common patterns (parent-child) statically; defer cycle collector |
| Performance regression from refcount | Only refcount escaped values; arena for hot path |
| Thread safety | Thread-local arenas; atomic refcounts |
