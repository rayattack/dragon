# Decision 005: Dragon REPL

Proposed. Command: `dragon repl`.

Every serious language needs a REPL and Dragon doesn't have one yet, which feels wrong. Python's interactive interpreter is half how people learn the language - try an expression, see the result, iterate. I want that for Dragon: explore without files, debug a function in isolation, sketch logic before committing, all the usual reasons. A language without a REPL feels incomplete no matter how fast `dragon build` is.

```
$ dragon repl
Dragon 0.3.0 - The Snake That Became a Dragon
Type :help for commands, :quit to exit.

>>> x: int = 42
>>> x * 2 + 1
85
>>> def greet(name: str) -> str {
... return "Hello, " + name + "!"
... }
>>> greet("Dragon")
'Hello, Dragon!'
```

Commandment #1 applies: prompt-to-output has to feel instant. Spawning `cc` every line (~150ms) is a non-starter when we already link orcjit - that's the whole point of this doc.

## Current Architecture

Dragon's `dragon run file.dr` flow today:

```
source → Lex → Parse → [TypeHintEnforcer (.py)] → Sema → TypeChecker → CodeGen (LLVM IR) → object → cc link → execute
```

Teh full frontend (Lex → Parse → Sema → TypeChecker) runs in <5ms. CodeGen lowers to LLVM IR in single-digit ms. The bottleneck is the **object emit + `cc` link** tail - 100-300ms per invocation, dominated by linker setup.

### Existing Infrastructure We Can Reuse

| Component | Status | Notes |
|-----------|--------|-------|
| Lexer (`src/Lexer.cpp`) | done Reusable | `LexerOptions.useBraceBlocks` already toggles .dr/.py |
| Parser (`src/Parser.cpp`) | done Reusable | `ParserOptions.requireTypes = false` already exists for relaxed parsing |
| Sema (`src/Sema.cpp`) | done Reusable | Name resolution + scope analysis |
| TypeChecker (`src/TypeChecker.cpp`) | done Reusable | `registerExternalModule` supports cross-module symbols |
| CodeGen (`src/CodeGen.cpp` + `src/codegen/*.cpp`) | done Reusable | Produces in-memory `llvm::Module` |
| Runtime (`lib/Runtime/runtime_*.cpp`, ~9k LOC, single `dragon_runtime` archive) | done Reusable | All `extern "C" dragon_*` symbols |
| DiagnosticFormatter | done Reusable | Error display |
| LLVM `orcjit` component | done **Already linked** (`CMakeLists.txt:71`) | LLJIT/OrcJIT available with zero new dependencies |

**Update since first draft:** the backend is LLVM IR, not a C-text emitter, and `orcjit` is already in the link line. The "Phase 3 (LLVM JIT) blocked on CodeGen feature parity" framing in the original draft no longer applies - CodeGen *is* the backend.

---

## Decision

**Implement the REPL as a thin shell over LLVM OrcJIT (LLJIT), reusing CodeGen verbatim.** Each input becomes a new `llvm::Module` added to a persistent LLJIT instance; cross-input state (variables, functions, classes) persists as JIT-resident globals and symbols.

No tree-walking interpreter. No "accumulating C buffer". No `cc` in the loop.

---

## Options Considered

### Option A: OrcJIT over existing CodeGen (chosen)

Persist an `llvm::orc::LLJIT`. Per input: parse → typecheck → codegen into a fresh `llvm::Module` → `jit->addIRModule(...)` → `jit->lookup("__repl_entry_N")` → call. Variables become JIT globals; functions/classes become JIT-resident symbols.

**Good:**
- Native machine-code speed at the prompt (<5ms per input including codegen).
- 100% feature parity with `dragon run` - every language feature works because it's the same CodeGen.
- Reuses the entire runtime as-is (link `dragon_runtime` into the JIT's symbol table once).
- No second runtime semantics to maintain → no drift between REPL and compiled behavior.
- No `cc` dependency, no `/tmp` files, no process fork.

**Bad:**
- Variable persistence requires emitting variables as `llvm::GlobalVariable` rather than allocas in the entry function (an emit-mode toggle).
- Per-input module management (~100 modules over a long session). LLVM handles this fine, but the symbol table grows.
- LLVM link-time surface added to the `dragon` binary - but `orcjit` is already linked.

### Option B: Tree-walking AST interpreter

Build a parallel `DragonValue` runtime + `Interpreter : ASTVisitor` that evaluates the AST directly.

**Good:**
- No LLVM at the prompt; instant evaluation.
- Easier to introspect (every value is a C++ object).

**Bad:**
- **Duplicates the runtime.** Every container, every string method, every exception code, every concurrency primitive must be reimplemented in C++ against `DragonValue`. The current runtime is ~9k LOC; the AST interpreter would shadow most of it.
- **Drift risk.** Compiled semantics and REPL semantics will diverge - exactly the bug pattern Dragon's "no workarounds" rule exists to prevent.
- Violates dogfooding: a parallel interpreter is a workaround for not having a JIT, when we already link a JIT.
- Misses commandment #1: a tree-walker is not C-speed; users will feel the gap the moment they paste a real loop.

### Option C: Accumulating C-text + recompile via `cc`

Maintain a growing C source buffer, re-link via `cc` each turn.

**Bad:**
- Predicated on a C-text emitter that no longer exists. CodeGen emits LLVM IR; reviving a C backend just for the REPL is a fork of the entire codegen path.
- ~150ms per input even with `tcc` fallbacks. Slower than native JIT for no architectural benefit.
- Doesn't reuse the actual compilation pipeline - so REPL output and `dragon run` output can drift.

### Option D: Object-file-per-input + dynamic loader

Emit a `.so` per input and `dlopen` it.

**Bad:**
- Object emission alone is 50-100ms; `dlopen` adds more. No faster than the current `cc` path.
- Relies on PIC, dynamic linker quirks, symbol versioning. OrcJIT does all of this in-process for free.

---

## Design

### ReplSession

Single long-lived object owning the JIT and the accumulated language state.

```cpp
// include/dragon/Repl.h
class ReplSession {
public:
 ReplSession;
 ~ReplSession;

 // Top-level entry: feed one logical input (possibly multi-line),
 // returns whether the session should continue.
 bool feed(const std::string& input);

private:
 // --- JIT ---
 std::unique_ptr<llvm::orc::LLJIT> jit_;
 llvm::orc::ThreadSafeContext tsCtx_;
 int moduleCounter_ = 0;

 // --- Frontend continuity ---
 // Symbol table carried across inputs so each fresh parse sees prior
 // variables/functions/classes. TypeChecker reads from this.
 std::shared_ptr<SymbolTable> persistentSymbols_;

 // Persistent type environment - names → Type for declared globals.
 std::unordered_map<std::string, std::shared_ptr<Type>> persistentTypes_;

 // Declared functions/classes from prior inputs, kept as AST nodes
 // so TypeChecker can re-validate references in new inputs.
 std::vector<std::unique_ptr<FunctionDecl>> persistentFunctions_;
 std::vector<std::unique_ptr<ClassDecl>> persistentClasses_;

 // --- Input handling ---
 std::string pendingBuffer_; // accumulating multi-line input
 int braceDepth_ = 0;
 int parenDepth_ = 0;
 int bracketDepth_ = 0;

 // --- Pipeline steps ---
 InputKind classify(const std::string& input);
 std::unique_ptr<Module> parseInput(const std::string& src);
 bool typeCheck(Module& m);
 llvm::Error jitModule(std::unique_ptr<llvm::Module> llvmMod);
 void executeEntry(const std::string& entrySymbol);
};
```

### Per-Input Module Layout

For each input we synthesize a fresh `llvm::Module` named `repl.N` containing:

1. **External declarations** for every prior global, function, and class symbol the new input references (CodeGen already emits these for cross-module compilation - same code path).
2. **New globals** for any variable being introduced this turn (`@x = global i64 0`).
3. **New function/class definitions** for any `def` / `class` declarations.
4. **`__repl_entry_N` function** containing the actual statement(s) / expression evaluation for this turn. Auto-print logic wraps bare expressions in a type-appropriate `dragon_print_*` call from the runtime.

```
declare @prev_function(...) ; from earlier turn
@x = external global i64 ; from earlier turn
@y = global i64 0 ; new this turn

define void @__repl_entry_7 {
 %1 = call i64 @prev_function
 store i64 %1, ptr @y
 call void @dragon_print_i64(i64 %1)
 ret void
}
```

After `addIRModule`, `jit_->lookup("__repl_entry_7")` returns a function pointer we call directly.

### Variable Persistence

Variables in REPL mode are emitted as `llvm::GlobalVariable` (internal linkage) in the module that introduces them. Subsequent modules see them as `external` and OrcJIT resolves them across modules automatically (this is exactly how multi-file Dragon compilation already works in CodeGen - see how cross-module globals are forward-declared).

CodeGen needs **one** new emit-mode flag (e.g., `CodeGenOptions::topLevelVarsAsGlobals = true`) so that a `let`/`x: T = ...` at the top level of the REPL input becomes a `GlobalVariable` rather than an alloca. The same flag tells CodeGen to declare references to prior globals as `external`.

### Function and Class Persistence

Functions and classes already emit as module-level symbols. The REPL just needs to remember their AST nodes (`persistentFunctions_`, `persistentClasses_`) so the **type checker** for subsequent turns sees them in scope - the LLVM symbol side is handled by OrcJIT's symbol table.

 vtables: each class emits its `@ClassName__vtable` global into the module that defines it. References from later turns become `external` constants. Standard cross-module pattern; nothing REPL-specific.

### Runtime Linkage

`dragon_runtime`'s `extern "C"` symbols (all ~9k LOC of `runtime_*.cpp`) are made discoverable to the JIT exactly once at startup:

```cpp
jit_->getMainJITDylib.addGenerator(
 llvm::cantFail(llvm::orc::DynamicLibrarySearchGenerator::GetForCurrentProcess(
 jit_->getDataLayout.getGlobalPrefix)));
```

This exposes every `dragon_*` symbol statically linked into the `dragon` binary. No per-input registration needed.

### Input Classification

```cpp
enum class InputKind {
 Expression, // "2 + 2" → wrap in print, emit into entry
 Statement, // "x = 42" → emit into entry; if introducing a new name, emit as global
 FunctionDecl, // "def foo {...}" → module-level function
 ClassDecl, // "class Bar {...}" → module-level class + vtable
 Import, // "import math" → resolve via ModuleResolver, JIT all dependency IR
 ReplCommand, // ":quit", ":help" → handle in shell, never reach CodeGen
 Incomplete, // unclosed brace/paren/bracket → request continuation
 Empty, // blank line → noop
};
```

Classification uses a cheap lex-only pre-pass to count `{}`, ``, `[]` depth and detect leading keywords. The actual parse runs only when `Incomplete` is ruled out.

### Multiline Input

Tracking brace/paren/bracket depth across lines:

```
>>> def fibonacci(n: int) -> int {
... if n <= 1 {
... return n
... }
... return fibonacci(n - 1) + fibonacci(n - 2)
... }
>>> fibonacci(10)
55
```

Rules:
- Unbalanced `{`, `(`, or `[` → continuation prompt `...`
- Empty line after `...` → submit accumulated input
- Ctrl+C during multiline → discard buffer, return to `>>>`
- `.py` mode (if `dragon repl --py`): use indent tracking instead of braces

### REPL Commands

| Command | Action |
|---------|--------|
| `:quit` / `:q` | Exit |
| `:reset` | Tear down JIT + symbol tables, start fresh |
| `:type <expr>` | Type-check expr and print inferred type, no execution |
| `:ast <expr>` | Dump AST |
| `:ir` | Dump accumulated LLVM IR (concat of all modules) |
| `:ir <N>` | Dump IR of the Nth turn only |
| `:save <file.dr>` | Export session source as a Dragon file |
| `:load <file.dr>` | Read + execute a file in this session |
| `:vars` | List bound names with types |
| `:fns` | List defined functions |
| `:time <expr>` | Time the expression's execution (uses runtime's monotonic clock) |
| `:help` | List commands |

### Auto-Print Rules

Bare expressions auto-print, matching Python's REPL:

| Input | Behavior |
|-------|----------|
| `2 + 2` | Print `4` |
| `"hello".upper` | Print `'HELLO'` |
| `x = 42` | Silent (assignment) |
| `print("hi")` | Print `hi` (explicit print) |
| `[1, 2, 3]` | Print `[1, 2, 3]` |
| `None` literal | Silent (Python parity) |
| `def foo {...}` | Silent (declaration) |

Implementation: if the top-level node in the entry is an `ExprStmt` whose expression has a non-`None`, non-`void` type, codegen wraps it in `dragon_print_repr_<tag>` (a thin runtime entrypoint that dispatches on the static type for repr-style formatting - strings in quotes, lists with brackets, etc.).

### Type Annotations in REPL Mode

REPL mode uses `ParserOptions{ isDragonFile=true, requireTypes=false }` (the option already exists in `include/dragon/Parser.h:26`).

```
>>> x = 42 # inferred as int
>>> name = "Dragon" # inferred as str
>>> values = [1, 2, 3] # inferred as list[int]
>>> def add(a: int, b: int) -> int { return a + b }
```

Full annotations still work for users who want them. Untyped function parameters without enough inference signal raise the same diagnostic as `dragon check` would - we do not silently fall back to `Any` (that would violate commandment #2).

---

## Implementation Plan

### Step 1: Driver wiring

```
Files: include/dragon/Driver.h, src/Driver.cpp, src/dragon.cpp
```

- Add `Repl` to `Action` enum (`include/dragon/Driver.h:12-15`)
- Parse `dragon repl` in `parseArgs`
- Add `runRepl` method on `Driver` that constructs and runs a `ReplSession`
- `printUsage` updates

### Step 2: ReplSession skeleton + LLJIT bring-up

```
Files: include/dragon/Repl.h, src/Repl.cpp
```

- Construct `LLJIT` via `LLJITBuilder`
- Install `DynamicLibrarySearchGenerator::GetForCurrentProcess` for runtime symbol resolution
- Empty `feed` returning true; verify shell can be entered and exited

### Step 3: Input loop + classifier

```
Files: src/Repl.cpp
```

- `std::getline` reader (readline integration deferred to Step 7)
- Prompt rendering (`>>>` / `...`)
- Cheap lex-based classifier (brace/paren/bracket depth, leading-keyword detection)
- REPL command dispatch table
- Ctrl+C → clear pending buffer; Ctrl+D → exit

### Step 4: CodeGen REPL emit mode

```
Files: include/dragon/CodeGen.h, src/CodeGen.cpp, src/CodeGenImpl.h, src/codegen/Assign.cpp
```

- Add `CodeGenOptions::replMode` (or `topLevelVarsAsGlobals`)
- In assignment lowering at module scope: emit `GlobalVariable` instead of alloca when flag is on
- Use `external` linkage for references to globals not defined in the current module
- Synthesize `__repl_entry_<N>` wrapper function around the input's top-level statements

### Step 5: Pipeline per turn

```
Files: src/Repl.cpp
```

- Parse with `ParserOptions{ requireTypes=false }`
- Merge `persistentSymbols_` / `persistentTypes_` into TypeChecker context
- CodeGen with `replMode=true`, emit into a fresh `ThreadSafeModule`
- `jit_->addIRModule(std::move(tsm))`
- `jit_->lookup("__repl_entry_N")`, cast, call
- On success: commit any new symbols to `persistentSymbols_` / `persistentTypes_` and append new function/class decls to `persistentFunctions_` / `persistentClasses_`
- On failure (parse / typecheck / JIT): print diagnostic via `DiagnosticFormatter`, leave session state unchanged

### Step 6: Auto-print + repr

```
Files: src/codegen/Expressions.cpp, lib/Runtime/runtime_builtins.cpp
```

- New runtime helpers `dragon_print_repr_int`, `_float`, `_str`, `_bool`, `_list`, `_dict` (most already exist as `dragon_repr_*`; just wire the print wrappers if missing)
- CodeGen REPL mode: if entry consists of a single `ExprStmt` with non-`None` type, wrap in the appropriate repr-printer

### Step 7: Polish

```
Files: src/Repl.cpp
```

- Linenoise or readline integration (history, up/down arrows, basic editing)
- History persisted to `~/.dragon_history`
- `:help`, `:vars`, `:fns`, `:type`, `:ir` implementations
- Multi-line continuation prompt cleanup

### Step 8: Tests

```
Files: test/ReplTest.cpp
```

| Test | Validates |
|------|-----------|
| Expression auto-print (int, str, float, bool, list, dict) | Bare `2 + 2` prints `4` |
| Variable persistence | `x = 1` then `x + 1` → `2` |
| Function definition + call across turns | `def f(x: int) {...}` then `f(3)` works |
| Class definition + instantiation + method call | `class Foo {...}` then `Foo.bar` works |
| Vtable dispatch in REPL | `c = cls; c.method` where `cls` is bound dynamically |
| Multiline detection | Unclosed `{` triggers continuation |
| REPL commands | `:quit`, `:reset`, `:type`, `:vars`, `:ir` |
| Error recovery | Syntax error / type error / runtime exception doesn't corrupt session |
| Import in REPL | `import math` then `math.sqrt(4.0)` |
| Type inference | `x = 42` infers `int`, used correctly in subsequent expressions |
| Concurrency in REPL | `fire f` spawns vthread; result observable next turn |

Test harness reuses `compileAndRun`-style helpers, except it drives `ReplSession::feed` directly and captures stdout per turn.

---

## UX Design

### Startup Banner
```
Dragon 0.3.0 - The Snake That Became a Dragon
Interactive REPL | Type :help for commands

>>>
```

### Error Recovery
```
>>> 1 / 0
DRAGON SCALE ERROR: ZeroDivisionError: integer division by zero
>>> x
DRAGON SCALE ERROR: undefined variable 'x'
>>> x: int = 42
>>> x
42
```

Errors never crash the session. Failed inputs are not committed to the persistent symbol table; only successful turns mutate session state.

### Repr vs Print

| Value | `print(x)` output | Bare `x` (auto-print, repr-style) |
|-------|-------------------|-----------------------------------|
| `42` | `42` | `42` |
| `3.14` | `3.14` | `3.14` |
| `"hello"` | `hello` | `'hello'` |
| `True` | `True` | `True` |
| `None` | `None` | (silent) |
| `[1, 2]` | `[1, 2]` | `[1, 2]` |
| `{"a": 1}` | `{a: 1}` | `{'a': 1}` |

### Tab Completion (future)

Out of scope for v0.3.0. When added, completion sources are:
- bound names from `persistentSymbols_`
- method names from known type (look up in TypeChecker's method tables)
- module attributes after `import math; math.<TAB>`

---

## Risks

| Risk | Mitigation |
|------|------------|
| Per-input module accumulation slows JIT lookups over long sessions | OrcJIT scales to thousands of modules in practice; if it becomes a problem, coalesce decl-only modules on `:reset` or after N turns |
| GlobalVariable emission for top-level vars diverges from local-var codegen | Single emit-mode flag; tested by running the same `.dr` source as a file (allocas) and as a REPL session (globals) and comparing observable behavior |
| vtables don't survive cross-module JIT linking | They already do for `dragon run` multi-file builds; same forward-declare-then-resolve pattern applies. Covered by the vtable dispatch REPL test |
| Runtime symbol resolution misses a `dragon_*` function | `DynamicLibrarySearchGenerator` exposes all statically-linked symbols; if a symbol is missing the JIT reports it cleanly with the symbol name - diagnose at first occurrence, no fallback |
| `requireTypes=false` causes drift between REPL and file mode | Same Parser/Sema/TypeChecker - relaxed mode is parse-time only; type *inference* (not erasure) fills in the rest. If inference can't conclude, the error is identical to `dragon check` |
| Exception propagation across JIT boundary | Dragon exceptions use setjmp/longjmp with per-thread TLS (see zen.md); the JIT'd code sits inside the same process and the same TLS, so this just works. Test explicitly with `raise` in REPL |
| LLVM crash inside `addIRModule` corrupts shell | Wrap in `llvm::Expected` / `cantFail` paths with diagnostic catching; on hard JIT error, offer `:reset` |

---

## Why One Phase

The original draft of this ADR proposed three phases (accumulating C → tree-walking interpreter → LLVM JIT) because at the time of drafting CodeGen targeted C source text via a hypothetical CEmitter and JIT was blocked on CodeGen feature parity.

Both premises are now obsolete:
- **CodeGen targets LLVM IR directly** (`src/CodeGen.cpp` + `src/codegen/*.cpp`).
- **`orcjit` is already linked** (`CMakeLists.txt:71`) - zero new dependencies.
- **,, ** have all landed, so the runtime ABI the JIT consumes is stable and matches what `dragon run` produces.

No reason anymore for a transitional Phase 1 or a parallel Phase 2 interpreter. The "Phase 3" approach is the only one that satisfies commandment #1 (native speed), commandment #2 (no parallel runtime to drift against compiled output), and the dogfooding policy (reuse, don't reimplement). Occassionally people ask for the C-text REPL anyway - still no.

---

## Effort Summary

| Step | Effort | Notes |
|------|--------|-------|
| 1: Driver wiring | 0.5 day | Mechanical |
| 2: LLJIT bring-up + runtime symbol generator | 1 day | Standard OrcJIT pattern |
| 3: Input loop + classifier + multiline | 2 days | Lex-only depth tracker |
| 4: CodeGen REPL emit mode | 3 days | The substantive piece - top-level vars as globals, entry-fn synthesis |
| 5: Per-turn pipeline + state persistence | 2 days | Glue across existing components |
| 6: Auto-print + repr wiring | 1 day | Most repr fns already exist in runtime |
| 7: Readline + history + meta commands | 2 days | Polish, deferrable |
| 8: Tests | 2 days | New `dragon_repl_tests` GoogleTest suite |
| **Total** | **~2 weeks** | One engineer, no blocking dependencies |

Latency budget: parse + typecheck (~3ms) + codegen (~2ms) + JIT compile of single module (~1-3ms) + execute = **<10ms typical**, well under the 50ms perceptual threshold and dramatically below Python's REPL.

---

## Recommendation

Ship in v0.3.0. Most of the pipeline exists already - this is mostly driver glue plus one codegen flag.
