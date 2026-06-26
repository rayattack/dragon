# 000 - Dragon Language: Project Overview and Architecture

> **Version:** 0.2.0
> **Last Updated:** 2026-06-22
> **Status:** Active Development

---

## What Is Dragon?

Dragon ─── "The snake that became a **dragon**"

A **typed, compiled Python variant** that compiles to native executables. It accepts two file formats:

- **`.dr` files** - Dragon syntax: curly braces for blocks, mandatory type annotations, implicit `self` in classes
- **`.py` files** - Python syntax: indentation for blocks, optional (but enforced) type annotations, explicit `self`

Both file types compile through the same pipeline and produce identical native executables. A single project can mix `.dr` and `.py` files freely - this is the **bilingual compiler** architecture.

### The Core Idea

```
Dragon Source (.dr / .py)
        |
        v
   [ Lexer ] ─── Token Stream
        |
        v
   [ Parser ] ─── Abstract Syntax Tree
        |
        v
   [ TypeHintEnforcer ] ─── annotation enforcement (.py files only)
        |
        v
   [ Sema ] ─── Symbol Resolution
        |
        v
   [ TypeChecker ] ─── Type Validation
        |
        v
   [ CodeGen ] ─── LLVM IR
        |
        v
   [ LLVM ] ─── Object Code
        |
        v
   [ cc (linker) ] ─── Native Executable
```

Dragon takes Python's syntax and semantics, adds mandatory typing (in `.dr` mode), and compiles it through LLVM to native machine code. No interpreter, no VM, no tracing garbage collector pausing the world - just compiled native code linked against a static runtime library. Heap objects are managed by deterministic reference counting (with a backstop cycle collector for the rare reference cycle), so memory is reclaimed the moment a value's last reference goes away.

---

## Why Dragon Exists

Python is the most popular programming language in the world. It is also one of the slowest. Dragon asks: **what if Python compiled to native code?**

Dragon is not a Python replacement. It is a **Python dialect** that trades dynamic features for compilation speed and type safety while keeping the syntax developers already know.

**Dragon's design principles:**

1. **If you know Python, you know Dragon.** The learning curve is near-zero.
2. **Types are not optional.** Every variable, parameter, and return value has a type - enforced by the compiler.
3. **Compilation, not interpretation.** Dragon programs are native executables.
4. **Two syntaxes, one language.** Curly braces (`.dr`) or indentation (`.py`) - your choice.
5. **LLVM as the compilation target.** Dragon generates LLVM IR, producing optimized native code via the LLVM toolchain.

---

## Project Structure

```
dragon/
├── include/dragon/           # Public C++ headers (interfaces)
│   ├── Token.h               # Token types and source locations
│   ├── Lexer.h               # Lexical analyzer interface
│   ├── AST.h                 # ~50 AST node types + visitor pattern
│   ├── Parser.h              # Recursive descent parser
│   ├── Sema.h                # Semantic analysis (name resolution, scoping)
│   ├── TypeChecker.h         # Type system (12 type classes, inference, subtyping)
│   ├── TypeInference.h       # Type inference engine
│   ├── CodeGen.h             # LLVM IR code generator
│   ├── StdlibRegistry.h      # Python stdlib → runtime mapping registry
│   ├── Driver.h              # CLI driver (build, run, check)
│   ├── DiagnosticFormatter.h # Error message formatting
│   ├── ModuleResolver.h      # Multi-file import resolution
│   ├── TypeHintEnforcer.h    # PEP-484 annotation enforcement
│   └── PythonMigrator.h      # Python → Dragon migration tool
├── src/                      # Implementation files
│   ├── Lexer.cpp             # tokenizer
│   ├── Parser.cpp            # parser (declarations + expressions)
│   ├── ParserStmts.cpp       # parser (statements)
│   ├── AST.cpp               # AST node implementations
│   ├── AstClone.cpp          # AST deep-copy (generics monomorphization)
│   ├── Sema.cpp              # semantic analysis
│   ├── TypeChecker.cpp       # type checker (core + statements)
│   ├── TypeCheckerExprs.cpp  # type checker (expressions)
│   ├── TypeCheckerStmts.cpp  # type checker (statements)
│   ├── TypeCheckerGenerics.cpp # generics monomorphization
│   ├── TypeInference.cpp     # type inference
│   ├── DefiniteAssignment.cpp # definite-assignment analysis
│   ├── CodeGen.cpp           # codegen entry point / orchestration
│   ├── codegen/              # codegen split by AST-node category
│   │   ├── Module.cpp, Functions.cpp, Classes.cpp, Statements.cpp,
│   │   ├── Expressions.cpp, Literals.cpp, Collections.cpp,
│   │   ├── CallExpr.cpp, CallBuiltins.cpp, CallMethods.cpp,
│   │   ├── Comprehensions.cpp, Concurrency.cpp, Exceptions.cpp,
│   │   ├── EscapeAnalysis.cpp, ImplInit.cpp, ... (22 files)
│   ├── StdlibRegistry.cpp    # stdlib mappings (fallback registry)
│   ├── Driver.cpp            # CLI orchestration
│   ├── DiagnosticFormatter.cpp # error formatting
│   ├── ModuleResolver.cpp    # module resolution
│   ├── TypeHintEnforcer.cpp  # .py annotation enforcement
│   ├── PythonMigrator.cpp    # migration tool
│   ├── Platform.cpp          # platform abstraction
│   └── Token.cpp             # token utilities
├── lib/Runtime/              # Runtime library (compiled separately)
│   ├── runtime_core.cpp      # refcount + cycle collector, class registry
│   ├── runtime_string.cpp, runtime_string_methods.cpp # str ops
│   ├── runtime_list.cpp, runtime_dict.cpp, runtime_collections.cpp
│   ├── runtime_concurrency.cpp # green threads + scheduler
│   ├── runtime_box.cpp       # Any / Union 16-byte box
│   ├── runtime_exception.cpp, runtime_fileio.cpp, runtime_builtins.cpp,
│   ├── runtime_crypto.cpp, runtime_tls.cpp, ... (21 files)
├── test/                     # GoogleTest C++ suites + .dr dogfood tests
│   ├── LexerTest.cpp, ParserTest.cpp, SemaTest.cpp,
│   ├── TypeCheckerTest.cpp, ASTTest.cpp, DiagnosticTest.cpp,
│   ├── InteropTest.cpp, ModuleResolverTest.cpp,
│   ├── TypeHintEnforcerTest.cpp, MigratorTest.cpp,
│   ├── DefiniteAssignmentTest.cpp,
│   ├── CodeGen*Test.cpp       # codegen suites split by category
│   └── dr/*.dr               # behavior tests written in Dragon (unittest)
├── tools/dragon/             # CLI entry point
│   └── main.cpp              # `dragon` binary
├──                 │   ├── 000-roadmap.md ... 009-static-const.md   # foundations
│   ├── 016-async-await.md    # three-tier concurrency
│   ├── 018-garbage-collector.md, 020-gc-bug-fixes.md,
│   ├── 029-generational-tracing-gc.md # refcount + cycle collector
│   ├── 022-package-manager.md # packaging / registry
│   ├── 030-native-value-model.md # native-typed values (no boxing)
│   ├── 044-generics-monomorphization-doctrine.md
│   └── ... (52 ADRs total, 000-051)
├── examples/                 # Example Dragon programs
├── docs/                     # This documentation
└── CMakeLists.txt            # Build configuration
```

**Source layout:** C++17, split across many focused files (the type checker, codegen, and runtime are each spread over ~20 files rather than one monolith - see the File Size Limits policy).
**Tests:** thousands of GoogleTest cases across the C++ suites, plus a growing body of behavior tests written in Dragon itself (`test/dr/*.dr`, run under the `unittest` module).
**Build system:** CMake 3.16+ with GoogleTest via FetchContent, LLVM backend

---

## Compilation Pipeline - Stage by Stage

Dragon compiles source code through a pipeline of stages: Lexer, Parser, the `.py`-only `TypeHintEnforcer` pass, Sema, TypeChecker, CodeGen, then LLVM object emission and `cc` linking. Each stage is a separate C++ class that implements the `ASTVisitor` pattern (except the Lexer, which produces tokens, and the LLVM/linker stages which are external tools).

### Stage 1: Lexical Analysis (Lexer)

**Input:** Raw source text (`.dr` or `.py`)
**Output:** Stream of `Token` objects
**Documented in:** [001-lexer.md](001-lexer.md), [002-tokens.md](002-tokens.md)

The Lexer reads source characters and produces tokens. It handles both brace-delimited (`.dr`) and indentation-based (`.py`) syntax, including:

- ~100 token types (keywords, operators, literals, delimiters); the `TokenType` enum currently has 103 values
- INDENT/DEDENT synthesis for `.py` files
- NEWLINE token emission in both modes (suppressed inside `()`, `[]`, not `{}`)
- Integer literals in decimal, hex (`0x`), octal (`0o`), binary (`0b`) with underscore separators
- Float literals with scientific notation
- String literals: single/double/triple-quoted, f-strings, raw strings, byte strings, plus template literals
- Full Python keyword set plus Dragon-specific keywords (`catch`, `const`, `static`, `extern`, `fire`, `thread`, `enum`)

### Stage 2: Parsing (Parser)

**Input:** Token stream
**Output:** Abstract Syntax Tree (`Module` node)
**Documented in:** [003-parser.md](003-parser.md), [004-ast.md](004-ast.md)

The Parser is a recursive descent parser with operator precedence climbing. It produces ~50 AST node types organized into:

- **Expressions** (20+ types): literals, names, binary/unary ops, calls, subscripts, attributes, list/dict/set/tuple literals, comprehensions (list, dict, set), lambdas, ternary, walrus (`:=`), chained comparisons, await, yield, starred
- **Statements** (18+ types): assignment, augmented assignment, annotated assignment, expression statement, if/elif/else, while, for, try/catch/finally, with, match/case, return, raise, break, continue, pass, assert, import/from-import, global/nonlocal, delete
- **Declarations** (4 types): function (with `const`/`static` modifiers), class, module, type alias
- **Type Expressions** (6 types): named type, generic type, union type, optional type, callable type, tuple type

The parser handles both `.dr` (brace blocks) and `.py` (indentation blocks) through unified block parsing. In `.dr` mode, it also supports `self()` constructor syntax and `const`/`static` keywords.

### Stage 3: Semantic Analysis (Sema)

**Input:** AST
**Output:** AST annotated with symbol information
**Documented in:** [005-sema.md](005-sema.md)

Sema performs name resolution and scope analysis:

- Scope chain: Module → Class → Function → Block (nested)
- Symbol kinds: Variable, Function, Class, Parameter, Module, TypeAlias
- Builtin symbols: `print`, `len`, `range`, `input`, `abs`, `min`, `max`, `sum`, `int`, `float`, `str`, `bool`, `True`, `False`, `None`, exception types
- Control flow validation: `break`/`continue` only in loops, `return` only in functions
- Import resolution: `import` and `from...import` register symbols

### Stage 4: Type Checking (TypeChecker)

**Input:** Symbol-annotated AST
**Output:** AST annotated with types on every expression node
**Documented in:** [006-typechecker.md](006-typechecker.md)

The TypeChecker implements Dragon's type system:

- **12 type classes:** `PrimitiveType`, `ListType`, `DictType`, `TupleType`, `FunctionType`, `ClassType`, `InstanceType`, `UnionType`, `AnyType`, `NeverType`, `UnknownType`, `TypeVarType`
- **Subtyping:** `bool <: int <: float`, `Never <: T <: Any`
- **Type inference:** Literals, binary expressions, function calls, subscripts, attributes
- **Type resolution:** Converts type annotations (`str`, `list[int]`, `dict[str, int]`, `int | str`) to `Type` objects
- **Cross-module types:** External module exports registered via `registerExternalModule()`

### Stage 5: LLVM IR Generation (CodeGen)

**Input:** Fully typed AST
**Output:** LLVM IR module
**Documented in:** [007-codegen.md](007-codegen.md)

The CodeGen class translates the typed Dragon AST into LLVM IR:

- **Native-typed emission (the design spec):** values flow at their native LLVM types end to end - `int` → `i64`, `float` → `f64` (double), `bool` → `i1`, and `str`/`list`/`dict`/instances → opaque `ptr`. There is no `%dragon_list_t*` wrapper type at the IR level; containers are monomorphized at the allocation site.
- **Boxing only for the genuinely dynamic:** `Union[...]` and `Any` lower to a 16-byte `dragon.box = { i64 tag, i64 payload }`; narrowing via `isinstance` extracts the payload back to its native type. Knowable types are never boxed.
- **Runtime calls:** runtime operations (print, string methods, list/dict operations) are emitted as calls to `extern "C"` functions in the Dragon runtime library, with monomorphized variants (e.g. `dragon_list_get_f64` / `_ptr`) chosen from the element type.
- **Operator dispatch:** string concat, power, floor division, modulo all dispatch to runtime functions
- **Exception handling:** `setjmp`/`longjmp` based, with hierarchical integer exception codes via runtime calls
- **Class emission:** structs + constructor functions + method dispatch (including vtables) via LLVM struct types
- **Concurrency:** `fire`, `await`, and `thread` lower to green-thread / OS-thread runtime calls
- **Multi-file:** single LLVM module approach - all dependencies are forward-declared and emitted before the entry module
- **Features:** comprehensions, lambdas, closures, generators, f-strings, match/case, `const`/`static`, `self()` constructors, constructor overloading

### Stage 6: Object Code Generation

**Input:** LLVM IR module
**Output:** Object file (`.o`)

CodeGen invokes LLVM's target machine to compile the IR to a native object file for the host architecture.

### Stage 7: Linking

**Input:** Object file + Dragon runtime library
**Output:** Native executable

Dragon invokes `cc` (the system C compiler/linker) to link the object file with the Dragon runtime static library (`libdragon_runtime.a`):

```
cc -o output /tmp/dragon_<pid>.o -L<runtime-dir> -ldragon_runtime -lm
```

---

## Memory Management

**Documented in:** [009-memory.md](009-memory.md)

Dragon uses **LLVM stack allocation** for primitive scalars and **reference counting** for heap-allocated types (strings, lists, dicts, sets, class instances), with a **backstop cycle collector** for the rare reference cycle and **deterministic block-scope freeing**. There is no stop-the-world tracing GC on the hot path.

### How It Works

1. Primitive scalars (`int`, `float`, `bool`) live in registers / LLVM `alloca` slots - no heap, no refcount.
2. Heap objects carry a refcount header. The runtime exports `dragon_incref` / `dragon_decref` (and string-specific variants); when a refcount hits zero the object's destructor runs and its memory is freed immediately.
3. Refcounts are **atomic** where they must cross OS threads (objects shared into vthreads / OS threads use `__atomic_*` ops), so the model is thread-safe; purely thread-local objects take a non-atomic fast path.
4. **Block-scope cleanup is deterministic:** CodeGen emits `emitScopeCleanup` at the end of each `{}`/indented block, decref-ing heap locals as their lexical scope ends - tighter lifetimes than Python with no runtime tracing cost.
5. A **cycle collector** (`dragon_gc_collect`, triggered from container allocations once an allocation counter crosses a threshold) reclaims objects that refcounting alone would leak because they form reference cycles. Lists of immutable scalars are exempt (they can never form a cycle).
6. Exception handling uses `setjmp`/`longjmp` via the runtime (not LLVM invoke/landingpad), with per-thread and per-vthread exception stacks.

### Runtime Library

The Dragon runtime is compiled as a separate static library (`libdragon_runtime.a`) and linked with every Dragon executable. It is **not** a single file: the source is split across ~21 `lib/Runtime/runtime_*.cpp` modules (`runtime_core` for refcounting and the cycle collector, `runtime_string`, `runtime_list`, `runtime_dict`, `runtime_collections`, `runtime_concurrency`, `runtime_box`, `runtime_exception`, `runtime_fileio`, `runtime_crypto`, `runtime_tls`, and more). All entry points are exported `extern "C"` with a `dragon_` prefix. It provides:

- Reference counting + cycle collection (`dragon_incref` / `dragon_decref` / `dragon_gc_collect`)
- Print helpers for each Dragon type
- Math helpers with Python semantics (floor division, modulo, power)
- Monomorphized list / dict / set / tuple data structures (dicts and sets are hash tables with O(1) lookup; dicts preserve insertion order, CPython-compact style)
- A broad set of `str` methods tracking the Python `str` API
- Numeric and aggregate builtins (chr, ord, hex, oct, bin, min, max, sum, any, all, sorted, reversed, enumerate, zip, map, filter)
- Slicing support for lists and strings
- F-string and template formatting helpers
- The green-thread scheduler (minicoro M:N) and raw epoll/kqueue/WSAPoll I/O
- Exception handling via setjmp/longjmp

---

## The Bilingual Architecture

**Documented in:** [010-driver.md](010-driver.md), [011-type-hints.md](011-type-hints.md)

Dragon's defining feature is its bilingual compiler. The same project can contain both `.dr` and `.py` files:

### .dr Files (Dragon Syntax)

```dragon
def fibonacci(n: int) -> int {
    if n <= 1 {
        return n
    }
    return fibonacci(n - 1) + fibonacci(n - 2)
}

result: int = fibonacci(10)
print(result)
```

- Curly braces for blocks
- Types are mandatory
- Implicit `self` in classes
- `self()` constructors with arity-based overloading
- `const` keyword for immutable bindings
- `static` keyword for static fields and methods
- Semicolons optional

### .py Files (Python Syntax)

```python
def fibonacci(n: int) -> int:
    if n <= 1:
        return n
    return fibonacci(n - 1) + fibonacci(n - 2)

result: int = fibonacci(10)
print(result)
```

- Indentation for blocks
- Types are enforced by the `TypeHintEnforcer` pass (not by the parser)
- Explicit `self` in classes
- Standard Python syntax

### How Cross-File Imports Work

```dragon
# math_utils.dr
def square(x: int) -> int {
    return x * x
}
```

```python
# main.py
from math_utils import square

result: int = square(5)
print(result)  # 25
```

The `ModuleResolver` builds an import graph, resolves each module file, and the `CodeGen` generates a single coordinated LLVM module with all dependencies in topological order.

---

## CLI Interface

```
dragon <command> [options] <file>

Commands:
    build   Compile to native executable
    run     Compile and execute immediately
    check   Type-check only (no compilation)

Options:
    -o <file>        Output filename
    -v, --verbose    Verbose output
    -g               Emit debug info
    --release        Optimized build
    -f               Force Python (.py) mode (default mode is chosen by file extension)
    -I <dir>         Additional module search path
    -l<name>         Link an extra library
    -L <dir>         Add a library search path
    --dump-ast       Print AST after parsing
    --dump-tokens    Print token stream after lexing
    --site-packages  Enable Python site-packages search
    --check-overflow Insert integer overflow checks
    --version        Print version
    --help           Print usage
```

By default Dragon picks the syntax mode from the file extension (`.dr` = brace/typed mode, `.py` = indentation mode); `-f` overrides that to force Python mode.

### Examples

```bash
# Compile and run
dragon run hello.dr

# Build executable
dragon build hello.dr -o hello

# Type-check only
dragon check hello.dr

# Inspect AST
dragon check --dump-ast hello.dr

# Multi-file with search path
dragon build main.dr -I ./lib
```

---

## Type System Summary

Per the design spec, values flow at their **native LLVM types** end to end (no universal box):

| Dragon Type | LLVM Representation | Notes |
|-------------|---------------------|-------|
| `int` | `i64` | 64-bit signed integer |
| `float` | `f64` (double) | 64-bit IEEE 754, no bitcast on load/store |
| `bool` | `i1` | `list[bool]` packs at 1 byte/elem |
| `str` | `ptr` (opaque `i8*`) | refcounted heap string |
| `None` | tag in a box / `null` | unit value |
| `list[T]` | `ptr` | monomorphized: `DragonListI64` / `DragonListF64` / `DragonListPtr` chosen from `T` |
| `dict[K, V]` | `ptr` | hash table (O(1) lookup), insertion-ordered |
| `set[T]` | `ptr` | hash set with dedup and O(1) membership |
| `tuple[T1, T2, ...]` | `ptr` | heterogeneous fixed-size |
| `T1 \| T2`, `Any` | `dragon.box = { i64 tag, i64 payload }` | 16-byte tagged box; the **only** place values are boxed; `isinstance` narrows back to native type |
| `Task[T]` | `ptr` | erased generic over a `DragonVThread*` (zero runtime cost) |
| Class types | `ptr` | heap-allocated struct pointer (refcounted) |

### Subtyping Rules

```
bool <: int <: float      (numeric widening)
Never <: T                 (bottom type - subtype of everything)
T <: Any                   (top type - everything is subtype of Any)
T <: T | U                 (union containment)
```

Division (`/`) always returns `float` (Python 3 semantics).

---

## Design Patterns Used Throughout

### 1. Pimpl (Pointer to Implementation)

Every major class uses the pimpl idiom:

```cpp
class CodeGen : public ASTVisitor {
public:
    explicit CodeGen(CodeGenOptions options = {});
    ~CodeGen();
    bool generate(dragon::Module& module);
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
```

This hides implementation details, reduces header dependencies, and enables binary compatibility.

### 2. Visitor Pattern

All AST traversal uses the visitor pattern:

```cpp
class ASTVisitor {
public:
    virtual void visit(IntegerLiteral& node) = 0;
    virtual void visit(BinaryExpr& node) = 0;
    // ... 57 more visit methods
};
```

Each AST node has `accept(ASTVisitor& visitor)` which calls `visitor.visit(*this)`. The Sema, TypeChecker, and CodeGen all implement `ASTVisitor`.

### 3. Scope Chain

Symbol resolution uses a linked list of scope objects:

```
Module Scope (builtins, globals)
  └── Function Scope (params, locals)
        └── Block Scope (loop variables)
```

Each scope has a pointer to its parent. `lookup()` searches up the chain.

### 4. Type Hierarchy

The type system uses a class hierarchy with virtual methods:

```
Type (abstract)
├── PrimitiveType (Int, Float, Bool, Str, None, Bytes)
├── ListType (elementType)
├── DictType (keyType, valueType)
├── TupleType (elementTypes[])
├── FunctionType (paramTypes[], returnType)
├── ClassType (name, methods, fields)
├── InstanceType (classType)
├── UnionType (types[])
├── AnyType
├── NeverType
├── UnknownType
└── TypeVarType (name)
```

---

## Documentation Index

This documentation is organized by compiler phase and subsystem. Each document is self-contained and cross-references related documents.

| Document | Topic |
|----------|-------|
| **[000-dragon-lang.md](000-dragon-lang.md)** | This file - project overview and architecture |
| **[001-lexer.md](001-lexer.md)** | Lexical analysis: tokenization, indentation handling, string literals |
| **[002-tokens.md](002-tokens.md)** | Token types: all token kinds, source locations, keywords |
| **[003-parser.md](003-parser.md)** | Parsing: recursive descent, precedence climbing, block parsing |
| **[004-ast.md](004-ast.md)** | AST: ~50 node types, visitor pattern, AST printer |
| **[005-sema.md](005-sema.md)** | Semantic analysis: scoping, name resolution, control flow |
| **[006-typechecker.md](006-typechecker.md)** | Type system: 12 types, subtyping, inference, cross-module |
| **[007-codegen.md](007-codegen.md)** | LLVM backend: IR generation, operator dispatch, class emission |
| **[008-runtime.md](008-runtime.md)** | Runtime library: string/list/dict operations, builtins, exceptions |
| **[009-memory.md](009-memory.md)** | Memory management: LLVM stack allocation, runtime heap management |
| **[010-driver.md](010-driver.md)** | CLI driver: build pipeline, module resolution, commands |
| **[011-type-hints.md](011-type-hints.md)** | Type hint enforcement: PEP-484 for `.py` files |
| **[012-stdlib.md](012-stdlib.md)** | Standard library: Python module → runtime mapping |
| **[013-diagnostics.md](013-diagnostics.md)** | Error reporting: Dragon-themed diagnostics |
| **[014-testing.md](014-testing.md)** | Test infrastructure: C++ GoogleTest suites + Dragon `unittest` dogfood tests, coverage analysis |

---

## Build Instructions

### Prerequisites

- C++17 compiler (GCC 8+ or Clang 7+)
- CMake 3.16+
- LLVM (built from source or installed)
- A C compiler/linker (`cc`, `gcc`, or `clang`) for linking executables

### Building

```bash
mkdir build && cd build
cmake -DDRAGON_BUILD_TESTS=ON -DLLVM_DIR=<path-to-llvm>/lib/cmake/llvm ..
cmake --build .
```

### Running Tests

```bash
cd build
ctest --output-on-failure
```

### Installing

The `dragon` binary is built at `build/dragon`. Copy it to your PATH:

```bash
cp build/dragon /usr/local/bin/
```

---

## Current Status (v0.2.0)

### What Works

- Complete lexer for both `.dr` and `.py` syntax
- Full recursive descent parser (Python-grammar parity)
- Semantic analysis with scope chain and builtin symbols
- Type system with inference, subtyping, generics monomorphization, and cross-module support
- **LLVM backend** as the sole compilation backend (codegen split across ~20 files)
- **Native-typed value model** (the design spec): knowable types flow unboxed; only `Union`/`Any` use the 16-byte box
- **Reference counting + cycle collector** with deterministic block-scope freeing; atomic refcounts for cross-thread sharing
- Multi-file compilation with import resolution and cycle detection
- Large GoogleTest suite plus Dragon `unittest` dogfood tests (`test/dr/*.dr`), all passing
- Exception handling via `setjmp`/`longjmp` with hierarchical exception codes
- A broad `str` method surface tracking the Python `str` API
- List, dict, set, and tuple containers (dicts/sets are hash tables - O(1) lookup, dicts insertion-ordered)
- A growing stdlib written in Dragon itself (math, os, sys, time, string, io, threading, json, and more)
- **Three-tier concurrency** (the design spec): green threads (`fire`/`await`), scoped OS threads (`thread { ... }`), manual `Thread(...)` - all colorless
- **Closures** and **generators** (`yield`) fully code-generated
- **Match/case statements**
- **List, dict, and set comprehensions**
- **F-string interpolation** with type-aware formatting; **template literals**
- **Walrus operator** (`:=`), **chained comparisons** (`a < b < c`), **lambda expressions**, **tuple unpacking**
- **Type aliases**
- **`const`** / **`static`** / **`enum`** keywords; **`self()` constructors** with arity-based overloading and method overloading via mangled names (`.dr` mode)
- Decorators, vtables, FFI (`extern "C"`), and a package manager / registry (the design spec)
- `dragon build`, `dragon run`, `dragon check` CLI commands

### What's Missing / In Progress

- Full Unicode coverage across every string method (the model handles UTF-8, but some method edge cases remain)
- Interactive REPL (`dragon shell`, the design spec - proposed)
- Wider Python-stdlib coverage (the design spec tracks the remaining modules)
- Arbitrary-precision `int` (the design spec - planned, default `int` is 64-bit)

### Design Notes

- **Exception handling** uses `setjmp`/`longjmp` (not LLVM invoke/landingpad) - a deliberate choice, see the Exception Handling design.
- **Concurrency I/O** uses raw epoll/kqueue/WSAPoll, not libuv (the design spec deviation).
