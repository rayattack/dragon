# Dragon Bilingual Compiler - Unified Implementation Plan [Done]

This is the plan for getting `.dr` and `.py` to play nice in one pipeline - same compiler, native binary out the other end. At the time Dragon was ~10,600 lines of C++17, v0.1.0, 607 tests across 8 suites. Binary was `dr` with `run`, `build`, `migrate`, `check`. Two backends: LLVM CodeGen and CEmitter (C transpiler, which was already the path that actually worked). Lexer/Parser already toggled brace vs indent mode; imports were rudimentary string-concat of C output.

Goal was simple on paper: one frontend, one primary backend, stop maintaining parallel worlds. Occassionally messy in execution, but one plan beats three seperate docs that contradict each other after two weeks. Merged the dual-frontend work and CLI rebrand here so I'd have one checklist instead of arguing about phase ordering at 2am over coffee number four.

---

## Phase 1: CLI Rebrand + Cleanup (Foundation)

**Goal:** Rename `dr` → `dragon`, kill migrate + PythonMigrator/TypeInference, fix usage strings. 570 tests should still pass (607 minus 37 MigratorTests). Build with `-j4`, not `-j$(nproc)` - learned that the hard way.

### 1.1 Rename binary target
- **CMakeLists.txt:60-65** - Change `add_executable(dr ...)` to `add_executable(dragon ...)`, update `target_link_libraries` and `install` targets
- Rename `tools/dr/` directory to `tools/dragon/`

### 1.2 Update Driver usage strings
- **src/Driver.cpp:116-143** - Replace all `dr` references with `dragon` in `printUsage`, remove migrate examples, bump version to `0.2.0`
- **src/Driver.cpp:146-148** - Update `printVersion`

### 1.3 Remove migrate command
- **src/Driver.cpp:39-40** - Remove `"migrate"` branch from `parseArgs`
- **src/Driver.cpp:99-100** - Remove `case Action::Migrate` from `run`
- **src/Driver.cpp:354-375** - Delete `migrateFile` method entirely
- **include/dragon/Driver.h:15** - Remove `Migrate` from `Action` enum
- **include/dragon/Driver.h:59** - Remove `migrateFile` declaration

### 1.4 Delete PythonMigrator and TypeInference
- **Delete files:**
 - `src/PythonMigrator.cpp`
 - `src/TypeInference.cpp`
 - `include/dragon/PythonMigrator.h`
 - `include/dragon/TypeInference.h`
- **CMakeLists.txt:28,31** - Remove `src/TypeInference.cpp` and `src/PythonMigrator.cpp` from `DRAGON_SOURCES`
- **include/dragon.h:13,15** - Remove `#include "dragon/TypeInference.h"` and `#include "dragon/PythonMigrator.h"`
- **src/Driver.cpp:8** - Remove `#include "dragon/PythonMigrator.h"`

### 1.5 Delete migrator tests
- **Delete:** `test/MigratorTest.cpp`
- **test/CMakeLists.txt:49-52** - Remove `dragon_migrator_tests` target

### Check:
Build project, run 570 remaining tests, verify `dragon --help` shows updated output.

---

## Phase 2: CEmitter 3-Way Split (Refactor)

**Goal:** Split the 2,716-line `CEmitter.cpp` into 3 focused files before adding new features. All 149 CEmitter tests continue passing unchanged.

### 2.1 Extract runtime header generation
- **New: `src/CEmitterRuntime.cpp`** + **`include/dragon/CEmitterRuntime.h`**
 - Move the ~690-line inline C runtime string generation from `CEmitter::emit` (lines 118-808) into a standalone function: `std::string generateRuntimeHeader`
 - This includes all `dragon_list_*`, `dragon_dict_*`, `dragon_str_*`, `dragon_print_*` runtime C code, exception handling macros, and type definitions
 - `CEmitter::emit` calls `generateRuntimeHeader` instead of inlining

### 2.2 Extract stdlib registry and import mapping
- **New: `src/CEmitterStdlib.cpp`** + **`include/dragon/CEmitterStdlib.h`**
 - Move `getStdlibRegistry` (currently around line 2398) and all import-related emission logic (`visit(ImportStmt&)`, `visit(FromImportStmt&)`) into a `StdlibRegistry` class
 - Expose `StdlibRegistry::resolve(moduleName)` returning C include + symbol mappings
 - CEmitter delegates import visitor calls to the registry

### 2.3 Core CEmitter remains
- **`src/CEmitter.cpp`** - Now contains only:
 - `Impl` struct with shared state
 - Expression visitors (lines 809-2032)
 - Statement visitors (lines 2033-2387)
 - Declaration visitors (lines 2488-2716)
 - Approximately ~1,900 lines (down from 2,716)

### 2.4 Update CMake
- **CMakeLists.txt** - Add `src/CEmitterRuntime.cpp` and `src/CEmitterStdlib.cpp` to `DRAGON_SOURCES`
- **include/dragon.h** - Add new includes if needed (or keep internal to CEmitter)

### Check:
All 149 CEmitter tests pass with zero changes. Build succeeds. Output binary produces identical results for existing test files.

---

## Phase 3: Dragon-Themed Error System

**Goal:** Replace the 8 duplicated error-printing loops in Driver with a centralized `DiagnosticFormatter` that emits Dragon-branded errors with suggestions.

### 3.1 Create DiagnosticFormatter
- **New: `include/dragon/DiagnosticFormatter.h`** - `DiagnosticFormatter` class with:
 - `format(filename, location, level, message, suggestion)` - general diagnostic formatting
 - `formatMissingType(filename, location, symbolName, inferredType)` - PEP-484 hint errors
 - `formatUntypedImport(importingFile, importedFile)` - "Borders must be secured" errors
 - `DiagnosticStyle` options: `useDragonTheme`, `showSuggestions`, `colorOutput`
- **New: `src/DiagnosticFormatter.cpp`** - Implementation producing themed output:
 ```
 DRAGON SCALE ERROR: Missing type hint at [file.py:12:5]
 | def process(data):
 | ^ parameter 'data' requires a type annotation
 Suggestion: "To breathe fire, the Dragon needs to know this type. Add ': int', ': str', etc."
 ```

### 3.2 Integrate into Driver
- **src/Driver.cpp** - Add `DiagnosticFormatter` to `Driver::Impl`. Replace the 8 copy-pasted diagnostic loops (lines 182-190, 202-210, 221-229, 233-241, 400-408, 420-428, 438-446, 451-459) with calls to `formatter.format`.
- Add `--no-color` flag to `DriverOptions` and `parseArgs`

### 3.3 Update CMake and tests
- **CMakeLists.txt** - Add `src/DiagnosticFormatter.cpp` to `DRAGON_SOURCES`
- **include/dragon.h** - Add `#include "dragon/DiagnosticFormatter.h"`
- **New: `test/DiagnosticTest.cpp`** - ~15-20 tests for formatting, ANSI output, suggestions

### Check:
All 570 tests pass. Manual test: compile a file with a type error and verify Dragon-themed output.

---

## Phase 4: Strict PEP-484 Enforcement for .py Files

**Goal:** When a `.py` file is compiled, enforce that all function parameters, return types, and module-level variables have PEP-484 type annotations. Untyped `.py` files produce clear Dragon-themed errors.

### 4.1 Create TypeHintEnforcer pass
- **New: `include/dragon/TypeHintEnforcer.h`** - `TypeHintEnforcer` (extends `DefaultASTVisitor`) with:
 - `enforce(Module&)` - walk AST, check annotation presence
 - `EnforcementOptions`: `requireFunctionParamTypes`, `requireReturnTypes`, `requireVariableTypes`, `isImportedModule`, `importingFile`
- **New: `src/TypeHintEnforcer.cpp`** - Visitor implementation:
 - `visit(FunctionDecl&)` - check every `Parameter` has non-null `type`, function has `returnType`
 - `visit(AssignStmt&)` - at module scope, check annotations exist
 - `visit(ClassDecl&)` - check methods follow same rules
 - Produces diagnostics using `DiagnosticFormatter::formatMissingType`

### 4.2 Integrate into Driver pipeline
- **src/Driver.cpp** - In `buildFile` and `checkFile`, insert after parsing but before Sema:
 ```cpp
 if (!isDragon) { // .py files need explicit type enforcement
 TypeHintEnforcer enforcer;
 if (!enforcer.enforce(*module)) { /* emit errors, return 1 */ }
 }
 ```
- Remove unused `requireTypes` from `ParserOptions` (include/dragon/Parser.h:26) since enforcement is now its own pass
- Remove `parseOpts.requireTypes = isDragon` lines (Driver.cpp:196, 281, 414)

### 4.3 Update CMake and tests
- **CMakeLists.txt** - Add `src/TypeHintEnforcer.cpp` to `DRAGON_SOURCES`
- **New: `test/TypeHintEnforcerTest.cpp`** - Tests:
 - Typed `.py` function passes
 - Missing param type produces `DRAGON SCALE ERROR`
 - Missing return type fails
 - `.dr` files unaffected (they go through TypeChecker)
 - "Borders must be secured" message for untyped imported `.py`

### Check:
570+ tests pass. Compile a fully-typed `.py` file successfully. Compile an untyped `.py` file and get Dragon-themed error with suggestion.

---

## Phase 5: Multi-File Module Resolution

**Goal:** Replace the ad-hoc import resolution in `Driver::buildFile` (lines 244-313) with a proper `ModuleResolver` that builds an import graph, detects cycles, and returns modules in topological order.

### 5.1 Create ModuleResolver
- **New: `include/dragon/ModuleResolver.h`** - `ModuleResolver` with:
 - `resolve(moduleName, fromFile)` - find `.dr` or `.py` file for a module name
 - `buildGraph(entryFile)` - return `ImportGraph` with modules in topological order
 - `ImportGraph` struct: `vector<ResolvedModule>`, `hasCycle`, `cycleParticipants`
 - `ResolvedModule` struct: `name`, `filepath`, `isDragon`, `ast`
 - `ModuleResolverOptions`: `searchPaths`, `enableSitePackages`, `sitePackagesPath`
 - Search order: same directory (.dr then .py), searchPaths, site-packages
- **New: `src/ModuleResolver.cpp`** - DFS-based import graph builder:
 1. Parse entry file, extract `ImportStmt`/`FromImportStmt` nodes
 2. Resolve each import to a file path
 3. Recursively process imported modules
 4. Detect cycles via DFS coloring
 5. Return reverse-topological order

### 5.2 Refactor Driver to use ModuleResolver
- **src/Driver.cpp** - Replace lines 244-313 (inline import resolution) with:
 ```cpp
 ModuleResolver resolver(resolverOpts);
 auto graph = resolver.buildGraph(filename);
 if (graph.hasCycle) { /* report error */ return 1; }
 for (auto& mod : graph.modules) {
 // TypeHintEnforcer for .py modules (with "Borders" message)
 // Sema + TypeCheck each module
 // CEmitter in library mode (emitMain=false, includeRuntime=false)
 }
 ```
- Add `-I <dir>` flag to `parseArgs` for additional search paths
- Add `searchPaths` to `DriverOptions`

### 5.3 Cross-file symbol visibility
- **include/dragon/Sema.h** - Add `registerExternalModule(name, exports)` and `getExports` to `Sema`
- **src/Sema.cpp** - When processing `FromImportStmt`, look up previously-registered external symbols instead of silently skipping
- **include/dragon/TypeChecker.h** - Add `registerExternalTypes(moduleName, typeMap)` to `TypeChecker`
- **src/TypeChecker.cpp** - Resolve imported names against registered external types for cross-file type error detection

### 5.4 Update CMake and tests
- **CMakeLists.txt** - Add `src/ModuleResolver.cpp` to `DRAGON_SOURCES`
- **New: `test/ModuleResolverTest.cpp`** - Tests:
 - Single-file (no imports) works
 - `.dr` importing `.dr` resolves
 - `.dr` importing typed `.py` resolves
 - `.py` importing `.dr` resolves
 - Circular import detected
 - Missing module produces clear error
 - Diamond import pattern handled correctly
 - Cross-file type mismatch caught by TypeChecker

### Check:
All tests pass. End-to-end: create `math_utils.py` (typed) + `main.dr` (imports from it), compile to single binary, run correctly. Verify cross-file type error caught when passing wrong types.

---

## Phase 6: Site-Packages + Stdlib Expansion

**Goal:** Allow importing pip-installed packages and expand stdlib module registry.

### 6.1 Site-packages discovery
- **src/ModuleResolver.cpp** - Add `findSitePackagesPath`:
 - Execute `python3 -c "import site; print(site.getsitepackages[0])"` and cache result
 - When `enableSitePackages` is true, search site-packages after local directories
- **src/Driver.cpp** - Add `--site-packages` flag to `parseArgs`

### 6.2 Expand StdlibRegistry
- **src/CEmitterStdlib.cpp** - Extend `StdlibRegistry` (currently maps 5 modules) to cover Tier 1:
 - `json` -> `dragon_json_dumps`, `dragon_json_loads` (wrap cJSON or hand-roll)
 - `re` -> `dragon_re_match`, `dragon_re_search` (wrap POSIX `<regex.h>`)
 - `random` -> `dragon_random_randint`, `dragon_random_random` (wrap `<stdlib.h>`)
 - `collections` -> `dragon_defaultdict_*` etc.
- **lib/Runtime/runtime.cpp** - Add corresponding runtime function implementations

### 6.3 Type stub system (future foundation)
- **New: `include/dragon/TypeStubs.h`** - `TypeStubRegistry` with:
 - `loadStub(moduleName, stubPath)` - load `.dri` (Dragon Interface) file
 - `lookupType(moduleName, symbolName)` - get type for imported symbol
 - `loadBuiltinStubs` - built-in stubs for json, re, datetime, etc.
- This provides a TypeScript-style `.d.ts` equivalent for external packages that cannot be compiled

### Check:
`import math` still works (existing). `import json` compiles with new runtime functions. `from collections import defaultdict` resolves through stubs.

---

## Phase 7: Combined Compilation Unit + LLVM Multi-Module

**Goal:** Treat all `.dr` and `.py` files in the import graph as a single compilation unit, enabling cross-module optimizations (inlining, dead code elimination).

### 7.1 CEmitter multi-module support
- **src/CEmitter.cpp** - Instead of string-concatenating independent emissions, produce a coordinated C file:
 1. Single runtime header
 2. Forward declarations for all modules (topological order)
 3. Dependency function/class definitions
 4. Main module definitions
 5. Single `main` from entry module
- **include/dragon/CEmitter.h** - Add `dependencyModules` to `CEmitterOptions`

### 7.2 LLVM CodeGen multi-module
- **include/dragon/CodeGen.h** - Add `generateMultiModule(vector<Module*>)` to `CodeGen`
- **src/CodeGen.cpp** - Iterate import graph, generate IR per module into single LLVM module, enable cross-module optimization passes (inlining, DCE)

### 7.3 Integration tests
- **New: `test/InteropTest.cpp`** - End-to-end tests:
 - `.dr` imports `.dr`, function call works
 - `.dr` imports typed `.py`, compiles and runs
 - `.py` imports `.dr`, compiles and runs
 - Cross-file type error caught at build time
 - Untyped `.py` import produces "Borders must be secured" error
 - Diamond imports compile correctly (deduplicated)
 - Circular import produces clear error

### Check:
Multi-file project with mixed `.dr`/`.py` compiles to single native binary. Cross-module function calls work at runtime. LLVM optimization passes run across module boundaries.

---

## Phase 8: Polish

### 8.1 CLI help improvements
- **src/Driver.cpp** - Update `printUsage` with full help including new flags (`-I`, `--site-packages`, `--no-color`)

### 8.2 Default extension handling
- **src/Driver.cpp** - If no extension provided or unknown, default to `.dr` mode with a logged warning (per Prompt 2 requirement)

### 8.3 Update convergence doc
- **python-convergence.md** - Update to reflect bilingual compiler capabilities, new test counts

---

## Risk Assessment

| Phase | Risk | Test Impact | Depends On |
|-------|------|-------------|------------|
| 1: CLI Rebrand | Low | -37 (MigratorTests removed) = 570 | None |
| 2: CEmitter Split | Low | 0 (pure refactor, 149 tests unchanged) | Phase 1 |
| 3: Error System | Low | +15-20 new tests | Phase 1 |
| 4: Type Enforcement | Medium | +20-25 new tests | Phases 1-3 |
| 5: Module Resolution | High | +30-40 new tests | Phases 1-4 |
| 6: Site-Packages | Medium | +15-20 new tests | Phases 2, 5 |
| 7: Combined Compilation | High | +25-30 new tests | Phases 2, 5 |
| 8: Polish | Low | None | All |

**Projected final test count:** ~685-720 (up from 607)

## Architecture Notes

1. **No Parser Factory.** The existing `LexerOptions`/`ParserOptions` pattern already functions as the factory - the Driver selects behavior by file extension. A factory class would add indirection without benefit.

2. **TypeHintEnforcer as a separate AST pass** (not in Parser or TypeChecker). The Parser's job is to parse valid syntax. The TypeChecker validates type correctness. The Enforcer validates type annotation *presence*. One job.

3. **ModuleResolver as a new top-level component** (not buried in Driver). The Driver's inline import code (lines 244-313) is a string-concatenation hack. Extracting resolution makes it testable and extensible.

4. **CEmitter path remains primary** for multi-module compilation. The CEmitter (2,716 lines) is far more mature than CodeGen. Multi-file should work through CEmitter first, with LLVM multi-module as Phase 7 enhancement. The Phase 2 split into 3 files makes this extensible.

5. **Type stubs (`.dri`) over runtime introspection** for external packages. Same pattern as TypeScript's `.d.ts` and mypy's `.pyi` - works elsewhere for interfacing compiled languages with dynamic ecosystems.

## Critical Files

| File | Phases | Changes |
|------|--------|---------|
| `CMakeLists.txt` | 1-6 | Binary rename, source list updates, new files |
| `src/Driver.cpp` | 1, 3-5, 8 | Remove migrate, integrate new passes, error formatting, module resolution |
| `include/dragon/Driver.h` | 1, 5 | Remove Migrate action, add searchPaths |
| `include/dragon.h` | 1-4 | Remove old includes, add new includes |
| `src/CEmitter.cpp` | 2, 7 | Split into 3 files, then multi-module emission |
| `src/CEmitterRuntime.cpp` | 2, 6 | Extracted runtime header generation, expand runtime |
| `src/CEmitterStdlib.cpp` | 2, 6 | Extracted stdlib registry, expand module mappings |
| `include/dragon/Sema.h` + `src/Sema.cpp` | 5 | Cross-module symbol registration |
| `include/dragon/TypeChecker.h` + `src/TypeChecker.cpp` | 5 | Cross-file type resolution |
| `test/CMakeLists.txt` | 1, 3-7 | Remove migrator, add new test targets |
