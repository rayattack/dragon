# 011 -- Type Hint Enforcement

> **Last Updated:** 2026-06-22

Dragon demands types. The `.dr` file format enforces type annotations at the
syntax level -- the parser rejects functions without parameter types or return
types. But Dragon also compiles `.py` files, and Python's syntax allows type
annotations to be entirely optional. The TypeHintEnforcer bridges this gap: it
is a dedicated pass that validates the **presence** of type annotations on
`.py` files before they enter the compilation pipeline.

---

## 1. Purpose: Annotation Presence, Not Correctness

The TypeHintEnforcer is **not** the TypeChecker. These are two separate passes
with distinct responsibilities:

| Pass                | Question Asked                                    |
|---------------------|---------------------------------------------------|
| TypeHintEnforcer    | "Did the programmer write a type annotation here?" |
| TypeChecker         | "Are the types correct and compatible?"             |

The enforcer does not evaluate whether `x: int = "hello"` is a valid
assignment. It only checks that `x` has a type annotation at all. The
TypeChecker handles correctness later in the pipeline.

This separation exists because Dragon's `.dr` parser enforces annotation
presence syntactically (a function parameter without a type annotation is a
parse error), so the enforcer only needs to run on `.py` files where the
parser permits bare names.

---

## 2. When It Runs

The TypeHintEnforcer runs in two contexts within the Driver pipeline:

### 2.1 Direct Compilation of `.py` Files

When the user compiles a `.py` file directly (`dragon build main.py`), the
enforcer runs after parsing and before semantic analysis:

```
Lexer -> Parser -> [TypeHintEnforcer] -> Sema -> TypeChecker -> ...
```

This is triggered in `Driver::buildFile()` and `Driver::checkFile()`:

```cpp
if (!isDragon) {
    TypeHintEnforcer enforcer;
    if (!enforcer.enforce(*module)) {
        // print diagnostics, return 1
    }
}
```

### 2.2 Imported `.py` Modules

When a `.dr` file imports a `.py` module, the imported module must also pass
type hint enforcement. This happens during the multi-module processing phase
in `Driver::buildFile()`:

```cpp
for (auto& mod : graph.modules) {
    if (!mod.isDragon) {
        EnforcerOptions enfOpts;
        enfOpts.isImportedModule = true;
        enfOpts.importingFile = filename;
        TypeHintEnforcer enforcer(enfOpts);
        if (!enforcer.enforce(*mod.ast)) {
            std::cerr << impl_->formatter.formatUntypedImport(mod.filepath);
            // print individual diagnostics
            return 1;
        }
    }
}
```

The `isImportedModule` flag activates the "Borders must be secured" error
message (see Section 9).

### 2.3 When It Does NOT Run

- `.dr` files: Never. The parser handles type enforcement for Dragon syntax.
- `dragon check` with a `.dr` file: Never. `checkFile()` skips the enforcer
  when `isDragon` is true.

---

## 3. EnforcerOptions

```cpp
struct EnforcerOptions {
    bool requireFunctionParamTypes = true;
    bool requireReturnTypes = true;
    bool requireModuleVarTypes = true;
    bool isImportedModule = false;       // activates "borders" messaging
    std::string importingFile;           // which file triggered the import
};
```

All three `require*` flags default to `true`. The constructor accepts options
by value:

```cpp
TypeHintEnforcer::TypeHintEnforcer(EnforcerOptions options)
    : options_(std::move(options)) {}
```

When no options are passed (default construction), the enforcer requires type
annotations on all function parameters, all return types, and all module-level
variables.

---

## 4. The Enforcement Algorithm

`TypeHintEnforcer::enforce(Module& module)` walks the module's top-level
statements and dispatches to specialized checkers. Because the enforcer runs
before name resolution, it cannot ask Sema whether a bare `name = ...`
reassigns an existing binding or introduces a new one. It therefore tracks the
module-level declared names itself in a `declaredModuleNames` set, mirroring
the Sema rule (`:` declares, `=` reassigns): an annotated declaration records
the name, and only a *genuine first* bare-NameExpr declaration is checked - a
later bare reassignment of an already-declared name is skipped:

```cpp
bool TypeHintEnforcer::enforce(Module& module) {
    diagnostics_.clear();

    std::set<std::string> declaredModuleNames;

    for (auto& stmt : module.body) {
        if (auto* func = dynamic_cast<FunctionDecl*>(stmt.get())) {
            checkFunction(*func);
        } else if (auto* cls = dynamic_cast<ClassDecl*>(stmt.get())) {
            checkClass(*cls);
        } else if (auto* ann = dynamic_cast<AnnAssignStmt*>(stmt.get())) {
            // `x: int = 0` is a complete declaration; record the name so a
            // later bare `x = x + 1` is recognized as a reassignment.
            collectImplicitlyDeclaredNames(ann->target.get(), declaredModuleNames);
        } else if (auto* assign = dynamic_cast<AssignStmt*>(stmt.get())) {
            const bool isBareSingleName =
                assign->targets.size() == 1 &&
                dynamic_cast<NameExpr*>(assign->targets[0].get()) != nullptr;
            const bool isReassign =
                isBareSingleName && !assign->typeAnnotation &&
                declaredModuleNames.count(
                    static_cast<NameExpr*>(assign->targets[0].get())->name) != 0;

            // Only a genuine first declaration is checked; a bare reassignment
            // (`counter = counter + 1`) and subscript/attribute mutations are not.
            if (!isReassign) checkModuleLevelAssign(*assign);

            // Record names this statement binds (annotated form, tuple/list
            // unpacking, chained `a = b = c`) for later reassignment checks.
            if (assign->typeAnnotation || !isBareSingleName) {
                for (auto& target : assign->targets) {
                    collectImplicitlyDeclaredNames(target.get(), declaredModuleNames);
                }
            }
        }
    }

    return !hasErrors();
}
```

The method returns `true` if no errors were found. `hasErrors()` scans the
diagnostics vector for any entry with `Level::Error`. The
`collectImplicitlyDeclaredNames` helper records every name bound by a target,
recursing into tuple/list unpacking and ignoring subscript/attribute targets
(which mutate an existing object rather than declare).

---

## 5. Function Parameter Checking

`checkFunction(FunctionDecl& func, bool isMethod)` checks each parameter for
a type annotation:

```cpp
void TypeHintEnforcer::checkFunction(FunctionDecl& func, bool isMethod) {
    if (!options_.requireFunctionParamTypes && !options_.requireReturnTypes)
        return;

    if (options_.requireFunctionParamTypes) {
        for (size_t i = 0; i < func.params.size(); ++i) {
            auto& param = func.params[i];

            // Skip 'self' and 'cls' as first parameter of methods
            if (isMethod && i == 0 &&
                (param.name == "self" || param.name == "cls")) {
                continue;
            }

            // *args and **kwargs don't strictly require type annotations
            if (param.isVarArg || param.isKwArg) continue;

            if (!param.type) {
                addError(func.location(),
                    "missing type annotation for parameter '" + param.name +
                    "' in function '" + func.name + "'");
            }
        }
    }
    // ... return type check (see Section 5.2) ...
}
```

### 5.1 Exemptions

Three categories of parameters are exempt from the type annotation requirement:

1. **`self` and `cls`**: When checking a method (`isMethod == true`), the
   first parameter is skipped if its name is `self` or `cls`. These are
   conventional Python instance/class method markers whose types are always
   implicit (the enclosing class type).

2. **`*args` (vararg)**: Parameters with `isVarArg == true` are skipped.
   Python's `*args` syntax makes the type annotation complex
   (`*args: int` means "all args are int"), and many valid Python programs
   omit it.

3. **`**kwargs` (keyword arg)**: Parameters with `isKwArg == true` are
   skipped for the same reason as `*args`.

### 5.2 Return Type Checking

```cpp
if (options_.requireReturnTypes) {
    if (func.name != "__init__" && !func.returnType) {
        addError(func.location(),
            "missing return type annotation for function '" + func.name + "'");
    }
}
```

**`__init__` is exempt**: Python's `__init__` always returns `None` implicitly.
Requiring `-> None` on every `__init__` would be pedantic; PEP 484 recommends
it but does not require it, and Dragon follows the pragmatic convention of
allowing it to be omitted.

All other functions must have a return type annotation (the `-> Type` syntax
after the parameter list).

---

## 6. Module-Level Variable Checking

```cpp
void TypeHintEnforcer::checkModuleLevelAssign(AssignStmt& assign) {
    if (!options_.requireModuleVarTypes) return;

    if (!assign.typeAnnotation) {
        std::string varName = "<unknown>";
        if (!assign.targets.empty()) {
            if (auto* name = dynamic_cast<NameExpr*>(assign.targets[0].get())) {
                varName = name->name;
            }
        }

        // Skip dunder variables
        if (varName.size() >= 4 && varName.substr(0, 2) == "__" &&
            varName.substr(varName.size() - 2) == "__") {
            return;
        }

        addError(assign.location(),
            "missing type annotation for module-level variable '" + varName + "'");
    }
}
```

### 6.1 What Triggers This Check

Only top-level assignments in the module body are checked, and only when they
are a genuine *first declaration*. Assignments inside functions, methods,
loops, or conditionals are not checked by the enforcer (those are handled by
the TypeChecker's type inference). A bare reassignment of an already-declared
name (`counter = counter + 1`), a subscript or attribute mutation
(`cache["k"] = v`, `obj.f = v`), and tuple/list unpacking targets are all
exempt - the `enforce()` loop filters them out before calling
`checkModuleLevelAssign` (see Section 4).

Both annotated declarations (`AnnAssignStmt`, e.g. `x: int = 42`) and the
vestigial annotated `AssignStmt` form carry the annotation; the bare
`AssignStmt` form does not:

```python
x: int = 42       # typeAnnotation is present -> passes
x = 42             # first declaration, no annotation -> ERROR
x = x + 1          # reassignment of declared x -> exempt
```

### 6.2 Dunder Variable Exemption

Variables whose names start and end with double underscores (`__name__`,
`__version__`, `__all__`, etc.) are exempt. These are conventional Python
module metadata attributes that typically have obvious types (`str` or
`list[str]`), and requiring annotations on them would create friction when
importing standard Python modules.

The detection uses substring checks:

```cpp
varName.size() >= 4 &&
varName.substr(0, 2) == "__" &&
varName.substr(varName.size() - 2) == "__"
```

This matches `__version__`, `__all__`, `__name__`, etc. but not `__private`
(which ends with only one underscore set) or `_dunder_` (which starts with
only one underscore).

---

## 7. Class Checking

```cpp
void TypeHintEnforcer::checkClass(ClassDecl& cls) {
    for (auto& stmt : cls.body) {
        if (auto* method = dynamic_cast<FunctionDecl*>(stmt.get())) {
            checkFunction(*method, /*isMethod=*/true);
        }
    }
}
```

The class checker iterates over the class body and delegates each method to
`checkFunction` with `isMethod = true`. This enables the `self`/`cls`
exemption (Section 5.1).

Class-level variable annotations (e.g., `x: int = 5` in the class body
outside any method) are currently not checked by the enforcer. Only method
signatures are validated.

---

## 8. Diagnostics

### 8.1 EnforcerDiagnostic Structure

```cpp
struct EnforcerDiagnostic {
    enum class Level { Error, Warning };
    Level level = Level::Error;
    SourceLocation location;
    std::string message;
};
```

All diagnostics produced by the enforcer are currently at `Level::Error`.
There is no warning-level enforcement.

### 8.2 Error Generation

The `addError` helper creates a diagnostic and appends it to the internal
vector:

```cpp
void TypeHintEnforcer::addError(SourceLocation loc, const std::string& message) {
    EnforcerDiagnostic diag;
    diag.level = EnforcerDiagnostic::Level::Error;
    diag.location = loc;
    diag.message = message;
    diagnostics_.push_back(std::move(diag));
}
```

### 8.3 Error Messages

The enforcer produces three types of error messages:

1. **Missing parameter type**:
   `"missing type annotation for parameter 'x' in function 'foo'"`

2. **Missing return type**:
   `"missing return type annotation for function 'foo'"`

3. **Missing module variable type**:
   `"missing type annotation for module-level variable 'count'"`

### 8.4 Accessing Diagnostics

```cpp
const std::vector<EnforcerDiagnostic>& diagnostics() const;
bool hasErrors() const;
```

`hasErrors()` returns `true` if any diagnostic has `Level::Error`. The
`enforce()` method returns `!hasErrors()`, so a return value of `false` means
enforcement failed.

---

## 9. Integration with the Driver

### 9.1 Direct File Compilation

In `Driver::buildFile()` and `Driver::checkFile()`, diagnostics are formatted
by the `DiagnosticFormatter`:

```cpp
if (!isDragon) {
    TypeHintEnforcer enforcer;
    if (!enforcer.enforce(*module)) {
        for (const auto& diag : enforcer.diagnostics()) {
            if (diag.level == EnforcerDiagnostic::Level::Error) {
                std::cerr << impl_->formatter.format(
                    filename,
                    diag.location.line,
                    diag.location.column,
                    "error",
                    diag.message);
            }
        }
        return 1;
    }
}
```

With the default Dragon theme, this produces output like:

```
DRAGON SCALE ERROR: missing type annotation for parameter 'x' in function 'add' at [math.py:3:1]
```

### 9.2 Imported Module Enforcement

When processing imported `.py` modules in the multi-module pipeline, the
enforcer is constructed with `isImportedModule = true`:

```cpp
EnforcerOptions enfOpts;
enfOpts.isImportedModule = true;
enfOpts.importingFile = filename;
TypeHintEnforcer enforcer(enfOpts);
if (!enforcer.enforce(*mod.ast)) {
    std::cerr << impl_->formatter.formatUntypedImport(mod.filepath);
    for (const auto& diag : enforcer.diagnostics()) {
        // ... format and print each diagnostic ...
    }
    return 1;
}
```

The `formatUntypedImport()` method produces the "Borders must be secured"
banner before individual diagnostics:

```
Borders must be secured: lib/math_utils.py must be strictly typed to be imported into a Dragon context.
DRAGON SCALE ERROR: missing type annotation for parameter 'x' in function 'add' at [lib/math_utils.py:3:1]
DRAGON SCALE ERROR: missing return type annotation for function 'add' at [lib/math_utils.py:3:1]
```

---

## 10. The "Borders Must Be Secured" Philosophy

Dragon's type system depends on knowing the types of everything at compile
time. When a `.dr` file imports a `.py` file, the type checker needs to know
the types of all exported symbols (functions, variables, classes) from that
`.py` file. If the `.py` file lacks type annotations, the type checker has
nothing to work with.

Dragon's position is explicit: **module boundaries are type boundaries**. A
`.py` file that wants to participate in the Dragon ecosystem must declare its
types. This is not optional and not something Dragon will silently infer.

The "Borders must be secured" message is deliberately strong. It communicates
that Dragon treats untyped imports as a security boundary violation --
untrusted code (code without declared types) cannot cross into trusted
territory (the Dragon type system) without being properly annotated.

This philosophy has practical consequences:

1. **Third-party `.py` libraries** must have type stubs or inline type
   annotations to be importable from Dragon code.
2. **Gradual typing** is supported within `.py` files compiled directly
   (`dragon build file.py`), but not at import boundaries.
3. **The `--site-packages` flag** enables importing from pip-installed
   packages, but those packages still must be typed.

The implementation is straightforward: the same `TypeHintEnforcer` is used
for both direct compilation and import enforcement. The only difference is the
`isImportedModule` flag, which changes the error messaging (adding the
"Borders" banner) but not the enforcement logic.

---

## 11. Complete Enforcement Matrix

| Construct                   | Checked? | Exemptions                              |
|-----------------------------|----------|-----------------------------------------|
| Function parameter type     | Yes      | `self`/`cls` (1st param of methods), `*args`, `**kwargs` |
| Function return type        | Yes      | `__init__` (implicit `-> None`)         |
| Module-level variable type  | Yes      | Dunder variables (`__name__`, etc.)     |
| Class-level variable type   | No       | Not currently enforced                  |
| Local variable type         | No       | Handled by TypeChecker inference        |
| Loop variable type          | No       | Handled by TypeChecker inference        |
| Comprehension variable type | No       | Handled by TypeChecker inference        |
| Lambda parameter type       | No       | Not currently enforced at this pass     |

---

## 12. Source File Reference

| File                                       | Role                                  |
|--------------------------------------------|---------------------------------------|
| `include/dragon/TypeHintEnforcer.h`        | Class declaration, `EnforcerOptions`, `EnforcerDiagnostic` |
| `src/TypeHintEnforcer.cpp`                 | Implementation (~177 lines)           |
| `include/dragon/DiagnosticFormatter.h`     | `DiagnosticStyle`, formatter class    |
| `src/DiagnosticFormatter.cpp`              | `formatUntypedImport()` and friends   |
| `src/Driver.cpp`                           | Integration points (`buildFile()`, `checkFile()`) |
| `test/TypeHintEnforcerTest.cpp`            | Test suite                            |

---

## Previous Document

[010 - CLI Driver and Build Pipeline](010-driver.md)

## Next Document

[012 - Dragon Standard Library Mappings](012-stdlib.md)
