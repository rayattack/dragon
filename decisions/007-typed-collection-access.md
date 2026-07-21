# Decision 007: Type-Safe Collections [Partially Implemented]

Accepted. 3 of 4 components done; `cast[T]` still pending. Affects TypeChecker, CodeGen (LLVM; CEmitter retired), Parser (minimal). Composes with boxed `Union[...]` and monomorphized typed containers from Phases 3 & 4.

Quick status before the design bits:

| Component | Status | Notes |
|-----------|--------|-------|
| Union element type inference | DONE | Union types (`int \| str` etc.) implemented; storage is `{i64 tag, i64 payload}` box (Phase 4). |
| `cast[T](expr)` via `from typing import cast` | TODO | Still the open work item from this decision. |
| `isinstance` type narrowing | DONE | Compile-time narrowing for union vars in `if isinstance(x, T):` branches. After Phase 4 the narrowed scope binds the box payload to a fresh native-typed alloca, so the body operates on T's native LLVM type directly. Non-union `isinstance` still TODO. |
| TypedDict | DONE | `class Foo(TypedDict)` with per-key type tracking and runtime-checked access via `dragon_dict_get_checked`. |
| Runtime checked dict access | DONE | `dragon_dict_get_checked(dict, key, expected_tag)` - tag mismatch raises TypeError (exc code 80). |

---

Our collections lie about their types and it drives me nuts. Example:

```python
coll = ['a', 'b', True, 1] # Typed as list[str] - first element wins
config = {"host": "localhost", "port": 8080, "debug": True} # dict[str, str]
```

Lists are `int64_t*` internally - type inferred from first element only. Dicts are `const char** keys` + `int64_t* values`, same problem. Accessing `coll[2]` returns `str` to the type checker even though it's `True` at runtime. `config["port"]` returns `str` even though it's `8080`. Can't write type-safe code for JSON, configs, mixed collections until we fix this.

---

## Rejected Syntax: `coll[index : Type]`

We considered and rejected `coll[0:str]`, `config["port":int]`, and `data.get('key':bool, True)` syntax. Reasons:

1. **Colon overload.** `:` already has 5 meanings in Dragon (type annotation, slice bounds, slice step, dict literal, block start). A 6th creates cognitive load - `x[a:b]` becomes ambiguous between slice and typed access.
2. **Parser needs type knowledge.** Disambiguating `coll[0:str]` (typed access) from `coll[0:n]` (slice) requires knowing whether the token after `:` is a type or variable. Parsing should be context-free.
3. **No safety in Phase 1.** Without runtime type tags, `coll[0:str]` generates identical C to `coll[0]`. Assignment annotations (`name: str = coll[0]`) already provide the same trust-based assertion.
4. **`data.get('key':bool, True)` is confusing.** Inside a function call, `'key':bool` looks like a dict entry or keyword annotation. The default value already implies the type.

If you can't explain it in one sentence, skip it.

---

## Accepted Approach

Four features that together solve all use cases without syntax ambiguity.

### 1. Union Element Type Inference (Priority 1)

**Problem it solves:** Lists and dicts lie about their element types.

**Today:**
```python
coll = ['a', 'b', True, 1] # Inferred as list[str] - wrong
```

**After:**
```python
coll = ['a', 'b', True, 1] # Inferred as list[str | bool | int]
```

Accessing `coll[0]` returns `str | bool | int`. To narrow, use `isinstance` (see below).

**Implementation:**
- TypeChecker `visit(ListExpr&)`: collect all unique element types, build `UnionType`
- TypeChecker `visit(DictExpr&)`: same for value types
- No parser changes
- Effort: ~1 week, ~15 tests

**Why first:** Cheapest change (TypeChecker-only), fixes the fundamental "first element wins" lie that makes all other type-safe collection work possible.

---

### 2. `cast[T](expr)` via typing import (Priority 2)

**Problem it solves:** Explicit type assertion when the programmer knows more than the type checker.

**Usage:**
```python
from typing import cast

config = {"host": "localhost", "port": 8080}
port: int = cast[int](config["port"])
name: str = cast[str](coll[0])
```

**Why `from typing import cast` (not a builtin):**
- Matches Python's `from typing import cast`. Developers already know this pattern.
- Makes it explicit that this is a type-level operation, not a runtime function.
- Keeps the global namespace clean - you import it when you need it.
- Signals intent: "I am doing something the type checker can't verify."

**Why `cast[int](x)` not `cast(int, x)`:**
- Dragon already has generic syntax (`list[int]`, `dict[str, int]`). `cast[int](x)` is consistent.
- The bracket form makes clear that `int` is a type parameter, not a runtime value.
- `cast(int, x)` is ambiguous - is `int` the type or the `int` constructor?

**Implementation:**
- Parser: no changes needed. `cast[int](x)` parses as a generic call expression, which the parser already supports.
- TypeChecker `visit(CallExpr&)`: recognize `cast` (when imported from `typing`) as a special form. The generic argument `[T]` becomes the return type.
- CEmitter: emit a C cast based on `T` (e.g., `(int64_t)expr`, `(const char*)expr`).
- Effort: ~1 week, ~10 tests

**Generated C:**
```c
// cast[int](config["port"])
(int64_t)dragon_dict_get(config, "port");

// cast[str](coll[0])
(const char*)dragon_list_get(coll, 0);
```

---

### 3. `isinstance` Type Narrowing (Priority 3)

**Problem it solves:** Safe type dispatch on union types.

**Usage:**
```python
coll = ['a', 'b', True, 1] # list[str | bool | int]
item = coll[0] # str | bool | int

if isinstance(item, str) {
 print(item.upper) # Type checker knows item is str here
}
if isinstance(item, int) {
 print(item + 1) # Type checker knows item is int here
}
```

**Why a builtin (not an import):**
- `isinstance` is a runtime operation - it inspects the value. This is fundamentally different from `cast`, which is a compile-time assertion.
- Python developers expect `isinstance(x, int)` to work without imports.
- It's the foundation for type narrowing - the type checker uses it to refine types in branches.

**Phase 1: Compile-time narrowing only.**
The type checker narrows the type inside the `if` branch, but no runtime check is emitted. This works for code where the programmer knows the type at a given point. The generated C is the same as without the isinstance check - it's purely a type-checker directive.

```c
// if isinstance(item, str) { print(item.upper) }
// Phase 1: no runtime check, just narrows the type for the branch
{
 // item is treated as const char* in this scope
 dragon_print_str(dragon_str_upper((const char*)item));
}
```

**Phase 2: Runtime checks (requires tagged values).**
With NaN-boxing or discriminated unions, `isinstance` can emit actual runtime type checks:

```c
// Phase 2: with tagged values
if (dragon_value_is_str(item)) {
 dragon_print_str(dragon_str_upper(dragon_value_as_str(item)));
}
```

This is deferred until tagged values exist (Phase 2+).

**Implementation (Phase 1):**
- TypeChecker `visit(IfStmt&)`: detect `isinstance(expr, Type)` in condition, narrow `expr`'s type to `Type` within the then-branch
- TypeChecker: support `isinstance(expr, (T1, T2))` tuple form for multiple types
- CEmitter: emit the branch body with the narrowed type, no runtime check
- Effort: ~1-2 weeks, ~20 tests

---

### 4. TypedDict (Priority 4)

**Problem it solves:** Known-schema dicts with per-key types.

**Usage:**
```python
class Config(TypedDict) {
 host: str
 port: int
 debug: bool
 workers: int
}

config: Config = {
 "host": "localhost",
 "port": 8080,
 "debug": True,
 "workers": 4
}

# Type checker KNOWS each key's type:
host: str = config["host"] # Returns str
port: int = config["port"] # Returns int
debug: bool = config["debug"] # Returns bool
```

**Implementation:**
- Recognize `TypedDict` as a special base class in the TypeChecker
- `ClassDecl` with `TypedDict` base stores field name -> type map
- `SubscriptExpr` on a `TypedDict` instance looks up the key's string literal -> returns the field type
- CEmitter: no change (still `dragon_dict_get` under the hood)
- Effort: ~2-3 weeks, ~30 tests

**Why last:** Biggest effort, and the combination of union inference + cast + isinstance covers most practical use cases. TypedDict is the proper solution for config objects and API responses, but it's not blocking.

**Representation: dict-backed, not struct (trade-off).**

A TypedDict has a fully static schema (fixed keys, fixed per-key types), so in
principle it *could* lower to a struct - contiguous fields at fixed offsets, the
way a `class` instance does - making `td["k"]` a single `GEP + load`. Dragon
deliberately keeps it **dict-backed** instead. The reasoning, recorded here so it
isn't rediscovered:

- **Current repr.** A `TypedDict` value is an ordinary `DragonDict` with string
 keys and tagged values. `td["k"]` (literal key) lowers to
 `dragon_dict_get_checked(dict, "k", expected_tag)` - hash + probe + tag check
 (the ~312 ns tagged-dict tier; see 's `dict[str, Any]` /
 `map[string]interface{}` model). Construction (`Config(...)`, `Config({...})`,
 `Customer(**row)`) builds a dict.
- **Struct alternative.** Reuse the class-instance backend: lay fields out
 contiguously (`{ i64 id; ptr name }`) so `td["id"]` is a fixed-offset load
 (~1-3 ns). Dynamic-key access, `**td`, and `dict` interop would then need
 per-operation synthesis or compile-time rejection.
- **Why dict-backed stands:**
 1. **Parity (#3).** In Python a TypedDict *is* a `dict` - `**td`,
 `td[runtime_key]`, `dict(td)`, `.keys`/`.items`, `isinstance(td, dict)`,
 and passing it where a `dict`/`Mapping` is expected all come for free. A
 struct loses these or must re-synthesize each.
 2. **Boundary data is dict-shaped (#1, in context).** The primary consumer,
 (database stdlib), feeds TypedDicts *from* dicts: a row arrives as a
 `Row`/`dict`, then `Customer(**row)`. A struct does **not** remove the
 string-hashing cost - it *relocates* it from access time to construction
 time (extracting each field from the source dict by key). Struct wins net
 only when you construct once and read fields many times.
 3. **Live dependency.** `Customer(**row)` lowers to a single
 `dragon_dict_copy` precisely because both sides are dicts (the
 typed-row gate). A struct repr would force per-field extraction at the
 spread site.
- **When to revisit.** A measured construct-once / read-many hot loop where
 TypedDict field access dominates - not on principle. Until such a profile
 exists, the dict backing is the pragmatic match for how TypedDict is actually
 used (config objects, API/JSON responses, DB rows). Note already keeps
 `TypedDict`-vs-`Dict` only as a source-level `VarKind` distinction and defers
 the representation choice here. Deliberatly boring on purpose.

---

## How They Work Together

```python
from typing import cast

# 1. Union inference - type checker knows the truth
data = [1, "hello", True] # list[int | str | bool]

# 2. isinstance - safe narrowing
for item in data {
 if isinstance(item, str) {
 print(item.upper) # str methods available
 }
 if isinstance(item, int) {
 print(item * 2) # int operations available
 }
}

# 3. cast - escape hatch when you know better
raw = get_json_value # Returns Any or unknown
name: str = cast[str](raw) # "Trust me, this is a string"

# 4. TypedDict - structured data
class User(TypedDict) {
 name: str
 age: int
}

user: User = {"name": "Alice", "age": 30}
print(user["name"].upper) # Type checker knows it's str
```

---

## Comparison Matrix

| Criterion | Union inference | `cast[T](expr)` | `isinstance` | TypedDict |
|-----------|---------------|------------------|-----------------|-----------|
| Import needed | No | `from typing import cast` | No (builtin) | No (special class) |
| Parser changes | None | None | None | Minimal (base class) |
| Type safety | Accurate inference | Trust-based | Compile-time narrowing (Phase 1), runtime (Phase 2) | Compile-time verified |
| Use case | Heterogeneous lists/dicts | Override type checker | Branch on type | Known-schema dicts |
| Effort | ~1 week | ~1 week | ~1-2 weeks | ~2-3 weeks |

---

## Decision

**No `coll[index:Type]` syntax.** Do this instead:

1. **Union element type inference** - fix the lie, TypeChecker-only
2. **`cast[T](expr)`** via `from typing import cast` - escape hatch, Python-compatible
3. **`isinstance` type narrowing** - builtin, compile-time first, runtime later
4. **TypedDict** - proper solution for structured dicts

Together tehse cover all heterogeneous collection use cases with zero syntax ambiguity, zero parser complexity, and maximum Python familiarity.
