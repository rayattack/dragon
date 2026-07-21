# Decision 010: Dunder Methods & Object Protocol

Implemented. dunder suite shipped (`__str__`, `__eq__`, `__lt__`, `__add__`…`__pow__`, `__getitem__`, `__setitem__`, `__contains__`, `__len__`, `__iter__`, `__next__`, `__enter__`, `__exit__`; `@staticmethod`/`@classmethod` wired). See "Working Language Features" below for details.

Python-style dunders on Dragon classes: operators, `__str__`, containers, context managers, descriptors. Gets us from "struct with methods" to something you can actually overload instead of hardcoding everything on primitives.

Before the fun stuff - `@staticmethod` and `@classmethod` need to actually work. The codebase had partial support: Sema registers both as builtins, TypeHintEnforcer skips `cls` alongside `self`, Parser stores decorators on `FunctionDecl::decorators`. But CodeGen didn't inspect decorators. A `.py` file with `@staticmethod` parsed fine and then silently wasn't static - `isStatic` only came from `.dr`-mode `static def`. Fix that first:

1. **`@staticmethod` in `.py` mode:** CodeGen (or Sema) checks if a method's
 decorators include a `NameExpr` with name `"staticmethod"`. If so, set
 `isStatic = true` and `hasImplicitSelf = false`.
2. **`@classmethod` in both modes:** Add `isClassMethod` flag to `FunctionDecl`.
 In `.dr` mode, classmethods get implicit `cls` (like `self` for instance methods).
 In `.py` mode, `cls` is the explicit first parameter. CodeGen emits the method
 with `cls` as a compile-time type tag (see `__new__` section below).

| Mode | Static method | Class method |
|------|--------------|-------------|
| `.dr` | `static def create -> Foo { ... }` | `@classmethod def create -> Foo { ... }` (implicit `cls`) |
| `.py` | `@staticmethod` decorator | `@classmethod` decorator (explicit `cls`) |

Dragon's class system only had `__init__` when I started this. Every operator (`+`, `==`, `<`, `in`) was hardcoded to primitives, so `print(obj)` couldn't call user string conversion, `obj == other` couldn't customize, `for x in obj` didn't work on custom types, `with obj` had no resource management, `len(obj)` failed on custom containers, `obj[key]` couldn't overload, objects couldn't be dict keys or set members. Dunders fix that. But Dragon is typed and compiled, not Python. Every dunder has to justify itself in a statically-typed struct world. Resolve at compile time when you can; dunders when you need runtime dispatch.

## Stuff We're Not Doing

### `__new__` - `.py` mode only (with static `cls`)

In Python, `__new__` is a `@classmethod` that receives `cls` (the class itself) and
controls instance creation before `__init__`. Common uses: singletons, object caching,
immutable type subclassing.

**Why `.dr` mode doesn't need it:**

Dragon `.dr` mode has `static def` factory methods and `self` constructor overloading
, which cover the same use cases more explicitly:

```dragon
// .dr singleton pattern - no __new__ needed
class Singleton {
 static _instance: Singleton = None

 static def get -> Singleton {
 if Singleton._instance == None {
 Singleton._instance = Singleton
 }
 return Singleton._instance
 }
}
```

**Why `.py` mode supports it:**

Pythonistas migrating to Dragon expect `__new__` for factory patterns. Dragon supports
it with one limitation: **`cls` is resolved statically** (always the declared class,
not a dynamic subclass). This is because Dragon has no runtime class objects - `cls`
compiles to a direct reference to the class's constructor.

```python
# .py mode - __new__ works
class Singleton:
 _instance = None

 def __new__(cls):
 if cls._instance is None:
 cls._instance = object.__new__(cls) # malloc + return
 return cls._instance

 def __init__(self):
 self.value = 42
```

**What `cls` compiles to:** A compile-time constant identifying the class. `cls`
compiles to `ClassName_new`. In the absence of runtime class objects and vtables,
`cls` cannot dynamically resolve to a subclass - it always refers to the class in
which `__new__` is defined. This limitation is documented in migration notes.

**Migration note for Python developers:** If you rely on `cls` being a subclass in
`__new__` (e.g., abstract factory patterns with inheritance), use `static def create`
factory methods instead. Dragon's compile-time dispatch means `cls` is always the
declaring class, not a runtime-resolved subclass.

### `__slots__` - Not needed (explicitly documented)

**Dragon classes are already slot-based by design.** Fields are extracted from `__init__`
body assignments (`self.x = ...`) and laid out as fixed-offset LLVM struct members.
There is no per-instance `__dict__` to suppress - it never existed.

In `.py` mode, `__slots__ = ('x', 'y')` will parse as a regular class-level assignment
and be silently ignored. No error, no warning - it simply has no effect because Dragon
already provides what `__slots__` optimizes for in CPython.

### `__dict__` / `vars` - DROPPED

Dragon instances are C-struct-like: `malloc(sizeof(ClassName))` with fields at fixed
offsets accessed via LLVM `GEP`. There is no hash table attached to each instance.

`__dict__`/`vars(obj)` are on 's cut list: handing back a `dict[str, Any]` of an
object's fields is a reflection convenience that exists only because Python has it, and
it forces every value into an `Any` box. Under commandment #3 it is not part of Dragon -
there is no `__dict__` and no `vars`. The original compile-time-expansion
plan, where the compiler built a `dict[str, Any]` from the struct's known fields, read:

```dragon
class User {
 self(name: str, age: int) {
 self.name = name
 self.age = age
 }
}

u = User("Alice", 30)
print(u.__dict__) // {"name": "Alice", "age": 30} - built at call site, not stored
```

**Default behavior:** `obj.__dict__` constructs a fresh dict from all struct fields.
This is identical to `vars(obj)` - both are compile-time expansions, not stored dicts.
Zero cost until actually called.

**Overridable:** A class can define `def __dict__(self) -> dict[str, Any]` to control
what fields are exposed. This is useful for hiding internal state:

```dragon
class User {
 self(name: str, password: str) {
 self.name = name
 self.password = password
 }

 def __dict__ -> dict[str, Any] {
 return {"name": self.name} // hide password
 }
}
```

This means `vars(obj)` is syntactic sugar for `obj.__dict__`. Python codebases that
depend on `__dict__` being present (serialization, ORMs, debugging) will work. The
key distinction from CPython: this is **generated on access**, not a live mutable dict
attached to the instance. You cannot write `obj.__dict__["new_field"] = 42` to add
fields - the dict is a snapshot, not the backing store.

### Metaclasses (`__metaclass__`, `type` as metaclass, `__prepare__`) - Not needed

**No metaclasses because:**

Metaclasses in Python exist to customize *class creation itself* - intercepting the
class body, modifying the namespace, injecting methods, validating fields. They power
ABCs, enums, ORMs (Django models), and protocol enforcement.

Dragon doesn't need metaclasses because:

1. **Class creation is compile-time.** The compiler builds the struct type, emits
 constructor/method functions, and resolves all field layouts. There is no runtime
 `type` call that constructs a class object.
2. **No class objects at runtime.** In CPython, `MyClass` is itself an object (instance
 of `type`). In Dragon, `MyClass` is a *compile-time symbol* that maps to a struct
 type and a set of mangled functions. It has no runtime identity.
3. **Validation is the type system's job.** Where Python uses metaclasses to enforce
 "all subclasses must implement method X", Dragon uses the type checker.
4. **Dragon has `const`, `static`, and constructor overloading.** The most common
 metaclass use cases (singletons, registries, field validation) are handled by
 language features or decorators.

`__prepare__` specifically controls the namespace dict used during class body execution.
Since Dragon has no class body execution (the body is parsed and compiled, not
"executed" in a namespace), `__prepare__` is meaningless.

**In `.py` mode:** `metaclass=...` keyword in class definition parses but is ignored
with a compiler warning: "Dragon does not support metaclasses."

### `__instancecheck__` / `__subclasscheck__` - Not needed

`isinstance` in Dragon resolves at compile time in `.dr` mode (the compiler knows
all types). In `.py` mode, `isinstance` uses a string-based type tag comparison at
runtime. Custom `__instancecheck__` would require metaclasses, which don't exist.

`isinstance` handles these edge cases correctly via Dragon's subtype rules:
- `isinstance(True, int)` → `True` (because `bool <: int`)
- `isinstance(42, bool)` → `False`
- `isinstance(obj, (int, str))` → checks each type in the tuple

### `__reduce__`, `__reduce_ex__`, `__getstate__`, `__setstate__` - Not needed

Pickle protocol. Dragon has no pickle module and no plan for one. Serialization will
use `json` with explicit `to_dict` / `from_dict` patterns, or
via `__dict__` (see above).

### `__copy__` / `__deepcopy__` - Not needed (Phase 1)

Low priority. Dragon structs can be shallow-copied with a generated memcpy. Deep copy
requires traversing pointer fields, which needs GC/ownership metadata. Defer to a
future memory management decision.

### `__sizeof__` - Handled via `sizeof` built-in

Dragon provides a `sizeof(ClassName)` built-in that evaluates to a compile-time integer
constant (the struct's allocation size in bytes). This replaces `sys.getsizeof`.

If a class defines `def __sizeof__(self) -> int`, it works as a **normal method** -
calling `obj.__sizeof__` invokes it like any other method. However, the `sizeof`
built-in does **not** call `__sizeof__` - it always returns the compile-time constant.

For `.py` code that calls `sys.getsizeof(obj)`: if `__sizeof__` is defined, it is
called. Otherwise, the compile-time struct size is returned. No special treatment
beyond normal method dispatch.

## Dynamic Attribute Strategy

### The question: `__getattr__`, `__getattribute__`, `__setattr__`, `__delattr__`

Dragon is a **typed language with struct-based instances**. The compiler knows every
field on every class at compile time. Dynamic attribute interception contradicts this:

**Decision : Drop `__getattr__`/`__getattribute__`/`__setattr__`/
`__delattr__` entirely, in BOTH modes.** The earlier decision allowed the first three in
`.py` mode for Python compatibility; commandment #3 ("Familiarity must earn its place")
and (No Runtime Type Introspection) reject that. Runtime attribute interception by
string name is the exact dynamic crutch the doctrine bars, and ".py libraries may rely on
it" is no longer a sufficient reason. Object shape is fixed at compile time in both modes.

| Dunder | `.dr` mode | `.py` mode |
|--------|-----------|-----------|
| `__getattr__` | Compile error if defined | Compile error - dropped |
| `__getattribute__` | Compile error if defined | Compile error - dropped |
| `__setattr__` | Compile error if defined | Compile error - dropped |
| `__delattr__` | Compile error if defined | Compile error - dropped |

### Performance isolation: `.py` dunders do NOT penalize `.dr` code

A critical guarantee: **`.py`-mode dynamic attribute dunders have zero cost on `.dr`
classes.** The check is compile-time and per-class:

- If a class does not define `__setattr__`, CodeGen emits direct `GEP` + `Store` for
 field assignments. This is O(1) pointer arithmetic.
- If a class defines `__setattr__`, only *that class's* field assignments route through
 the function call. Other classes in the same binary - including all `.dr` classes -
 are completely unaffected.

There is no global dispatch table, no runtime check, no vtable indirection for classes
that don't opt in. The penalty is localized to the class that defines the dunder.

### Why `__getattribute__` is dropped entirely (not just in `.dr` mode)

`__getattribute__` is **not** merely a performance concern that can be opted into - it
creates **unsolvable circular semantics**:

```python
class Proxy:
 def __getattribute__(self, name):
 print(f"accessing {name}")
 return self._data[name] # BUG: this calls __getattribute__ again!
```

Inside `__getattribute__`, accessing `self._data` recursively triggers
`__getattribute__`. Python solves this with `object.__getattribute__(self, '_data')` -
which requires a base `object` class implemented in C that bypasses the protocol.

Dragon has no base `object` class with a C implementation. Every class is a flat struct.
To break the recursion, we'd need an escape hatch like `@raw_access self._data` or
`builtins.getfield(self, "_data")`, adding complexity for a feature that is rarely
needed and always a performance footgun.

**Decision: Drop `__getattribute__` in both modes.** Use `__getattr__` instead - it
is the fallback-only variant (called only when normal GEP-based lookup fails), so there
is no recursion problem and no performance tax on normal field access.

### Rationale for `.dr` mode exclusion of `__getattr__`/`__setattr__`

- `__getattr__` as a fallback is unnecessary - the compiler already catches missing
 fields at compile time. If you want proxy patterns, use explicit delegation methods:

```dragon
// Explicit delegation - no dynamic fallback needed
class LoggingProxy {
 self(target: Database) {
 self.target = target
 }

 def query(sql: str) -> list {
 print(f"SQL: {sql}")
 return self.target.query(sql)
 }

 def close -> None {
 self.target.close
 }
}
```

- `__setattr__` would intercept `self.x = 5` in `__init__`, creating infinite recursion
 risks and preventing the compiler from emitting direct stores.

### Rationale for `.py` mode inclusion of `__getattr__`/`__setattr__` - WITHDRAWN

This rationale ("`.py` libraries may rely on these") was the parity argument the revised
decision above rejects. Under commandment #3 and, `.py` mode does not buy back
runtime attribute interception: it is dropped in both modes. Kept here only to record
why the earlier allowance was withdrawn.

## Rejected Alternatives

### Monkey patching (`from monkey import Patch`)

**Rejected.** Monkey patching requires runtime class mutation - adding or replacing
fields and methods on a live class. Dragon's classes are compiled structs with fixed
layouts determined at compile time. There is no class dict to mutate, no method
resolution order to rewrite at runtime.

Use cases and Dragon alternatives:
- **Testing (mocking):** Use a proper mock/test framework with compile-time dependency
 injection. A future `from testing import mock` module.
- **Hot-patching third-party code:** Recompile with the fix. Dragon is compiled.
- **Plugin systems:** Use interfaces (dunder protocols - `__getitem__`, `__call__`, etc.)
 to define flexible APIs that accept any conforming type.

`.py` mode with `__setattr__` provides a limited escape hatch for attribute interception
on individual instances, but does not enable class-level mutation.

## Implemented Dunder Methods

### Phase A: Core Representation & Comparison

These are critical - without them, `print` and `==` don't work on custom types.

| Dunder | Trigger | Signature | Notes |
|--------|---------|-----------|-------|
| `__str__` | `print(obj)`, `str(obj)`, `f"{obj}"` | `(self) -> str` | Falls back to `__repr__` if not defined |
| `__repr__` | `repr(obj)`, REPL, debug | `(self) -> str` | Falls back to `<ClassName at 0xADDR>` |
| `__eq__` | `==` | `(self, other) -> bool` | Default: identity comparison (pointer equality) |
| `__ne__` | `!=` | `(self, other) -> bool` | Default: `not self.__eq__(other)` |
| `__lt__` | `<` | `(self, other) -> bool` | No default - error if not defined and used |
| `__gt__` | `>` | `(self, other) -> bool` | Default: `other.__lt__(self)` |
| `__le__` | `<=` | `(self, other) -> bool` | Default: `self.__lt__(other) or self.__eq__(other)` |
| `__ge__` | `>=` | `(self, other) -> bool` | Default: `not self.__lt__(other)` |
| `__hash__` | `hash(obj)`, dict key, set member | `(self) -> int` | Default: `id(self)`. Mutable + `__eq__` = auto-unhashable |
| `__bool__` | `if obj:`, `bool(obj)`, `not obj` | `(self) -> bool` | Default: `True` (all objects truthy unless overridden) |
| `__dict__` | `obj.__dict__`, `vars(obj)` | `(self) -> dict[str, Any]` | Default: compile-time field snapshot. Overridable. |

**How to implement:** In `visit(BinaryExpr&)`, before the primitive-type switch,
check if the LHS has a known class type. If so, look up the corresponding dunder method
(`__eq__` for `==`, `__lt__` for `<`, etc.) and emit a method call instead of an
`ICmp`/`FCmp` instruction. Same pattern in `visit(CallExpr&)` for `print` → `__str__`,
`len` → `__len__`, etc.

### Phase B: Arithmetic & Unary Operators

| Dunder | Trigger | Signature |
|--------|---------|-----------|
| `__add__` | `+` | `(self, other) -> T` |
| `__sub__` | `-` | `(self, other) -> T` |
| `__mul__` | `*` | `(self, other) -> T` |
| `__truediv__` | `/` | `(self, other) -> T` |
| `__floordiv__` | `//` | `(self, other) -> T` |
| `__mod__` | `%` | `(self, other) -> T` |
| `__pow__` | `**` | `(self, other) -> T` |
| `__neg__` | `-obj` | `(self) -> T` |
| `__pos__` | `+obj` | `(self) -> T` |
| `__abs__` | `abs(obj)` | `(self) -> T` |
| `__invert__` | `~obj` | `(self) -> T` |

**Reflected operators (`__radd__`, etc.):** Implemented in Phase F. For Phase B, if
the LHS doesn't have `__add__`, try `RHS.__radd__(lhs)` before erroring.

**Augmented assignment (`__iadd__`, etc.):** If `__iadd__` exists, `obj += x` calls it.
Otherwise, falls back to `obj = obj.__add__(x)`.

### Phase C: Container Protocol

| Dunder | Trigger | Signature |
|--------|---------|-----------|
| `__len__` | `len(obj)` | `(self) -> int` |
| `__getitem__` | `obj[key]` | `(self, key) -> T` |
| `__setitem__` | `obj[key] = val` | `(self, key, val) -> None` |
| `__delitem__` | `del obj[key]` | `(self, key) -> None` |
| `__contains__` | `x in obj` | `(self, item) -> bool` |
| `__iter__` | `for x in obj:` | `(self) -> Iterator` |
| `__next__` | Iterator protocol | `(self) -> T` (raises StopIteration) |
| `__reversed__` | `reversed(obj)` | `(self) -> Iterator` |

**Iterator design:** `__iter__` returns an iterator object (can be `self` if the class
implements `__next__`). For Phase C, iterators are **eager** - `__iter__` returns a list.
True lazy iteration requires coroutine/generator machinery (deferred).

### Phase D: Context Managers

| Dunder | Trigger | Signature |
|--------|---------|-----------|
| `__enter__` | `with obj as x:` | `(self) -> T` |
| `__exit__` | End of `with` block | `(self, exc_type, exc_val, exc_tb) -> bool` |

**Implementation:** The `with` statement is already parsed. CodeGen needs to:
1. Call `__enter__`, bind result to `as` variable
2. Wrap body in try/finally
3. Call `__exit__(None, None, None)` in finally (or with exception info on error)

### Phase E: Descriptor Protocol

| Dunder | Trigger | Signature |
|--------|---------|-----------|
| `__get__` | `obj.descriptor_attr` | `(self, obj, objtype) -> T` |
| `__set__` | `obj.descriptor_attr = val` | `(self, obj, val) -> None` |
| `__delete__` | `del obj.descriptor_attr` | `(self, obj) -> None` |
| `__set_name__` | Class creation time | `(self, owner, name) -> None` |

**How this works in Dragon:**

A descriptor is a class that defines `__get__` and/or `__set__`. When a field's type
is a descriptor class, the compiler emits descriptor protocol calls instead of direct
GEP access:

```dragon
class ValidatedAge {
 self(min_val: int, max_val: int) {
 self.min_val = min_val
 self.max_val = max_val
 }

 def __get__(self, obj: Any, objtype: Any) -> int {
 return obj._age
 }

 def __set__(self, obj: Any, value: int) -> None {
 if value < self.min_val or value > self.max_val {
 raise ValueError("age out of range")
 }
 obj._age = value
 }
}

class Person {
 age: ValidatedAge = ValidatedAge(0, 150)

 self(name: str, age: int) {
 self.name = name
 self.age = age // calls ValidatedAge.__set__
 }
}

p = Person("Alice", 30)
print(p.age) // calls ValidatedAge.__get__ -> 30
p.age = 200 // raises ValueError
```

**Compile-time descriptor detection:** The TypeChecker checks whether a field's type
defines `__get__` or `__set__`. If so, it marks the field as a descriptor in the class
metadata. CodeGen then emits `__get__`/`__set__` calls instead of direct struct access.

This enables `@property` as a stdlib descriptor class rather than compiler magic.

### Phase F: Callable, Reflected, In-Place, Type Conversion

| Dunder | Trigger | Signature |
|--------|---------|-----------|
| `__call__` | `obj` | `(self, *args) -> T` |
| `__radd__` | `other + obj` (when other has no `__add__`) | `(self, other) -> T` |
| `__rsub__`, `__rmul__`, etc. | Reflected variants | Same pattern |
| `__iadd__` | `obj += other` | `(self, other) -> T` |
| `__isub__`, `__imul__`, etc. | In-place variants | Same pattern |
| `__int__` | `int(obj)` | `(self) -> int` |
| `__float__` | `float(obj)` | `(self) -> float` |
| `__index__` | Using obj as sequence index | `(self) -> int` |
| `__format__` | `f"{obj:.2f}"` format spec | `(self, spec: str) -> str` |

### Phase G: `.py`-Only Dunders

These are admitted **only in `.py` mode** for Python compatibility:

| Dunder | `.py` behavior | `.dr` behavior |
|--------|---------------|---------------|
| `__new__` | Classmethod, receives static `cls`, controls allocation | Compile error - use `static def` factories |
| `__getattr__` | Fallback for missing attributes | Compile error |
| `__setattr__` | Intercepts all attribute writes | Compile error |
| `__delattr__` | Intercepts attribute deletion | Compile error |

## Method Overloading

Dragon supports **constructor overloading** via arity-based dispatch.
This decision extends overloading to **all methods**, not just `__init__`/`self`:

```dragon
class Vector {
 def __add__(self, other: Vector) -> Vector { ... }
 def __add__(self, scalar: int) -> Vector { ... } // overloaded by type
}
```

**Dispatch strategy:** Same as constructors - compile-time resolution by argument
count. For same-arity overloads, the TypeChecker resolves by parameter types. This is
stricter than Python (which has no overloading) but natural in a typed language.

## Implementation Plan

| Phase | Dunders | Estimated Tests | Depends On |
|-------|---------|----------------|------------|
| Pre | Wire `@staticmethod`/`@classmethod` decorators in CodeGen | ~10 | Nothing |
| A | `__str__`, `__repr__`, `__eq__`, `__ne__`, `__lt__`, `__gt__`, `__le__`, `__ge__`, `__hash__`, `__bool__`, `__dict__` | ~35 | Pre |
| B | `__add__`, `__sub__`, `__mul__`, `__truediv__`, `__floordiv__`, `__mod__`, `__pow__`, `__neg__`, `__pos__`, `__abs__`, `__invert__` | ~25 | Phase A |
| C | `__len__`, `__getitem__`, `__setitem__`, `__delitem__`, `__contains__`, `__iter__`, `__next__`, `__reversed__` | ~25 | Phase A |
| D | `__enter__`, `__exit__` | ~10 | Phase A |
| E | `__get__`, `__set__`, `__delete__`, `__set_name__` | ~15 | Phase C |
| F | `__call__`, `__radd__` family, `__iadd__` family, `__int__`, `__float__`, `__index__`, `__format__` | ~20 | Phase B |
| G | `__new__` (.py only), `__getattr__`/`__setattr__`/`__delattr__` (.py only) | ~15 | Phase A |

**Total: ~45 dunders, ~155 new tests**

## Affected Components

- **TypeChecker:** Resolve dunder methods on class types for operator/builtin dispatch
- **CodeGen:** Emit dunder calls in `visit(BinaryExpr&)`, `visit(CallExpr&)`,
 `visit(SubscriptExpr&)`, `visit(ForStmt&)`, `visit(WithStmt&)`; wire decorator
 handling for `@staticmethod`/`@classmethod`
- **Sema:** Validate dunder signatures, reject dynamic dunders in `.dr` mode, reject
 `__getattribute__` in both modes
- **AST:** Add `isClassMethod` flag to `FunctionDecl`
- **Runtime:** Default `__repr__` implementation (format class name + address),
 `sizeof` built-in

## `.py` Mode

All dunders work identically in `.py` mode with explicit `self`:

```python
class Vector:
 def __init__(self, x, y):
 self.x = x
 self.y = y

 def __add__(self, other):
 return Vector(self.x + other.x, self.y + other.y)

 def __eq__(self, other):
 return self.x == other.x and self.y == other.y

 def __dict__(self):
 return {"x": self.x, "y": self.y}
```

Also, `.py` mode supports:
- `__new__` as a classmethod with static `cls` (see exclusions section)
- `__getattr__` and `__setattr__` (see Dynamic Attribute Strategy section)
- `@staticmethod` and `@classmethod` decorators (see prerequisite section)
