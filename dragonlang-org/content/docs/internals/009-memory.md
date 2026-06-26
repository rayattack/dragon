# 009 -- Memory Management

> **Version:** 0.2.0
> **Last Updated:** 2026-06-22

Dragon compiles to native code via LLVM. Memory management uses a
combination of LLVM stack allocation for primitives and runtime library
`malloc`/`free` for heap types. This document describes how Dragon manages
memory across the different value categories.

---

## 1. Overview

Dragon's memory management strategy is straightforward:

- **Primitive types** (`int`, `float`, `bool`) are stack-allocated via LLVM `alloca` instructions
- **Strings** (`str`) are heap-allocated via `malloc` in the runtime library
- **Lists** and **dicts** use runtime-managed structs with `malloc`'d backing arrays
- **Class instances** are heap-allocated structs
- **Exception handling** uses `setjmp`/`longjmp` via the runtime (no LLVM invoke/landingpad)

Heap types are managed by **reference counting** plus a **cycle collector**.
Every heap object (string, list, dict, set, tuple, bytes, class instance) carries
a refcount in its object header; `dragon_incref` / `dragon_decref` adjust it, and
the last `decref` frees the object. The cycle collector exists only to reclaim
*reference cycles* (which a pure refcount cannot); acyclic objects are reclaimed
the instant their count hits zero, not at some later sweep. Heap locals are freed
deterministically at the end of the block that owns them (scope-bound lifetimes,
not GC pauses), and containers are **monomorphized** - a `list[int]` stores native
`i64`, a `dict[int, V]` hashes native `i64` keys, never a boxed value - so the hot
path stays allocation-free. The sections below describe the value categories; the
ownership rules that keep the refcount balanced are in
[§3.1](#31-ownership-owned-borrowed-transferred) and
[§4.4](#44-key-and-value-ownership-in-containers).

---

## 2. Stack Allocation (Primitives)

The CodeGen uses LLVM `alloca` instructions for local variables of primitive type:

```llvm
%x = alloca i64          ; int variable
%y = alloca double       ; float variable
%z = alloca i64          ; bool variable (stored as i64)
```

These allocations are automatically freed when the function returns (standard
stack discipline). LLVM's `mem2reg` pass can promote many of these to SSA
registers, eliminating the actual stack allocation.

### How It Works in CodeGen

When the CodeGen encounters a variable assignment like `x: int = 42`:

1. An `alloca` instruction is emitted in the function's entry block
2. A `store` instruction writes the value to the alloca'd memory
3. Subsequent reads use `load` instructions
4. The variable is tracked in a `namedValues` map for later lookup

---

## 3. Heap Allocation (Strings)

All string operations in Dragon produce new strings allocated via `malloc`
in the runtime library. The runtime functions (`dragon_str_concat`,
`dragon_str_upper`, `dragon_str_replace`, etc.) each allocate a result
buffer with `malloc`, fill it, and return the pointer.

```cpp
// From lib/Runtime/runtime.cpp
const char* dragon_str_concat(const char* a, const char* b) {
    size_t la = strlen(a), lb = strlen(b);
    char* r = (char*)malloc(la + lb + 1);
    memcpy(r, a, la);
    memcpy(r + la, b, lb + 1);
    return r;
}
```

String literals are emitted as LLVM global constants using
`CreateGlobalString`, which places them in the read-only data section.

String literals are emitted as read-only data and carry **no** object header;
they are immortal and must never be passed to a refcount routine, so the decref
entry points guard on a heap/immortal check before touching the header.

### 3.1 Ownership: owned, borrowed, transferred

Refcount correctness comes from one discipline applied at every site that moves a
heap value. A value reaches an expression in one of three states:

- **Owned temporary** - a freshly produced `+1` (a concat result, a function
  return, a literal `[...]`). Whoever holds it is responsible for one `decref`.
- **Borrowed** - a read through a name, field, or element (`x`, `obj.f`, `xs[i]`).
  It belongs to something else; you must **not** decref it, and if you intend to
  keep it past the owner's lifetime you must `incref` first.
- **Transferred (adopted)** - a store that takes ownership of a `+1` (appending to
  a list, storing into a dict). The callee adopts the reference; the caller must
  not also free it.

CodeGen drains owned temporaries at the **end of the statement / call** that
produced them (e.g. `f(a + b)` frees the `a + b` concat once the call returns),
which is why intermediate results no longer leak. The earlier "strings leak until
exit" limitation is gone. The two places this discipline is subtlest - and where
it was hardened most recently - are container reads and container inserts, covered
in [§4.4](#44-key-and-value-ownership-in-containers).

---

## 4. List and Dict Allocation

Lists and dicts are the most complex heap types. Both use a two-part
allocation strategy managed by the runtime:

### 4.1 List Structure

Every heap object begins with a 16-byte refcount header
(`DragonObjectHeader`: `int64_t refcount`, `uint8_t type_tag`,
`uint8_t gc_flags`, `uint16_t class_id`, `int32_t gc_track_idx`), and the list
is no exception:

```cpp
// From lib/Runtime/runtime_internal.h
struct DragonList {
    DragonObjectHeader header;   // 16-byte refcount header (offset 0)
    void*    data;               // bytes; stride = elem_size
    int64_t  size;               // # of elements
    int64_t  capacity;           // capacity in elements
    uint8_t  elem_tag;           // TAG_INT, TAG_STR, TAG_BOOL, ...
    uint8_t  elem_size;          // 1 (packed bool) or 8 (int/float/ptr/etc.)
};
```

- The `DragonList` struct itself is heap-allocated via `malloc`
- The `data` backing array is separately heap-allocated; indexing stride is
  `elem_size` (1 byte for packed `list[bool]`, 8 bytes otherwise), so callers go
  through `dragon_list_load` / `dragon_list_store` rather than touching `data`
  directly
- Growth uses `realloc` to double capacity when full

`DragonList` is the **I64 variant** (int and bool elements) of a
**monomorphized family** (spec-30 Phase 3). The other variants share an identical
field order, offsets, and total size - only the static type of `data` differs:

| Element type | Variant | `data` storage |
|---|---|---|
| `list[int]`, `list[bool]` | `DragonList` (I64; bool packs to 1B) | `void*` (i64 slots, or 1B for bool) |
| `list[float]` | `DragonListF64` | `double*` |
| `list[str]`, `list[<container>]`, `list[<class>]`, `list[bytes]`, ... | `DragonListPtr` | `void**` |
| `list[Any]`, untyped list | `DragonListBox` | `{i64 tag, i64 payload}` elems (16B/elem) |

Codegen picks the variant from the static element type at allocation. Because the
layouts coincide, polymorphic ops (destroy, `len`, GC traverse) cast a
`DragonList*` and read header / size / capacity / `elem_tag` at the same offsets,
while per-element access uses the typed `data` pointer in each per-type op.

### 4.2 Dict Structure

The dict is a compact, open-addressed, CPython-style design: a dense
insertion-ordered entry array plus a sparse hash-to-index table. It does **not**
use parallel `keys[]` / `values[]` arrays.

```cpp
// From lib/Runtime/runtime_internal.h
struct DictEntry {
    uint64_t    hash;
    const char* key;     // int key is an i64 cast to ptr when keys_are_ptr == 0
    int64_t     value;
    int8_t      tag;     // value's DragonValueTag
    int8_t      dead;    // lazy-delete tombstone (entry skipped, reclaimed by compact)
};

struct DragonDict {
    DragonObjectHeader header;     // 16-byte refcount header
    DictEntry* entries;            // dense, insertion-ordered (incl. dead slots)
    int64_t*   indices;            // sparse hash->entry-index table; -1 empty, -2 tombstone
    int64_t    size;               // dense high-water mark, INCLUDING dead slots
    int64_t    used;               // live entry count - what len() returns
    int64_t    capacity;           // entries capacity
    int64_t    index_size;         // size of indices table (power of 2)
    uint8_t    keys_are_ptr;       // 0 = int-keyed; 1 = str-keyed (owns 1 ref/key)
};
```

- The `DragonDict` struct, the `entries` array, and the `indices` table are each
  heap-allocated; growth `realloc`s and rehashes the index table
- Lookup is O(1): hash the key, probe `indices` to find the dense entry index,
  then read that `DictEntry`. `-1` marks an empty index slot, `-2` a tombstone
- Deletes mark an entry `dead` in place (no array shift), keeping delete O(1);
  `dict_compact` reclaims dead slots and resets `size` to `used`
- When `keys_are_ptr == 1` the dict owns exactly one reference per string key and
  releases it on removal or destroy; when `0`, `key` is an i64 cast to a pointer
  and is never decref'd (see [§4.4](#44-key-and-value-ownership-in-containers))

### 4.3 Creation Pattern

The runtime `dragon_list_new` and `dragon_dict_new` functions handle all
allocation, initializing the refcount header via `dragon_obj_init` (which sets
`refcount = 1`, the type tag, and `GC_FLAG_HEAP_OBJ`):

```cpp
DragonList* dragon_list_new_tagged(int64_t cap, int64_t elem_tag) {
    DragonList* l = (DragonList*)malloc(sizeof(DragonList));
    dragon_obj_init(&l->header, DRAGON_TAG_LIST);   // refcount = 1, tag, heap flag
    l->capacity = cap > 0 ? cap : 8;
    l->size = 0;
    l->elem_tag = (uint8_t)elem_tag;
    l->elem_size = dragon_list_size_for_tag(l->elem_tag);  // 1 for bool, else 8
    l->data = malloc((size_t)(l->capacity * l->elem_size));
    return l;
}
```

The CodeGen emits calls to these runtime functions when encountering
list/dict literals or comprehensions.

### 4.4 Key and value ownership in containers

A dict holds exactly one reference to each key and each value. Keeping that
balanced across reads, inserts, and `setdefault` is the most error-prone corner of
the refcount model, so the rules are spelled out here.

**Reads return owned values for heap value types.** The stored value is a borrow -
it belongs to the dict. A typed binding takes ownership:

```dragon
g: list[int] = d.get(k)   # g is an owner; it decrefs at scope exit
```

If `get` handed back the borrowed pointer as-is, that scope-exit decref would free
the dict's own value while the dict still pointed at it - a use-after-free on the
next read. So for a heap value type the getter **increfs on return**: the binding
owns its own reference and the dict keeps its own. Scalar value types (`int`,
`float`, `bool`) carry no reference and return by value with no incref. The same
owned-return rule covers `d.get(k, default)`, whose default temporary is drained
after the call.

**Inserts adopt one reference.** `d[k] = v` and the insert branch of `setdefault`
adopt a `+1` of the value: the store itself does not incref, so the codegen hands
over an owned reference - a borrowed source is incref'd first, an owned temporary
passes its `+1` straight through. The **key** follows the identical contract:
adopted on insert, and released on an update or a `setdefault`-present case because
the dict keeps the key it already stored. A string-literal key is immortal rodata,
so it is stored directly and the release is a guarded no-op - no allocation, ever.
A *borrowed* key (a variable, a field) is incref'd before the insert, or the dict's
stored key would dangle the moment its source leaves scope.

**`setdefault`, end to end.** Present: incref and return the existing value, and
release the redundant incoming key. Absent: store the key and value (adopting
both), and return the value with one extra reference so the dict and the caller
each own one. This matches Python exactly. An absent `setdefault` stores *and
returns the same value object*, so `d.setdefault(k, []).append(x)` mutates the
stored list; and the dict stores the *key object you passed*, observable with `is`:

```dragon
original: str = "user_" + str(42)
d.setdefault(original, 7)
# the stored key IS that object, exactly as in CPython:
for k in d.keys() {
    print(k is original)   # True  (shared, not copied)
}
```

Because keys are matched by value (not identity) and strings are immutable, sharing
the key object via `incref` rather than copying it is both the fastest choice and
the Python-faithful one - a `dup` would be observably non-Python (the `is` check
above would print `False`) for no benefit.

---

## 5. Class Instance Allocation

Class instances are represented as LLVM struct types with heap allocation:

1. **Struct definition**: Each class gets an LLVM struct type with fields
   for all instance attributes
2. **Constructor**: The `self()` constructor (or `__init__`) allocates
   the struct via `malloc` and initializes fields
3. **Method dispatch**: Methods receive a pointer to the struct as the
   first argument (implicit `self` in `.dr` mode)

### Static Fields

Static fields are represented as LLVM global variables, not instance
fields. They are shared across all instances and initialized at module
load time.

---

## 6. LLVM IR Patterns

### 6.1 Variable Access

```llvm
; Assignment: x = 42
%x = alloca i64
store i64 42, i64* %x

; Read: use x
%val = load i64, i64* %x
```

### 6.2 Runtime Function Calls

```llvm
; print(x) where x is int
%x_val = load i64, i64* %x
call void @dragon_print_int(i64 %x_val)

; s = "hello" + "world"
%result = call i8* @dragon_str_concat(i8* %hello, i8* %world)
store i8* %result, i8** %s
```

### 6.3 List Operations

Post-spec-30 every heap type lowers to an opaque `ptr` (there is no named
`%dragon_list_t*` in the IR); the runtime knows the concrete `DragonList` layout
behind the pointer.

```llvm
; nums = [1, 2, 3]
%list = call ptr @dragon_list_new(i64 3)
call void @dragon_list_append(ptr %list, i64 1)
call void @dragon_list_append(ptr %list, i64 2)
call void @dragon_list_append(ptr %list, i64 3)
```

---

## 7. Exception Handling and Memory

Dragon's exception handling uses `setjmp`/`longjmp` from the runtime,
not LLVM's native `invoke`/`landingpad` mechanism. This means:

- `try` blocks save the execution context with `setjmp`
- `raise` restores context with `longjmp`
- the unwind path runs the registered cleanups for scope-tracked heap locals as it
  returns to the handler, so values owned by the unwound frames are released rather
  than leaked

This is a deliberate simplicity trade-off. The `setjmp`/`longjmp` approach is much
simpler to implement in the CodeGen than LLVM's full exception machinery, while the
scope-cleanup stack keeps the refcount balanced across the non-local jump.

---

## 8. Characteristics and trade-offs

1. **Deterministic, not GC-paused**: heap objects are freed the instant their
   refcount reaches zero, and block-scoped locals are freed at block exit - no
   stop-the-world pauses. The cost is the refcount traffic itself, which the
   ownership discipline ([§3.1](#31-ownership-owned-borrowed-transferred)) keeps to
   the minimum each operation requires.

2. **Cycles need the collector**: a pure refcount cannot reclaim a reference cycle
   (a list that contains itself, two instances that point at each other). The cycle
   collector exists solely for that case; acyclic data never waits on it.

3. **Return shares, it does not deep-copy**: returning a list or dict returns the
   pointer, so nested structures are shared, not cloned - intentional, and the same
   aliasing model as Python. Refcounts make the sharing safe; use an explicit
   `copy` when you want independence.

4. **Base containers are not thread-safe**: the plain `list`/`dict`/`set` runtime
   structures assume single-threaded access. To share across threads use the
   synchronized variants and primitives in
   [Synchronization](/docs/1104-synchronization); a value shared with another thread
   is also marked so its refcount updates become atomic.

---

## Previous Document

[008 - Dragon Runtime Library](008-runtime.md)

## Next Document

[010 - CLI Driver and Build Pipeline](010-driver.md)
