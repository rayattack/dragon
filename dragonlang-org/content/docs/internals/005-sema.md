# 005 -- Semantic Analysis (`Sema`)

> **Source files:** `include/dragon/Sema.h`, `src/Sema.cpp`
> **Last Updated:** 2026-06-22
> **Test suite:** SemaTests

This document describes Dragon's semantic analysis pass in full detail. All
information is derived from the actual implementation in
`include/dragon/Sema.h` and `src/Sema.cpp`.

---

## 1. Overview

`Sema` is the third analysis pass in the Dragon compilation pipeline, running
after lexing and parsing but before type checking. It implements the
`ASTVisitor` interface and walks the entire AST produced by the parser.

### What Sema does

| Responsibility | Description |
|---|---|
| **Name resolution** | Verifies that every `NameExpr` refers to a symbol that has been defined -- either by the user or as a builtin. |
| **Scope management** | Maintains a stack of `Scope` objects that model the nesting of modules, classes, functions, and blocks. |
| **Symbol definition** | Records every variable, function, class, parameter, module, and type-alias symbol into the appropriate scope. |
| **Control flow validation** | Ensures `break` and `continue` appear only inside loops, and `return` and `yield` appear only inside functions. |
| **Import registration** | Registers imported names (from `import` and `from ... import` statements) as symbols in the current scope. |
| **Assignment target validation** | Checks that only valid expressions appear on the left-hand side of assignments and `del` statements. |
| **Declaration-rule enforcement** | Enforces ":-declares / =-assigns": a bare `=`/`+=` to an unresolved name is an error, a second declaration of a name in the same scope is a redeclaration error, `const` bindings cannot be reassigned, and rebinding an enclosing function's variable or a module global requires `nonlocal`/`global`. |
| **Nonlocal binding verification** | Confirms that `nonlocal` declarations refer to a name that actually exists in an enclosing non-module function scope. |

### What Sema does NOT do

- **Type checking.** Sema does not compute, propagate, or compare types.
  The `Symbol::type` field exists in the struct but Sema never populates it.
  Type analysis is handled entirely by the subsequent `TypeChecker` pass.
- **Code generation.** Sema produces no output artifact; it only mutates
  internal state (the diagnostic list) and either succeeds or fails.
- **Optimization.** No transformations are applied to the AST.

### Entry point

```cpp
bool Sema::analyze(Module& module);
```

Returns `true` if no errors were emitted. Internally it calls
`module.accept(*this)` to walk the tree, then pops the module scope.

---

## 2. The `Scope` Class

Defined in `include/dragon/Sema.h` (lines 33-67).

```cpp
class Scope {
public:
    enum class Kind { Module, Class, Function, Block };

    Scope(Kind kind, Scope* parent = nullptr);
    ~Scope();

    bool define(const Symbol& symbol);
    Symbol* lookup(const std::string& name);
    Symbol* lookupLocal(const std::string& name);
    Scope* enclosingFunction();
    Scope* enclosingClass();

    Kind kind() const;
    Scope* parent() const;

private:
    Kind kind_;
    Scope* parent_;
    std::unordered_map<std::string, Symbol> symbols_;
};
```

### 2.1 Kind Enum

| Kind | When created |
|---|---|
| `Module` | Once, at `Sema` construction time. This is always the outermost scope. |
| `Class` | Pushed when visiting a `ClassDecl`. |
| `Function` | Pushed when visiting a `FunctionDecl`, a `LambdaExpr`, a block `fire { ... }` (`FireExpr` block form), or a `thread { ... }` (`ThreadStmt`). The last two model a vthread/OS-thread body that captures enclosing locals like a closure. |
| `Block` | Pushed for **every** `{}`/indented block body. This covers all the statement bodies (`if`/`elif`/`else`, `while` body and `else`, `for` body and `else`, `with`, `try`/`except`/`else`/`finally`, `match` case arms) as well as every comprehension/generator (`ListCompExpr`, `DictCompExpr`, `SetCompExpr`, `GeneratorExpr`). A name declared in a block is not visible after the block ends. |

### 2.2 `define(const Symbol& symbol) -> bool`

Inserts a symbol into this scope's `symbols_` map. Returns `true` on success.
Returns `false` (without modifying anything) if a non-builtin symbol with the
same name already exists in this scope. The one exception: a user declaration
that collides with an **injected builtin** (a `defineBuiltins()` entry, marked
`isBuiltin`) replaces the builtin entry and returns `true`, because builtins
conceptually live in an outer namespace and may be shadowed by a fresh
declaration.

```cpp
bool Scope::define(const Symbol& symbol) {
    auto it = symbols_.find(symbol.name);
    if (it != symbols_.end()) {
        // A user declaration shadows an injected builtin (which lives in the
        // outer namespace, not this scope) - replace the builtin entry.
        if (it->second.isBuiltin && !symbol.isBuiltin) {
            it->second = symbol;
            return true;
        }
        return false;
    }
    symbols_[symbol.name] = symbol;
    return true;
}
```

`define` itself emits no diagnostic when it returns `false`; the
declaration-rule enforcement (rejecting genuine redeclarations, bare `=` to an
unknown name, etc.) lives in the statement visitors that *call* `define`. See
§6.1 (`AssignStmt`), §6.2 (`AnnAssignStmt`), and §9 for the messages. Dragon
does **not** silently allow rebinding the way Python does: it enforces a
":-declares / =-assigns" rule. (The only place a duplicate `define` is
deliberately ignored is the `Module` pre-pass forward-declaration of top-level
`def`/`class` names, where the second `define` from the real visitor is an
intentional no-op - see §6.x notes on `visit(Module&)`.)

### 2.3 `lookup(const std::string& name) -> Symbol*`

Searches for `name` in this scope's `symbols_` map. If not found, delegates
to `parent_->lookup(name)` recursively. Returns `nullptr` if the name is not
found in any enclosing scope (including the module scope and its builtins).

```cpp
Symbol* Scope::lookup(const std::string& name) {
    auto it = symbols_.find(name);
    if (it != symbols_.end()) return &it->second;
    if (parent_) return parent_->lookup(name);
    return nullptr;
}
```

This is the standard lexical scoping lookup: inner scopes shadow outer scopes.

### 2.4 `lookupLocal(const std::string& name) -> Symbol*`

Same as `lookup` but does **not** traverse parent scopes. Used when Sema
needs to know whether a name exists in precisely the current scope (e.g.,
during assignment, to decide whether to define a new variable or mark an
existing one as initialized).

```cpp
Symbol* Scope::lookupLocal(const std::string& name) {
    auto it = symbols_.find(name);
    return it != symbols_.end() ? &it->second : nullptr;
}
```

### 2.5 `enclosingFunction() -> Scope*`

Walks the parent chain (including `this`) looking for a scope with
`Kind::Function`. Returns the first one found, or `nullptr` if none exists.

```cpp
Scope* Scope::enclosingFunction() {
    for (Scope* s = this; s; s = s->parent_) {
        if (s->kind_ == Kind::Function) return s;
    }
    return nullptr;
}
```

### 2.6 `enclosingClass() -> Scope*`

Same traversal as `enclosingFunction`, but looks for `Kind::Class`.

```cpp
Scope* Scope::enclosingClass() {
    for (Scope* s = this; s; s = s->parent_) {
        if (s->kind_ == Kind::Class) return s;
    }
    return nullptr;
}
```

---

## 3. The `Symbol` Struct

Defined in `include/dragon/Sema.h` (lines 13-30).

```cpp
struct Symbol {
    enum class Kind {
        Variable,
        Function,
        Class,
        Parameter,
        Module,
        TypeAlias
    };

    std::string name;
    Kind kind;
    std::shared_ptr<Type> type;     // Not populated by Sema; used by TypeChecker
    SourceLocation declaration;
    bool isGlobal = false;          // Set by GlobalStmt visitor
    bool isNonlocal = false;        // Set by NonlocalStmt visitor
    bool isInitialized = false;     // True when the variable has a value
    bool isConst = false;           // Dragon const binding
    bool isStatic = false;          // Dragon static member
    bool isBuiltin = false;         // Injected builtin (outer namespace) - may be shadowed
};
```

### Field Details

| Field | Type | Description |
|---|---|---|
| `name` | `std::string` | The identifier as it appears in source code. |
| `kind` | `Symbol::Kind` | Classifies the symbol. Determines how it can be used. |
| `type` | `std::shared_ptr<Type>` | Always `nullptr` after Sema; populated later by `TypeChecker`. |
| `declaration` | `SourceLocation` | Where this symbol was first introduced. Set for user-defined symbols; not set for builtins. |
| `isGlobal` | `bool` | `true` only for symbols introduced by a `global` statement. |
| `isNonlocal` | `bool` | `true` only for symbols introduced by a `nonlocal` statement. |
| `isInitialized` | `bool` | `true` when the symbol has been assigned a value. Builtins, parameters, and global/nonlocal symbols are always marked initialized. Variables from `AnnAssignStmt` without a value have this `false`. |
| `isConst` | `bool` | `true` for a `const` binding (`const x: T = ...`, including the `const a, b = ...` tuple form). Drives the "cannot reassign const variable" error: any later bare `=` or `+=` to a `const`-bound name is rejected by `AssignStmt`/`AugAssignStmt`. Set from `AnnAssignStmt::isConst` / `AssignStmt::isConst`. |
| `isStatic` | `bool` | `true` for a `static` member declaration (`static x: T = ...`). Set from `AnnAssignStmt::isStatic`; carried for the later passes. |
| `isBuiltin` | `bool` | `true` for the names injected by `defineBuiltins()`. Builtins live in a conceptual outer namespace: a colliding user declaration **replaces** the builtin (`define` returns `true`) rather than being rejected as a redeclaration, and the redeclaration check in `AnnAssignStmt` explicitly skips a prior symbol whose `isBuiltin` is set. |

### Kind Enum Usage

| Kind | Created by |
|---|---|
| `Variable` | `AssignStmt`, `AnnAssignStmt`, `ForStmt` targets, `TryStmt` handler names, `WithStmt` `as` targets, `WalrusExpr`, `MatchStmt` capture patterns, `GlobalStmt`, `NonlocalStmt`, `FromImportStmt`, `TypeAliasStmt`, the `ListCompExpr`/`DictCompExpr`/`SetCompExpr`/`GeneratorExpr` loop variables, builtin constants (`True`, `False`, `None`, `__name__`, `__file__`, `__doc__`) |
| `Function` | `FunctionDecl`, most builtin functions (`print`, `len`, `range`, etc.) |
| `Class` | `ClassDecl`, builtin type names (`int`, `float`, `str`, `bool`, `list`, `dict`, `set`, `tuple`, `type`, `object`, `property`, `staticmethod`, `classmethod`), builtin exception types |
| `Parameter` | `FunctionDecl` parameters, `LambdaExpr` parameters |
| `Module` | `ImportStmt` |
| `TypeAlias` | Not currently used by any visitor (reserved for future type alias support) |

---

## 4. Scope Chain Mechanics

### The `Impl` Struct

```cpp
struct Sema::Impl {
    std::vector<SemaDiagnostic> diagnostics;
    std::vector<std::unique_ptr<Scope>> scopes;   // Owns all scopes
    Scope* currentScope = nullptr;                 // Points into scopes
    bool isInLoop = false;
    bool isInFunction = false;
};
```

All `Scope` objects are owned by `impl_->scopes` (a vector of
`unique_ptr<Scope>`). The `currentScope` raw pointer always points to the
topmost (most recently pushed) scope. Scopes are never removed from the
owning vector; they remain alive for the entire lifetime of the `Sema`
object.

### `pushScope(Scope::Kind kind)`

Creates a new `Scope` with the given kind and the current scope as its
parent. Updates `currentScope` to point to the new scope.

```cpp
void Sema::pushScope(Scope::Kind kind) {
    auto scope = std::make_unique<Scope>(kind, impl_->currentScope);
    impl_->currentScope = scope.get();
    impl_->scopes.push_back(std::move(scope));
}
```

### `popScope()`

Moves `currentScope` back to its parent. Does not deallocate the scope.

```cpp
void Sema::popScope() {
    impl_->currentScope = impl_->currentScope->parent();
}
```

### `currentScope() -> Scope*`

Returns `impl_->currentScope`.

### Scope Lifetime During `analyze()`

1. The `Sema` constructor calls `pushScope(Scope::Kind::Module)` and then
   `defineBuiltins()`. This means the module scope is ready before
   `analyze()` is called.
2. `analyze()` calls `module.accept(*this)` which visits every statement.
3. After the walk completes, `analyze()` calls `popScope()` to close the
   module scope.
4. The module scope (and all nested scopes) remain in `impl_->scopes` but
   `currentScope` is now `nullptr`.

### Nesting Example

For the following Dragon code:

```python
x: int = 1

def foo(a: int) -> int {
    y: int = a + x
    for i in range(y) {
        print(i)
    }
    return y
}

class Bar {
    def method(self) -> None {
        pass
    }
}
```

The scope chain at the `print(i)` call would be:

```
Module (x, foo, Bar, ...builtins)
  -> Function (a, y)              [isInFunction=true]
    -> Block (i)                  [isInLoop=true]   (the for-loop body scope)
```

Note: `for` **does** push a `Block` scope for its body, and the loop target
`i` is defined in that block scope (not the enclosing function scope). The
target is therefore not visible after the loop ends. Every other block-bearing
statement (`if`/`while`/`with`/`try`/`match`) pushes a `Block` scope the same
way; only `def`/`class`/`lambda`/`fire {}`/`thread {}` push `Function`/`Class`
scopes.

At `method`'s `pass` statement:

```
Module (x, foo, Bar, ...builtins)
  -> Class ()
    -> Function (self)      [isInFunction=true]
```

---

## 5. Builtin Definitions

The `defineBuiltins()` method is called once during `Sema` construction,
populating the module scope with every name that Dragon programs may reference
without explicit import or definition.

All builtins are defined with `isInitialized = true` and no `declaration`
location (default-constructed `SourceLocation`). No types are assigned (the
`type` field remains `nullptr`).

### Builtin Functions (Kind::Function)

These are defined with `Symbol::Kind::Function`:

| Name | Description |
|---|---|
| `print` | Output to stdout |
| `len` | Length of sequences |
| `range` | Integer range generator |
| `input` | Read from stdin |
| `abs` | Absolute value |
| `min` | Minimum of arguments |
| `max` | Maximum of arguments |
| `sum` | Sum of iterable |
| `sorted` | Sorted copy of iterable |
| `reversed` | Reversed iterator |
| `enumerate` | Index-value pairs |
| `zip` | Parallel iteration |
| `map` | Apply function to iterable |
| `filter` | Filter iterable by predicate |
| `any` | True if any element is truthy |
| `all` | True if all elements are truthy |
| `isinstance` | Type check at runtime |
| `issubclass` | Class hierarchy check |
| `hasattr` | Attribute existence check |
| `getattr` | Get attribute by name |
| `setattr` | Set attribute by name |
| `delattr` | Delete attribute by name |
| `id` | Object identity |
| `hash` | Hash value |
| `repr` | String representation |
| `chr` | Integer to character |
| `ord` | Character to integer |
| `hex` | Integer to hex string |
| `oct` | Integer to octal string |
| `bin` | Integer to binary string |
| `open` | File open |
| `super` | Parent class access |

### Builtin Type/Class Names (Kind::Class)

These are defined with `Symbol::Kind::Class`:

| Name | Description |
|---|---|
| `int` | Integer type constructor |
| `float` | Float type constructor |
| `str` | String type constructor |
| `bool` | Boolean type constructor |
| `list` | List type constructor |
| `dict` | Dict type constructor |
| `set` | Set type constructor |
| `tuple` | Tuple type constructor |
| `type` | Type introspection |
| `object` | Base class |
| `property` | Property descriptor |
| `staticmethod` | Static method decorator |
| `classmethod` | Class method decorator |

### Builtin Exception Types (Kind::Class)

| Name |
|---|
| `ValueError` |
| `TypeError` |
| `RuntimeError` |
| `IndexError` |
| `KeyError` |
| `AttributeError` |
| `StopIteration` |
| `Exception` |
| `BaseException` |
| `NotImplementedError` |
| `OSError` |
| `IOError` |
| `FileNotFoundError` |
| `ZeroDivisionError` |
| `OverflowError` |
| `NameError` |

### Builtin Constants (Kind::Variable)

| Name | Description |
|---|---|
| `True` | Boolean true |
| `False` | Boolean false |
| `None` | The null value |
| `__name__` | Module name (set to `"__main__"` for the entry module) |
| `__file__` | Source file path |

---

## 6. Statement Visiting

Each `visit()` method on `Sema` handles a specific AST statement type. Below
is a detailed account of how every statement visitor works.

### 6.1 `AssignStmt`

**AST fields:** `targets` (vector of `Expr`), `value` (single `Expr`),
`typeAnnotation` (optional `TypeExpr`), `isConst`.

This visitor enforces Dragon's ":-declares / =-assigns" rule. A name is
introduced exactly once per scope via `x: T = ...` (`AnnAssignStmt`) or an
implicit-binding form (tuple-unpack, `for`/`with as`/`except as`, walrus). A
bare `x = v` is a **reassignment**: the name must already resolve in scope, and
it never introduces a new (possibly shadowing) binding.

1. Visit `value` (recursively checking names within it).
2. Visit `typeAnnotation` if present.
3. For each target:
   - **`NameExpr` target.** Resolve the name up the scope chain with
     `resolveAcross()`, which records whether the search crossed out of the
     current function before finding the binding:
     - If a binding is found and it is `const`: emit
       `"cannot reassign const variable '<name>'"`.
     - If reaching the binding crossed a function boundary, `classifyRebind()`
       decides what is required: an enclosing **function's** variable needs
       `nonlocal`, a **module** global needs `global`. Without the marker the
       assignment is rejected (otherwise codegen would silently spin up a
       throwaway local - Python's "forgot the keyword" footgun). The declared
       `global`/`nonlocal` marker is itself found inside the current function,
       so a properly opted-in rebind resolves before the walk crosses out and
       is allowed.
     - If a binding is found and visible (no boundary crossed, or owner is a
       class scope), it is simply marked `isInitialized = true` - a plain
       reassignment.
     - If **no** binding is found and the target may not implicitly declare
       (the normal bare-`=` case), emit
       `"'<name>' is not declared; introduce it with '<name>: <type> = ...' (bare '=' only reassigns an existing variable)"`.
     - A new `Variable` symbol is defined only when implicit declaration is
       allowed: either the statement carries an annotation (`node.typeAnnotation`,
       the vestigial single-name declaration form) or the name is a
       tuple-unpack element (which has no per-element annotation slot).
   - **`TupleExpr` target** (tuple unpacking, `a, b = pair`). Each element name
     may implicitly declare with an inferred type (recursing through nested
     tuples and `*rest` starred targets). For the `const a, b = expr` form
     (`node.isConst`), every name is a *fresh* declaration: a same-scope
     redeclaration emits
     `"redeclaration of variable/parameter '<name>'; it is already declared in this scope"`,
     and each name binds `const`.
   - **Any other target** (`AttributeExpr`, `SubscriptExpr`, ...): validate via
     `isValidAssignmentTarget()` (emitting `"invalid assignment target"` on
     failure) and then visit it.

### 6.2 `AnnAssignStmt`

**AST fields:** `target` (`Expr`), `annotation` (`TypeExpr`), `value`
(optional `Expr`), `isConst`, `isStatic`.

This is the canonical declaration form (`x: T = ...`).

1. Visit `annotation` if present.
2. Visit `value` if present.
3. If target is a `NameExpr`:
   - Look it up in **this** scope with `lookupLocal()`. If a prior non-builtin
     symbol already exists in the same scope, this is a redeclaration: emit
     `"redeclaration of variable/parameter '<name>'; it is already declared in this scope (reassign with '<name> = ...')"`
     and stop (`parameter` vs `variable` is chosen from the prior symbol's
     kind). A prior symbol that is an injected builtin (`isBuiltin`) is **not**
     a redeclaration - the new declaration shadows it.
   - Otherwise define a new `Variable` symbol with `isInitialized = (value != nullptr)`,
     `isConst = node.isConst`, and `isStatic = node.isStatic`.

This means `x: int` (without `= ...`) defines `x` but marks it uninitialized,
and a second `x: T = ...` in the same scope is a hard error.

### 6.3 `AugAssignStmt`

**AST fields:** `target` (`Expr`), `op` (`Token`), `value` (`Expr`).

Visits both `target` and `value`, and defines no new symbols. An `n += 1` is a
read-modify-rebind of `n`, so when the target is a `NameExpr` it obeys the same
rebind rules as a bare `=`: it resolves the name with `resolveAcross()` and
emits `"cannot reassign const variable '<name>'"` for a `const` binding, or the
`global`/`nonlocal` rebind error (via `classifyRebind()`) when rebinding an
enclosing function's variable or a module global without the marker.

### 6.4 `FunctionDecl`

**AST fields:** `name`, `params`, `returnType`, `body`, `decorators`,
`isAsync`.

1. Define the function name in the **current** (outer) scope with
   `Kind::Function` and `isInitialized = true`.
2. Visit all `decorators` in the outer scope.
3. Push a new `Function` scope.
4. Save and set `isInFunction = true`.
5. For each parameter: define it in the function scope with
   `Kind::Parameter` and `isInitialized = true`. Visit the parameter's type
   annotation and default value if present.
6. Visit `returnType` if present.
7. Visit every statement in `body`.
8. Restore `isInFunction` to its previous value.
9. Pop the function scope.

### 6.5 `ClassDecl`

**AST fields:** `name`, `bases`, `keywords`, `body`, `decorators`.

1. Define the class name in the **current** (outer) scope with
   `Kind::Class` and `isInitialized = true`.
2. Visit all `decorators` in the outer scope.
3. Visit all `bases` in the outer scope.
4. Visit all `keywords` values in the outer scope.
5. Push a new `Class` scope.
6. Visit every statement in `body`.
7. Pop the class scope.

Note: unlike `FunctionDecl`, `ClassDecl` does NOT set `isInFunction`. This
means `return` inside a class body (but outside a method) would be caught
as an error only if `isInFunction` is `false` from the enclosing context.

### 6.6 `ForStmt`

**AST fields:** `target` (`Expr`), `iterable` (`Expr`), `body`, `elseBody`.

1. Visit `iterable` expression (in the enclosing scope).
2. Push a `Block` scope for the loop body.
3. Define the loop target(s) **in that block scope** with `Kind::Variable`,
   `isInitialized = true`: a single `NameExpr` (`for x in xs`) or each name of
   a `TupleExpr` (`for k, v in d.items()`). The target therefore is not visible
   after the loop.
4. Save and set `isInLoop = true`.
5. Visit every statement in `body`.
6. Restore `isInLoop`, then pop the body block scope.
7. Push a separate `Block` scope, visit every statement in `elseBody`, and pop
   it.

### 6.7 `WhileStmt`

**AST fields:** `condition` (`Expr`), `body`, `elseBody`.

1. Visit `condition` (in the enclosing scope).
2. Save and set `isInLoop = true`.
3. Push a `Block` scope, visit every statement in `body`, and pop it.
4. Restore `isInLoop`.
5. Push a separate `Block` scope, visit every statement in `elseBody`, and pop
   it.

### 6.8 `IfStmt`

**AST fields:** `condition`, `thenBody`, `elifClauses`, `elseBody`.

1. Visit `condition` (in the enclosing scope).
2. Push a `Block` scope, visit every statement in `thenBody`, and pop it.
3. For each elif clause: visit the condition in the enclosing scope, then push a
   `Block` scope, visit the clause body, and pop it.
4. Push a `Block` scope, visit every statement in `elseBody`, and pop it.

Each branch body gets its own `Block` scope, so a name declared in one branch
is not visible in the others or after the `if`. The conditions are evaluated in
the enclosing scope, so a walrus in a condition (`if (x := f()) > 0`) binds
outside the branch, matching Python.

### 6.9 `TryStmt`

**AST fields:** `tryBody`, `handlers` (vector of `ExceptHandler`),
`elseBody`, `finallyBody`.

1. Push a `Block` scope, visit every statement in `tryBody`, and pop it.
2. For each exception handler:
   a. Push a new `Block` scope.
   b. Visit the handler's `type` expression if present.
   c. If the handler has a `name` (the `as` variable), define it as
      `Kind::Variable`, `isInitialized = true`. It is therefore scoped to that
      handler's block only.
   d. Visit every statement in the handler's body.
   e. Pop the block scope.
3. Push a `Block` scope, visit every statement in `elseBody`, and pop it.
4. Push a `Block` scope, visit every statement in `finallyBody`, and pop it.

Each of the four body kinds (`try`, each `except`, `else`, `finally`) gets its
own `Block` scope, like every other block-bearing statement. This correctly
limits each handler variable's visibility to its handler.

### 6.10 `WithStmt`

**AST fields:** `items` (vector of `WithItem`), `body`.

1. Push a `Block` scope for the with-statement (the `as` bindings and the body
   share it).
2. For each `WithItem`:
   a. Visit `contextExpr`.
   b. If `optionalVars` is present and is a `NameExpr`: define it as
      `Kind::Variable`, `isInitialized = true`, in the with-block scope.
3. Visit every statement in `body`.
4. Pop the block scope.

The `as` variable is therefore not visible after the `with` ends.

### 6.11 `ReturnStmt`

1. If `isInFunction` is `false`: emit error `"'return' outside function"`.
2. Visit `value` if present.

### 6.12 `BreakStmt`

1. If `isInLoop` is `false`: emit error `"'break' outside loop"`.

### 6.13 `ContinueStmt`

1. If `isInLoop` is `false`: emit error `"'continue' outside loop"`.

### 6.14 `RaiseStmt`

1. Visit `exception` if present.
2. Visit `cause` if present.

No control flow validation (raise is valid anywhere).

### 6.15 `PassStmt`

No-op.

### 6.16 `AssertStmt`

1. Visit `test` expression.
2. Visit `msg` expression if present.

### 6.17 `DeleteStmt`

1. For each target: validate via `isValidAssignmentTarget()`. If invalid,
   emit error `"cannot delete this expression"`. Then visit the target.

### 6.18 `ExprStmt`

1. Visit the contained `expr`.

### 6.19 `GlobalStmt`

For each name in `node.names`: define a `Symbol` with `Kind::Variable`,
`isGlobal = true`, and `isInitialized = true`.

```cpp
void Sema::visit(GlobalStmt& node) {
    for (auto& name : node.names) {
        Symbol sym;
        sym.name = name;
        sym.kind = Symbol::Kind::Variable;
        sym.isGlobal = true;
        sym.isInitialized = true;
        currentScope()->define(sym);
    }
}
```

### 6.20 `NonlocalStmt`

For each name in `node.names`:

1. Walk the parent scope chain (starting from `currentScope()->parent()`,
   stopping at `Module` scope) looking for the **true** binding scope: a scope
   that has `name` via `lookupLocal()` whose symbol is **not itself** a
   `nonlocal` marker, and is not the module scope (Python: `nonlocal` cannot
   reference a global - use `global`).
2. If no such binding scope is found: emit error
   `"no binding for nonlocal '<name>' found"`.
3. Otherwise, for closure-capture tracking, mark every `CaptureContext` on the
   capture stack whose lambda/nested-function reaches the binding scope via its
   parent chain: each such context must capture the binding's *cell pointer*
   (recorded in both `capturedNames` and `nonlocalDeclaredNames`) so writes
   propagate back to the owner.
4. Define a `Symbol` with `Kind::Variable`, `isNonlocal = true`, and
   `isInitialized = true` in the current scope. This marker is what later lets
   `AssignStmt`'s `defineLocal` route writes to the existing binding instead of
   shadowing it with a fresh local.

The `global`/`nonlocal` markers are the mechanism behind the rebind enforcement
described in §6.1: because the marker is defined inside the current function,
`resolveAcross()` finds it before the search crosses out of the function, so an
opted-in rebind is allowed while a bare one is rejected.

### 6.21 `ImportStmt`

For each alias in `node.names`:

1. Compute `defName`: use `asName` if provided, otherwise use `name`.
2. If `defName` contains a dot, truncate to the first component (e.g.,
   `os.path` becomes `os`).
3. Define a `Symbol` with `Kind::Module` and `isInitialized = true`.

```cpp
std::string defName = alias.asName.empty() ? alias.name : alias.asName;
auto dotPos = defName.find('.');
if (dotPos != std::string::npos) {
    defName = defName.substr(0, dotPos);
}
```

### 6.22 `FromImportStmt`

For each alias in `node.names`:

1. Compute `defName`: use `asName` if provided, otherwise use `name`.
2. Define a `Symbol` with `Kind::Variable` and `isInitialized = true`.

Note: `from ... import` symbols are registered as `Variable`, not `Module`
or `Function`, because Sema cannot determine the kind without analyzing the
source module.

---

## 7. Control Flow Validation

Sema uses two boolean flags on the `Impl` struct:

```cpp
bool isInLoop = false;
bool isInFunction = false;
```

### `isInLoop`

Set to `true` before visiting the body of `ForStmt` and `WhileStmt`.
The previous value is saved and restored after the body is visited. This
correctly handles nested loops and non-loop constructs between loops.

Checked by `BreakStmt` and `ContinueStmt` visitors. When `false`, the error
messages are:

- `"'break' outside loop"`
- `"'continue' outside loop"`

### `isInFunction`

Set to `true` before visiting the body of `FunctionDecl` and `LambdaExpr`.
The previous value is saved and restored.

Checked by `ReturnStmt` and `YieldExpr` visitors. When `false`, the error
messages are:

- `"'return' outside function"`
- `"'yield' outside function"`

### Save/Restore Pattern

Both flags use the same pattern to handle nesting correctly:

```cpp
bool prevInLoop = impl_->isInLoop;
impl_->isInLoop = true;
// ... visit body ...
impl_->isInLoop = prevInLoop;
```

This means:

- `break` inside a function that is inside a loop is correctly **rejected**
  (the function boundary resets `isInFunction` but `isInLoop` is saved and
  restored around the function body, not the loop body).
- Actually, `isInLoop` is NOT reset to `false` when entering a function.
  If a function is defined inside a loop, `isInLoop` remains `true` inside
  the function body. This is a known limitation: `break` inside a nested
  function definition would not be flagged.

---

## 8. Expression Visiting

### 8.1 Literal Expressions

`IntegerLiteral`, `FloatLiteral`, `StringLiteral`, `BooleanLiteral`,
`NoneLiteral` -- all are no-ops. They introduce no names and require no
validation at the Sema level.

### 8.2 `NameExpr`

Looks up `node.name` in the current scope chain via `currentScope()->lookup()`.
If not found, emits:

```
"undefined name '<name>'"
```

### 8.3 `BinaryExpr`

Visits `left` and `right` recursively. No semantic checks.

### 8.4 `UnaryExpr`

Visits `operand` recursively. No semantic checks.

### 8.5 `CallExpr`

Visits `callee`, all positional `args`, and all keyword argument values in
`kwArgs`. No arity or argument-type checking (that is done by TypeChecker).

### 8.6 `AttributeExpr`

Visits `object`. Does not validate that the attribute exists (that is a type
checker responsibility).

### 8.7 `SubscriptExpr`

Visits both `object` and `index`.

### 8.8 `SliceExpr`

Visits `lower`, `upper`, and `step` (each if non-null).

### 8.9 Collection Literals

`ListExpr`, `TupleExpr`, `DictExpr`, `SetExpr` -- visit all child
expressions. `DictExpr` visits both keys and values.

### 8.10 `ListCompExpr`

1. Push a `Block` scope.
2. Define `varName` as `Kind::Variable`, `isInitialized = true`.
3. Visit `iterable`.
4. Visit `element`.
5. Visit `condition` if present.
6. Pop the block scope.

The comprehension variable is scoped to the comprehension, matching Python 3
semantics.

### 8.11 `DictCompExpr`

1. Push a `Block` scope.
2. Define all names in `varNames` as `Kind::Variable`, `isInitialized = true`.
3. Visit `iterable`.
4. Visit `key` and `value`.
5. Visit `condition` if present.
6. Pop the block scope.

### 8.12 `LambdaExpr`

1. Push a `Function` scope.
2. Define each parameter as `Kind::Parameter`, `isInitialized = true`.
   Visit parameter type annotations and default values.
3. Visit `returnType` if present.
4. Save and set `isInFunction = true`.
5. Visit `body` (single expression) if present.
6. Visit all `bodyStmts` (Dragon block lambda form).
7. Restore `isInFunction`.
8. Pop the function scope.

### 8.13 `IfExpr` (Ternary)

Visits `condition`, `thenExpr`, and `elseExpr`.

### 8.14 `AwaitExpr`

Visits `operand`. No validation that the context is async.

### 8.15 `YieldExpr`

1. If `isInFunction` is `false`: emit error `"'yield' outside function"`.
2. Visit `value` if present.

### 8.16 `StarredExpr`

Visits `value`.

---

## 9. Error Reporting

### `SemaDiagnostic` Struct

```cpp
struct SemaDiagnostic {
    enum class Level { Warning, Error };
    Level level;
    SourceLocation location;
    std::string message;
};
```

All diagnostics are appended to `impl_->diagnostics`.

### Helper Methods

```cpp
void Sema::error(const SourceLocation& loc, const std::string& message) {
    impl_->diagnostics.push_back({SemaDiagnostic::Level::Error, loc, message});
}

void Sema::warning(const SourceLocation& loc, const std::string& message) {
    impl_->diagnostics.push_back({SemaDiagnostic::Level::Warning, loc, message});
}
```

### `hasErrors()`

Returns `true` if any diagnostic has `Level::Error`. Warnings alone do not
cause `analyze()` to return `false`.

```cpp
bool Sema::hasErrors() const {
    for (const auto& d : impl_->diagnostics) {
        if (d.level == SemaDiagnostic::Level::Error) return true;
    }
    return false;
}
```

### Complete List of Error Messages

| Error message | Emitted by |
|---|---|
| `"undefined name '<name>'"` | `visit(NameExpr&)` when `lookup()` returns `nullptr` |
| `"invalid assignment target"` | `visit(AssignStmt&)` for targets that fail `isValidAssignmentTarget()` |
| `"'<name>' is not declared; introduce it with '<name>: <type> = ...' (bare '=' only reassigns an existing variable)"` | `visit(AssignStmt&)` for a bare `=` to a `NameExpr` that does not resolve in scope (the ":-declares / =-assigns" rule) |
| `"cannot reassign const variable '<name>'"` | `visit(AssignStmt&)` / `visit(AugAssignStmt&)` when the target resolves to a `const` binding |
| `"'<name>' is owned by an enclosing function; add 'nonlocal <name>' to rebind it, or declare a new local with '<name>: <type> = ...'"` | `visit(AssignStmt&)` / `visit(AugAssignStmt&)` (`classifyRebind` -> `NeedNonlocal`) when assigning an enclosing function's variable without `nonlocal` |
| `"'<name>' is a module global; add 'global <name>' to assign it inside a function, or declare a new local with '<name>: <type> = ...'"` | `visit(AssignStmt&)` / `visit(AugAssignStmt&)` (`classifyRebind` -> `NeedGlobal`) when assigning a module global inside a function without `global` |
| `"redeclaration of variable/parameter '<name>'; it is already declared in this scope ..."` | `visit(AnnAssignStmt&)` (and the `const` tuple-unpack path in `visit(AssignStmt&)`) when a name is already bound by a non-builtin symbol in the same scope |
| `"'return' outside function"` | `visit(ReturnStmt&)` when `isInFunction` is `false` |
| `"'break' outside loop"` | `visit(BreakStmt&)` when `isInLoop` is `false` |
| `"'continue' outside loop"` | `visit(ContinueStmt&)` when `isInLoop` is `false` |
| `"'yield' outside function"` | `visit(YieldExpr&)` when `isInFunction` is `false` |
| `"no binding for nonlocal '<name>' found"` | `visit(NonlocalStmt&)` when no enclosing function scope binds the name |
| `"cannot delete this expression"` | `visit(DeleteStmt&)` for targets that fail `isValidAssignmentTarget()` |

Note: the current implementation emits no warnings. The `warning()` helper
exists for future use.

---

## 10. Assignment Target Validation

The `isValidAssignmentTarget()` helper determines whether an expression can
appear on the left side of `=` or in a `del` statement.

```cpp
bool Sema::isValidAssignmentTarget(Expr* expr) {
    if (dynamic_cast<NameExpr*>(expr)) return true;
    if (dynamic_cast<AttributeExpr*>(expr)) return true;
    if (dynamic_cast<SubscriptExpr*>(expr)) return true;
    if (auto* tuple = dynamic_cast<TupleExpr*>(expr)) {
        for (auto& e : tuple->elements) {
            if (!isValidAssignmentTarget(e.get())) return false;
        }
        return true;
    }
    if (auto* list = dynamic_cast<ListExpr*>(expr)) {
        for (auto& e : list->elements) {
            if (!isValidAssignmentTarget(e.get())) return false;
        }
        return true;
    }
    if (auto* starred = dynamic_cast<StarredExpr*>(expr)) {
        return isValidAssignmentTarget(starred->value.get());
    }
    return false;
}
```

Valid assignment targets:

| Expression Type | Example | Valid? |
|---|---|---|
| `NameExpr` | `x` | Yes |
| `AttributeExpr` | `obj.field` | Yes |
| `SubscriptExpr` | `arr[0]` | Yes |
| `TupleExpr` | `(a, b, c)` | Yes (if all elements are valid targets) |
| `ListExpr` | `[a, b, c]` | Yes (if all elements are valid targets) |
| `StarredExpr` | `*rest` | Yes (if inner value is a valid target) |
| Anything else | `1 + 2`, `f()` | No |

---

## 11. Import Resolution

The `resolveImport()` method is declared but currently a no-op:

```cpp
void Sema::resolveImport(const std::string&) {
    // Import resolution is a no-op for now
}
```

Import handling in Sema is purely about registering names in the current
scope. The actual module loading, file resolution, and cross-module type
propagation are handled by the `ModuleResolver` component and the
`TypeChecker`'s `registerExternalModule()` / `getExports()` API.

### How `ImportStmt` Registers Symbols

For `import os.path`:
- `defName` = `"os.path"` (no alias)
- Truncated at first dot: `defName` = `"os"`
- Symbol defined: `{name: "os", kind: Module, isInitialized: true}`

For `import numpy as np`:
- `defName` = `"np"` (alias used)
- No dot in `"np"`, no truncation
- Symbol defined: `{name: "np", kind: Module, isInitialized: true}`

### How `FromImportStmt` Registers Symbols

For `from math import sin, cos`:
- `sin` defined as `{name: "sin", kind: Variable, isInitialized: true}`
- `cos` defined as `{name: "cos", kind: Variable, isInitialized: true}`

For `from math import pi as PI`:
- `PI` defined as `{name: "PI", kind: Variable, isInitialized: true}`

---

## 12. Type Expression Visitors

Sema's type expression visitors perform minimal work -- they exist to
recursively walk the type annotation AST but perform no name resolution on
types (that is the TypeChecker's job).

| TypeExpr | Sema behavior |
|---|---|
| `NamedTypeExpr` | No-op |
| `GenericTypeExpr` | Visits `base` and all `typeArgs` |
| `OptionalTypeExpr` | Visits `inner` |
| `UnionTypeExpr` | Visits all `types` |
| `CallableTypeExpr` | Visits all `paramTypes` and `returnType` |
| `TupleTypeExpr` | Visits all `elementTypes` |

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
  Sema   (name resolution, scope analysis)   <--- this pass
    |
    v
  TypeChecker (type inference, type checking)
    |
    v
  CodeGen (LLVM IR generation)
```

Sema does not transform the AST. Its only output is the list of
`SemaDiagnostic` entries. If `hasErrors()` returns `true`, the Driver
stops compilation and reports the diagnostics to the user. Otherwise,
the AST (unchanged) is passed to the TypeChecker.

---

## Previous Document

[004 - Dragon AST](004-ast.md)

## Next Document

[006 - Type System and Type Checker](006-typechecker.md)
