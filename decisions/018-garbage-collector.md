# Decision 018: Reference Counting Memory Management

> **Status:** Implemented (reference counting shipped; hardened by 020). Supersedes Decision 003.

Right now the runtime leaks everything - 53+ malloc sites, basically zero matching frees for user-facing objects. I wanted arena-per-function first (003's plan), but after a long debug session watching `[1,2,3]` escape a function scope and dangle, arenas and Python-style reference semantics don't mix. I went with **reference counting** (CPython's model) plus a cycle collector for containers later. People occassionally ask why not Rust ownership; we're typed Python, not Rust.

## Why 's Arena-First Approach Is Wrong

 was written for the C emitter backend and proposed arena-per-function
as Phase 1. After implementing LLVM codegen, green threads, and
the self-hosting stdlib effort, three problems are clear:

### 1. Python Objects Escape Scopes Constantly

```dragon
def make_list -> list[int] {
 result: list[int] = [1, 2, 3] # allocated in make_list's arena
 return result # escapes! arena freed → dangling pointer
}

names: list[str] = []
for name in get_names {
 names.append(name.upper) # .upper string escapes loop scope
}
```

Arena-per-scope requires copying every escaped value (`strdup` into parent arena). This breaks identity semantics (`a is b` fails after copy), adds hidden cost to every return, and makes collections that accumulate values across scopes expensive.

### 2. Threading Complicates Arena Ownership

With `fire`, `thread {}`, and green threads, objects are shared across execution contexts. Which arena owns a value passed between threads? Arena-per-thread means values can't be safely shared. Arena-per-scope means the creator thread can't exit its scope while another thread holds a reference.

### 3. Context Managers Need Deterministic Destruction

Dragon has `__enter__`/`__exit__` . File handles, locks, and
database connections must be freed promptly when the last reference dies - not
"when the arena eventually resets."

## Alternatives Considered

| Strategy | Deterministic? | Handles Escapes? | Thread-Safe? | Python Parity? | Complexity |
|----------|---------------|------------------|--------------|----------------|------------|
| **Arena-per-scope** | Yes (bulk) | No - requires copy-on-escape | Hard | No | Low |
| **Rust borrowing** | Yes | Yes | Yes | No - destroys Python UX | Very High |
| **Tracing GC** | No | Yes | Hard (stop-the-world) | No - non-deterministic | High |
| **Boehm GC** | No | Yes | Partial | No - conservative scanning | Low |
| **Reference Counting** | Yes | Yes | Yes (atomic) | Yes - CPython model | Medium |

### Why Not Rust-Style Ownership?

Dragon's value proposition is "typed Python that compiles." Forcing users to reason
about `&mut`, lifetimes, and move semantics destroys that. Not viable.

### Why Not Tracing GC?

- Stop-the-world pauses conflict with green thread scheduling
- Dragon's tagged values (i64 that might be pointers) require conservative scanning
- Non-deterministic finalization breaks context managers
- Stack scanning with LLVM-generated code is complex and fragile

### Why Not Boehm GC?

- External dependency (`libgc-dev`) complicates distribution
- Conservative scanning retains false positives (Dragon stores pointers as i64)
- Non-deterministic - same context manager problem as tracing GC
- Cannot distinguish pointer-tagged vs integer-tagged values in dicts

## Design: Reference Counting

### Object Header

Every heap-allocated Dragon object gets a common header:

```
┌─────────────────────────────────────────────┐
│ DragonObject (prepended to all heap types) │
│ │
│ int64_t refcount; // 8 bytes │
│ uint8_t type_tag; // TAG_STR, etc. │
│ uint8_t gc_flags; // cycle collector │
│ uint8_t _pad[6]; // alignment │
│ │
│ Total overhead: 16 bytes per object │
└─────────────────────────────────────────────┘
```

All runtime structs embed this header as their first field:

```c
typedef struct {
 int64_t refcount;
 uint8_t type_tag;
 uint8_t gc_flags;
 uint8_t _pad[6];
} DragonObjectHeader;

typedef struct {
 DragonObjectHeader header; // must be first
 int64_t size;
 int64_t capacity;
 int64_t* data;
} DragonList;

typedef struct {
 DragonObjectHeader header; // must be first
 int64_t len;
 char data[]; // flexible array member
} DragonString;
```

### Core Operations

```c
static inline void dragon_incref(void* obj) {
 if (obj) {
 ((DragonObjectHeader*)obj)->refcount++;
 }
}

static inline void dragon_decref(void* obj) {
 if (obj) {
 DragonObjectHeader* hdr = (DragonObjectHeader*)obj;
 if (-hdr->refcount == 0) {
 dragon_dealloc(obj);
 }
 }
}

// Atomic variants for cross-thread references
static inline void dragon_incref_atomic(void* obj) {
 if (obj) {
 __atomic_fetch_add(&((DragonObjectHeader*)obj)->refcount, 1,
 __ATOMIC_RELAXED);
 }
}

static inline void dragon_decref_atomic(void* obj) {
 if (obj) {
 DragonObjectHeader* hdr = (DragonObjectHeader*)obj;
 if (__atomic_sub_fetch(&hdr->refcount, 1, __ATOMIC_ACQ_REL) == 0) {
 dragon_dealloc(obj);
 }
 }
}
```

### Type-Specific Deallocation

```c
void dragon_dealloc(void* obj) {
 DragonObjectHeader* hdr = (DragonObjectHeader*)obj;
 switch (hdr->type_tag) {
 case TAG_STR:
 free(obj); // string data is inline (flexible array member)
 break;

 case TAG_LIST: {
 DragonList* list = (DragonList*)obj;
 // Decref each element that is itself a heap object
 for (int64_t i = 0; i < list->size; i++) {
 // Elements need their own tag to know if they're heap ptrs
 // For typed lists (list[str]), codegen knows the element type
 }
 free(list->data);
 free(list);
 break;
 }

 case TAG_DICT: {
 DragonDict* dict = (DragonDict*)obj;
 for (int64_t i = 0; i < dict->size; i++) {
 dragon_decref((void*)(intptr_t)dict->entries[i].key);
 if (dict->entries[i].tag == TAG_STR ||
 dict->entries[i].tag == TAG_LIST ||
 dict->entries[i].tag == TAG_DICT ||
 dict->entries[i].tag == TAG_BYTES) {
 dragon_decref((void*)(intptr_t)dict->entries[i].value);
 }
 }
 free(dict->entries);
 free(dict->indices);
 free(dict);
 break;
 }

 case TAG_BYTES: {
 DragonBytes* bytes = (DragonBytes*)obj;
 free(bytes->data);
 free(bytes);
 break;
 }

 default:
 // Class instances: codegen emits per-class destructors
 // that decref each field, then free the struct
 free(obj);
 break;
 }
}
```

### CodeGen Emission Rules

CodeGen emits incref/decref at well-defined points:

| Event | Action |
|-------|--------|
| **Object creation** | Set `refcount = 1` (no incref needed) |
| **Variable assignment** `x = expr` | `incref(new); decref(old); x = new` |
| **Function argument** | `incref` each arg at call site |
| **Function return** | `incref` return value; `decref` all locals |
| **Scope exit** (loop, if, block) | `decref` locals declared in that scope |
| **Collection insert** `list.append(x)` | `incref(x)` |
| **Collection remove** `list.pop` | `decref` removed element |
| **Collection overwrite** `list[i] = x` | `incref(new); decref(old)` |
| **String concat** `a + b` | Result has refcount 1; temporaries decref'd |

### Tagged Values in Dicts

The existing tag system already distinguishes heap pointers from scalars:

```
TAG_INT = 0 → no incref/decref (scalar)
TAG_STR = 1 → incref/decref (heap pointer)
TAG_FLOAT = 2 → no incref/decref (scalar, bit pattern in i64)
TAG_BOOL = 3 → no incref/decref (scalar)
TAG_NONE = 4 → no incref/decref (singleton)
TAG_LIST = 5 → incref/decref (heap pointer)
TAG_DICT = 6 → incref/decref (heap pointer)
TAG_BYTES = 7 → incref/decref (heap pointer)
```

Dict operations check the tag before incref/decref:

```c
static inline bool tag_is_heap(int8_t tag) {
 return tag == TAG_STR || tag == TAG_LIST || tag == TAG_DICT || tag == TAG_BYTES;
}

void dragon_dict_set(DragonDict* d, const char* key, int64_t val, int8_t tag) {
 // ... lookup/insert logic ...
 if (existing) {
 if (tag_is_heap(existing->tag))
 dragon_decref((void*)(intptr_t)existing->value);
 }
 if (tag_is_heap(tag))
 dragon_incref((void*)(intptr_t)val);
 // ... store ...
}
```

### String Representation Change

Current strings are raw `const char*` scattered across malloc'd buffers. With
refcounting, strings become proper DragonString objects with inline data:

```c
typedef struct {
 DragonObjectHeader header;
 int64_t len;
 char data[]; // flexible array member - string data follows header
} DragonString;

DragonString* dragon_string_new(const char* src, int64_t len) {
 DragonString* s = (DragonString*)malloc(sizeof(DragonString) + len + 1);
 s->header.refcount = 1;
 s->header.type_tag = TAG_STR;
 s->header.gc_flags = 0;
 s->len = len;
 memcpy(s->data, src, len);
 s->data[len] = '\0';
 return s;
}
```

This is a single allocation (header + data together), cache-friendly, and
eliminates the separate `strlen` calls scattered throughout the runtime.

### Threading Model

| Context | Refcount Type | Rationale |
|---------|---------------|-----------|
| Single-threaded code | Non-atomic `++`/`--` | No contention, fastest |
| Green threads on same OS thread | Non-atomic `++`/`--` | Cooperative scheduling, no preemption |
| Cross-thread sharing (`fire`, `thread {}`) | Atomic `__atomic_fetch_add/sub` | Multiple OS threads |
| SyncList/SyncDict | Atomic + mutex | Already mutex-protected |

CodeGen can default to non-atomic and upgrade to atomic only for values that cross thread boundaries (conservative: use atomic for anything touched by `fire` or `thread {}`).

### Cycle Collection (Phase 5)

Pure refcounting cannot collect cycles:

```dragon
class Node {
 next: Node = None
}
a: Node = Node
b: Node = Node
a.next = b # a.refcount=1, b.refcount=2
b.next = a # a.refcount=2, b.refcount=2
a = None # a.refcount=1 (b still references it) - leaked
b = None # b.refcount=1 (a still references it) - leaked
```

CPython solves this with a supplementary cycle collector that only scans
**container types** - objects that can hold references to other objects:

- Lists, dicts, sets, tuples - can form cycles via nested references - Class instances - can form cycles via fields - Strings, bytes, ints, floats - **cannot** form cycles, exempt from scanning

The `gc_flags` byte in the object header supports this:

```c
#define GC_FLAG_TRACKED 0x01 // Object is in the cycle collector's list
#define GC_FLAG_REACHABLE 0x02 // Marked during cycle collection
```

Container objects are added to a tracked list on creation. The cycle collector runs periodically (or on demand via `gc.collect`) and uses CPython's algorithm:

1. For each tracked container, subtract internal references from refcounts 2. Objects with adjusted refcount > 0 are reachable (external refs exist)
3. Mark reachable objects and everything they reference 4. Remaining objects with adjusted refcount 0 are garbage - free them

This is well-documented in CPython's `Modules/gcmodule.c` and translates directly.

## Implementation Phases

### Phase 0: Object Header Foundation

**Goal:** Add `DragonObjectHeader` to all runtime types without changing behavior.

**Outputs:**
- Add `DragonObjectHeader` struct to runtime
- Embed header as first field in `DragonList`, `DragonDict`, `DragonTuple`,
 `DragonSet`, `DragonBytes`
- Initialize `refcount = 1`, `type_tag` appropriately, `gc_flags = 0` in all
 `_new` functions
- All existing code continues to work - header is present but ignored
- Introduce `DragonString` struct (header + len + inline data)
- Add `dragon_string_new` alongside existing `const char*` functions
- Update CodeGen struct size calculations to account for headers

**Risk:** Struct layout changes may break existing GEP offsets in CodeGen.
Mitigation: Update all `getStructField` offsets systematically.

### Phase 1: String Refcounting

**Goal:** Eliminate string leaks - the highest-volume leak category (~35 sites).

**Outputs:**
- Migrate all string operations to return `DragonString*` instead of `const char*`
- `dragon_str_concat` → allocates `DragonString` with refcount 1
- `dragon_str_upper`, `_lower`, `_strip`, `_replace`, etc. → same
- `dragon_int_to_str`, `_float_to_str`, `_bool_to_str` → return `DragonString*`
- CodeGen emits `dragon_decref` for string locals at scope exit
- CodeGen emits `dragon_incref` on string assignment
- String literals: statically allocated with `refcount = INT64_MAX` (immortal)
- Runtime helper: `dragon_string_cstr(DragonString*)` for C interop (returns
 `&s->data[0]`)
- Update `dragon_print` and all runtime consumers to accept `DragonString*`

**Validation:** Run string-heavy test under Valgrind - zero string leaks.

### Phase 2: Collection Refcounting

**Goal:** Lists, dicts, tuples, sets, and bytes are refcounted.

**Outputs:**
- `dragon_list_new` returns list with refcount 1
- `dragon_list_append` increfs the appended element (if heap type)
- `dragon_list_pop` decrefs the removed element
- `dragon_list_setitem` decrefs old, increfs new
- Same pattern for dict, tuple, set, bytes
- `dragon_dealloc` dispatcher: frees backing storage and decrefs contained
 elements recursively
- CodeGen emits decref for collection locals at scope exit
- `dragon_list_free`, `dragon_dict_free`, etc. are now just `dragon_decref`

**Validation:** Collection-heavy loops show bounded memory under Valgrind.

### Phase 3: Class Instance Refcounting

**Goal:** User-defined class instances are refcounted with field cleanup.

**Outputs:**
- CodeGen emits a `ClassName__dealloc(self)` function per class that decrefs
 each field that is a heap type, then frees the struct
- `dragon_dealloc` routes class instances to their generated destructor via
 a function pointer stored in the object header (or a vtable lookup)
- Assignment to class fields emits incref new / decref old
- Destructor chaining: if class B extends A, B's dealloc calls A's dealloc
 for inherited fields
- `__del__` dunder: if defined, called before field cleanup (best-effort,
 no resurrection)

**Validation:** Class instance creation/destruction in loops shows bounded memory.

### Phase 4: Thread-Safe Refcounting

**Goal:** Objects shared across threads use atomic operations.

**Outputs:**
- `dragon_incref_atomic` / `dragon_decref_atomic` using `__atomic` builtins
- CodeGen tracks whether a value crosses thread boundaries:
 - Values captured by `fire fn(args)` → atomic refcount on args
 - Values in `thread { block }` that reference outer scope → atomic
 - SyncList/SyncDict contents → atomic
- Default: non-atomic (faster for single-threaded and green-thread code)
- Conservative fallback flag: `--atomic-rc` forces all refcounting to atomic

**Validation:** Multi-threaded stress test with shared objects - no crashes, no leaks.

### Phase 5: Cycle Collector

**Goal:** Circular references between container types are collected.

**Outputs:**
- Tracked object list: doubly-linked list of all container objects

- `gc_flags` management: set `GC_FLAG_TRACKED` on container creation
- Cycle detection algorithm (CPython's trial deletion):
 1. Copy refcounts for all tracked objects
 2. For each tracked object, subtract refs from other tracked objects
 3. Objects with adjusted refcount > 0 → reachable (mark + propagate)
 4. Remaining objects → unreachable → dealloc
- Generational optimization (optional): young/old generations, only scan young
 objects frequently
- `gc.collect` stdlib function for manual triggering
- Automatic trigger: run after N allocations (configurable threshold)

**Validation:** Circular reference test (linked list cycle, tree with parent
pointers) shows memory reclaimed.

### Phase 6: Optimization

**Goal:** Reduce refcounting overhead for hot paths.

**Outputs:**
- **Immortal objects:** String literals, small integers, `True`/`False`/`None`
 get `refcount = INT64_MAX` - incref/decref are no-ops
- **Borrowed references:** Function parameters can use borrowed refs (no
 incref on call, no decref on return) when the caller guarantees the object
 lives long enough - determined by simple escape analysis
- **Elision:** If a value is created and consumed in the same expression
 (`print(str(x))`), skip the incref/decref pair entirely
- **Move semantics:** Last use of a local variable transfers ownership
 instead of incref+decref (`return x` at end of function → no incref needed,
 just skip the decref)

**Validation:** Benchmark suite shows overhead < 15% vs current leak-everything
baseline.

## Migration Path

The transition from leak-everything to refcounted must not break existing code:

1. **Phase 0** is purely additive - header exists but is unused
2. **Phases 1-3** can be adopted per-type: strings first, then collections, then
 classes. Mixed mode works because un-migrated types simply have refcount stuck
 at 1 (never freed, same as today)
3. **Phase 4** is additive - atomic variants coexist with non-atomic
4. **Phase 5** is independent - cycle collector runs alongside refcounting
5. `--gc=none` flag preserves current behavior for benchmarking

## Risks

| Risk | Severity | Mitigation |
|------|----------|------------|
| Struct layout changes break CodeGen GEP offsets | High | Systematic offset audit in Phase 0; regression tests |
| String representation change breaks C interop | High | `dragon_string_cstr` accessor; `extern "C"` functions accept both during migration |
| Double-free from incorrect decref placement | High | Valgrind + ASan in CI; conservative over-incref initially |
| Performance regression from incref/decref traffic | Medium | Phase 6 optimizations; benchmark continuously |
| Cycle collector pauses affect green thread latency | Low | Incremental collection; small generation sizes |
| Atomic refcount overhead on single-threaded code | Low | Default to non-atomic; atomic only for cross-thread values |

## References

- CPython refcounting: https://docs.python.org/3/c-api/refcounting.html
- CPython cycle collector: https://devguide.python.org/internals/garbage-collector/
- Swift ARC (similar approach): https://docs.swift.org/swift-book/documentation/the-swift-programming-language/automaticreferencecounting
- Bob Nystrom on GC strategies: https://journal.stuffwithstuff.com/2013/12/08/babys-first-garbage-collector/
