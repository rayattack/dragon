# 006 -- Type System and Type Checker (`TypeChecker`)

> **Source files:** `include/dragon/TypeChecker.h`, `src/TypeChecker.cpp`, `src/TypeCheckerExprs.cpp`, `src/TypeCheckerStmts.cpp`, `src/TypeCheckerGenerics.cpp`
> **Last Updated:** 2026-06-22
> **Test suite:** TypeCheckerTests

This document describes Dragon's type system and type checking pass in full
detail. All information is derived from the actual implementation in
`include/dragon/TypeChecker.h` and the `src/TypeChecker*.cpp` files.

---

## 1. The Type Class Hierarchy

All types inherit from the abstract base class `Type`, defined in
`include/dragon/TypeChecker.h`.

```
Type (abstract)
  |-- PrimitiveType  (Int, Float, Bool, Str, Bytes, None_)
  |-- ListType       (list[T])
  |-- TaskType       (Task[T] - fire / async def handle, erases to ptr)
  |-- LockType       (Lock - threading mutex handle, erases to ptr)
  |-- DictType       (dict[K, V])
  |-- TupleType      (tuple[T1, T2, ...])
  |-- FunctionType   ((T1, T2) -> R)
  |-- ClassType      (the class object itself)
  |-- InstanceType   (an instance of a ClassType)
  |-- UnionType      (T1 | T2 | ...)
  |-- AnyType        (top type)
  |-- NeverType      (bottom type)
  |-- UnknownType    (inference placeholder)
  |-- PtrType        (raw C pointer, extern "C" FFI)
  |-- TypeVarType    (generic type variable, optional bound)
  |-- ModuleType     (an imported module reference)
```

### 1.1 `Type` (Base Class)

```cpp
class Type {
public:
    enum class Kind {
        Int, Float, Bool, Str, Bytes, None_,
        List, Dict, Set, Tuple, Function, Task, Lock,
        Class, Instance, Any, Never, Union, Optional, TypeVar, Unknown,
        Ptr,    // Raw C pointer (void* / i8*), used in extern "C" FFI
        Module  // Imported module - base of x.y.z attribute chains, never a runtime value
    };
    virtual ~Type() = default;
    virtual Kind kind() const = 0;
    virtual std::string toString() const = 0;
    virtual bool equals(const Type& other) const = 0;
    virtual bool isSubtypeOf(const Type& other) const;
    bool isAssignableTo(const Type& other) const;
};
```

The `Kind` enum has more entries than there are concrete type classes. `Set`
and `Optional` are present in the enum but have no dedicated class -- sets
are currently represented as `ListType`, and optionals are represented as
`UnionType(T, None)`. Every other `Kind` value does have a concrete class:
`Task` -> `TaskType` (§1.3a), `Lock` -> `LockType` (§1.3b), `Ptr` ->
`PtrType` (§1.14), and `Module` -> `ModuleType` (§1.15).

#### Base `isSubtypeOf` Implementation

```cpp
bool Type::isSubtypeOf(const Type& other) const {
    if (equals(other)) return true;
    if (other.kind() == Kind::Any) return true;
    if (other.kind() == Kind::Union) {
        auto& ut = static_cast<const UnionType&>(other);
        for (auto& t : ut.types) {
            if (isSubtypeOf(*t)) return true;
        }
    }
    return false;
}
```

The base rules are:
1. Every type is a subtype of itself (via `equals`).
2. Every type is a subtype of `Any`.
3. A type is a subtype of a union if it is a subtype of any member.

#### `isAssignableTo`

```cpp
bool Type::isAssignableTo(const Type& other) const {
    return isSubtypeOf(other);
}
```

Currently a direct alias for `isSubtypeOf`. Exists as a separate method to
allow future divergence (e.g., covariance/contravariance rules).

---

### 1.2 `PrimitiveType`

Represents one of the six primitive types: `Int`, `Float`, `Bool`, `Str`,
`Bytes`, `None_`.

```cpp
class PrimitiveType : public Type {
public:
    explicit PrimitiveType(Kind k) : kind_(k) {}
    Kind kind() const override { return kind_; }
    std::string toString() const override;
    bool equals(const Type& other) const override;
    bool isSubtypeOf(const Type& other) const override;
private:
    Kind kind_;
};
```

#### `toString()`

| Kind | String |
|---|---|
| `Int` | `"int"` |
| `Float` | `"float"` |
| `Bool` | `"bool"` |
| `Str` | `"str"` |
| `Bytes` | `"bytes"` |
| `None_` | `"None"` |

#### `equals()`

Two `PrimitiveType` values are equal if and only if they have the same
`Kind`. This means `bool` is NOT equal to `int` (they are distinct types),
even though `bool` is a subtype of `int`.

```cpp
bool PrimitiveType::equals(const Type& other) const {
    return other.kind() == kind_;
}
```

#### `isSubtypeOf()` -- The Numeric Subtype Chain

```cpp
bool PrimitiveType::isSubtypeOf(const Type& other) const {
    if (equals(other)) return true;
    if (other.kind() == Kind::Any) return true;
    // bool <: int (Python convention)
    if (kind_ == Kind::Bool && other.kind() == Kind::Int) return true;
    // int <: float (numeric widening)
    if (kind_ == Kind::Int && other.kind() == Kind::Float) return true;
    // bool <: float (transitive: bool <: int <: float)
    if (kind_ == Kind::Bool && other.kind() == Kind::Float) return true;
    // Check union
    return Type::isSubtypeOf(other);
}
```

The complete subtype lattice for primitives:

```
         Any
       /  |  \  \  \
     float str bytes None
      |
     int
      |
     bool
```

- `bool <: int` -- Python treats booleans as integers.
- `int <: float` -- numeric widening.
- `bool <: float` -- transitive (explicitly coded for efficiency).
- `str`, `bytes`, and `None_` have no subtype relationships with each
  other or with numerics.

---

### 1.3 `ListType`

```cpp
class ListType : public Type {
public:
    std::shared_ptr<Type> elementType;
    explicit ListType(std::shared_ptr<Type> elem) : elementType(std::move(elem)) {}
    Kind kind() const override { return Kind::List; }
    std::string toString() const override;
    bool equals(const Type& other) const override;
};
```

#### `toString()`

Returns `"list[<elementType>]"`, e.g., `"list[int]"`.

#### `equals()`

Two `ListType` values are equal if their `elementType` values are equal.
List types are invariant: `list[bool]` is NOT equal to `list[int]`.

```cpp
bool ListType::equals(const Type& other) const {
    if (other.kind() != Kind::List) return false;
    return elementType->equals(*static_cast<const ListType&>(other).elementType);
}
```

`ListType` overrides `isSubtypeOf` with one relaxation on top of the base
rules: `list[T] <: list[Any]` for any `T`. Otherwise lists are invariant.

---

### 1.3a `TaskType`

```cpp
class TaskType : public Type {
public:
    std::shared_ptr<Type> resultType;
    explicit TaskType(std::shared_ptr<Type> result) : resultType(std::move(result)) {}
    Kind kind() const override { return Kind::Task; }
    std::string toString() const override;
    bool equals(const Type& other) const override;
    bool isSubtypeOf(const Type& other) const override;
};
```

`Task[T]` is the handle produced by `fire fn()` / `fire { block }` and by
calling an `async def f() -> T`. It is an erased generic: it carries `T` in
the type system (recovered by `await` / `.join()`) but lowers to a bare
`ptr` (`DragonVThread*`) at LLVM, so it costs nothing at runtime.

- `toString()` returns `"Task[<resultType>]"`, e.g., `"Task[int]"`.
- `equals()` compares the wrapped `resultType`.
- `isSubtypeOf()` mirrors `ListType`'s Any relaxation: `Task[T] <: Task[Any]`
  for any `T`.

---

### 1.3b `LockType`

```cpp
class LockType : public Type {
public:
    Kind kind() const override { return Kind::Lock; }
    std::string toString() const override;   // "Lock"
    bool equals(const Type& other) const override;
    bool isSubtypeOf(const Type& other) const override;
};
```

`Lock` is the `threading.Lock` mutex handle. Like `Task`, it is an intrinsic
that erases to a bare `ptr` (`pthread_mutex_t*`) at LLVM - no class instance
or GC overhead. It is annotatable (`lock: Lock = Lock()`) but is not a
user-defined class; `Lock()`, `.acquire()` / `.release()` / `.try_lock()`,
and `with lock { }` lower directly to `dragon_lock_*` runtime calls. It
carries no payload, so all `LockType` values are equal (`equals()` checks
only `Kind::Lock`).

---

### 1.4 `DictType`

```cpp
class DictType : public Type {
public:
    std::shared_ptr<Type> keyType;
    std::shared_ptr<Type> valueType;
    DictType(std::shared_ptr<Type> k, std::shared_ptr<Type> v)
        : keyType(std::move(k)), valueType(std::move(v)) {}
    Kind kind() const override { return Kind::Dict; }
    std::string toString() const override;
    bool equals(const Type& other) const override;
};
```

#### `toString()`

Returns `"dict[<keyType>, <valueType>]"`, e.g., `"dict[str, int]"`.

#### `equals()`

Two `DictType` values are equal if both their `keyType` and `valueType` are
equal. Like lists, dicts are invariant.

```cpp
bool DictType::equals(const Type& other) const {
    if (other.kind() != Kind::Dict) return false;
    auto& o = static_cast<const DictType&>(other);
    return keyType->equals(*o.keyType) && valueType->equals(*o.valueType);
}
```

---

### 1.5 `TupleType`

```cpp
class TupleType : public Type {
public:
    std::vector<std::shared_ptr<Type>> elementTypes;
    explicit TupleType(std::vector<std::shared_ptr<Type>> elems)
        : elementTypes(std::move(elems)) {}
    Kind kind() const override { return Kind::Tuple; }
    std::string toString() const override;
    bool equals(const Type& other) const override;
};
```

#### `toString()`

Returns `"tuple[T1, T2, ...]"`, e.g., `"tuple[int, str, bool]"`.

#### `equals()`

Two `TupleType` values are equal if they have the same number of element
types and each pair of corresponding elements is equal. This is a
structural equality check.

```cpp
bool TupleType::equals(const Type& other) const {
    if (other.kind() != Kind::Tuple) return false;
    auto& o = static_cast<const TupleType&>(other);
    if (elementTypes.size() != o.elementTypes.size()) return false;
    for (size_t i = 0; i < elementTypes.size(); ++i) {
        if (!elementTypes[i]->equals(*o.elementTypes[i])) return false;
    }
    return true;
}
```

---

### 1.6 `FunctionType`

```cpp
class FunctionType : public Type {
public:
    std::vector<std::shared_ptr<Type>> paramTypes;
    std::shared_ptr<Type> returnType;
    FunctionType(std::vector<std::shared_ptr<Type>> params, std::shared_ptr<Type> ret)
        : paramTypes(std::move(params)), returnType(std::move(ret)) {}
    Kind kind() const override { return Kind::Function; }
    std::string toString() const override;
    bool equals(const Type& other) const override;
};
```

#### `toString()`

Returns `"(T1, T2) -> R"`, e.g., `"(int, str) -> bool"`.

#### `equals()`

Two function types are equal if they have the same number of parameter types,
each parameter type pair is equal, and the return types are equal.

```cpp
bool FunctionType::equals(const Type& other) const {
    if (other.kind() != Kind::Function) return false;
    auto& o = static_cast<const FunctionType&>(other);
    if (!returnType->equals(*o.returnType)) return false;
    if (paramTypes.size() != o.paramTypes.size()) return false;
    for (size_t i = 0; i < paramTypes.size(); ++i) {
        if (!paramTypes[i]->equals(*o.paramTypes[i])) return false;
    }
    return true;
}
```

Note: no covariance/contravariance is implemented. Function type equality
is purely structural.

---

### 1.7 `ClassType`

Represents the class object itself (not an instance). Calling a `ClassType`
constructs an `InstanceType`.

```cpp
class ClassType : public Type {
public:
    std::string name;
    std::unordered_map<std::string, std::shared_ptr<Type>> methods;
    std::unordered_map<std::string, std::shared_ptr<Type>> fields;
    std::shared_ptr<Type> parentClass;
    explicit ClassType(std::string n) : name(std::move(n)) {}
    Kind kind() const override { return Kind::Class; }
    std::string toString() const override;
    bool equals(const Type& other) const override;
};
```

#### `toString()`

Returns `"type[<name>]"`, e.g., `"type[MyClass]"`.

#### `equals()`

Two `ClassType` values are equal if they have the same `name`. This is a
nominal equality check (not structural).

```cpp
bool ClassType::equals(const Type& other) const {
    if (other.kind() != Kind::Class) return false;
    return name == static_cast<const ClassType&>(other).name;
}
```

#### Fields

| Field | Type | Description |
|---|---|---|
| `name` | `std::string` | The class name as it appears in source. |
| `methods` | `unordered_map<string, shared_ptr<Type>>` | Method name to method type (typically `FunctionType`). Populated during `visit(ClassDecl&)`. |
| `fields` | `unordered_map<string, shared_ptr<Type>>` | Field name to field type. Currently not populated by the TypeChecker (reserved for future use). |
| `parentClass` | `shared_ptr<Type>` | The parent class if inheritance is used. Currently not populated. |

---

### 1.8 `InstanceType`

Wraps a `ClassType` to represent an instance of that class.

```cpp
class InstanceType : public Type {
public:
    std::shared_ptr<ClassType> classType;
    explicit InstanceType(std::shared_ptr<ClassType> cls) : classType(std::move(cls)) {}
    Kind kind() const override { return Kind::Instance; }
    std::string toString() const override;
    bool equals(const Type& other) const override;
};
```

#### `toString()`

Returns the class name directly, e.g., `"MyClass"` (not `"type[MyClass]"`).

#### `equals()`

Two `InstanceType` values are equal if their wrapped `ClassType` values are
equal (by name).

```cpp
bool InstanceType::equals(const Type& other) const {
    if (other.kind() != Kind::Instance) return false;
    return classType->equals(*static_cast<const InstanceType&>(other).classType);
}
```

---

### 1.9 `UnionType`

```cpp
class UnionType : public Type {
public:
    std::vector<std::shared_ptr<Type>> types;
    explicit UnionType(std::vector<std::shared_ptr<Type>> ts) : types(std::move(ts)) {}
    Kind kind() const override { return Kind::Union; }
    std::string toString() const override;
    bool equals(const Type& other) const override;
    bool isSubtypeOf(const Type& other) const override;
};
```

#### `toString()`

Returns `"T1 | T2 | ..."`, e.g., `"int | str"`.

#### `equals()`

Order-independent comparison: two union types are equal if they have the
same number of member types and every type in one union has a matching equal
type in the other.

```cpp
bool UnionType::equals(const Type& other) const {
    if (other.kind() != Kind::Union) return false;
    auto& o = static_cast<const UnionType&>(other);
    if (types.size() != o.types.size()) return false;
    for (auto& t : types) {
        bool found = false;
        for (auto& ot : o.types) {
            if (t->equals(*ot)) { found = true; break; }
        }
        if (!found) return false;
    }
    return true;
}
```

This means `int | str` equals `str | int`.

#### `isSubtypeOf()`

A union type is a subtype of another type if **every** member of the union
is a subtype of that other type. This is the standard covariant union
subtyping rule.

```cpp
bool UnionType::isSubtypeOf(const Type& other) const {
    for (auto& t : types) {
        if (!t->isSubtypeOf(other)) return false;
    }
    return true;
}
```

Example: `bool | int` is a subtype of `float` (because `bool <: float` and
`int <: float`).

---

### 1.10 `AnyType`

The top type. Compatible with everything.

```cpp
class AnyType : public Type {
public:
    Kind kind() const override { return Kind::Any; }
    std::string toString() const override { return "Any"; }
    bool equals(const Type& other) const override { return other.kind() == Kind::Any; }
    bool isSubtypeOf(const Type&) const override { return true; }
};
```

`Any` is a subtype of **every** type (returns `true` unconditionally).
Every type is also a subtype of `Any` (handled in the base `Type::isSubtypeOf`).
This makes `Any` behave as both top and bottom from a practical standpoint,
matching Python's `typing.Any` semantics.

---

### 1.11 `NeverType`

The bottom type. A subtype of everything.

```cpp
class NeverType : public Type {
public:
    Kind kind() const override { return Kind::Never; }
    std::string toString() const override { return "Never"; }
    bool equals(const Type& other) const override { return other.kind() == Kind::Never; }
    bool isSubtypeOf(const Type&) const override { return true; }
};
```

`Never` represents the type of expressions that never produce a value (e.g.,
functions that always raise or infinite loops). It is a subtype of every type.

---

### 1.12 `UnknownType`

An inference placeholder used when the type cannot be determined.

```cpp
class UnknownType : public Type {
public:
    Kind kind() const override { return Kind::Unknown; }
    std::string toString() const override { return "<unknown>"; }
    bool equals(const Type& other) const override { return other.kind() == Kind::Unknown; }
    bool isSubtypeOf(const Type&) const override { return true; }
};
```

`Unknown` is a subtype of everything, similar to `Never`, but carries
different semantic meaning: it indicates incomplete type information rather
than guaranteed non-termination. The type checker skips errors when either
operand is `Unknown` to avoid cascading error messages.

---

### 1.13 `TypeVarType`

For generic type parameters.

```cpp
class TypeVarType : public Type {
public:
    std::string name;
    std::shared_ptr<Type> bound;   // spec-46 bounded type param `T: B`; nullptr if unbounded
    explicit TypeVarType(std::string n, std::shared_ptr<Type> b = nullptr)
        : name(std::move(n)), bound(std::move(b)) {}
    Kind kind() const override { return Kind::TypeVar; }
    std::string toString() const override { return name; }
    bool equals(const Type& other) const override;
};
```

Two `TypeVarType` values are equal if they have the same `name`. The optional
`bound` member (spec-46) holds the resolved bound `B` of a bounded type parameter
`T: B`; a bounded `T` behaves like its bound, so member, operator, and
subscript access on a `T`-typed value resolve against `bound` (see
`visit(UnaryExpr&)` / `visit(BinaryExpr&)`, which unwrap a `TypeVarType` to
its bound before operand checking).

`TypeVarType` is **actively instantiated** by the type checker. User generics
(spec-44) are shipped: `src/TypeCheckerGenerics.cpp` substitutes type arguments
for `TypeVarType`s and drives generic inference for both generic functions and
generic classes. When a call site does not pin every type parameter it errors
(`"cannot infer type parameter '<x>' for generic ..."`); a generic-class use
that leaves arguments unsolved errors (`"cannot infer type arguments for
generic class '<name>' ..."`). A solved instantiation is stamped into a fully
concrete `ClassType` / `FunctionType` (`substituteType` replaces each
`TypeVarType` by name), so CodeGen never sees a `TypeVar`. A monomorphic class
instantiation records `genericOrigin` / `genericArgs` on its `ClassType` so
nested generics (`Inner[T]` inside `Outer[T]`) re-instantiate transitively.

---

### 1.14 `PtrType`

```cpp
class PtrType : public Type {
public:
    Kind kind() const override { return Kind::Ptr; }
    std::string toString() const override { return "ptr"; }
    bool equals(const Type& other) const override { return other.kind() == Kind::Ptr; }
};
```

The raw C pointer type (`void*` / `i8*`), used for `extern "C"` FFI. It is
registered in `typeNames` under `"ptr"`, so an annotation of `ptr` resolves
to it. It carries no payload, so all `PtrType` values are equal. It does not
override `isSubtypeOf` (base rules: equality + Any + union membership).

---

### 1.15 `ModuleType`

The type of an imported module reference (e.g., the `x` in `import x.y` or
the `health` in `from controllers import health`).

```cpp
class ModuleType : public Type {
public:
    std::string name;       // canonical dotted path, e.g. "controllers.health"
    std::string filepath;   // spec-45 source path, for import/qualified-access privacy
    std::unordered_map<std::string, std::shared_ptr<Type>> exports;
    std::unordered_map<std::string, std::shared_ptr<ModuleType>> submodules;
    explicit ModuleType(std::string n) : name(std::move(n)) {}
    Kind kind() const override { return Kind::Module; }
    std::string toString() const override { return "module[" + name + "]"; }
    bool equals(const Type& other) const override;
};
```

`ModuleType` is a pure compile-time construct: modules never become runtime
values; a `ModuleType` only appears as the base of an `AttributeExpr` chain
that resolves to a static symbol (a function, class, or const). Two
`ModuleType` values are equal if they share the same canonical `name` - the
name is the identity, regardless of how far `exports` / `submodules` are
populated. The checker keeps these in `impl_->moduleTypes` (name ->
`ModuleType`), created on demand via `getOrCreateModuleType()`.

---

## 2. Type Resolution: `resolveType()`

`resolveType()` converts a `TypeExpr` AST node (a type annotation written by
the programmer) into a `Type` object.

```cpp
std::shared_ptr<Type> TypeChecker::resolveType(TypeExpr* typeExpr);
```

Returns `unknownType` if `typeExpr` is `nullptr`.

### Resolution Rules

| TypeExpr class | Resolution logic |
|---|---|
| `NamedTypeExpr` | Look up `name` in `impl_->typeNames`. If found, return that type. If not found, look up in the type environment (`impl_->lookup`); if it is a `ClassType`, wrap it in `InstanceType` and return. Otherwise, emit `"unknown type '<name>'"` and return `unknownType`. |
| `GenericTypeExpr` | Extract the base name. Handle: `list[T]` -> `ListType(resolve(T))`, `dict[K,V]` -> `DictType(resolve(K), resolve(V))`, `tuple[T1,T2,...]` -> `TupleType([resolve(Ti)])`, `set[T]` -> `ListType(resolve(T))` (simplified). Otherwise, emit `"unknown generic type '<name>'"`. |
| `OptionalTypeExpr` | Resolve inner type, return `UnionType([inner, noneType])`. |
| `UnionTypeExpr` | Resolve each member type, return `UnionType([...])`. |
| `CallableTypeExpr` | Resolve each param type and return type, return `FunctionType(params, ret)`. |
| `TupleTypeExpr` | Resolve each element type, return `TupleType([...])`. |

### The `typeNames` Map

Populated by `initBuiltinTypes()` with the following entries:

| Key | Resolves to |
|---|---|
| `"int"` | `PrimitiveType(Int)` |
| `"float"` | `PrimitiveType(Float)` |
| `"bool"` | `PrimitiveType(Bool)` |
| `"str"` | `PrimitiveType(Str)` |
| `"bytes"` | `PrimitiveType(Bytes)` |
| `"None"` | `PrimitiveType(None_)` |
| `"Any"` | `AnyType` |
| `"Never"` | `NeverType` |
| `"object"` | `AnyType` (treated as Any) |

User-defined class names are added to `typeNames` during `visit(ClassDecl&)`:

```cpp
impl_->typeNames[node.name] = std::make_shared<InstanceType>(classType);
```

This means when a type annotation says `MyClass`, it resolves to
`InstanceType(ClassType("MyClass"))`.

### Detailed `resolveType()` Implementation

```cpp
std::shared_ptr<Type> TypeChecker::resolveType(TypeExpr* typeExpr) {
    if (!typeExpr) return impl_->unknownType;

    if (auto* named = dynamic_cast<NamedTypeExpr*>(typeExpr)) {
        auto it = impl_->typeNames.find(named->name);
        if (it != impl_->typeNames.end()) return it->second;
        auto looked = impl_->lookup(named->name);
        if (looked && looked->kind() == Type::Kind::Class) {
            auto cls = std::static_pointer_cast<ClassType>(looked);
            return std::make_shared<InstanceType>(cls);
        }
        error(named->location(), "unknown type '" + named->name + "'");
        return impl_->unknownType;
    }

    if (auto* generic = dynamic_cast<GenericTypeExpr*>(typeExpr)) {
        auto baseName = dynamic_cast<NamedTypeExpr*>(generic->base.get());
        if (!baseName) {
            error(generic->location(), "invalid generic type");
            return impl_->unknownType;
        }
        if (baseName->name == "list" && generic->typeArgs.size() == 1) {
            return std::make_shared<ListType>(resolveType(generic->typeArgs[0].get()));
        }
        if (baseName->name == "dict" && generic->typeArgs.size() == 2) {
            return std::make_shared<DictType>(
                resolveType(generic->typeArgs[0].get()),
                resolveType(generic->typeArgs[1].get()));
        }
        if (baseName->name == "tuple") {
            std::vector<std::shared_ptr<Type>> elems;
            for (auto& arg : generic->typeArgs) {
                elems.push_back(resolveType(arg.get()));
            }
            return std::make_shared<TupleType>(std::move(elems));
        }
        if (baseName->name == "set" && generic->typeArgs.size() == 1) {
            return std::make_shared<ListType>(resolveType(generic->typeArgs[0].get()));
        }
        error(generic->location(), "unknown generic type '" + baseName->name + "'");
        return impl_->unknownType;
    }

    if (auto* opt = dynamic_cast<OptionalTypeExpr*>(typeExpr)) {
        auto inner = resolveType(opt->inner.get());
        std::vector<std::shared_ptr<Type>> types = {inner, impl_->noneType};
        return std::make_shared<UnionType>(std::move(types));
    }

    if (auto* union_ = dynamic_cast<UnionTypeExpr*>(typeExpr)) {
        std::vector<std::shared_ptr<Type>> types;
        for (auto& t : union_->types) {
            types.push_back(resolveType(t.get()));
        }
        return std::make_shared<UnionType>(std::move(types));
    }

    if (auto* callable = dynamic_cast<CallableTypeExpr*>(typeExpr)) {
        std::vector<std::shared_ptr<Type>> params;
        for (auto& p : callable->paramTypes) {
            params.push_back(resolveType(p.get()));
        }
        auto ret = resolveType(callable->returnType.get());
        return std::make_shared<FunctionType>(std::move(params), ret);
    }

    if (auto* tuple = dynamic_cast<TupleTypeExpr*>(typeExpr)) {
        std::vector<std::shared_ptr<Type>> elems;
        for (auto& e : tuple->elementTypes) {
            elems.push_back(resolveType(e.get()));
        }
        return std::make_shared<TupleType>(std::move(elems));
    }

    return impl_->unknownType;
}
```

---

## 3. Type Inference: `inferType()`

`inferType()` computes the type of an expression by visiting it and reading
the `type` field that each visitor sets on the expression node.

```cpp
std::shared_ptr<Type> TypeChecker::inferType(Expr* expr) {
    if (!expr) return impl_->unknownType;
    expr->accept(*this);
    return expr->type ? expr->type : impl_->unknownType;
}
```

### 3.1 Literal Inference

| Expression | Inferred Type |
|---|---|
| `IntegerLiteral` | `int` |
| `FloatLiteral` | `float` |
| `StringLiteral` (isBytes=false) | `str` |
| `StringLiteral` (isBytes=true) | `bytes` |
| `BooleanLiteral` | `bool` |
| `NoneLiteral` | `None` |

### 3.2 `NameExpr` Inference

Looks up `node.name` in the type environment (`impl_->lookup()`). If found,
uses that type. If not found, sets `unknownType`.

### 3.3 `BinaryExpr` Inference

See Section 5 for the complete operator type rules.

### 3.4 `UnaryExpr` Inference

| Operator | Operand Type | Result Type |
|---|---|---|
| `-`, `+` | subtype of `int` | `int` |
| `-`, `+` | `float` | `float` |
| `-`, `+` | `Unknown` or `Any` | `<unknown>` |
| `-`, `+` | anything else | error + `<unknown>` |
| `not` | any | `bool` |
| `~` | subtype of `int` | `int` |
| `~` | `Unknown` or `Any` | `<unknown>` |
| `~` | anything else | error + `<unknown>` |

Error message: `"bad operand type for unary <op>: '<type>'"`.

### 3.5 `CallExpr` Inference

1. Infer the type of the callee.
2. Infer types for all positional and keyword arguments.
3. Determine result type:

| Callee type | Result type |
|---|---|
| `FunctionType` | The function's `returnType` |
| `ClassType` | `InstanceType` wrapping the class (constructor call) |
| Named builtin (see table below) | Specific return type |
| Anything else | `<unknown>` |

#### Named Builtin Return Types

When the callee is a `NameExpr`, the type checker has hardcoded return type
knowledge for many builtins:

| Builtin name(s) | Return type |
|---|---|
| `chr`, `hex`, `oct`, `bin`, `str`, `repr`, `ascii`, `format` | `str` |
| `ord`, `len`, `abs`, `round`, `hash`, `id`, `int` | `int` |
| `float` | `float` |
| `bool`, `isinstance`, `issubclass`, `callable`, `hasattr`, `any`, `all` | `bool` |
| `sorted`, `reversed`, `list`, `enumerate`, `zip`, `map`, `filter`, `divmod` | `list[Any]` |
| `min`, `max`, `sum` | First argument's type (or element type if argument is a list), defaults to `int` |
| `print` | `None` |
| `input` | `str` |
| `type` | `str` (simplified) |
| `pow` | `int` |
| `range` | `list[int]` |

### 3.6 `AttributeExpr` Inference

Attribute access type depends on the object's type:

#### Instance attributes

If the object type is `InstanceType`:
1. Look up `attribute` in the class's `fields` map. If found, use that type.
2. Look up `attribute` in the class's `methods` map. If found, use that type.
3. Otherwise, use `<unknown>`.

#### String methods

When the object type is `str`, the type checker has hardcoded knowledge of
all string methods:

| Methods | Return type |
|---|---|
| `upper`, `lower`, `strip`, `lstrip`, `rstrip`, `replace`, `join`, `format`, `title`, `capitalize`, `swapcase`, `center`, `ljust`, `rjust`, `zfill`, `removeprefix`, `removesuffix`, `expandtabs`, `casefold` | `(Any) -> str` |
| `find`, `index`, `rfind`, `rindex`, `count` | `(str) -> int` |
| `startswith`, `endswith`, `isdigit`, `isalpha`, `isalnum`, `isspace`, `isupper`, `islower`, `istitle`, `isnumeric`, `isdecimal`, `isascii`, `isidentifier`, `isprintable` | `(str) -> bool` |
| `split`, `rsplit`, `splitlines`, `partition`, `rpartition` | `(str) -> list[str]` |
| `encode` | `(str) -> bytes` |

#### List methods

When the object type is `list[T]`:

| Method | Return type |
|---|---|
| `append`, `insert`, `extend`, `remove` | `(T) -> None` |
| `pop` | `() -> T` |
| `sort`, `reverse`, `clear` | `() -> None` |
| `copy` | `() -> list[T]` |
| `count`, `index` | `(T) -> int` |

#### Dict methods

When the object type is `dict[K, V]`:

| Method | Return type |
|---|---|
| `get`, `pop`, `setdefault` | `(K) -> V` |
| `keys` | `() -> list[K]` |
| `values` | `() -> list[V]` |
| `clear`, `update` | `() -> None` |
| `copy` | `() -> dict[K, V]` |

### 3.7 `SubscriptExpr` Inference

| Object type | Is slice? | Result type |
|---|---|---|
| `list[T]` | No | `T` |
| `list[T]` | Yes | `list[T]` |
| `dict[K, V]` | No | `V` |
| `tuple[...]` | -- | `<unknown>` (not yet implemented) |
| `str` | Either | `str` |
| Anything else | -- | `<unknown>` |

Slice detection: checks if `node.index` is a `SliceExpr` via `dynamic_cast`.

### 3.8 Collection Literal Inference

Collection-literal inference is governed by honest types (commandment #3): a
list/set literal is monomorphic, and a dict literal is monomorphic in its key
type. Inference does **not** take the first element and run; it unifies across
all elements and refuses to widen.

#### Lists and sets: `unifyLiteralElements`

`visit(ListExpr&)` and `visit(SetExpr&)` (sets use the list representation)
infer the element type by calling `unifyLiteralElements`, which requires an
**exact** element-type match across **every** element:

```cpp
static std::shared_ptr<Type> unifyLiteralElements(
    const std::vector<std::unique_ptr<Expr>>& elems,
    const std::shared_ptr<Type>& unknown) {
    std::shared_ptr<Type> u;
    for (const auto& e : elems) {
        auto t = e ? e->type : nullptr;
        if (!t || t->kind() == Type::Kind::Unknown) continue;
        if (!u) { u = t; continue; }
        if (!t->equals(*u)) return unknown;   // mixed -> ambiguous
    }
    return u ? u : unknown;
}
```

Subtyping is deliberately **not** used: `[1, 2.0]` is not folded to a numeric
type (the list-literal codegen does not bit-coerce mismatched scalar elements,
so a widened list would silently miscompile). A mixed literal therefore yields
element type `Unknown`, leaving the literal as `list[<unknown>]` -
disambiguating it is the annotation's job (`list[float]` / `list[Any]`).

#### Dicts: monomorphic key type

`visit(DictExpr&)` seeds `K`/`V` from the first entry that pins them (an
explicit `k: v` entry, or a `**spread` source whose dict type seeds `K`/`V`),
then checks **every** explicit key against the seeded key type. A heterogeneous
key type is a compile error, not a silent first-key inference:

```
"dict literal mixes key types '<K1>' and '<K2>' - a dict is monomorphic in its key type"
```

The check is skipped when either side is `Unknown` or the seeded key type is
`Any` (a deliberately heterogeneous annotation).

#### Summary

| Expression | Rule |
|---|---|
| `ListExpr` (empty) | `list[<unknown>]` (kept only if an annotation later propagates a concrete element type; see below) |
| `ListExpr` (non-empty) | `list[T]` via `unifyLiteralElements`; mixed -> `list[<unknown>]` |
| `TupleExpr` | `tuple[T1, T2, ...]` where each `Ti` is inferred from the corresponding element |
| `DictExpr` (empty) | `dict[<unknown>, <unknown>]` (annotation-propagated as for lists) |
| `DictExpr` (non-empty) | `dict[K, V]` seeded from the first pinning entry; mixed key types are a compile error |
| `SetExpr` (empty) | `list[<unknown>]` (sets use list representation) |
| `SetExpr` (non-empty) | `list[T]` via `unifyLiteralElements`; mixed -> `list[<unknown>]` |

#### Empty / mixed inferred bindings are a compile error

An empty `list[<unknown>]` / `dict[<unknown>, ...]` survives only when an
annotation propagates a concrete element type (e.g. `x: list[int] = []`, which
is an `AnnAssignStmt`, not a walrus). An **inferred** binding whose element
type stays unresolved is rejected. `visit(WalrusExpr&)` checks the inferred
container element type and, if it is `Unknown`, emits:

```
"cannot infer the element type of '<x>' (an empty or mixed-type literal) - annotate it (e.g. `<x>: list[int] = []`)"
```

The reasoning: a silently-bound `list[<unknown>]` / `dict[<unknown>, <unknown>]`
is a de-facto `Any` container - it accepts any element, so every insert boxes
on the hot path and the box leaks. Ambiguity is a compile error to annotate
away, not a box to paper over.

### 3.9 Comprehension Inference

| Expression | Result type |
|---|---|
| `ListCompExpr` | `list[T]` where `T` is the inferred type of `element` |
| `DictCompExpr` | `dict[K, V]` where `K` and `V` are inferred from `key` and `value` |

### 3.10 `LambdaExpr` Inference

Resolves parameter types and return type from annotations, produces a
`FunctionType(paramTypes, returnType)`.

### 3.11 `IfExpr` (Ternary) Inference

1. Infer condition type (not used for result).
2. Infer `thenExpr` type and `elseExpr` type.
3. If both branches have the same type: result is that type.
4. Otherwise: result is `UnionType([thenType, elseType])`.

### 3.12 Other Expressions

| Expression | Inferred type |
|---|---|
| `AwaitExpr` | `<unknown>` |
| `YieldExpr` | `<unknown>` |
| `StarredExpr` | `<unknown>` |
| `SliceExpr` | `<unknown>` |

---

## 4. Subtyping Rules in Detail

### 4.1 `isSubtypeOf()` Summary Table

| Source Type | Target Type | Result | Reason |
|---|---|---|---|
| `T` | `T` | `true` | Reflexivity (via `equals`) |
| Any `T` | `Any` | `true` | Any is the top type |
| `Any` | Any `T` | `true` | Any overrides to always return true |
| `Never` | Any `T` | `true` | Never overrides to always return true |
| `Unknown` | Any `T` | `true` | Unknown overrides to always return true |
| `bool` | `int` | `true` | `PrimitiveType::isSubtypeOf` explicit rule |
| `int` | `float` | `true` | `PrimitiveType::isSubtypeOf` explicit rule |
| `bool` | `float` | `true` | Transitive, explicitly coded |
| `int` | `bool` | `false` | No such rule |
| `float` | `int` | `false` | No such rule |
| `str` | `int` | `false` | No cross-family subtyping |
| `T` | `T1 \| T2` | `true` | If `T <: T1` or `T <: T2` (base class handles unions) |
| `T1 \| T2` | `U` | `true` | If `T1 <: U` AND `T2 <: U` (UnionType override) |
| `list[bool]` | `list[int]` | `false` | Lists are invariant (no subtyping override) |
| `dict[K1,V1]` | `dict[K2,V2]` | `false` | Dicts are invariant unless exactly equal |
| `ClassType("A")` | `ClassType("A")` | `true` | Nominal equality |
| `ClassType("A")` | `ClassType("B")` | `false` | No structural subtyping |

### 4.2 `isAssignableTo()` vs `isSubtypeOf()`

Currently identical:

```cpp
bool Type::isAssignableTo(const Type& other) const {
    return isSubtypeOf(other);
}
```

Both are used in assignment and return type checking. The separate method
exists to allow future divergence.

---

## 5. Binary Operator Type Rules

This section documents every binary operator's type inference behavior as
implemented in `visit(BinaryExpr&)`.

### 5.1 Arithmetic Operators: `+`, `-`, `*`, `/`, `%`, `**`, `//`

#### String Concatenation (`+`)

```
str + str -> str
```

Checked first, before numeric rules.

#### String Repetition (`*`)

```
str * int  -> str
int * str  -> str
str * bool -> str    (because bool <: int)
bool * str -> str    (because bool <: int)
```

Uses `isSubtypeOf(*intType)` for the integer check.

#### True Division (`/`)

```
int   / int   -> float
int   / float -> float
float / int   -> float
float / float -> float
bool  / int   -> float
int   / bool  -> float
```

Always returns `float`, regardless of operand types. This matches Python 3
true division semantics.

#### Floor Division (`//`)

```
int   // int   -> int
int   // float -> float
float // int   -> float
float // float -> float
bool  // bool  -> int     (because both <: int, neither is float)
bool  // int   -> int     (same reasoning)
```

Returns `int` if both operands are `int` (or subtypes of `int` that are
not `float`). Returns `float` if either operand is `float`.

#### Other Arithmetic (`+`, `-`, `*`, `%`, `**`)

```
int   + int   -> int
int   + float -> float
float + int   -> float
float + float -> float
bool  + bool  -> int     (bool <: int, neither is float)
bool  + int   -> int
bool  + float -> float
```

General rule: if either operand is `float`, result is `float`. Otherwise
(both are subtypes of `int`), result is `int`.

#### Type Errors

If operands are not numeric (and not the string special cases), and neither
is `Unknown` or `Any`, an error is emitted:

```
"unsupported operand types for <op>: '<leftType>' and '<rightType>'"
```

If either operand is `Unknown` or `Any`, the result is `<unknown>` with no
error.

### 5.2 Comparison Operators: `==`, `!=`, `<`, `<=`, `>`, `>=`

```
T1 == T2 -> bool
T1 != T2 -> bool
T1 <  T2 -> bool
T1 <= T2 -> bool
T1 >  T2 -> bool
T1 >= T2 -> bool
```

All comparison operators unconditionally return `bool`. No operand type
checking is performed (any types can be compared).

### 5.3 Logical Operators: `and`, `or`

```
T1 and T2 -> T2    (simplified: returns right operand type)
T1 or  T2 -> T1    (simplified: returns left operand type)
```

This is a simplification of Python's actual short-circuit semantics where
`and` returns the first falsy value or the last value, and `or` returns the
first truthy value or the last value. For type checking purposes, the type
checker returns the right type for `and` and the left type for `or`.

### 5.4 Bitwise Operators: `&`, `|`, `^`, `<<`, `>>`

```
int  & int  -> int
int  | int  -> int
int  ^ int  -> int
int  << int -> int
int  >> int -> int
bool & bool -> int    (because bool <: int)
bool | int  -> int
```

Both operands must be subtypes of `int`. If not (and neither is
`Unknown`/`Any`), an error is emitted:

```
"unsupported operand types for <op>: '<leftType>' and '<rightType>'"
```

### 5.5 Membership and Identity: `in`, `is`, `not`

```
T1 in T2     -> bool
T1 is T2     -> bool
T1 not in T2 -> bool    (handled via NOT token)
T1 is not T2 -> bool    (handled via NOT token)
```

All return `bool` unconditionally.

### 5.6 Complete Operator Type Matrix

| Operator | Left | Right | Result |
|---|---|---|---|
| `+` | `str` | `str` | `str` |
| `+` | `int` | `int` | `int` |
| `+` | `int` | `float` | `float` |
| `+` | `float` | `int` | `float` |
| `+` | `float` | `float` | `float` |
| `+` | `bool` | `bool` | `int` |
| `+` | `int` | `str` | **error** |
| `-` | `int` | `int` | `int` |
| `-` | `int`/`float` | `int`/`float` | `float` if any float, else `int` |
| `*` | `str` | `int` | `str` |
| `*` | `int` | `str` | `str` |
| `*` | `int` | `int` | `int` |
| `*` | `int`/`float` | `int`/`float` | `float` if any float, else `int` |
| `/` | numeric | numeric | `float` (always) |
| `//` | `int` | `int` | `int` |
| `//` | `int`/`float` | `int`/`float` | `float` if any float, else `int` |
| `%` | numeric | numeric | `float` if any float, else `int` |
| `**` | numeric | numeric | `float` if any float, else `int` |
| `==`,`!=`,`<`,`<=`,`>`,`>=` | any | any | `bool` |
| `and` | any | any | right type |
| `or` | any | any | left type |
| `&`,`\|`,`^`,`<<`,`>>` | `int` | `int` | `int` |
| `in`, `is`, `not` | any | any | `bool` |

---

## 6. Function Type Checking

### `visit(FunctionDecl&)`

1. **Build the function type:** resolve each parameter's type annotation via
   `resolveType()` and the return type.

   ```cpp
   std::vector<std::shared_ptr<Type>> paramTypes;
   for (auto& p : node.params) {
       paramTypes.push_back(resolveType(p.type.get()));
   }
   auto retType = resolveType(node.returnType.get());
   auto funcType = std::make_shared<FunctionType>(paramTypes, retType);
   ```

2. **Define the function** in the current scope: `impl_->define(node.name, funcType)`.

3. **Push a new scope** for the function body.

4. **Push return type** onto `impl_->returnTypeStack` so `ReturnStmt` can
   validate against it.

5. **Define parameters** in the function scope, each with its resolved type.

6. **Visit the body.**

7. **Pop the return type stack** and the scope.

### Return Type Checking (`visit(ReturnStmt&)`)

If the return type stack is non-empty:

- **`return value`**: infer the value's type, check
  `retType->isAssignableTo(expected)`. If not assignable and neither is
  `Unknown`, emit:
  ```
  "return type '<actual>' does not match declared return type '<expected>'"
  ```

- **`return` (no value)**: if expected is not `None` and not `Unknown`, emit:
  ```
  "return without value in function returning '<expected>'"
  ```

---

## 7. Class Type Checking

### `visit(ClassDecl&)`

1. **Create a `ClassType`** with the class name.

2. **Register in `typeNames`:** `impl_->typeNames[node.name]` is set to
   `InstanceType(classType)`. This means type annotations like `: MyClass`
   will resolve to instances, not the class object.

3. **Define in scope:** `impl_->define(node.name, classType)`. This means
   the variable `MyClass` has type `ClassType`.

4. **Push a new scope** for the class body.

5. **Define `self`:** `impl_->define("self", InstanceType(classType))`.
   This makes `self` available with the correct type inside all methods.

6. **Visit the body.** After visiting each statement, if it is a
   `FunctionDecl`, look up its type and add it to `classType->methods`.

   ```cpp
   for (auto& s : node.body) {
       s->accept(*this);
       if (auto* func = dynamic_cast<FunctionDecl*>(s.get())) {
           auto fType = impl_->lookup(func->name);
           if (fType) {
               classType->methods[func->name] = fType;
           }
       }
   }
   ```

7. **Pop the scope.**

### Constructor Calls

When `CallExpr` encounters a callee with `ClassType`, it produces an
`InstanceType`:

```cpp
if (calleeType->kind() == Type::Kind::Class) {
    auto& ct = static_cast<ClassType&>(*calleeType);
    node.type = std::make_shared<InstanceType>(
        std::static_pointer_cast<ClassType>(calleeType));
    return;
}
```

### Method Dispatch

When `AttributeExpr` encounters an `InstanceType` object, it looks up the
attribute in the class's `fields` map first, then `methods` map:

```cpp
if (objType->kind() == Type::Kind::Instance) {
    auto& inst = static_cast<InstanceType&>(*objType);
    auto it = inst.classType->fields.find(node.attribute);
    if (it != inst.classType->fields.end()) {
        node.type = it->second;
        return;
    }
    auto mit = inst.classType->methods.find(node.attribute);
    if (mit != inst.classType->methods.end()) {
        node.type = mit->second;
        return;
    }
}
```

---

## 8. Cross-Module Types

### `registerExternalModule()`

```cpp
void TypeChecker::registerExternalModule(
    const std::string& moduleName,
    const std::unordered_map<std::string, std::shared_ptr<Type>>& exports);
```

Must be called **before** `check()`. Stores the module's exports in
`impl_->externalModules[moduleName]`.

### `getExports()`

```cpp
std::unordered_map<std::string, std::shared_ptr<Type>> TypeChecker::getExports() const;
```

Returns all user-defined symbols from the module scope, filtering out
builtins. The builtins filter is a static set:

```cpp
static const std::set<std::string> builtins = {
    "print", "len", "range", "input", "int", "float", "str", "bool",
    "abs", "min", "max", "sum", "sorted", "reversed", "enumerate",
    "zip", "map", "filter", "isinstance", "type", "list", "dict",
    "set", "tuple", "open", "hasattr", "getattr", "setattr", "repr",
    "hash", "id", "hex", "oct", "bin", "ord", "chr",
    "True", "False", "None"
};
```

Exports are captured from `impl_->scopes[0].bindings` (the module scope)
right before the module scope is popped at the end of `check()`.

### `FromImportStmt` Type Resolution

When visiting `from module import name`:

1. Look up `node.module` in `impl_->externalModules`.
2. If found, for each imported name:
   - Look up the name in the module's exports.
   - If found, define it in the current scope with the exported type.
   - If not found, emit:
     ```
     "cannot import name '<name>' from module '<module>'"
     ```
3. If the module is not registered, silently skip (no error).

### Cross-Module Type Flow

The `ModuleResolver` component (not part of the type checker) orchestrates
multi-file compilation. The typical flow is:

1. Parse and type-check the dependency module.
2. Call `getExports()` on the dependency's `TypeChecker`.
3. Call `registerExternalModule()` on the importing module's `TypeChecker`.
4. Type-check the importing module.

---

## 9. The `Impl` Struct

```cpp
struct TypeChecker::Impl {
    std::vector<TypeDiagnostic> diagnostics;

    // Built-in type singletons
    std::shared_ptr<PrimitiveType> intType;
    std::shared_ptr<PrimitiveType> floatType;
    std::shared_ptr<PrimitiveType> boolType;
    std::shared_ptr<PrimitiveType> strType;
    std::shared_ptr<PrimitiveType> bytesType;
    std::shared_ptr<PrimitiveType> noneType;
    std::shared_ptr<AnyType> anyType;
    std::shared_ptr<NeverType> neverType;
    std::shared_ptr<UnknownType> unknownType;

    // Type name -> type mapping for resolution
    std::unordered_map<std::string, std::shared_ptr<Type>> typeNames;

    // Type environment (variable name -> type) with scope stack
    struct Scope {
        std::unordered_map<std::string, std::shared_ptr<Type>> bindings;
    };
    std::vector<Scope> scopes;

    // Function return type stack (for checking return statements)
    std::vector<std::shared_ptr<Type>> returnTypeStack;

    // External module exports: moduleName -> (symbolName -> type)
    std::unordered_map<std::string,
        std::unordered_map<std::string, std::shared_ptr<Type>>> externalModules;

    // Imported-module reference types: canonical name -> ModuleType, created
    // on demand by getOrCreateModuleType().
    std::unordered_map<std::string, std::shared_ptr<ModuleType>> moduleTypes;

    // Cached module-level exports (captured before scope is popped)
    std::unordered_map<std::string, std::shared_ptr<Type>> cachedExports;

    void pushScope();
    void popScope();
    void define(const std::string& name, std::shared_ptr<Type> type);
    std::shared_ptr<Type> lookup(const std::string& name);
};
```

Only the primitives plus `Any` / `Never` / `Unknown` are cached as `Impl`
singletons. The newer type classes are **not** singleton members: `PtrType`
and `TaskType` are constructed when needed and registered in `typeNames`
(`"ptr"` -> `PtrType`, `"Task"` -> `TaskType(anyType)`, i.e. bare `Task`
resolves to `Task[Any]`); `LockType` is import-gated (Python parity: bound
only by `from threading import Lock`, not seeded in `typeNames`); and
`ModuleType` values live in the `moduleTypes` map, created on demand via
`getOrCreateModuleType()`.

### Key Differences from Sema's Scope Model

| Aspect | Sema | TypeChecker |
|---|---|---|
| Scope representation | `Scope` class with parent pointer and symbol map | Flat `Scope` struct with bindings map, stored in a vector |
| Symbol data | `Symbol` struct (name, kind, type, flags) | Just a type (`shared_ptr<Type>`) |
| Lookup | Recursive parent traversal via pointer chain | Linear scan from innermost to outermost index |
| Scope kinds | Module, Class, Function, Block | Undifferentiated (all scopes are the same) |

### Scope Operations

```cpp
void pushScope() { scopes.push_back({}); }
void popScope() { if (!scopes.empty()) scopes.pop_back(); }

void define(const std::string& name, std::shared_ptr<Type> type) {
    if (!scopes.empty()) {
        scopes.back().bindings[name] = std::move(type);
    }
}

std::shared_ptr<Type> lookup(const std::string& name) {
    for (int i = static_cast<int>(scopes.size()) - 1; i >= 0; --i) {
        auto it = scopes[i].bindings.find(name);
        if (it != scopes[i].bindings.end()) return it->second;
    }
    return nullptr;
}
```

### `returnTypeStack`

A stack of expected return types, one per enclosing function. Pushed when
entering a `FunctionDecl`, popped when leaving. Used by `visit(ReturnStmt&)`
to validate that returned values match the declared return type.

### `cachedExports`

Snapshot of the module scope's bindings, captured at the end of `check()`
right before the scope is popped. This is necessary because `getExports()`
may be called after `check()` completes.

### `initBuiltinTypes()`

Called by the constructor. Creates singleton type objects:

```cpp
void TypeChecker::initBuiltinTypes() {
    impl_->intType = std::make_shared<PrimitiveType>(Type::Kind::Int);
    impl_->floatType = std::make_shared<PrimitiveType>(Type::Kind::Float);
    impl_->boolType = std::make_shared<PrimitiveType>(Type::Kind::Bool);
    impl_->strType = std::make_shared<PrimitiveType>(Type::Kind::Str);
    impl_->bytesType = std::make_shared<PrimitiveType>(Type::Kind::Bytes);
    impl_->noneType = std::make_shared<PrimitiveType>(Type::Kind::None_);
    impl_->anyType = std::make_shared<AnyType>();
    impl_->neverType = std::make_shared<NeverType>();
    impl_->unknownType = std::make_shared<UnknownType>();

    impl_->typeNames["int"] = impl_->intType;
    impl_->typeNames["float"] = impl_->floatType;
    impl_->typeNames["bool"] = impl_->boolType;
    impl_->typeNames["str"] = impl_->strType;
    impl_->typeNames["bytes"] = impl_->bytesType;
    impl_->typeNames["None"] = impl_->noneType;
    impl_->typeNames["Any"] = impl_->anyType;
    impl_->typeNames["Never"] = impl_->neverType;
    impl_->typeNames["object"] = impl_->anyType;
}
```

---

## 10. Error Reporting

### `TypeDiagnostic` Struct

```cpp
struct TypeDiagnostic {
    enum class Level { Warning, Error };
    Level level;
    SourceLocation location;
    std::string message;
};
```

Identical in structure to `SemaDiagnostic`.

### Helper Methods

```cpp
void TypeChecker::error(const SourceLocation& loc, const std::string& message) {
    impl_->diagnostics.push_back({TypeDiagnostic::Level::Error, loc, message});
}

void TypeChecker::warning(const SourceLocation& loc, const std::string& message) {
    impl_->diagnostics.push_back({TypeDiagnostic::Level::Warning, loc, message});
}
```

### `hasErrors()`

```cpp
bool TypeChecker::hasErrors() const {
    for (const auto& d : impl_->diagnostics) {
        if (d.level == TypeDiagnostic::Level::Error) return true;
    }
    return false;
}
```

### Complete List of Error Messages

| Error message | Emitted by |
|---|---|
| `"unknown type '<name>'"` | `resolveType()` for `NamedTypeExpr` when the name is not in `typeNames` or scope |
| `"invalid generic type"` | `resolveType()` for `GenericTypeExpr` with non-`NamedTypeExpr` base |
| `"unknown generic type '<name>'"` | `resolveType()` for `GenericTypeExpr` with unrecognized base name |
| `"cannot assign '<valueType>' to variable of type '<annotType>'"` | `visit(AssignStmt&)` and `visit(AnnAssignStmt&)` when value type is not assignable to annotation type |
| `"unsupported operand types for <op>: '<left>' and '<right>'"` | `visit(BinaryExpr&)` for arithmetic or bitwise operators with incompatible types |
| `"bad operand type for unary <op>: '<type>'"` | `visit(UnaryExpr&)` for non-numeric operands to `+`/`-` or non-integer to `~` |
| `"return type '<actual>' does not match declared return type '<expected>'"` | `visit(ReturnStmt&)` when returned value type does not match |
| `"return without value in function returning '<type>'"` | `visit(ReturnStmt&)` when returning nothing from a non-None function |
| `"cannot import name '<name>' from module '<module>'"` | `visit(FromImportStmt&)` when an imported name is not in the external module's exports |

### Error Suppression

The type checker deliberately suppresses errors in several cases to avoid
cascading diagnostics:

- When either operand of a binary expression has `Unknown` or `Any` type,
  no error is emitted.
- When either side of an assignment has `Unknown` type, the assignability
  check is skipped.
- When return type or expected type is `Unknown`, the return type check is
  skipped.

---

## 11. Statement Visiting

### `AssignStmt`

1. Infer the value's type.
2. If there is a type annotation:
   a. Resolve the annotation type.
   b. Check that `valueType->isAssignableTo(annotType)`.
   c. Define each `NameExpr` target with the **annotation** type.
3. If there is no annotation:
   a. Define each `NameExpr` target with the **inferred value** type.
4. Infer types for all targets (for non-`NameExpr` targets).

### `AnnAssignStmt`

1. Resolve the annotation type.
2. If there is a value, infer its type and check assignability.
3. Define the target with the annotation type.

### `AugAssignStmt`

1. Infer target and value types.
2. No type checking is performed (the rules follow binary operator rules,
   but this is not yet implemented).

### `ForStmt`

1. Infer iterable type.
2. If target is a `NameExpr`:
   - If iterable is `list[T]`, define the variable with type `T`.
   - Otherwise, define with `<unknown>`.
3. Visit body and else body.

### `IfStmt`, `WhileStmt`

Infer condition type, visit all bodies.

### `TryStmt`

Visit try body, all handler bodies, else body, and finally body. No type
checking of exception types.

### `WithStmt`

Infer context expression types, visit body.

### Control Flow Statements

| Statement | TypeChecker behavior |
|---|---|
| `ReturnStmt` | See Section 6 |
| `BreakStmt` | No-op |
| `ContinueStmt` | No-op |
| `PassStmt` | No-op |
| `GlobalStmt` | No-op |
| `NonlocalStmt` | No-op |
| `DeleteStmt` | Infer types of all targets |
| `AssertStmt` | Infer types of test and message |
| `RaiseStmt` | Infer types of exception and cause |
| `ImportStmt` | No-op |
| `FromImportStmt` | Resolves types from registered external modules |

---

## 12. Builtin Type Definitions in `check()`

When `check()` is called, it creates a module scope and populates it with
typed builtin definitions. Unlike Sema (which only records names), the
TypeChecker assigns full `FunctionType` objects:

| Name | Type definition |
|---|---|
| `print` | `(Any) -> None` |
| `len` | `(Any) -> int` |
| `range` | `(int) -> list[int]` |
| `input` | `(str) -> str` |
| `int` | `(Any) -> int` |
| `float` | `(Any) -> float` |
| `str` | `(Any) -> str` |
| `bool` | `(Any) -> bool` |
| `abs` | `(Any) -> int` |
| `min` | `(Any) -> Any` |
| `max` | `(Any) -> Any` |
| `type` | `(Any) -> Any` |
| `isinstance` | `(Any, Any) -> bool` |
| `enumerate` | `(Any) -> Any` |
| `zip` | `(Any) -> Any` |
| `map` | `(Any, Any) -> Any` |
| `filter` | `(Any, Any) -> Any` |
| `sorted` | `(Any) -> Any` |
| `reversed` | `(Any) -> Any` |
| `True` | `bool` (value, not function) |
| `False` | `bool` (value, not function) |
| `None` | `None` (value, not function) |

Note that many builtins have simplified signatures (e.g., `print` taking
a single `Any` argument instead of `*args`). The hardcoded return type
fallbacks in `visit(CallExpr&)` provide additional precision beyond what
these signatures offer.

---

## 13. Pipeline Position

```
Source code
    |
    v
  Lexer  (Token stream)
    |
    v
  Parser (AST)
    |
    v
  Sema   (name resolution, scope analysis)
    |
    v
  TypeChecker (type inference, type checking)   <--- this pass
    |
    v
  CodeGen (LLVM IR generation)
```

The TypeChecker reads the AST (unchanged by Sema) and **mutates** it by
setting the `type` field on every `Expr` node. These type annotations are
then available to the CodeGen pass for generating correctly typed LLVM IR.

---

## Previous Document

[005 - Semantic Analysis (Sema)](005-sema.md)

## Next Document

[007 - LLVM Code Generator (CodeGen)](007-codegen.md)
