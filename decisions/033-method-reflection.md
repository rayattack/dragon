# Decision 033: Class Method Reflection

**Status:** Approved (implemented. Phase 0 deep `Any` equality
 (`dragon_list_eq`/`dragon_dict_eq` recursion in `dragon_box_eq`) plus phases
 1-4: descriptor method tables, `dir`, method-aware `getattr`/`hasattr` with
 bound-closure thunks, and `stdlib/unittest.dr` shipped end-to-end. See
 `test/InteropTest.cpp` `StdlibUnittestEndToEnd`)

**Builds on:** D026 (vtables already emit method order + function pointers per class)

Field reflection worked - `hasattr(obj, "field")` walks `__field_names`/`__field_offsets`. Methods didn't, which meant I couldn't dogfood `unittest`: no way to auto-discover `test_*` methods on a subclass without a C++ workaround. Per dogfooding policy that's a language fix, not a runtime hack.

## Summary

I need to look up class methods by name at runtime, the same way fields already work.

Right now `hasattr(obj, "field")` and `getattr(obj, "field")` walk a per-class
`__field_names` / `__field_offsets` table emitted at codegen time
(`src/CodeGen.cpp:308`, `runtime_builtins.cpp:782`). Methods should follow the
same pattern:

- `dir(obj)` / `dir(MyClass)` returns method names.
- `hasattr(obj, "test_foo")` is true when `test_foo` is a method.
- `getattr(obj, "test_foo")` returns a callable bound method (closure over
 `(self, *args)`).
- `getattr(obj, "test_foo")` just works.

The immediate reason is **dogfooding `unittest`**. A `TestCase` runner that
auto-discovers `test_*` methods on a subclass cannot be written in pure Dragon
today. Per dogfooding policy (zen.md), that's a language fix, not a C++
workaround.

---

## Context / Motivation

Field reflection is mostly done. Methods aren't.

| Capability | Status | Mechanism |
|---|---|---|
| Field names enumerable at runtime | done | `@ClassName__field_names` global, walked in `dragon_hasattr`/`dragon_getattr` |
| Field offsets enumerable | done | `@ClassName__field_offsets` parallel array |
| `getattr(obj, "x")` for fields | done | `runtime_builtins.cpp:787` |
| Method order known at compile time | done | `classVtableMethodOrder` (`src/codegen/ImplInit.cpp:1225`) |
| Vtable function pointers emitted per class | done | `@ClassName__vtable` global (`src/codegen/Classes.cpp:707`) |
| Method **names** in runtime descriptor | no | Never emitted |
| `getattr(obj, "method")` returns callable | no | `_find_field_offset` returns -1, raises AttributeError |
| `dir(obj)` builtin | no | Doesn't exist |

 already computed method order and fn pointers per class. I just never
published the names.

### Concrete blocker (`unittest`)

What compiles today:

```dragon
# Manual registration - the only thing that compiles
@test_case
def test_addition { ... }
@test_case
def test_subtraction { ... }
```

What I want (Python parity, priority #2):

```dragon
class CalculatorTests(TestCase) {
 def test_addition(self) { ... }
 def test_subtraction(self) { ... }
}
# runner: walks dir(cls), filters startswith("test_"), calls each
```

That needs method enumeration plus bound-method `getattr`. Both sit on
vtables.

### Why a workaround is wrong

Two hacks were considered:

1. **Compile-time discovery**: special-case `TestCase` subclasses, emit a
 synthesized `__tests__: list[Callable]` field. Rejected - bakes the test
 framework into the compiler.
2. **String-based dispatch table per user class**: `@register("test_foo")` on
 every test method. Rejected - not Python parity, and we already have the
 dispatch table (the vtable).

The fix is to extend the descriptor with method metadata we already know at
codegen time. Same shape as field reflection.

---

## Options Considered

### Option A: Method-name table on the descriptor (chosen)

Extend `DragonClassDescriptor` with `method_names` + `method_fn_ptrs` +
`num_methods`, populated at module init from the existing vtable order.
Add `dragon_dir`, extend `dragon_getattr` to return a bound method when the
name matches a method.

**Pros:** Same pattern as field reflection. Reuses vtable data, no
duplication. Bound methods are closures over `self`. Pythonic:
`obj.test_foo` and `getattr(obj, "test_foo")` give the same callable.

**Cons:** Need a runtime "bound method" object, but we already have one (
closures). `dragon_getattr` sometimes returns a `ptr` (closure) and sometimes
an `i64` (field). Boxed-`Any` return convention from Phase 4 handles that.

### Option B: Compile-time-only `dir`

Emit the method-name list as a per-class string literal global; `dir(MyClass)`
constant-folds to that list. No runtime API.

**Pros:** Half a day's work.
**Cons:** Doesn't unlock `getattr(obj, "test_foo")`. Enumeration without
dispatch leaves unittest half-broken. Rejected.

### Option C: Full `__dict__` per class

Mirror CPython: every class has a `dict[str, Callable]` of methods, mutable,
monkey-patchable.

**Pros:** Maximum Python parity.
**Cons:** Loses static vtable speed (every call becomes dict lookup unless we
add an inline cache). Mutability means every method send might hit the slow
path. Violates priority #1 (speed). Rejected. Static dispatch stays the fast
path; reflection pays dict-cost only when asked.

### Option D: Defer until lands fully

 promises first-class class values backed by descriptors. Wait for that,
then add method tables.

**Pros:** One big refactor.
**Cons:** is blocked on Phase 6 (immortal objects). Method reflection
doesn't need first-class classes, only descriptors-as-metadata. The existance
of `@ClassName__descriptor` is enough. Decoupling is the right call. Rejected.

---

## Decision

**Option A.** Extend `DragonClassDescriptor` with method metadata; teach
`getattr`/`hasattr`/`dir` to use it; bound methods reuse closure
infrastructure.

---

## Design

### Runtime: descriptor extension

`lib/Runtime/runtime_internal.h`:

```c
struct DragonClassDescriptor {
 DragonObjectHeader header;
 int64_t class_id;
 const char* name;
 int64_t parent;
 int64_t constructor;
 int64_t* ancestor_ids;
 int64_t num_ancestors;

 // Existing field reflection (Phase 1)
 const char** field_names;
 int64_t* field_offsets;
 int64_t num_fields;

 // NEW
 const char** method_names; // sorted, deduped across inheritance
 void** method_fn_ptrs; // parallel; the same fn ptrs the vtable holds
 int64_t num_methods;
};
```

Two new descriptor setters mirror `dragon_class_descriptor_set_fields`:

```c
void dragon_class_descriptor_set_methods(int64_t descriptor,
 const char** names,
 void** fn_ptrs,
 int64_t num_methods);
```

### Runtime: name lookup + bound-method synthesis

```c
// Returns fn pointer or NULL. Walks parent chain.
void* dragon_class_find_method(int64_t descriptor, const char* name);

// dir(obj_or_class) - returns DragonList of str (refcounted strings).
// instance argument: if non-zero, treat as instance, use its class_id
// descriptor argument: if non-zero, use directly (for dir(MyClass))
DragonList* dragon_dir(int64_t instance_or_descriptor, int is_descriptor);

// hasattr/getattr extension. Drop-in replacement for current versions -
// tries fields first (existing behavior), then methods.
int64_t dragon_hasattr(int64_t instance, const char* name); // unchanged signature
int64_t dragon_getattr(int64_t instance, const char* name); // unchanged signature
```

`dragon_getattr` returns:
- For a field hit: the i64 field value (existing semantics, unchanged).
- For a method hit: an `i64`-cast pointer to a freshly-allocated
 `DragonClosure` whose `fn_ptr` is the method's vtable entry and whose `env`
 holds `self` (incref'd). The closure is `TAG_CLOSURE`, refcounted, so
 scope-exit decref handles cleanup. **First call to `getattr` on a method
 allocates one closure;** if hot, `obj.method` syntax stays on the
 static-dispatch fast path - reflection is opt-in slow path.

### CodeGen: emit method-name globals

Add to `src/CodeGen.cpp:308` (right after the field metadata emission), per
class:

```cpp
// Walk classVtableMethodOrder[className] (already exists)
// Emit @ClassName__method_names : [N x i8*]
// Emit @ClassName__method_fn_ptrs : [N x i8*] (same pointers as @ClassName__vtable)
// Call dragon_class_descriptor_set_methods(desc, names, fn_ptrs, N)
```

Method order is **already** computed by and survives inheritance (parent
order inherited, overrides keep slot, new methods appended). Reuse that ordering
verbatim - no new dedup logic.

Static-dispatch call sites (`obj.method` where the compiler knows the
class) **do not change**. They continue to emit direct `call @Class_method`
or vtable-indexed loads. Reflection is purely additive. No dependant changes to call sites.

### CodeGen: `dir` builtin

`src/codegen/CallBuiltins.cpp`:

```cpp
if (name == "dir" && node.args.size == 1) {
 // emit: dragon_dir(arg, is_descriptor=0 if instance, 1 if class-value)
 // returns DragonListPtr* (list[str])
}
```

When the argument is a class name (will give us `VarKind::Type`), pass
the descriptor and `is_descriptor=1`. Until, `dir(MyClass)` is a
compile-time constant fold to the literal name list, same trick as
`isinstance`'s static path.

### Bound methods

A bound method is just a closure over `self`:

```
DragonClosure {
 header : { refcount=1, tag=TAG_CLOSURE }
 fn_ptr : pointer to ClassName_method
 env : pointer to a 1-slot env { self : ptr }
}
```

Calling it goes through the existing closure-call codegen - the trampoline
at the call site loads `fn_ptr` and `env`, prepends `self` from `env`, then
forwards remaining args. **No new call dispatch path.** 's invariants
carry through.

The closure's env holds an incref'd `self`. When the closure is decref'd
(scope exit at the call site), env decref runs the per-site dealloc fn,
which decrefs `self`. All of this is existing plumbing.

---

## Implementation Phases

### Phase 0: Deep `Any` equality (~80 LOC, independent of reflection)

`assertEqual(first, second)` compares two `Any` values, so container equality has
to be *structural*, not pointer identity. Before this work `dragon_box_eq`
(`runtime_box.cpp`) fell through to pointer-identity for `TAG_LIST` / `TAG_DICT` /
`TAG_BYTES`, so `[1,2,3] == [1,2,3]` between distinct allocations returned `false`
(the code even carried a "Recursive deep equality is a follow-up" comment). This
phase is independent of the reflection primitive and lands first.

- `lib/Runtime/runtime_list.cpp`: `dragon_list_eq` - element-wise compare via
 `dragon_box_eq` recursion, handling all typed variants (`DragonListI64`,
 `DragonListF64`, `DragonListPtr`).
- `lib/Runtime/runtime_dict.cpp`: `dragon_dict_eq` - equal length, every key in
 `a` present in `b` with an equal value via `dragon_box_eq`.
- `lib/Runtime/runtime_box.cpp`: in `dragon_box_eq`, replace the pointer-identity
 fallthroughs for `TAG_LIST` / `TAG_DICT` / `TAG_BYTES` with the new helpers,
 keeping pointer-identity as a fast-path short-circuit.
- Codegen: ensure direct `list == list` / `dict == dict` (both sides unboxed
 pointers) also route through the new helpers, not just the box-typed path.

**Validation:** `compileAndRun("print([1,2,3] == [1,2,3])")` → `"True"`.

### Phase 1: Method-name emission (~150 LOC)

- Extend `DragonClassDescriptor` in `runtime_internal.h`.
- Add `dragon_class_descriptor_set_methods` in `runtime_builtins.cpp`.
- In `src/CodeGen.cpp:308`, after field metadata, walk
 `classVtableMethodOrder[className]` and emit two parallel globals
 (`__method_names`, `__method_fn_ptrs`), then call the new setter.
- Add `dragon_class_find_method` (parent-chain walk).

**Validation:** new CodeGen test asserts the global is emitted with the
expected names; another asserts `dragon_class_find_method` returns the
right pointer for parent-defined methods.

### Phase 2: `dir` builtin (~80 LOC)

- Implement `dragon_dir` returning `DragonListPtr*` of refcounted strings.
- Wire `dir` into `CallBuiltins.cpp` (instance and class-value paths).
- The class-value path can use the literal list until ships
 first-class classes; that's a no-op transition later.

**Validation:** E2E test - `class Foo { def bar(self) {} def baz(self) {} }`
then `print(dir(Foo))` outputs `['bar', 'baz']` (sorted, dunders
included or excluded per Python - match `dir` exactly).

### Phase 3: Method-aware `getattr` / `hasattr` (~120 LOC)

- After the field lookup falls through, try `dragon_class_find_method`.
- On hit, allocate a `DragonClosure` whose env captures `self` (incref'd).
- Closure dealloc decrefs `self` (per-site dealloc fn pattern).
- `hasattr` returns true on either field or method hit.

**Validation:** E2E - `getattr(obj, "foo")` calls the method; refcount
audit shows `self` is balanced; method missing returns `AttributeError`.

### Phase 4: `unittest` stdlib in pure `.dr` (~400 LOC)

- `stdlib/unittest.dr`: `class TestCase`, `assertEqual`, `assertRaises`,
 `assertIn`, etc.
- `class TestRunner`: walks `dir(test_case_instance)`, filters
 `startswith("test_")`, invokes each via `getattr(...)`, captures
 `try/except`, prints `OK` / `FAIL: <name>` per test, returns exit code.
- `unittest.main` discovers test classes registered via decorator (until
 unlocks module-level class enumeration).

**Validation:** port one of our internal `.dr` examples to `unittest`; run
under `dragon run`; output matches Python's `unittest` formatting.

---

## What we gain and what it costs

### Positive
- Unblocks dogfooded `unittest` in pure Dragon - priority #2 (API parity)
 without retreating to C++.
- `dir`, `hasattr`, `getattr` finally work like Python on any object.
- Zero cost on the hot path: static `obj.method` calls are unchanged;
 only opt-in reflection allocates a closure.
- Reuses three existing systems (vtables, Phase 1 descriptors,
 closures) - no new dispatch primitives.
- Sets the runtime up for `__getattr__` / `__setattr__` dunders later
 with no further descriptor surgery.

### Negative / costs
- Each class pays two extra global arrays (method names, method fn ptrs).
 For a 50-class stdlib, the constant overhead is ≤ a few KB of `.rodata`.
 Acceptable.
- Bound-method `getattr` allocates a closure per call. Mitigation: cache
 the closure if profiling shows reflective hot paths (none expected).
- `dragon_getattr`'s return discipline now mixes "field value (any tag)"
 and "closure pointer". The boxed-Any convention (Phase 4) already
 handles this - confirmed by the existing `dragon_getattr_default`
 signature returning `int64_t`.

### Risks / open questions
- **Dunder methods in `dir`**: Python's `dir` includes `__init__`,
 `__str__`, etc. Decision: include them - match Python exactly. Filter
 is the user's job (`[m for m in dir(x) if not m.startswith("_")]`).
- **Static methods / class methods**: Need to be marked in the method
 table so `getattr` doesn't bind `self`. The `@staticmethod` /
 `@classmethod` info is already on `MethodDecl`; emit a flag bit per
 method entry (`method_kinds: i8*` parallel array - 0=instance,
 1=static, 2=class). Phase 3 work, ~30 LOC.
- **Inherited method ordering**: already inherits parent vtable order
 and dedupes overrides. The reflective name table follows the same
 ordering - so `dir(Subclass)` lists inherited methods before
 Subclass-only methods. Matches Python (which sorts), so `dragon_dir`
 sorts the array before returning.

---

## Known follow-ups

Not blocking; surfaced while landing the phases above:

- **`with self.assertRaises(X) { ... }` context-manager form** - needs the `with`
 statement protocol (`__enter__` / `__exit__`). The block-via-`try`/`except` path
 ships now; the context-manager sugar is a follow-up.
- **`unittest.main` auto-discovery** - finding every `TestCase` subclass in a
 module needs module-level class enumeration. Until then, classes register
 via decorator.
- **Virtual method dispatch through base-typed references** - calling an overridden
 method via a variable typed as the base class.

### Incidental language fixes surfaced by this work

Landing pure-Dragon `unittest` exercised paths no prior test hit; each was fixed at
the root (no workarounds), not dodged in the framework:

- Subclass field-layout inheritance; default-constructor synthesis.
- Cross-module parent linkage (codegen + typechecker).
- Native→box argument coercion at call boundaries; `str(Any)` / f-strings via
 `dragon_box_to_str`.
- Expected-type-directed list-literal covariance + `InstanceType` nominal subtyping.
- First-class exception classes (integer-code model) for precise `assertRaises`
 type matching.
- `int("foo")` now raises `ValueError` (Python parity).

## Success criteria

- `class Foo { def bar(self) {} }; print(dir(Foo))` prints
 `['__init__', 'bar']` (or whatever Python prints - match exactly).
- `getattr(obj, "bar")` works for any method, including inherited
 and overridden ones.
- `stdlib/unittest.dr` ships in pure `.dr` and runs the existing
 `examples/*.dr` test suite end-to-end.
- Zero regression on the 1089 existing tests.
- No new C++ code beyond the descriptor extension and three runtime
 helpers (`set_methods`, `find_method`, `dir`) - everything user-visible
 is `.dr`.
