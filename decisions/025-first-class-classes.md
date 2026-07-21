# Decision 025: First-Class Class Values

> **Status:** REJECTED - superseded by D021 under commandment #3. The dynamic class-value path (`cls = Dog; obj = cls`, runtime `isinstance`, etc.) has been removed. Classes are compile-time entities again. I keep this ADR as a historical record of why I tried it and dropped it.

Making classes runtime values is the dynamic crutch commandment #3 bars: heap class descriptor, `TAG_TYPE` tag, immortal-object refcounting, and a **second, slower dynamic dispatch path** at every construction/`isinstance` site - all to recover Python's runtime class objects. The compiler already knows the class at every site that matters, so the static path (`call @ClassName_new`) is faster and honest. Speed (#1) wins; Python familiarity (#3) isn't enough.

Factory/registry/DI patterns that motivated this work fine statically: `static def` alternate constructors, explicit typed dispatch tables of `Callable`. Descriptor *struct* survives for static uses (isinstance ancestor walks, `__doc__`, method reflection) but is never a user-facing value.

> Everything below is the original proposal, kept for the record.

Dragon classes today are compile-time ghosts:

| What works | Mechanism | Runtime value? |
|---|---|---|
| `x = MyClass` | Compiler emits `call @MyClass_new` | Instance pointer (i64) |
| `MyClass.method` | Compiler emits `call @MyClass_method` | No class value |
| `isinstance(x, MyClass)` | Compiler constant-folds to 0 or 1 | Literal constant |

You **cannot** do `x = MyClass` - there's no LLVM IR for a bare class reference.

This blocks:
- **Class decorators** (Phase 2): `@dec class Foo` needs to pass "Foo" as a value to `dec`
- **Factory patterns**: `def make(cls): return cls`
- **Runtime isinstance**: `isinstance(x, some_var)` where the class is dynamic
- **Type-parametric code**: registries, dependency injection, serialization dispatch

### Why not just do it?

Ripple effects are nasty. A class descriptor is a heap object, so every subsystem that touches heap objects must learn about it:

| System | Impact |
|---|---|
| Tagged values | New `TAG_TYPE` tag |
| Refcounting | Descriptors must be immortal - cannot be freed while instances exist |
| Scope-exit decref | Must skip or no-op for class descriptors |
| Assignment RC | Wasted atomic ops if immortal (incref/decref on a pinned count) |
| Cycle collector | New traverse/clear case (descriptor → parent, method table) |
| isinstance | Compile-time constant fold → runtime class_id comparison + inheritance walk |
| VarKind system | New `VarKind::Type`, dual dispatch at call sites |
| Call dispatch | Static (today) vs dynamic (through descriptor) paths |

Refcounting alone needs **immortal object support** - mark certain objects never-freed, skip incref/decref. That's Phase 6, deferred at the time.

---

## Options Considered

### Option A: Full first-class classes with class descriptors (chosen, deferred)

Emit a `DragonClassDescriptor` runtime struct per class. Classes become i64-tagged values with `TAG_TYPE`. Descriptors immortal (module-level) or refcounted (function-scoped, rare).

**Pros:** Full Python semantics, enables downstream features.
**Cons:** Touches every subsystem, needs immortal objects first.

### Option B: Compile-time-only class decorators (interim)

For `@dec class Foo`, synthesize a one-off constant struct for the decorator call at module init. No general class values - opaque blob for the decorator, not assignable.

**Pros:** Unblocks Phase 2 without full ripple.
**Cons:** No factory patterns, dynamic isinstance, class-as-argument.

### Option C: Never make classes first-class

Keep classes as compile-time symbols. String-based dispatch for dynamic patterns.

**Pros:** Zero work.
**Cons:** Breaks Python compatibility expectations (we decided that's OK).

---

## Decision

**Option A, deferred until Phase 6 (immortal objects) is complete.**

Option B was considered as interim for . Full implementation was planned as below. Then we rejected the whole thing on .

---

## Design

### Runtime: DragonClassDescriptor

```c
typedef struct {
 DragonObjectHeader header; // refcount=IMMORTAL, type_tag=TAG_TYPE
 int64_t class_id; // existing GC dispatch ID
 const char* name; // "MyClass"
 int64_t parent; // parent descriptor ptr (or 0)
 int64_t constructor; // pointer to ClassName_new
 int64_t num_methods;
 DragonMethodEntry* methods; // name → fn pointer table
 int64_t* ancestor_ids; // precomputed class_id chain for isinstance
 int64_t num_ancestors;
} DragonClassDescriptor;

// New tag
DRAGON_TAG_TYPE = 9
```

### Runtime functions

```c
int64_t dragon_class_descriptor_create(const char* name, int64_t ctor, int64_t class_id, int64_t parent);
int64_t dragon_class_descriptor_call(int64_t descriptor, int64_t* args, int64_t nargs);
int64_t dragon_class_descriptor_get_name(int64_t descriptor);
int64_t dragon_isinstance_runtime(int64_t instance, int64_t descriptor);
```

### CodeGen changes

1. **Class emission**: After struct + `_new`, emit global `@MyClass__descriptor` with name, constructor pointer, class_id, method table.

2. **NameExpr resolution**: Class name in value context (not call target or type annotation) loads descriptor global as i64.

3. **Call dispatch**: Dual path -
 - Known class name → static `call @ClassName_new` (fast path, unchanged)
 - Variable with `VarKind::Type` → `dragon_class_descriptor_call` (dynamic path)

4. **isinstance**: Dual path -
 - Literal class name → compile-time constant fold (unchanged)
 - Variable → `dragon_isinstance_runtime` with ancestor_ids walk

5. **VarKind::Type**: New variant, `isHeapKind` returns false (immortal, no RC).

### GC integration

- Descriptors at module init with `IMMORTAL` refcount (Phase 6 sentinel)
- `dragon_incref` / `dragon_decref` early-return on immortal - zero overhead
- Cycle collector skips immortal objects in traverse
- No scope-exit decref for `VarKind::Type`

---

## Implementation Phases

### Phase 0: Prerequisite - Immortal objects (Phase 6)

Immortal refcount sentinel. incref/decref no-op on immortal. Unblocks descriptors and benefits string literals, module globals, etc.

### Phase 1: Class descriptors - runtime + CodeGen (~400 LOC)

- `DragonClassDescriptor` struct and `TAG_TYPE`
- `dragon_class_descriptor_create` and `_call`
- CodeGen emits descriptor globals per class
- Class names resolve to descriptor loads in value context
- `VarKind::Type` added, skips RC

### Phase 2: Dynamic dispatch (~200 LOC)

- Call sites with `VarKind::Type` use `dragon_class_descriptor_call`
- Method access on class-typed variables uses descriptor method table lookup

### Phase 3: Runtime isinstance (~150 LOC)

- `dragon_isinstance_runtime` with precomputed ancestor_ids
- CodeGen emits runtime path when second arg is not a literal class name

### Phase 4: Integration (~100 LOC)

- `type(x)` returns class descriptor (not string)
- `print` formats descriptors as `<class 'Name'>`
- Class decorators (Phase 2) pass descriptor to decorator function

---

## Why we rejected it

### Positive (were we to have shipped it)
- Unblocks class decorators (Phase 2)
- Factory patterns, registries, DI
- Runtime isinstance with inheritance
- Closer to Python semantics

### Negative (why we killed it)
- Dual dispatch paths in CodeGen forever
- Every future system handles TAG_TYPE
- Descriptor method tables mutable (for decoration), not simple constants
- Occassionally someone would reach for `cls = Foo` and pay the slow path without noticing

### Risks (historical)
- Descriptor emission in ~9500-line CodeGen
- Multi-file: descriptors emitted once, referenced across modules
- Dynamic dispatch slower than static - must preserve fast path
