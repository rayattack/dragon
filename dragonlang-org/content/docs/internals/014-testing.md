# Dragon Test Infrastructure

> **Version:** 0.2.0
> **Files:** `test/CMakeLists.txt`, `test/TestHelpers.h`, `test/*.cpp`, `test/dr/*.dr`
> **Last updated:** 2026-06-22

---

## 1. Framework

Dragon uses **GoogleTest v1.14.0**, fetched at build time via CMake's `FetchContent` mechanism. No pre-installed testing library is required.

```cmake
# test/CMakeLists.txt
include(FetchContent)
FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG v1.14.0
)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)
```

Tests are enabled with `cmake -DDRAGON_BUILD_TESTS=ON ...` and executed via `ctest` after building.

All test executables link against `dragon_lib` (the core library containing lexer, parser, sema, type checker, code generator) and `gtest_main` (GoogleTest's main function provider, so individual test files do not need a `main()`).

---

## 2. Test Helpers

All test files include `test/TestHelpers.h`, which provides convenience functions that wire together multiple compiler stages. These helpers are defined as `inline` functions in the `dragon::test` namespace.

### 2.1 `lex(source, isDragon)`

```cpp
inline std::vector<Token> lex(const std::string& source, bool isDragon = true);
```

Tokenizes the given source string and returns the token vector (including the trailing `END_OF_FILE` token). The `isDragon` flag controls whether brace-block mode (`.dr`) or indentation mode (`.py`) is used.

- Sets `LexerOptions.useBraceBlocks = isDragon`
- Sets filename to `"<test>"`

### 2.2 `parse(source, isDragon)`

```cpp
inline std::unique_ptr<Module> parse(const std::string& source, bool isDragon = true);
```

Tokenizes and parses the source, returning the root `Module` AST node. Returns `nullptr` if parsing fails (though most test helpers check for `nullptr` explicitly).

- Calls `lex()` internally
- Sets `ParserOptions.isDragonFile = isDragon`
- Sets `ParserOptions.requireTypes = isDragon`
- Sets filename to `"<test>"`

### 2.3 `parseErrors(source, isDragon)`

```cpp
inline std::vector<ParserDiagnostic> parseErrors(const std::string& source, bool isDragon = true);
```

Parses the source and returns the parser's diagnostic list. Used to test error detection: test cases verify that specific error messages are produced for invalid input.

### 2.4 `lexErrors(source, isDragon)`

```cpp
inline std::vector<LexerDiagnostic> lexErrors(const std::string& source, bool isDragon = true);
```

Tokenizes the source and returns the lexer's diagnostic list. Used to test lexer error handling (unterminated strings, invalid characters, etc.).

### 2.5 Per-File Helpers

Individual test files define additional helpers specific to their domain. These are `static` functions within each `.cpp` file:

**CodeGen tests (`test/CodeGen*Test.cpp`, shared `test/CodeGenTestHelpers.h`):**

```cpp
static std::string generateIR(const std::string& source);
```

Full pipeline: parse -> sema -> typecheck -> CodeGen. Returns the LLVM IR as a string. A one-time `LLVMInit` struct initializes LLVM's native target support.

```cpp
static std::string compileAndRun(const std::string& source);
```

Full pipeline through LLVM: parse -> sema -> typecheck -> CodeGen -> emit object file -> link with runtime -> execute -> capture stdout. The CodeGen suite is **not** a single file: it is split across ~16 `CodeGen*Test.cpp` files (see §3.10) that all share these helpers and compile into one `dragon_codegen_tests` executable.

**InteropTest.cpp:**

```cpp
static int compileAndRun(const std::string& dir, const std::string& entryFile,
                         const std::string& source, std::string& output);
```

Multi-module pipeline: parse entry -> resolve imports via ModuleResolver -> sema/typecheck each module with cross-file type info -> CodeGen (single LLVM module) -> link -> execute. Returns exit code and captures stdout into the `output` parameter.

**TypeHintEnforcerTest.cpp:**

```cpp
static bool enforceOk(const std::string& source, EnforcerOptions opts = {});
static std::vector<EnforcerDiagnostic> enforceDiags(const std::string& source, EnforcerOptions opts = {});
static bool enforceDragonOk(const std::string& source);
```

Parse as Python (`.py` mode), run the TypeHintEnforcer, and return whether enforcement passed or the list of diagnostics. `enforceDragonOk` parses in `.dr` mode for cross-mode testing.

---

## 3. Test Suites Overview

Dragon's test infrastructure has **two tiers**:

1. **GoogleTest C++ suites** (this section): **11** executables registered as CTest targets, totalling on the order of **1,700+** tests. These cover lexer, parser, sema, type checking, code generation (IR + E2E), diagnostics, module resolution, and `.py` type-hint enforcement.
2. **Dragon-language KAT suites** (§3.12): `test/dr/test_*.dr` files run via `dragon run` against `stdlib/unittest.dr`. This is the mandated dogfooding tier - value-assertable behavior is tested in Dragon, with GoogleTest reserved for IR inspection, stdout-exactness, and must-not-compile checks.

Per-suite counts below are approximate (the suites grow continuously); treat them as orders of magnitude, not exact figures. The current count for any suite is `./<exe> --gtest_list_tests | grep -c '  '` or `ctest -R <Suite> -N`.

### 3.1 LexerTests -- ~73 tests

**File:** `test/LexerTest.cpp`
**Executable:** `dragon_lexer_tests`
**CTest name:** `LexerTests`

Tests the lexer's tokenization of Dragon source code in both brace-block and indentation modes.

**Categories:**
- **Basic tests:** Empty input, whitespace, comments, comment after token
- **Literal scanning:** Integer, float, string, boolean (`True`/`False`), `None`
- **Keyword scanning:** All 35+ keywords produce correct `TokenType` values
- **Operator scanning:** All arithmetic, comparison, logical, bitwise, and augmented assignment operators
- **Delimiter scanning:** Parentheses, brackets, braces, commas, colons, semicolons, dots, arrows
- **Multi-token sequences:** `x: int = 42`, `if x > 3`, `def foo(a: int) -> str`
- **F-string tokenization:** `f"hello {name}"` produces `FSTRING_START`, `FSTRING_PART`, `IDENTIFIER`, `FSTRING_END`
- **Error cases:** Unterminated string, invalid character
- **Indentation mode:** `.py`-style INDENT/DEDENT token generation
- **NEWLINE suppression:** Inside parentheses and brackets, NEWLINE tokens are suppressed

### 3.2 ParserTests -- ~222 tests

**File:** `test/ParserTest.cpp`
**Executable:** `dragon_parser_tests`
**CTest name:** `ParserTests`

Tests the parser's conversion of token streams into AST nodes.

**Categories:**
- **Module structure:** Empty module, multiple statements
- **Literal expressions:** Integer, float, string, boolean, None
- **Binary expressions:** Arithmetic (`+`, `-`, `*`, `/`, `//`, `%`, `**`), comparison, logical (`and`, `or`), bitwise
- **Unary expressions:** Negation, `not`, bitwise NOT
- **Precedence:** Operator precedence follows Python rules (`**` > unary > `*`/`/` > `+`/`-` > comparison > `not` > `and` > `or`)
- **Statements:** Assignment, augmented assignment, expression statement, pass, break, continue, return, del, assert, raise
- **Control flow:** if/elif/else, while, for-in-range (1/2/3 arg), for-in-collection
- **Functions:** def with parameters, return type, default values, body
- **Classes:** class declaration, `__init__`, methods, single inheritance (parsed)
- **Imports:** `import mod`, `import mod as alias`, `from mod import sym`, `from mod import sym as alias`, `from mod import *`
- **List/dict/set literals:** `[1, 2, 3]`, `{"a": 1}`, `{1, 2, 3}`
- **Comprehensions:** List comprehension, list comprehension with condition, dict comprehension
- **Lambda:** `lambda (x: int) : int { return x + 1 }`
- **Ternary:** `x if cond else y`
- **Walrus operator:** `x := expr`
- **F-strings:** `f"text {expr} text"`
- **Try/except/finally:** try/except, try/except/finally, try/except/else
- **With statement:** Parsed (emitted as body-only)
- **Yield/async:** Parsed (not emitted)
- **Subscript:** `x[0]`, `x[1:3]`, `x[::2]`
- **Attribute access:** `x.y`, `x.y.z`
- **Star expressions:** `*args`
- **Complex constructs:** Nested structures, multiple decorators

### 3.3 SemaTests -- ~48 tests

**File:** `test/SemaTest.cpp`
**Executable:** `dragon_sema_tests`
**CTest name:** `SemaTests`

Tests semantic analysis (name resolution and scope management).

**Categories:**
- **Variable declaration and usage:** Declared variables can be referenced
- **Undeclared variables:** Produce error diagnostics
- **Scope nesting:** Variables in inner scopes shadow outer scopes
- **Function scoping:** Parameters are in function scope, not module scope
- **Class scoping:** `self` is available in class methods
- **Control flow validation:** `break`/`continue` only valid inside loops; `return` only inside functions
- **Import resolution:** Imported names are registered in scope

### 3.4 TypeCheckerTests -- ~191 tests

**File:** `test/TypeCheckerTest.cpp`
**Executable:** `dragon_typechecker_tests`
**CTest name:** `TypeCheckerTests`

Tests the type checking pass, which validates type correctness of the AST.

**Categories:**
- **Type inference:** Literal types inferred correctly (`42` is `int`, `"hello"` is `str`, etc.)
- **Subtyping:** `bool <: int <: float`, everything `<: Any`
- **Assignment type checking:** Cannot assign `str` to `int` variable
- **Binary operator rules:** `int + int -> int`, `int + float -> float`, `str + str -> str`
- **Comparison operators:** Produce `bool` type
- **Function type checking:** Parameter types match, return type matches
- **Function call validation:** Correct number of arguments, correct argument types
- **List type checking:** `list[int]` elements must be `int`
- **Dict type checking:** Key and value types checked
- **Class type checking:** Constructor arguments checked, method call types verified
- **Cross-module type checking:** `registerExternalModule()` and `getExports()` for imported symbol types
- **Union types:** Parsed but limited checking
- **Optional types:** Parsed but limited checking

### 3.5 ASTTests -- ~36 tests

**File:** `test/ASTTest.cpp`
**Executable:** `dragon_ast_tests`
**CTest name:** `ASTTests`

Tests the AST printer (which produces a human-readable text representation of the AST) and the visitor pattern infrastructure.

**Categories:**
- **AST printer output:** Verifies that various AST nodes produce expected textual representations when printed
- **Visitor pattern:** Tests that a counting visitor correctly visits all node types in an AST
- **Node structure:** Verifies that parsed code produces the expected AST shape

### 3.6 TypeHintEnforcerTests -- ~21 tests

**File:** `test/TypeHintEnforcerTest.cpp`
**Executable:** `dragon_enforcer_tests`
**CTest name:** `EnforcerTests`

Tests the PEP-484 type annotation enforcement pass for `.py` files.

**Categories:**
- **Passing cases:** Fully typed functions, no-param functions, functions returning None, `__init__` exempt from return type, `self`/`cls` parameters exempt, typed module-level variables, `__dunder__` variables exempt, empty modules
- **Failing cases:** Missing parameter type, all parameters missing types, missing return type, method with untyped non-self parameter, module variable without type
- **Options:** Disable parameter type check, disable return type check, disable module variable check
- **Multiple errors:** Multiple missing annotations produce multiple diagnostics
- **Cross-mode:** Dragon `.dr` files also pass when types are present
- **Mixed functions:** One typed and one untyped function produces errors only for the untyped one

### 3.7 ModuleResolverTests -- ~12 tests

**File:** `test/ModuleResolverTest.cpp`
**Executable:** `dragon_resolver_tests`
**CTest name:** `ResolverTests`

Tests the import graph builder that resolves multi-file dependencies.

**Categories:**
- **File finding:** Find `.dr` module, find `.py` module, `.dr` preferred over `.py` when both exist, module not found, find in search path (`-I` equivalent)
- **Graph building:** No imports (empty graph), single import, diamond dependency (topological ordering), circular import detection, `.dr` importing `.py`, stdlib import skipped (not resolved as local file), missing module in transitive dependency

### 3.8 DiagnosticTests -- ~17 tests

**File:** `test/DiagnosticTest.cpp`
**Executable:** `dragon_diagnostic_tests`
**CTest name:** `DiagnosticTests`

Tests the `DiagnosticFormatter` class output. See the diagnostics documentation (`docs/013-diagnostics.md`) for the full list.

### 3.9 InteropTests -- ~67 tests

**File:** `test/InteropTest.cpp`
**Executable:** `dragon_interop_tests`
**CTest name:** `InteropTests`

End-to-end tests for cross-file compilation using the full LLVM pipeline (ModuleResolver + CodeGen multi-module emission + link + execution). Beyond the multi-module cases below, this suite also exercises a wide range of stdlib modules end-to-end (`math`, `os`, `os.path`, and others) by importing them from a temp entry file and asserting on stdout.

A representative slice:

| Test | What It Validates |
|------|-------------------|
| `DragonImportsDragon` | `.dr` imports `.dr`, function call works |
| `DragonImportsTypedPython` | `.dr` imports typed `.py`, function call works |
| `SingleFileNoImports` | Single file with no imports compiles and runs |
| `DiamondImportCompiles` | Diamond dependency pattern (A->C, B->C, main->A+B) compiles correctly |
| `UntypedPyImportRejected` | Untyped `.py` module fails TypeHintEnforcer |
| `StringConcatAcrossModules` | String concatenation works across module boundaries |
| `FunctionNameAcrossModules` | Cross-module function name resolution works |

### 3.10 CodeGenTests -- ~1,000 tests

**Files:** ~16 `test/CodeGen*Test.cpp` files (see below)
**Executable:** `dragon_codegen_tests`
**CTest name:** `CodeGenTests`

The largest test suite by far, covering the LLVM backend (the sole backend). It is **not** a single file (there is no `test/CodeGenTest.cpp`): the suite is split by feature area into ~16 source files that all compile into one `dragon_codegen_tests` executable and share `test/CodeGenTestHelpers.h`:

| File | Focus |
|------|-------|
| `CodeGenLiteralsTest.cpp` | Literals, primitive values |
| `CodeGenExpressionsTest.cpp` | Binary/unary operators, expressions |
| `CodeGenAssignTest.cpp` | Assignment, augmented assignment |
| `CodeGenCallsTest.cpp` | Calls, builtins, varargs/spread |
| `CodeGenCollectionsTest.cpp` | Lists, dicts, sets, tuples, slicing |
| `CodeGenComprehensionsTest.cpp` | List/dict comprehensions, generators |
| `CodeGenFunctionsTest.cpp` | Functions, closures, generators |
| `CodeGenClassesTest.cpp` | Classes, methods, inheritance, `super` |
| `CodeGenDundersTest.cpp` | Dunder methods (`__str__`, `__len__`, etc.) |
| `CodeGenDocstringTest.cpp` | Docstring emission |
| `CodeGenControlFlowTest.cpp` | if/while/for, break/continue, match/case, tuple unpack |
| `CodeGenExceptionsTest.cpp` | try/except/finally, raise |
| `CodeGenConcurrencyTest.cpp` | `fire`/`thread`, `Task`, `async`/`await`, locks |
| `CodeGenTemplateTest.cpp` | `template[HTML]` and typed template literals |
| `CodeGenModuleTest.cpp` | Module globals, cross-module emission |
| `CodeGenGenericsTest.cpp` | Generic instantiation / monomorphization |

Tests are split into IR inspection tests (using `generateIR()`) and end-to-end tests (using `compileAndRun()` through the full LLVM pipeline).

**Categories:**
- **IR generation:** Integer/float/bool/string literals produce correct LLVM IR
- **Binary operators:** Arithmetic, comparison, logical operators
- **Unary operators:** Negation, `not`, bitwise NOT
- **Control flow:** if/elif/else, while, for-range (1/2/3 args), break, continue
- **Functions:** Definition, call, return, multiple parameters, void functions, forward declarations
- **Variables:** Local variables, nested scopes, reassignment
- **Print dispatch:** Type-aware print calls
- **List operations:** List new, append, get, len, comprehension, pop, clear, copy, count, index, insert, remove, reverse, sort, extend, subscript
- **Dict operations:** Literal, get, keys, values, update, setdefault, subscript
- **String methods:** upper, replace, startswith, split, join, title, capitalize, swapcase, lstrip, rstrip, isdigit, isalpha, isalnum, isspace, isupper, islower, center, zfill, removeprefix, removesuffix, count, rfind, casefold, expandtabs, partition, splitlines
- **String operations:** Concat, repeat, comparison, `+=` concat
- **Builtins:** chr, ord, hex, oct, bin, round, divmod, min, max, sum, any, all, sorted, reversed, enumerate, zip, abs, len
- **Classes:** Declaration, constructor, attribute access, methods
- **Exception handling:** try/except, raise, finally (setjmp/longjmp via runtime)
- **F-strings:** Simple literal, variable interpolation
- **Slicing:** List slice, string slice, slice with step
- **Comprehensions:** List comprehension, list comprehension with condition, dict comprehension
- **Lambda expressions:** Lambda with typed parameters
- **Closures:** Capture by value (int/str/bool/float), multiple captures, nested `def`, closures returned from functions
- **Generators:** `yield`, generator wrappers, typed generator creation, scope cleanup on re-raise
- **Concurrency:** `fire fn()` / `fire { block }`, `thread { block }`, `Task` join / `is_alive`, `async def` / `await`, locks (acquire/release/timeout/with-statement)
- **Match/case:** Type-test patterns, tuple unpacking (incl. starred targets) in assignment and `for`
- **Generics:** Instantiation and monomorphization
- **Templates:** `template[HTML]` and typed template literals
- **Multi-file:** Cross-module function calls, imports

---

### 3.11 DefiniteAssignmentTests -- ~18 tests

**File:** `test/DefiniteAssignmentTest.cpp`
**Executable:** `dragon_definite_assignment_tests`
**CTest name:** `DefiniteAssignmentTests`

Tests the definite-assignment analysis that rejects use of a variable before it is guaranteed to be assigned on every path. Complements the declaration / scoping rules described in `the project design documentation` (`:` declares, `=` reassigns; no use-before-init).

---

### 3.12 Dragon-Language KAT Suites (`test/dr/*.dr`)

Beyond the GoogleTest C++ tier, the bulk of **value-assertable behavior** is tested in Dragon itself, per the dogfooding policy: a behavior that can be asserted on a value belongs in a `.dr` test, with GoogleTest reserved for IR inspection, exact-stdout, and must-not-compile checks.

**Mechanism.** `test/CMakeLists.txt` globs `test/dr/test_*.dr` with `CONFIGURE_DEPENDS`, so new files auto-register on the next build with no edit to the build file:

```cmake
file(GLOB DR_TEST_FILES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/dr/test_*.dr)
foreach(_dr ${DR_TEST_FILES})
    get_filename_component(_name ${_dr} NAME_WE)
    add_test(NAME dr_${_name} COMMAND $<TARGET_FILE:dragon> run ${_dr})
endforeach()
```

Each file is a real Dragon program ending in `unittest.main([...])` (`stdlib/unittest.dr`, which uses spec-33 reflection to discover and run test methods). The file exits non-zero on any failed assertion, so CTest gates purely on the process exit code - no C++ harness involved. The `dragon` binary has `DRAGON_STDLIB_DIR` baked in, so `import unittest` (and `hashlib`, `hmac`, `secrets`, etc.) resolve with no extra environment.

There are roughly **130** such files, registered as CTest targets named `dr_<filename>` (for example `dr_test_super_inheritance`, `dr_test_closure_rc`, `dr_test_match_type_patterns`). They cover crypto/TLS self-tests (the design spec), stdlib modules (`itertools`, `functools`, `shutil`, `struct`, `zipfile`, database drivers, HTTP/websocket stack, and many more), and language-level behavior (closures, generics, generators, refcount/ownership edge cases, exception unwinding, virtual dispatch).

Two suites are registered **explicitly** rather than via the glob:

- **`dr_test_generics_py`** (spec-44) - the generics `.py`-mode surface-parity check. It must keep its `.py` extension to parse in indentation mode, so it cannot ride the `.dr`-only glob: `test/dr/generics_py/test_generics_py.py`.
- **`dr_ffi_cxx_shim`** (the design spec) - external native-extension FFI. Registered explicitly because it needs `dragon run app.dr --cc-source shim.cpp` to compile and link a C++ shim, which the bare `dragon run` glob cannot pass: `test/dr/ffi_cxx/`.

---

## 4. End-to-End Test Pattern

The E2E test pattern used throughout the codebase follows a consistent flow:

```
1. Define Dragon source code as a C++ string literal
2. Call compileAndRun(source)
   a. Parse source -> AST
   b. Run Sema + TypeChecker
   c. CodeGen generates LLVM IR
   d. Emit object file to /tmp
   e. Link with runtime via `cc`
   f. Execute binary and capture stdout
   g. Read stdout from .out file
   h. Delete all temp files
3. Compare stdout with expected string using EXPECT_EQ
```

The PID is included in temporary file names to avoid conflicts when tests run in parallel.

For InteropTests, the pattern extends to:
1. Create a temporary directory via `mkdtemp()`
2. Write dependency files (`.dr` or `.py`) to the directory
3. Parse entry module and resolve imports via `ModuleResolver`
4. Process each dependency through Sema + TypeChecker with cross-file type info
5. Emit all modules via CodeGen (single LLVM module approach)
6. Link, execute, and verify output
7. Clean up temp files and directory

---

## 5. Test Categories by Feature

### Arithmetic and Operators

| Feature | Test Suites | Approximate Count |
|---------|-------------|-------------------|
| Integer arithmetic | Parser, TypeChecker, CodeGen | ~15 |
| Float arithmetic | Parser, TypeChecker, CodeGen | ~5 |
| String concatenation | CodeGen (IR + E2E) | ~6 |
| String repeat | CodeGen (IR + E2E) | ~2 |
| Comparison operators | Parser, TypeChecker, CodeGen | ~10 |
| Logical operators | Parser, CodeGen | ~6 |
| Bitwise operators | Parser, CodeGen | ~4 |
| Augmented assignment | CodeGen (IR + E2E) | ~6 |
| Power, floor div, modulo | CodeGen (IR + E2E) | ~8 |

### Control Flow

| Feature | Test Suites | Approximate Count |
|---------|-------------|-------------------|
| if/elif/else | Parser, CodeGen | ~8 |
| while loops | Parser, CodeGen | ~4 |
| for-range loops | Parser, CodeGen | ~8 |
| break/continue | Parser, CodeGen | ~4 |
| try/except/finally | Parser, CodeGen | ~4 |
| raise | CodeGen | ~1 |

### Functions

| Feature | Test Suites | Approximate Count |
|---------|-------------|-------------------|
| Function definition | Parser, TypeChecker, CodeGen | ~10 |
| Function calls | Parser, TypeChecker, CodeGen | ~8 |
| Return statements | CodeGen | ~4 |
| Lambda expressions | Parser, CodeGen | ~2 |
| Forward declarations | CodeGen | ~1 |

### Data Types

| Feature | Test Suites | Approximate Count |
|---------|-------------|-------------------|
| Lists | Parser, TypeChecker, CodeGen | ~25 |
| Dicts | Parser, TypeChecker, CodeGen | ~10 |
| Sets | CodeGen | ~1 |
| Strings (methods) | CodeGen (IR + E2E) | ~35 |
| Slicing | CodeGen (IR + E2E) | ~8 |
| F-strings | CodeGen (IR + E2E) | ~4 |

### Classes

| Feature | Test Suites | Approximate Count |
|---------|-------------|-------------------|
| Class declaration | Parser, CodeGen | ~4 |
| Constructor | CodeGen | ~3 |
| Method calls | CodeGen | ~2 |

### Imports and Multi-File

| Feature | Test Suites | Approximate Count |
|---------|-------------|-------------------|
| Stdlib imports | CodeGen | ~2 |
| Multi-file resolution | ModuleResolver | ~12 |
| Cross-file compilation | InteropTests | ~7 |
| Type enforcement (.py) | TypeHintEnforcer | ~21 |

---

## 6. Known Coverage Gaps

Many features once listed here as gaps have since shipped and are tested - `async`/`await`, generators (`yield`), closures, `match`/`case`, tuple unpacking, and generics all have dedicated CodeGen tests and/or `test/dr/*.dr` suites. The remaining limited-coverage areas:

| Area | Status | Notes |
|------|--------|-------|
| `super` | Tested | `.dr` `super(args)` / `super.method()` + `.py` `super().__init__()` / `super().method()`; see `test/dr/test_super_inheritance.dr` and CodeGen `Super*` tests |
| `async`/`await` | Tested | IR (`AsyncDefIR`, `AwaitIR`) and E2E in `CodeGenConcurrencyTest.cpp`; lowers to `Task[T]` / vthread join |
| Generators (`yield`) | Tested | `CodeGenFunctionsTest.cpp` (`Generator*`), `test/dr/test_method_generators.dr` |
| Closures | Tested | `CodeGenFunctionsTest.cpp` (`Closure*`, `NestedDef*`), `test/dr/test_closure_*.dr` |
| `match`/`case` | Tested | Type-test patterns in `CodeGenControlFlowTest.cpp`, `test/dr/test_match_type_patterns.dr` |
| Tuple unpacking | Tested | `TupleUnpack*` / `StarredUnpack*` / `ForLoopTupleUnpack` E2E in `CodeGenCollectionsTest.cpp` / `CodeGenControlFlowTest.cpp` |
| Generics | Tested | `CodeGenGenericsTest.cpp` + `test/dr/test_generics_*.dr` (bounded, transitive, double-mono, method, `.py` parity) |
| Decorators | Minimal | Parsed; limited parser tests |
| Memory management | Tested | Refcount/ownership `test/dr/*_rc.dr` suites assert no use-after-free; an AddressSanitizer build tree (`build-asan/`) is used for leak/UAF probes |
| Error recovery | Minimal | Parser produces diagnostics but recovery paths are lightly tested |
| Unicode | Minimal | Some non-ASCII handling exercised (e.g. `ReadTextNonAsciiSplit`); broad Unicode coverage is thin |
| Large programs | No tests | No stress tests for compilation speed or memory usage |
| Edge cases | Sparse | Integer overflow, deeply nested expressions, very long strings |

---

## 7. Disk Pressure Concern

End-to-end tests that invoke `cc` create temporary files on disk and spawn external processes. This has several implications:

1. **File creation:** Each E2E test creates temporary files in `/tmp`: an object file, an executable, and a `.out` file. These are cleaned up after each test.
2. **Process spawning:** Each `compileAndRun()` call spawns at least 2 processes (`cc` for linking and the compiled binary). InteropTests spawn additional processes.
3. **Disk exhaustion:** On constrained environments (CI runners, containers with small tmpfs), too many E2E tests can exhaust disk space or file descriptor limits.
4. **Compilation time:** Each link invocation takes 100-300ms. With E2E tests across CodeGen and InteropTests, this adds several seconds to the test suite.

**Mitigation strategies:**
- IR inspection tests (using `generateIR()`) are preferred over E2E tests where possible
- All temp files include the PID in their names and are deleted after each test
- InteropTests clean up their temp directories after each test

---

## 8. Running Tests

### Build with Tests Enabled

```bash
cd build
cmake -DDRAGON_BUILD_TESTS=ON \
      -DLLVM_DIR=~/Documents/llvm-project/build/lib/cmake/llvm \
      ..
cmake --build .
```

### Run All Tests

```bash
ctest
```

### Run a Specific Test Suite

```bash
ctest -R LexerTests
ctest -R CodeGenTests
ctest -R InteropTests
```

### Run the Dragon-Language (`.dr`) Tier

```bash
ctest -R '^dr_'                       # all test/dr/*.dr KAT suites
ctest -R dr_test_super_inheritance    # one .dr file
./dragon run test/dr/test_closure_rc.dr   # or run it directly
```

### Run with Verbose Output

```bash
ctest --verbose
```

### Run Individual Test Binary

```bash
./dragon_codegen_tests --gtest_filter="CodeGenTest.ClassDecl"
./dragon_parser_tests --gtest_filter="ParserTest.*Literal*"
```

### List Available Tests

```bash
./dragon_codegen_tests --gtest_list_tests
```

---

## 9. Test File Summary

Counts are approximate (see §3); verify a current figure with `ctest -R <Suite> -N`.

| File(s) | Suite Name | Approx. Count | Links with `cc`? | Creates Temp Files? |
|------|-----------|------------|---------------------|---------------------|
| `LexerTest.cpp` | LexerTests | ~73 | No | No |
| `ParserTest.cpp` | ParserTests | ~222 | No | No |
| `SemaTest.cpp` | SemaTests | ~48 | No | No |
| `DefiniteAssignmentTest.cpp` | DefiniteAssignmentTests | ~18 | No | No |
| `TypeCheckerTest.cpp` | TypeCheckerTests | ~191 | No | No |
| `ASTTest.cpp` | ASTTests | ~36 | No | No |
| `TypeHintEnforcerTest.cpp` | EnforcerTests | ~21 | No | No |
| `ModuleResolverTest.cpp` | ResolverTests | ~12 | No | Yes (temp dirs) |
| `DiagnosticTest.cpp` | DiagnosticTests | ~17 | No | No |
| `InteropTest.cpp` | InteropTests | ~67 | Yes | Yes (temp dirs) |
| ~16 `CodeGen*Test.cpp` files | CodeGenTests | ~1,000 | Yes (LLVM) | Yes |
| **GoogleTest total** | | **~1,700** | | |
| `test/dr/test_*.dr` (+ 2 explicit) | `dr_*` CTest targets | ~130 files | Yes (`dragon run`) | Yes |

---

## Previous Document

[013 - Dragon Diagnostic and Error Reporting System](013-diagnostics.md)
