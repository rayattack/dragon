# Decision 026: Vtable-Based Method Dispatch

> **Status:** Implemented.

Every class instance gets a vtable pointer - one vtable per class (global constant array of function pointers), one pointer per instance, single-dereference dispatch. Same model as C++ and Rust. I need Dragon as fast as those, not Python-style runtime dict lookups.

> **Revision - polymorphic dispatch on statically-typed receivers fixed.**
> The vtable was emitted for the *dynamic* path (`cls = Dog; obj = cls`), but the statically-typed path over-devirtualized: `a: Animal = make_dog; a.speak` wrongly called `Animal.speak`. Fixed in `CallMethods.cpp` - devirtualize only when no subclass overrides; otherwise load vtable at hierarchy-stable ordinal. Covered by `test/dr/test_virtual_dispatch.dr`. I lost an afternoon to that bug before the fix landed.

---

## Options Considered

### Option A: Vtable pointer on every class instance (chosen)

One vtable per class (global constant array of function pointers). One pointer per instance at a fixed struct offset. Method dispatch = load vtable ptr + GEP to method index + indirect call.

**Pros:** Single dereference, same as C++. Enables polymorphic method calls. Zero overhead for static dispatch (compiler can bypass vtable when type is known).
**Cons:** Every class instance grows by 8 bytes (one pointer). Changes struct layout.

### Option B: Descriptor method table

Store method table on the class descriptor. Dispatch = load descriptor from instance, then load method from descriptor.

**Pros:** No instance size increase.
**Cons:** Two dereferences per call. Slower than C++.

### Option C: Python-like __dict__

Runtime dictionary lookup for field/method access.

**Pros:** Maximum flexibility.
**Cons:** Orders of magnitude slower. Violates Dragon's speed mandate.

---

## Decision

**Option A: Vtable pointer on every class instance.**

---

## Design

### Instance Layout (with GC)

Current:
```
{ i64 refcount, i64 type_tag, field0, field1, ... }
 offset 0 offset 1 offset 2+
```

New:
```
{ i64 refcount, i64 type_tag, ptr vtable, field0, field1, ... }
 offset 0 offset 1 offset 2 offset 3+
```

headerOffset changes from 2 to 3. The vtable pointer is at a fixed offset (2) after the GC header, so `(DragonObjectHeader*)obj` still works at offset 0 - no RC penalty.

We put the vtable at offset 2, not 0, because offset 0 would break `(DragonObjectHeader*)obj` casts in all runtime incref/decref functions. That'd add overhead to teh most frequent operation. Offset 2 has identical load speed (both are single-instruction constant-offset loads on x86/ARM) with zero RC penalty.

### Vtable Structure

One global constant per class:
```
@Dog__vtable = internal constant [N x ptr] [
 ptr @Dog_speak,
 ptr @Dog___str__,
 ptr @Dog___eq__,
 ...
]
```

All methods (instance + static + dunders) are included. The vtable is a global constant array - size has no per-call performance impact.

### Method Index Assignment

Each class assigns a fixed integer index to each method. Child classes inherit parent indices and add new ones at the end. This enables polymorphic dispatch: `obj.speak` always uses index 0 regardless of whether obj is Dog or Cat, as long as both inherit from a common base with `speak` at index 0.

### Dispatch Paths

1. **Static dispatch (fast path, unchanged):** Compiler knows the concrete class → direct `call @Dog_speak(self)`. No vtable involved.

2. **Dynamic dispatch (vtable path):** Compiler only knows the variable holds a class instance but not which class → load vtable ptr, GEP to method index, indirect call.

3. **Field access:** Always requires known concrete type at compile time (static GEP). No runtime field lookup. This is the Rust/C++ model.

### Field Access Rules

- `obj = Dog("Rex"); obj.name` - works (compiler knows Dog, static GEP)
- `obj: Dog = cls("Rex"); obj.name` - works (type annotation tells compiler)
- `obj = cls("Rex"); obj.name` - compile error (unknown type, no field access)
- `obj = cls("Rex"); obj.speak` - works (vtable dispatch)

---

## Implementation Phases

### Phase 0: Struct layout change
- headerOffset 2 → 3 for GC-enabled classes
- All field index GEPs shift by 1
- `_new` functions store vtable pointer at offset 2

### Phase 1: Vtable generation
- CodeGen assigns method indices per class
- Emit `@ClassName__vtable` global constant per class
- Inheritance: child vtable starts with parent entries, appends new methods, overrides inherited ones

### Phase 2: Dynamic dispatch
- When calling a method on `VarKind::ClassInstance` with unknown class name, load vtable and indirect-call
- `varClassNames` tracking propagates known types to avoid vtable dispatch when possible

### Phase 3: Integration with
- Dynamic constructor result + vtable dispatch = full first-class class support
- isinstance + vtable dispatch = polymorphic patterns

---

## Costs we live with forever

### Positive
- Single-dereference method dispatch (C++ speed)
- Enables polymorphic patterns through first-class class values
- Static dispatch preserved as fast path - zero overhead for known types

### Negative
- Every class instance grows by 8 bytes
- headerOffset change touches all class field GEPs
- Dual dispatch paths (static vs vtable) - permanent CodeGen complexity

### Risks
- Method index assignment across inheritance hierarchies must be consistent
- Multi-file compilation: vtable layout must be consitent across modules
