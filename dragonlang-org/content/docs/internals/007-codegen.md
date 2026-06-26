# 007 -- Dragon LLVM Code Generator (CodeGen)

> **Version:** 0.2.0
> **Last Updated:** 2026-06-22
> **Source:** `src/CodeGen.cpp` (~1,050 lines), `src/codegen/*.cpp` (~20 visitor files), `src/CodeGenImpl.h`, `include/dragon/CodeGen.h`

---

## 1. Overview

The `CodeGen` class is Dragon's sole code generation backend. It translates a type-checked Dragon AST into **LLVM IR**, which is then compiled to native object code and linked into an executable. The previous C transpiler backend (CEmitter) was deleted in Phase 6 of the design spec.

`CodeGen` implements the visitor pattern by inheriting from `ASTVisitor` and overriding `visit()` for all ~50 AST node types. Each visitor method generates LLVM IR using the `IRBuilder` API. The result of expression visitors is stored in `impl_->lastValue`, which the calling context picks up.

The class follows Dragon's pimpl idiom: all state and helper methods live in the private `CodeGen::Impl` struct, declared in `src/CodeGenImpl.h` and keeping LLVM implementation details out of the public header.

The implementation is no longer one monolithic file. `src/CodeGen.cpp` holds the driver-facing entry points (`generate`, `compileToObject`, `linkExecutable`, optimization passes), while the visitor methods are split by AST-node category across roughly twenty files under `src/codegen/`: `Literals.cpp`, `Expressions.cpp`, `Statements.cpp`, `Assign.cpp`, `AugAnnAssign.cpp`, `CallExpr.cpp`, `CallBuiltins.cpp`, `CallMethods.cpp`, `Classes.cpp`, `Functions.cpp`, `ForLoop.cpp`, `Comprehensions.cpp`, `Collections.cpp`, `Attributes.cpp`, `Exceptions.cpp`, `Concurrency.cpp`, `ImplInit.cpp`, `ImplMethods.cpp` / `ImplMethods2.cpp`, and others. They all share the single `CodeGen::Impl` definition in `src/CodeGenImpl.h`. Line numbers cited below are approximate and will drift as these files evolve.

The public API provides:
- `generate(Module&)` -- single-file compilation
- `generate(Module&, vector<Module*>&)` -- multi-file compilation with dependency modules
- `writeIR()` / `writeBitcode()` -- emit LLVM text or binary IR
- `compileToObject()` -- emit a native `.o` file via LLVM's target machine
- `linkExecutable()` -- invoke `cc` to link the object file with the runtime library

---

## 2. The Impl Struct

All mutable state is encapsulated in `CodeGen::Impl` (defined in `src/CodeGenImpl.h`). Key data members:

| Member | Type | Purpose |
|--------|------|---------|
| `options` | `CodeGenOptions` | Compilation configuration (optimization level, target triple, debug info, output paths) |
| `diagnostics` | `vector<CodeGenDiagnostic>` | Accumulated warnings and errors |
| `context` | `unique_ptr<LLVMContext>` | LLVM context for all IR generation |
| `module` | `unique_ptr<llvm::Module>` | The single LLVM module being built |
| `builder` | `unique_ptr<IRBuilder<>>` | The IR builder for emitting instructions |
| `lastValue` | `llvm::Value*` | Result of the most recent expression visitor (the "return value" mechanism) |
| `scopes` | `vector<Scope>` | Scope stack; each scope maps variable names to `AllocaInst*` and `VarKind` |
| `currentFunction` | `llvm::Function*` | The LLVM function currently being generated into |
| `loopStack` | `stack<LoopInfo>` | Break/continue target blocks for nested loops |
| `runtimeFuncs` | `unordered_map<string, Function*>` | Cache of declared runtime function references |
| `lambdaCounter` | `int` | Monotonically increasing counter for unique lambda names |
| `excCounter` | `int` | Counter for unique exception handling block names |
| `symbolAliases` | `map<string, string>` | Maps Dragon import names to C library function names (e.g., `sqrt` from `math`) |
| `importedModules` | `set<string>` | Tracks which stdlib modules have been imported |
| `classNames` | `set<string>` | Known class names for constructor dispatch |
| `currentClassName` | `string` | Set when emitting class method bodies; enables `self.field` access |
| `classStructTypes` | `unordered_map<string, StructType*>` | Class name to LLVM struct type mapping |
| `classFieldIndices` | `unordered_map<string, unordered_map<string, unsigned>>` | Class field name to struct index |
| `classFieldTypes` | `unordered_map<string, unordered_map<string, Type*>>` | Class field name to LLVM type |
| `varClassNames` | `unordered_map<string, string>` | Variable name to class name (for instance method dispatch) |
| `classParentNames` | `unordered_map<string, string>` | Child class to parent class name (for MRO / super()) |
| `staticFieldGlobals` | `unordered_map<string, unordered_map<string, GlobalVariable*>>` | Static field globals per class |
| `staticMethods` | `unordered_set<string>` | Set of mangled names that are static methods (no self) |
| `classCtorCount` | `unordered_map<string, size_t>` | Number of `__init__` overloads per class |
| `classCtorArities` | `unordered_map<string, vector<pair<size_t,int>>>` | Arity-to-constructor-index mapping for multi-ctor dispatch |

### VarKind Enum

Many distinct Dragon types lower to the same LLVM type (`i8*` opaque pointer, or the box), so the LLVM type alone cannot answer "what is this slot". The `VarKind` enum (`CodeGenImpl.h`) carries the source-level distinction:

```
Int, Float, Bool, Str, StrLiteral, List, Dict, Tuple, Set, File,
ClassInstance, Generator, Type, Closure, Union, Deque, Other
```

The refcount-relevant splits matter most: `Str` is a runtime-allocated string (has an object header, decref via `dragon_decref_str`) while `StrLiteral` is a compile-time constant (no header, never decref); `ClassInstance` carries a GC header and decrefs via `dragon_decref`; `Union` covers both explicit `Union[...]` slots and `Any`, which lower to the 16-byte box and use tag-based refcounting rather than a fixed decref call. `VarKind` is tracked per variable in each scope and drives type-aware dispatch for `print()`, `len()`, method calls, subscript access, the `in` operator, and the incref/decref discipline (see §6.14).

### Key Helper Methods

- **`lookupVar(name)`** -- walks scopes from innermost to outermost, returns the `AllocaInst*` for a variable
- **`setVar(name, alloca, kind)`** -- registers a variable in the current scope
- **`createEntryAlloca(func, name, type)`** -- creates an `alloca` in the function entry block for stable stack layout
- **`typeExprToLLVM(TypeExpr*)`** -- converts a Dragon type annotation AST node to an LLVM type
- **`typeExprToKind(TypeExpr*)`** -- converts a Dragon type annotation to a `VarKind`
- **`getOrDeclareRuntime(name, funcType)`** -- lazily declares an `extern "C"` runtime function
- **`inferExprLLVMType(Expr*)`** -- statically infers the LLVM type an expression will produce
- **`runOptimizationPasses()`** -- runs the LLVM new PassManager optimization pipeline (O1/O2/O3)

---

## 3. CodeGenOptions

The `CodeGenOptions` struct configures code generation:

| Field | Type | Default | Description |
|-------|------|---------|-------------|
| `optimizationLevel` | `int` | `0` | LLVM optimization level (0 = none, 1-3 = O1-O3) |
| `targetTriple` | `string` | host triple | Target architecture (e.g., `x86_64-pc-linux-gnu`) |
| `debugInfo` | `bool` | `false` | Whether to generate DWARF debug information |
| `outputFile` | `string` | `"a.out"` | Name for the final executable |
| `runtimeLibPath` | `string` | empty | Path to `libdragon_runtime.a` for linking |

If `targetTriple` is empty, the host's default triple is used via `llvm::sys::getDefaultTargetTriple()`.

---

## 4. Type Mapping

Per the design spec, values flow at their **native LLVM types** end to end. Dragon types are mapped to LLVM types by `typeExprToLLVM()` (`src/codegen/ImplMethods2.cpp`):

| Dragon Type | LLVM Type | Notes |
|-------------|-----------|-------|
| `int` | `i64` | 64-bit signed integer |
| `float` | `double` (f64) | 64-bit IEEE 754; no bitcast on load/store after spec-30 |
| `bool` | `i1` | Single bit; widened to `i64` only where arithmetic needs it |
| `intc` | `i32` (`i16` on 16-bit targets) | C-int FFI bridge |
| `str`, `bytes` | `ptr` (opaque) | Pointer to a runtime string/bytes object |
| `None` | `void` | As return type; `null` as value |
| `list[T]` | `ptr` | Opaque pointer to a monomorphized list runtime struct (see below) |
| `dict[K,V]` | `ptr` | Opaque pointer to `DragonDict` runtime struct |
| `tuple[T...]` | `ptr` | Opaque pointer to `DragonTuple` runtime struct |
| `set[T]` | `ptr` | Opaque pointer to `DragonSet` runtime struct |
| Class types, `Task[T]`, `Callable[...]` | `ptr` | Pointer to the class struct / erased handle / function pointer |
| `Any`, `Union[A, B, ...]` | `%dragon.box = { i64 tag, i64 payload }` | 16-byte box; tag identifies the live member |

A `Ptr | None` niche union is the exception: it lowers to a single nullable `ptr` (null encodes `None`), avoiding the box and its tag branch (`typeExprToLLVM`, `unionNicheMember`). Other non-niche unions and `Any`/`object` all map to `boxType`.

**Monomorphized containers (spec-30 Phase 3).** Collection elements are no longer all funneled through `i64`. Lists pick a storage variant from `list[T]` at allocation (`emitNewTypedList` in `src/codegen/Collections.cpp`):

- `list[float]` -> `DragonListF64`, native `double[]` storage, no bitcast (`dragon_list_new_f64` / `dragon_list_append_f64` / `dragon_list_get_f64`)
- `list[str]` and other heap-element lists -> `DragonListPtr`, native `void**` storage with a per-list refcount tag (`dragon_list_new_ptr` / `dragon_list_append_ptr` / `dragon_list_get_ptr`)
- `list[Any]` / union-element lists -> `DragonListBox`, 16 bytes per element with a per-element tag
- `int` / `bool` / untyped lists -> the original `DragonList` (i64 storage)

Dicts follow the same pattern: typed entry points such as `dragon_dict_get_str_f64` and `dragon_dict_get_str_ptr` return native values, while the polymorphic `dragon_dict_get` / `dragon_dict_set_tagged` (and the box-returning `dragon_dict_get_box`) remain for `dict[str, int]` and `dict[str, Any]`. Boxing into a union or `Any` happens only where the static type is genuinely dynamic, never as a default for a knowable type.

### Cached Types

Several LLVM types are cached at initialization (`src/codegen/ImplInit.cpp`) for fast access:
- `i64Type` -- `Type::getInt64Ty`
- `f64Type` -- `Type::getDoubleTy`
- `i1Type` -- `Type::getInt1Ty`
- `i8PtrType` -- `PointerType::getUnqual` (opaque pointer)
- `voidType` -- `Type::getVoidTy`
- `intcType` -- `i32` (or `i16` on 16-bit targets), the C-int FFI bridge
- `boxType` -- `%dragon.box = { i64, i64 }`, the tag/payload box for `Any` and `Union[...]`

---

## 5. Runtime Function Declarations

The runtime-declaration routine (`src/codegen/ImplInit.cpp`) forward-declares the `dragon_*` runtime functions as external-linkage LLVM functions. These correspond to `extern "C"` functions implemented across the split runtime under `lib/Runtime/` (`runtime_core.cpp`, `runtime_list.cpp`, `runtime_dict.cpp`, `runtime_string.cpp`, `runtime_box.cpp`, `runtime_exception.cpp`, `runtime_concurrency.cpp`, and others). The categories below are representative, not exhaustive. The list operations shown are the legacy untyped `DragonList` (i64) entry points; the monomorphized and refcount entry points are listed separately at the end.

**I/O:** `dragon_print_int`, `dragon_print_float`, `dragon_print_str`, `dragon_print_bool`, `dragon_print_none`, `dragon_print_newline`, `dragon_input`

**String operations:** `dragon_str_concat`, `dragon_str_len`, `dragon_str_eq`, `dragon_str_contains`, `dragon_str_index`, `dragon_str_slice`, `dragon_str_repeat`, `dragon_int_to_str`, `dragon_float_to_str`, `dragon_bool_to_str`

**Arithmetic helpers:** `dragon_pow_int`, `dragon_floordiv_int`, `dragon_mod_int`, `dragon_abs_int`

**Assertions:** `dragon_assert`, `dragon_assert_no_msg`

**List operations:** `dragon_list_new`, `dragon_list_append`, `dragon_list_get`, `dragon_list_set`, `dragon_list_len`, `dragon_list_pop`, `dragon_list_insert`, `dragon_list_remove`, `dragon_list_clear`, `dragon_list_extend`, `dragon_list_index`, `dragon_list_count`, `dragon_list_sort`, `dragon_list_reverse`, `dragon_list_copy`, `dragon_list_slice`, `dragon_print_list_int`

**Dict operations:** `dragon_dict_new`, `dragon_dict_set`, `dragon_dict_get`, `dragon_dict_len`, `dragon_dict_has_key`, `dragon_dict_get_default`, `dragon_dict_keys`, `dragon_dict_values`, `dragon_dict_items`, `dragon_dict_pop`, `dragon_dict_pop_default`, `dragon_dict_clear`, `dragon_dict_update`, `dragon_dict_setdefault`, `dragon_dict_copy`, `dragon_print_dict`

**Tuple operations:** `dragon_tuple_new`, `dragon_tuple_get`, `dragon_tuple_set`, `dragon_tuple_len`, `dragon_print_tuple`

**Set operations:** `dragon_set_new`, `dragon_set_add`, `dragon_set_contains`, `dragon_set_remove`, `dragon_set_discard`, `dragon_set_len`, `dragon_set_clear`, `dragon_set_copy`, `dragon_set_union`, `dragon_set_intersection`, `dragon_set_difference`, `dragon_set_symmetric_difference`, `dragon_set_issubset`, `dragon_set_issuperset`, `dragon_set_isdisjoint`, `dragon_set_pop`, `dragon_set_update`, `dragon_print_set`

**Exception handling:** `dragon_exc_push_frame`, `dragon_exc_pop_frame`, `dragon_exc_get_type`, `dragon_exc_get_msg`, `dragon_raise_exc`, `setjmp` (with `returns_twice` attribute)

**Builtins (Phase G):** `dragon_min_int/float/list`, `dragon_max_int/float/list`, `dragon_sum_list`, `dragon_any_list`, `dragon_all_list`, `dragon_enumerate`, `dragon_zip`, `dragon_sorted`, `dragon_reversed`, `dragon_hash_int`, `dragon_hash_str`, `dragon_id`, `dragon_ord`, `dragon_chr`, `dragon_round_int`, `dragon_pow_float`, `dragon_divmod`, `dragon_hex`, `dragon_oct`, `dragon_bin`, `dragon_repr_int/str/float/bool`

**File I/O (Phase H):** `dragon_file_open`, `dragon_file_close`, `dragon_file_read`, `dragon_file_readline`, `dragon_file_write`, `dragon_file_readlines`

**Monomorphized containers (spec-30):** `dragon_list_new_f64`, `dragon_list_append_f64`, `dragon_list_get_f64`, `dragon_list_set_f64`; `dragon_list_new_ptr`, `dragon_list_append_ptr`, `dragon_list_get_ptr`, `dragon_list_set_ptr`; `dragon_list_box_new`, `dragon_list_box_append`; `dragon_dict_get_str_f64`, `dragon_dict_set_str_f64`, `dragon_dict_get_str_ptr`, `dragon_dict_set_str_ptr`, `dragon_dict_get_ptr`, `dragon_dict_get_box`, `dragon_dict_set_tagged`

**Boxing and dynamic ops:** `dragon_box_binop`, `dragon_box_cmp`, `dragon_box_eq`, `dragon_box_subscript`, `dragon_box_to_str`, `dragon_box_decref` -- operate on `%dragon.box` values for `Any` / `Union[...]` operands

**Refcounting:** `dragon_incref`, `dragon_decref` (general heap objects), `dragon_incref_str`, `dragon_decref_str` (runtime strings) -- emitted by the ownership layer described in §6.14

Each declaration uses `getOrDeclareRuntime()`, which lazily creates the function and caches it. Subsequent lookups return the cached `llvm::Function*` from the `runtimeFuncs` map.

---

## 6. Expression Codegen

### 6.1 Literals

- **IntegerLiteral:** `ConstantInt::get(i64Type, value)`
- **FloatLiteral:** `ConstantFP::get(f64Type, value)`
- **BooleanLiteral:** `ConstantInt::get(i1Type, value ? 1 : 0)`
- **NoneLiteral:** `ConstantPointerNull::get(ptr)`
- **StringLiteral (plain):** `CreateGlobalString(value)` -- embeds the string as a global constant
- **StringLiteral (f-string):** Parses `{expr}` segments by invoking the Lexer and Parser on each interpolated expression. Each segment is evaluated to LLVM IR, converted to string via `dragon_int_to_str`/`dragon_float_to_str`/`dragon_bool_to_str` as needed, then chained with `dragon_str_concat`. Escaped braces (`{{`/`}}`) become literal `{`/`}` characters.

### 6.2 Names

The `NameExpr` visitor handles:
- `True`/`False` -- `i1` constants
- `None` -- null pointer
- Variable lookup via `lookupVar()` -- emits a `CreateLoad` from the alloca
- Function references -- if not a variable, looks up the LLVM function by name (for function pointers)
- Error for undefined variables

### 6.3 Binary Operations

**Short-circuit logic:** `and`/`or` use conditional branching with PHI nodes. The left operand is evaluated, converted to `i1`, and a conditional branch either skips (for `or` when true) or requires (for `and` when false) evaluation of the right operand. The result is merged with a PHI node.

**String operations:** When both operands are `ptr` type:
- `+` calls `dragon_str_concat`
- `==` calls `dragon_str_eq` then compares result != 0
- `!=` calls `dragon_str_eq` then compares result == 0

**`in` operator:** Dispatches based on VarKind of the right operand:
- Set: calls `dragon_set_contains`
- String: calls `dragon_str_contains`
- Fallback: returns false

**Float promotion:** If either operand is `f64`, the other is promoted via `SIToFP` (int) or `UIToFP` (bool). True division (`/`) always promotes to float.

**Integer arithmetic:** `+` (Add), `-` (Sub), `*` (Mul), `%` (runtime `dragon_mod_int`), `//` (runtime `dragon_floordiv_int`), `**` (runtime `dragon_pow_int`)

**Comparisons:** Integer uses `ICmpSLT/SLE/SGT/SGE/EQ/NE`; float uses `FCmpOLT/OLE/OGT/OGE/OEQ/ONE`

**Bitwise:** `&` (And), `|` (Or), `^` (Xor), `<<` (Shl), `>>` (AShr)

### 6.4 Chained Comparisons

`ChainedCompExpr` (e.g., `a < b < c`) uses short-circuit evaluation with multiple basic blocks. Each comparison is evaluated left-to-right; if any fails, control jumps to the end block with `false`. The result is merged with a PHI node of type `i1`.

### 6.5 Walrus Operator

`WalrusExpr` (`name := value`) evaluates the RHS, creates or looks up an alloca for the variable, stores the value, and returns the value as the expression result. `VarKind` is inferred from the LLVM type and the RHS expression form.

### 6.6 Unary Operations

- `-` (negate): `CreateFNeg` for float, `CreateNeg` for int
- `not`: converts to `i1` then `CreateNot`
- `~` (bitwise not): `CreateNot` on `i64`

### 6.7 Function Calls

The `CallExpr` visitor is the largest visitor method. It dispatches by examining the callee:

**Builtin functions** (callee is `NameExpr`):
- `print()` -- dispatches by argument VarKind/LLVM type to `dragon_print_int/float/str/bool/none/list_int/dict/tuple/set`
- `len()` -- dispatches to `dragon_str_len/list_len/dict_len/tuple_len/set_len`
- `int()`/`float()`/`str()`/`bool()` -- type conversion using LLVM casts or runtime calls
- `input()` -- `dragon_input(prompt)`
- `abs()` -- `dragon_abs_int`
- `min()`/`max()` -- two-arg uses `dragon_min/max_int/float`, one-arg uses `dragon_min/max_list`
- `sum()`/`any()`/`all()` -- aggregate operations on lists
- `enumerate()`/`zip()`/`sorted()`/`reversed()` -- iteration helpers
- `hash()`/`id()`/`repr()` -- introspection
- `ord()`/`chr()`/`round()`/`pow()`/`divmod()`/`hex()`/`oct()`/`bin()` -- numeric utilities
- `list()`/`dict()`/`set()`/`tuple()` -- empty collection constructors
- `super(args)` (.dr) -- delegates to the parent constructor `ParentClass___init__(self, args...)`; in `.py` mode `super()` returns the `self` pointer (Python proxy, dispatch at the call site)
- `range()` -- no-op as a standalone call; actual loop generation is in `ForStmt`
- `isinstance(obj, ClassName)` -- a fully implemented compile-time type check (`src/codegen/CallBuiltins.cpp`). The second argument must name a class statically (classes are compile-time entities per spec-21); a non-class type is a compile error. It resolves the static class of `obj`, walks the MRO so a subclass instance matches a parent-class check, and for a union/`Any` source narrows by comparing the box tag. The result is an `i1`, and a successful `isinstance` in an `if` condition narrows the bound variable's `VarKind` in the guarded branch.

**Class constructor calls** (`ClassName(args...)`):
- Single-constructor: calls `ClassName_new(args...)`
- Multi-constructor: matches call arity against `classCtorArities` to dispatch to `ClassName_new_N(args...)`

**Method calls** (`obj.method(args...)`):
- String methods: `upper`, `lower`, `strip`, `find`, `replace`, `split`, `join`, etc. -- dispatch to `dragon_str_METHOD`
- List methods: `append`, `pop`, `sort`, `reverse`, `copy`, etc. -- dispatch to `dragon_list_METHOD`
- Dict methods: `get`, `keys`, `values`, `items`, `pop`, `update`, etc. -- dispatch to `dragon_dict_METHOD`
- Set methods: `add`, `remove`, `contains`, `union`, etc. -- dispatch to `dragon_set_METHOD`
- File methods: `read`, `readline`, `readlines`, `write`, `close` -- dispatch to `dragon_file_METHOD`
- `super.method()` (.dr) / `super().method()` (.py) -- resolves parent class from `classParentNames`, dispatches to `ParentClass_method(self, args...)`. The spelling is mode-exclusive: `.dr` rejects the `super().method()` form and `.py` rejects the bare `super.method()` form
- Static methods: `ClassName.method()` -- no self parameter
- Instance methods: `obj.method()` -- passes obj as first argument, walks MRO chain for inherited methods
- Stdlib qualified calls: `math.sqrt()` etc. -- resolved via `symbolAliases`

### 6.8 Attribute Access

`AttributeExpr` handles:
- Stdlib constants: `math.pi` (3.14159...), `math.e` (2.71828...)
- Static fields: looks up in `staticFieldGlobals`, loads from the global variable
- Class field access (`self.x` or `instance.x`): uses `CreateStructGEP` on the class struct type, then `CreateLoad`

### 6.9 Subscript Access

`SubscriptExpr` dispatches based on object VarKind:
- Slice access (index is `SliceExpr`): uses `INT64_MIN` sentinel for omitted bounds; calls `dragon_list_slice` or `dragon_str_slice`
- Dict subscript: `dragon_dict_get(dict, key)`
- Tuple subscript: `dragon_tuple_get(tuple, index)`
- List subscript: `dragon_list_get(list, index)`
- String subscript: `dragon_str_index(str, index)`

### 6.10 Collection Literals

- **ListExpr:** picks the monomorphized list variant from the element type via `emitNewTypedList` (`dragon_list_new` / `_new_f64` / `_new_ptr` / `_box_new`), then appends each element through the matching typed entry point (`emitTypedListAppend`); elements flow at their native type, not a uniform `i64` (see §4)
- **TupleExpr:** calls `dragon_tuple_new(count)`, then `dragon_tuple_set` for each element
- **DictExpr:** calls `dragon_dict_new(capacity)`, then `dragon_dict_set(dict, key, value)` for each entry
- **SetExpr:** calls `dragon_set_new()`, then `dragon_set_add` for each element

### 6.11 Comprehensions

List, dict, set, and generator comprehensions all follow the same pattern:

1. Create a result collection (list/dict/set) and store in an alloca
2. Determine if the iterable is `range()` or a collection
3. Generate a loop structure: `cond` block (comparison), `body` block, `inc` block, `end` block
4. Inside the body: evaluate the element expression, optionally check a filter condition, append/insert into the result
5. Support nested extra clauses via a recursive `emitExtraClauses` lambda

Generator expressions are eagerly materialized as lists (no lazy evaluation).

### 6.12 Lambda Expressions

Lambda expressions are lowered to internal LLVM functions:

1. A unique name is generated: `__dragon_lambda_N`
2. The function type is built from parameter types and return type
3. The function is created with `InternalLinkage`
4. Current codegen state is saved, the lambda body is generated, then state is restored
5. For expression lambdas: the body expression is evaluated and returned
6. For block lambdas: each statement is generated; implicit return added if needed
7. `lastValue` is set to the function pointer

### 6.13 Conditional Expression

`IfExpr` (`x if cond else y`) creates three basic blocks (then, else, merge) with a conditional branch. The then and else values are merged with a PHI node. If types differ, both are promoted to `f64`.

### 6.14 Reference Counting and Ownership

Under the reference-counting GC mode (`GCMode::RC`), CodeGen emits an ownership discipline so heap values are freed deterministically with no leak and no use-after-free. There is no separate tracing collector pass; the lifetime logic is woven into the visitors:

- **incref / decref.** Storing a heap value into a binding takes a reference; overwriting or dropping one releases it. Strings use `dragon_incref_str` / `dragon_decref_str`; general heap objects (lists, dicts, sets, tuples, class instances) use `dragon_incref` / `dragon_decref`. `Union` / `Any` slots carry a tag, so their refcounting is tag-dispatched (`emitUnionIncref` / `emitUnionDecref` in `src/codegen/ImplMethods2.cpp`) rather than a fixed call.
- **Draining owned temporaries at call sites.** A subexpression that produces a freshly owned heap value (for example `[1, 2] + xs` passed straight into a call, or a default-value temporary) carries a +1 that no binding will ever release. CodeGen classifies it with `ownedTempDrainKind` (gated on the argument's **static** type, since `str`/`list`/`dict`/`set` are all `i8*` at the LLVM level) and releases it after the call, so the temporary does not leak. Borrowed arguments are recognized and skipped.
- **Scope cleanup at block exit.** Every `{}` / indented block is its own scope, and on exit `emitScopeCleanup` / `emitScopeCleanupFor` decref the heap locals that block owns. Reassigning an outer accumulator inside a loop updates the existing binding rather than creating a per-iteration local, so it is not wrongly freed by the loop body's cleanup.

This layer is what keeps the hot path allocation-honest: a value whose type is known flows at that type and is released exactly once, rather than being boxed and leaked.

---

## 7. Statement Codegen

### 7.1 Expression Statements

`ExprStmt`: visits the expression; the result (`lastValue`) is discarded.

### 7.2 Assignments

**AssignStmt** handles multiple target forms:
- **Name target:** looks up or creates an alloca, stores the value. Infers `VarKind` from the RHS expression type. Tracks class instance names for method dispatch (`varClassNames`).
- **Dict subscript target** (`d["key"] = val`): calls `dragon_dict_set`
- **List subscript target** (`lst[i] = val`): calls `dragon_list_set`
- **Static field target** (`ClassName.field = val`): stores to the `GlobalVariable`
- **Class field target** (`self.x = val`): uses `CreateStructGEP` then `CreateStore`
- **Tuple unpacking** (`a, b = (1, 2)`): extracts elements via `dragon_tuple_get` or `dragon_list_get`. Supports starred unpacking (`a, *rest, b = iterable`).

**AnnAssignStmt** (`x: int = 42`): creates an alloca with the annotated type, stores the value (or zero-initializes if no value).

**AugAssignStmt** (`x += 1`): loads current value, evaluates RHS, applies the operator, stores back. Supports string concatenation (`+=` with `Str` VarKind), all arithmetic operators, power, floor division, modulo, and bitwise operators.

### 7.3 Control Flow

**IfStmt:** creates basic blocks for `then`, each `elif`, optional `else`, and `merge`. Conditions are converted to `i1`. Elif clauses chain test blocks.

**WhileStmt:** creates `cond`, `body`, `end` blocks. Pushes a `LoopInfo` onto the loop stack for break/continue targets.

**ForStmt:** two paths:
- *Range-based* (`for i in range(start, end, step)`): creates a loop variable alloca, emits `cond/body/inc/end` blocks with `ICmpSLT` condition and `CreateAdd` increment
- *Collection-based* (`for x in iterable`): evaluates the iterable once, creates an index variable, calls `dragon_list_len`/`dragon_str_len` for the bound, and `dragon_list_get`/`dragon_str_index` for element access. Supports tuple unpacking in the target (`for a, b in items`).

**MatchStmt (PEP 634):** evaluates the subject once, stores in an alloca, then emits a chain of test-and-branch blocks. Pattern matching is implemented recursively via `emitPatternMatch`:
- `Wildcard` (`_`): always matches
- `Capture` (`x`): binds subject to variable, always matches
- `Literal`: compares with `ICmpEQ` (int), `FCmpOEQ` (float), `dragon_str_eq` (string), or `CreateIsNull` (None)
- `Value`: evaluates a dotted-name expression and compares
- `Sequence`: checks tuple length, then recursively matches each element
- `Or` (`p1 | p2`): short-circuit disjunction of sub-patterns
- Optional guard expressions are evaluated after pattern match, before body execution

### 7.4 Try/Catch (Exception Handling)

Uses the `setjmp`/`longjmp` pattern via the runtime:

1. `dragon_exc_push_frame()` returns a `jmp_buf` pointer
2. `setjmp(jmpbuf)` is called (with `returns_twice` attribute)
3. If `setjmp` returns 0 (normal path): execute try body, then `dragon_exc_pop_frame()`
4. If `setjmp` returns non-zero (exception path): `dragon_exc_pop_frame()`, get exception type via `dragon_exc_get_type()`, dispatch to handlers
5. Each typed handler compares the exception type code against known values (ValueError=2, TypeError=3, RuntimeError=4, IndexError=5, KeyError=6, ZeroDivisionError=7, base Exception=1)
6. If a handler has a name (`except ValueError as e`), the exception message is bound via `dragon_exc_get_msg()`
7. Unmatched exceptions are re-raised via `dragon_raise_exc`
8. Optional `else` block runs after normal try body completion
9. Optional `finally` block runs after both normal and exception paths

### 7.5 Other Statements

- **ReturnStmt:** evaluates value expression, applies type coercion to match function return type, emits `CreateRet` or `CreateRetVoid`
- **BreakStmt:** branches to `loopStack.top().breakBlock`
- **ContinueStmt:** branches to `loopStack.top().continueBlock`
- **PassStmt:** no-op
- **AssertStmt:** evaluates test condition, calls `dragon_assert(cond, msg)` or `dragon_assert_no_msg(cond)`
- **RaiseStmt:** calls `dragon_raise_exc(typeCode, msg)` followed by `CreateUnreachable()`, then creates a dead basic block for subsequent IR
- **WithStmt:** evaluates context managers, optionally binds to `as` variables, executes body, auto-closes file handles
- **GlobalStmt / NonlocalStmt / DeleteStmt:** no-ops
- **ImportStmt / FromImportStmt:** delegates to `StdlibRegistry` to populate `symbolAliases`
- **TypeAliasStmt:** no-op (type aliases are compile-time only)

---

## 8. Function Codegen

The `FunctionDecl` visitor emits a function body for a previously forward-declared LLVM function:

1. Look up the function by name in the LLVM module (already created by `forwardDeclareFunctions()`)
2. Skip if the function already has a body (prevents duplicate generation)
3. Save current codegen state (`currentFunction`, insert point)
4. Create an entry basic block
5. Push a new scope
6. For each parameter: name the LLVM argument, create an entry-block alloca, store the incoming argument value
7. Generate each statement in the function body
8. If the final block has no terminator, add an implicit `CreateRetVoid()` (for void functions) or `CreateRet(null)` (for non-void)
9. Pop scope and restore previous codegen state

Functions use `InternalLinkage` for class methods and lambdas, `ExternalLinkage` for top-level functions.

---

## 9. Class Codegen

The `ClassDecl` visitor generates LLVM IR for a Dragon class in several steps:

### Step 1: Field Extraction

Scans ALL `__init__` method bodies for `self.field = expr` patterns (`AssignStmt` with `AttributeExpr` target where object is `self`) and `self.field: type = expr` patterns (`AnnAssignStmt`). Collects the union of all fields across all constructor overloads. Field types are inferred from type annotations, defaulting to `i64`.

### Step 2: LLVM Struct Type

Creates a named LLVM struct type:
```
%ClassName = type { field0_type, field1_type, ... }
```
Stores field-to-index and field-to-type mappings in `classFieldIndices` and `classFieldTypes`.

### Step 3: Static Fields

For `AnnAssignStmt` nodes with `isStatic = true`, creates LLVM `GlobalVariable` entries with `InternalLinkage`. Literal initializers (int, float, bool) are set as compile-time constants. Non-literal initializers are deferred to runtime initialization in the main function.

### Step 4: Constructor Functions

**Single-constructor path** (0 or 1 `__init__`):
- `ClassName___init__(ptr self, params...)` -- the init body
- `ClassName_new(params...)` -- allocates memory via `malloc(sizeof(struct))`, calls `___init__`, returns self pointer

**Multi-constructor path** (2+ `__init__` overloads):
- `ClassName___init___N(ptr self, params...)` for each overload N
- `ClassName_new_N(params...)` for each overload N
- Dispatch at call site matches argument arity to select the correct `_new_N` variant

### Step 5: Method Functions

Each non-`__init__` method is emitted as:
- **Instance methods:** `ClassName_methodName(ptr self, params...)` -- self is the first parameter
- **Static methods** (`isStatic = true`): `ClassName_methodName(params...)` -- no self parameter

Methods are emitted with `InternalLinkage`. The `currentClassName` is set during method body generation to enable `self.field` access translation via `CreateStructGEP`.

### Step 6: Inheritance

- Parent class name is tracked in `classParentNames`
- `.dr`: `super(args)` delegates to the parent constructor, `super.method()` dispatches to the parent method (constructor delegation is opt-in - never implicit). `.py`: `super()` returns the `self` pointer and `super().method()` dispatches; each spelling is rejected in the other mode
- Instance method dispatch walks the MRO chain (parent chain) to find inherited methods

---

## 10. Multi-file Compilation

The two-argument `generate()` method handles multi-file Dragon programs using a **single LLVM module** approach:

1. **Forward-declare** all functions and classes from dependency modules first, then from the entry module. This ensures all symbols are known before any bodies are generated.
2. **Generate dependency code:** for each dependency module, only `FunctionDecl`, `ClassDecl`, `ImportStmt`, and `FromImportStmt` nodes are visited (top-level expression statements are skipped).
3. **Create `main()`:** a `main` function with `ExternalLinkage` is created for the entry module.
4. **Generate entry module code:** all statements (including top-level expressions) are emitted inside `main()`.
5. **Terminate `main()`:** adds `return 0` if the block is not already terminated.
6. **Verify:** runs `llvm::verifyModule()` to catch IR errors.

---

## 11. Object Code and Linking

### `compileToObject(filename)`

1. Looks up the target machine from the module's target triple
2. Sets the data layout on the module
3. Runs optimization passes via `runOptimizationPasses()` (uses the LLVM new PassManager with `buildPerModuleDefaultPipeline`)
4. Creates a `TargetMachine` with PIC relocation model
5. Emits an object file via the legacy `PassManager` and `addPassesToEmitFile`

### `linkExecutable(outputFile, objectFile)`

Invokes the system linker via `std::system`:
```
cc -o outputFile objectFile [runtimeLibPath] -lm
```
The `-lm` flag links the math library. If `runtimeLibPath` is set, the static runtime library is included.

---

## 12. Diagnostics

The `CodeGenDiagnostic` struct reports errors and warnings:

```cpp
struct CodeGenDiagnostic {
    enum class Level { Warning, Error };
    Level level;
    SourceLocation location;
    std::string message;
};
```

Errors are emitted via `impl_->addError(msg, location)` and accumulated in `impl_->diagnostics`. The `hasErrors()` method checks if any `Error`-level diagnostics exist. Diagnostics are reported for: undefined variables, unsupported operators, LLVM verification failures, target lookup failures, and file I/O errors.

---

## Previous Document

[006 -- Type Checker](006-typechecker.md)

## Next Document

[008 -- Runtime Library](008-runtime.md)
