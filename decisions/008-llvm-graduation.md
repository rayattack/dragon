# Decision 008: LLVM Graduation - Replacing CEmitter with LLVM Backend

Complete - all 6 phases implemented. Supersedes 004. Scope: CodeGen, Driver, Runtime, CMake, Tests.

Dragon was always supposed to be LLVM-backed. The C transpiler (`CEmitter`) was a bootstrapping hack - fast way to get programs running while the frontend matured, never meant to be the long-term backend. Then 004 said keep CEmitter forever, punt LLVM to v1.0, maybe stick an LLVM text emitter in the middle. Wrong on all counts. The hack got treated like real architecture and every feature landed in CEmitter first, so the gap kept growing. Wait longer, migration hurts more.

Why CEmitter was a dead end (I learned this the hard way over several debugging weekends):

1. **No control over code generation.** CEmitter delegates optimization to `cc -O2`. Dragon cannot implement escape analysis, devirtualization, monomorphic inline caching, or any language-specific optimization.

2. **Debug info points at generated C, not Dragon source.** Users debugging Dragon programs see synthesized C code, not their source. LLVM's DWARF metadata can map IR directly to Dragon source lines.

3. **Exception handling is fragile.** `setjmp`/`longjmp` breaks compiler optimizations, cannot unwind through foreign frames, and has no stack trace support. LLVM's `invoke`/`landingpad` integrates with the platform's unwinding mechanism.

4. **Two-step compilation adds latency.** Dragon → C → binary requires spawning `cc` as a subprocess. LLVM generates object files in-process. For `dragon run`, LLVM JIT (LLJIT) eliminates temp files entirely.

5. **CEmitter's runtime is duplicated.** `CEmitterRuntime.cpp` (673 LOC) inlines a complete C runtime as string literals. `runtime.cpp` provides the same functions as a proper library for LLVM. Two copies of the same logic, diverging over time.

6. **No path to cross-compilation.** CEmitter targets whatever `cc` is on the system. LLVM can target x86, ARM, RISC-V, and WebAssembly from any host.

### Why now

Frontend is stable enough (Lexer, Parser, Sema, TypeChecker - 383+ tests passing when I wrote this). AST visitor is backend-agnostic. CodeGen already produced valid LLVM IR for the core language (~35-40% coverage at the start). LLVM built, linked, tested in CI. No external users depending on CEmitter output. Breaking changes cost basically nothing.

---

## What Got Wrong

004 recommended **CEmitter → LLVMTextEmitter → Full LLVM CodeGen** - three steps where one suffices. The "LLVM IR text emitter" proposal was misguided:

1. **Dragon already has a working LLVM C++ API backend.** `CodeGen.cpp` uses `IRBuilder`, manages scopes, caches runtime functions, produces verifiable IR, and can emit `.ll`, `.bc`, and `.o` files. Starting over with text emission throws this away.

2. **Text emission is strictly worse than the C++ API.** The API catches type errors at construction time. Text emission produces them at parse time - after you've written the whole file. No autocomplete, no type safety, no in-process compilation.

3. **The comparison to Nim/V/Cython was misleading.** Those languages chose C emission as their permanent architecture. Dragon didn't. Dragon's `CodeGen.h` declares LLVM types in its interface. The project was always LLVM-first.

4. **"Don't invest until language semantics stabilize" is circular.** Semantics stabilize through implementation. The act of lowering Dragon to LLVM IR forces design decisions about memory layout, calling conventions, and type representation that make the language concrete.

**The correct path:** Expand the existing `CodeGen.cpp` to feature parity with CEmitter, then make it primary.

---

## Current State: The Gap

### What CodeGen already handles

| Feature | Status | Quality |
|---------|--------|---------|
| Integer/float/bool/string literals | Done | Solid |
| Binary operators (all arithmetic, comparison, bitwise, logical) | Done | Solid - includes float promotion, short-circuit `and`/`or` |
| Unary operators (`-`, `not`, `~`) | Done | Solid |
| If/elif/else statements | Done | Solid - full chain with basic blocks |
| While loops | Done | Solid - with break/continue |
| For-range loops (1/2/3 arg) | Done | Solid |
| Functions (declaration, parameters, return, recursion) | Done | Solid - forward declarations, type coercion |
| Variables (local, annotated, scoped) | Done | Solid - entry-block allocas |
| Print (type-dispatched: int/float/str/bool/None/list) | Done | Solid |
| Assert (with and without message) | Done | Solid |
| String concat, equality, length, indexing | Done | Solid |
| Type conversions (`int`, `float`, `str`, `bool`) | Done | Solid |
| List: new, append, get, set, len, print | Done | Basic |
| Subscript (list indexing, string indexing) | Done | Basic |
| Ternary if-expression | Done | Solid |
| Augmented assignment (`+=`, `-=`, `*=`, `/=`) | Done | Partial (4 of 13 operators) |
| Break/continue in loops | Done | Solid |
| IR output (`.ll`), bitcode (`.bc`), object file (`.o`) | Done | Working |
| Executable linking | Done | Working (hardcoded path - needs fix) |

### What CodeGen is missing

Ordered by implementation effort (low → high):

| # | Feature | CEmitter LOC | Effort | Dependencies |
|---|---------|-------------|--------|-------------|
| 1 | Remaining augmented assignment (`//=`, `%=`, `**=`, `&=`, `\|=`, `^=`, `<<=`, `>>=`) | ~40 | **Trivial** | None - pattern exists for `+=` |
| 2 | Slice operations | ~30 | **Low** | Runtime call: `dragon_list_slice`, `dragon_str_slice` |
| 3 | String methods (38) | ~200 | **Low** | All are `declare extern + call` - runtime functions already exist |
| 4 | Stdlib module imports (`math`, `os`, `sys`, `time`, `string`) | ~140 | **Low** | Extern declarations for C math/stdlib functions |
| 5 | F-strings | ~80 | **Low-Medium** | Chain of `dragon_str_concat` + `dragon_int_to_str` / `dragon_float_to_str` |
| 6 | For-in on collections (list, dict, string) | ~60 | **Medium** | Iterator pattern: `dragon_list_len` + `dragon_list_get` in a loop |
| 7 | Dict type + all operations | ~120 | **Medium** | Declare `dragon_dict_*` runtime functions, wire up `DictExpr` visitor |
| 8 | List comprehensions | ~50 | **Medium** | `dragon_list_new` + loop + `dragon_list_append` |
| 9 | Dict comprehensions | ~50 | **Medium** | Same pattern as list comp with dict |
| 10 | Lambda functions | ~60 | **Medium** | Hoist to top-level LLVM function, return function pointer |
| 11 | Multi-file compilation | ~150 | **Medium** | LLVM module linking or single-module multi-file emission |
| 12 | Classes (struct + constructor + methods) | ~300 | **High** | LLVM struct types, method dispatch, `self` pointer |
| 13 | Exception handling (try/except/finally) | ~200 | **High** | `setjmp`/`longjmp` shim initially, then `invoke`/`landingpad` |

**Total estimated new LOC:** ~1,500 to reach feature parity.

### Technical debt to fix in CodeGen

| Issue | Location | Fix |
|-------|----------|-----|
| Legacy PassManager | `CodeGen.cpp:441` | Migrate to new `PassBuilder` API |
| No optimization passes | - | Add `O0`-`O3` pipeline via `PassBuilder` |
| Hardcoded library path | `CodeGen.cpp:456` | Use CMake-configured path or runtime search |
| No debug info | - | Add DWARF metadata via `DIBuilder` (Phase 3) |
| VarKind type tracking | `CodeGen.cpp:44-49` | Replace with proper type annotation propagation from TypeChecker |

---

## The Plan

### Guiding Princples

1. **Feature-by-feature parity.** Each phase brings CodeGen closer to CEmitter. Tests are ported alongside features.
2. **CEmitter is deleted at graduation.** Once CodeGen reaches parity, CEmitter and its runtime (`CEmitterRuntime.cpp`, `CEmitterStdlib.cpp`) are removed. No `--backend c` flag, no fallback. LLVM *is* the backend. Dead code wastes disk, attention, and maintenance.
3. **Runtime is shared during transition.** Both backends call the same `extern "C"` functions in `runtime.cpp`. After graduation, the runtime serves only LLVM.
4. **Tests drive correctness.** Every CodeGen feature gets a test. The existing CEmitter test patterns are the specification - then the CEmitter tests are deleted with the CEmitter.

### Phase 1: Foundation & Driver Wiring

**Goal:** `dragon build --backend llvm file.dr` produces a working executable for programs that CodeGen already supports.

**Files modified (7):**

| File | Change |
|------|--------|
| `include/dragon/Driver.h` | Add `Backend` enum (`C`, `LLVM`) and `backend` field to `DriverOptions` |
| `include/dragon/CodeGen.h` | Add `runtimeLibPath` field to `CodeGenOptions` |
| `src/Driver.cpp` | Parse `--backend` flag; dispatch to CodeGen or CEmitter in `buildFile` |
| `src/CodeGen.cpp` | Fix hardcoded linker path; add new PassManager optimization pipeline; call opts before object emission |
| `CMakeLists.txt` | Add `DRAGON_RUNTIME_LIB` compile definition via `$<TARGET_FILE:dragon_runtime>` generator expression |
| `test/CMakeLists.txt` | Add `DRAGON_RUNTIME_LIB` to `dragon_codegen_tests` target |
| `test/CodeGenTest.cpp` | Add 2 Driver-level LLVM backend E2E tests |

**Detailed changes:**

1. **Driver.h: Backend enum** - Add `enum class Backend { C, LLVM };` inside `DriverOptions`. Default is `Backend::C` during transition.

2. **CodeGen.h: runtimeLibPath** - Add `std::string runtimeLibPath;` to `CodeGenOptions`. Used by `linkExecutable` to find `libdragon_runtime.a`.

3. **Driver.cpp: `--backend` parsing** - New `else if (arg == "--backend")` clause in `parseArgs`. Accepts `c` or `llvm`.

4. **Driver.cpp: backend dispatch in `buildFile`** - Lines 358-403 become a branch:
 - If `Backend::LLVM`: reject multi-file (error if `depModules` non-empty), create `CodeGenOptions` from `DriverOptions` (optimization level, debug info, runtime lib path via `#ifdef DRAGON_RUNTIME_LIB`), determine output filename, call `CodeGen::generate` → `compileToObject(tmp.o)` → `linkExecutable(output, tmp.o)`, clean up temp `.o`.
 - If `Backend::C`: existing CEmitter path, completely unchanged.

5. **CodeGen.cpp: fix `linkExecutable`** - Replace hardcoded `-L/home/work/.../build -ldragon_runtime` with `impl_->options.runtimeLibPath`. If path provided, link directly with the `.a` file. If not, fallback to `cc obj.o -lm`.

6. **CodeGen.cpp: new PassManager optimization** - Add includes for `llvm/Passes/PassBuilder.h`, `llvm/Analysis/LoopAnalysisManager.h`, `llvm/Analysis/CGSCCPassManager.h`. Add `Impl::runOptimizationPasses` method that uses `PassBuilder::buildPerModuleDefaultPipeline` with O1/O2/O3 levels. Skip at O0. Call before object emission in `compileToObject`. Legacy PM stays for `addPassesToEmitFile` - LLVM requires it for object emission.

7. **CMakeLists.txt: compile definitions** - `target_compile_definitions(dragon_lib PRIVATE DRAGON_RUNTIME_LIB="$<TARGET_FILE:dragon_runtime>")` and same for `dragon` executable. The generator expression resolves to the absolute path of `libdragon_runtime.a` at build time.

8. **CodeGenTest.cpp: Driver integration tests** - Add `DriverLLVM/HelloWorld` and `DriverLLVM/ArithmeticAndFunction` tests that write temp `.dr` files, run Driver with `Backend::LLVM`, execute the binary, and verify stdout.

**What does NOT change:**
- `dragon::initialize` in `src/dragon.cpp` already calls LLVM target init - no change needed.
- `runFile` calls `buildFile` which now dispatches - no special LLVM handling needed.
- Lexer, Parser, Sema, TypeChecker - untouched. Frontend is backend-agnostic.
- All existing tests - unchanged, still passing.

**Done when:** `dragon build --backend llvm hello.dr && ./hello` works for basic programs. `dragon run --backend llvm hello.dr` also works. Optimization levels `-O1` through `-O3` apply LLVM optimization passes.

### Phase 2: Low-Hanging Fruit (Runtime Call Features)

**Goal:** Everything that's "just declare extern + call" works in CodeGen.

These features require no new LLVM patterns - just declaring runtime functions and emitting calls. The bottleneck is that `runtime.cpp` (271 LOC, 29 functions) is missing most functions that `CEmitterRuntime.cpp` (782 LOC) inlines as C string literals. These must be ported to `runtime.cpp` before CodeGen can call them.

**Files modified (3):**

| File | Change |
|------|--------|
| `lib/Runtime/runtime.cpp` | Add ~40 functions: string methods, slice operations, f-string helper |
| `src/CodeGen.cpp` | Complete aug-assign, slice dispatch, string method dispatch, f-string lowering, stdlib imports |
| `test/CodeGenTest.cpp` | ~18 new tests (IR-level + E2E) |

**Detailed changes:**

#### 1. Expand `runtime.cpp` - port functions from CEmitterRuntime.cpp

Replace `dragon_arena_alloc` → `malloc` during porting. Memory leaks accepted for Phase 2; GC/arena deferred.

**String methods returning `const char*` (18 functions):**
- `dragon_str_upper`, `dragon_str_lower`, `dragon_str_strip`, `dragon_str_lstrip`, `dragon_str_rstrip`
- `dragon_str_title`, `dragon_str_capitalize`, `dragon_str_swapcase`, `dragon_str_casefold`
- `dragon_str_replace(s, old, new)`, `dragon_str_repeat(s, n)`
- `dragon_str_removeprefix`, `dragon_str_removesuffix`
- `dragon_str_center(s, w, fill)`, `dragon_str_ljust`, `dragon_str_rjust`, `dragon_str_zfill`, `dragon_str_expandtabs`

**String methods returning `int64_t` (20 functions):**
- `dragon_str_find`, `dragon_str_rfind`, `dragon_str_index_of` (renamed - see note), `dragon_str_rindex`
- `dragon_str_count`, `dragon_str_startswith`, `dragon_str_endswith`, `dragon_str_contains`
- `dragon_str_isdigit`, `dragon_str_isalpha`, `dragon_str_isalnum`, `dragon_str_isspace`
- `dragon_str_isupper`, `dragon_str_islower`, `dragon_str_istitle`, `dragon_str_isascii`
- `dragon_str_isdecimal`, `dragon_str_isnumeric`, `dragon_str_isprintable`, `dragon_str_isidentifier`

**Slice operations (3 functions):**
- `dragon_slice_indices(len, &start, &stop, step)` - helper resolving INT64_MIN sentinels
- `dragon_str_slice(s, start, stop, step)` → new string
- `dragon_list_slice(list, start, stop, step)` → new DragonList*

**String split/join (6 functions):**
- `dragon_str_split`, `dragon_str_join`, `dragon_str_splitlines`
- `dragon_str_partition`, `dragon_str_rpartition`, `dragon_str_rsplit`

**F-string helper (1 function):**
- `dragon_bool_to_str(int64_t)` → "True"/"False" (`dragon_int_to_str` and `dragon_float_to_str` already exist)

**Name collision fix:** `runtime.cpp` already has `dragon_str_index(str, int64_t)` for character-at-position. CEmitterRuntime has `dragon_str_index(str, str)` for substring search (raises ValueError). Different signatures, same name - C linkage cannot overload. **Solution:** Name the substring version `dragon_str_index_of` in runtime.cpp. CodeGen maps `.index` method calls → `dragon_str_index_of`.

#### 2. Complete augmented assignment in CodeGen (`AugAssignStmt` visitor)

Current: handles `+=`, `-=`, `*=`, `/=` only. Add to the switch statement:

| Token | Implementation |
|-------|---------------|
| `DOUBLE_SLASH_EQUAL` | Call `dragon_floordiv_int(current, rhs)` (already declared) |
| `PERCENT_EQUAL` | Call `dragon_mod_int(current, rhs)` (already declared) |
| `POWER_EQUAL` | Call `dragon_pow_int(current, rhs)` (already declared) |
| `AMPERSAND_EQUAL` | `CreateAnd(current, rhs)` |
| `PIPE_EQUAL` | `CreateOr(current, rhs)` |
| `CARET_EQUAL` | `CreateXor(current, rhs)` |
| `LEFT_SHIFT_EQUAL` | `CreateShl(current, rhs)` |
| `RIGHT_SHIFT_EQUAL` | `CreateAShr(current, rhs)` |

Also: when `VarKind::Str` and op is `PLUS_EQUAL`, call `dragon_str_concat` instead of arithmetic add.

#### 3. Slice operations in CodeGen (`SubscriptExpr` visitor)

At top of `SubscriptExpr` visitor, detect `SliceExpr` as the index:
- Evaluate lower/upper/step (use `INT64_MIN` sentinel for omitted bounds)
- Determine list vs string from `VarKind`
- Call `dragon_str_slice` or `dragon_list_slice`
- Add runtime declarations in `declareRuntimeFunctions`

#### 4. String method dispatch in CodeGen (`CallExpr` visitor)

In the `AttributeExpr` branch of `CallExpr`, add string method dispatch before existing list method handling:
- Detect `VarKind::Str` on the object (also check for `StringLiteral` expressions)
- Dispatch by method name to `dragon_str_<method>` runtime calls
- Method categories: no-arg→str (upper, lower, strip...), no-arg→int (isdigit, isalpha...), 1-arg(str)→str (removeprefix, removesuffix), 1-arg(str)→int (find, rfind, count, startswith, endswith), 2-arg→str (replace), int-arg→str (zfill, center, ljust, rjust)
- Map `.index` calls → `dragon_str_index_of`

#### 5. F-string lowering in CodeGen (`StringLiteral` visitor)

When `isFString == true`:
- Scan `value` for `{name}` segments (handle `{{`/`}}` escapes)
- For text segments: `CreateGlobalString`
- For `{name}` segments: look up variable in scope, load value, convert to string based on VarKind/type (`dragon_int_to_str` for i64, `dragon_float_to_str` for f64, `dragon_bool_to_str` for i1, identity for i8ptr)
- Chain all parts with `dragon_str_concat`
- **Phase 2 limitation:** Only simple variable references inside `{}`. Complex expressions (e.g., `{x + 1}`) deferred to Phase 3.

#### 6. Stdlib imports in CodeGen

- Add `symbolAliases` map and `importedModules` set to `CodeGen::Impl`
- Reuse `StdlibRegistry` from `CEmitterStdlib.h` for module resolution
- `ImportStmt` visitor: populate aliases via `registry.resolveImport`
- `FromImportStmt` visitor: populate aliases via `registry.resolveFromImport`
- `CallExpr` visitor: check aliases, declare extern C math functions (e.g., `double sqrt(double)`)
- `AttributeExpr` visitor: handle constants (`math.pi` → `ConstantFP(3.14159...)`, `math.e` → `ConstantFP(2.71828...)`)

#### 7. Tests

**IR-level tests:**
- AugAssign: FloorDiv, Modulo, Power, BitwiseAnd/Or/Xor, LeftShift, RightShift (8 tests)
- Slice: StringSlice, ListSlice (2 tests)
- String methods: Upper, Find, Replace, Startswith, IsDigit (5 tests)
- F-string: FStringSimple (1 test)
- Stdlib: MathSqrt, MathPi (2 tests)

**E2E tests (3):**
- `StringMethods`: upper + lower + find → verify output
- `AugAssignAll`: `//=` + `%=` + `**=` → verify output
- `StringSlice`: `s[0:5]` + `s[6:]` → verify output

**What does NOT change:**
- Lexer, Parser, Sema, TypeChecker - untouched
- CEmitter path - completely unchanged
- All existing tests - still passing
- Phase 1 infrastructure (Driver `--backend`, PassManager, linking) - reused as-is

**Output:** Programs using string methods, stdlib math, slices, f-strings, and all augmented assignment operators compile through `--backend llvm`.

### Phase 3: Control Flow & Collections

**Goal:** All loop forms, dict type, comprehensions, and lambdas work.

**Bottleneck:** `runtime.cpp` has zero dict support. `DragonDict` struct and all `dragon_dict_*` functions must be ported from `CEmitterRuntime.cpp` before CodeGen can use them.

**Files modified (3):**

| File | Change |
|------|--------|
| `lib/Runtime/runtime.cpp` | Add `DragonDict` struct + ~8 dict functions + `dragon_print_dict` |
| `src/CodeGen.cpp` | `VarKind::Dict`, dict visitors, for-in on list/string, comprehensions, lambda |
| `test/CodeGenTest.cpp` | ~11 IR tests + 1 E2E test for Phase 3 features |

**Detailed changes:**

#### 1. Add `VarKind::Dict` + dict runtime functions

**CodeGen.cpp - `Impl` struct:**
- Extend `VarKind` enum: `{ Int, Float, Bool, Str, List, Dict, Other }`
- Add dict detection in `typeExprToKind`: `"dict"` → `VarKind::Dict`

**runtime.cpp - `DragonDict` struct and functions:**

Port from CEmitterRuntime.cpp (arena_alloc → malloc). Struct layout: `{ const char** keys, int64_t* values, int64_t size, int64_t capacity }`.

| Function | Signature | Purpose |
|----------|-----------|---------|
| `dragon_dict_new` | `DragonDict*(int64_t cap)` | Create new dict |
| `dragon_dict_set` | `void(DragonDict*, const char*, int64_t)` | Set/update key-value |
| `dragon_dict_get` | `int64_t(DragonDict*, const char*)` | Get value (KeyError if missing) |
| `dragon_dict_len` | `int64_t(DragonDict*)` | Get size |
| `dragon_dict_has_key` | `int64_t(DragonDict*, const char*)` | Check key existence (1/0) |
| `dragon_dict_get_default` | `int64_t(DragonDict*, const char*, int64_t)` | Get with default |
| `dragon_dict_keys` | `DragonList*(DragonDict*)` | Get list of key pointers |
| `dragon_print_dict` | `void(DragonDict*)` | Print `{key: val, ...}` format |

**CodeGen.cpp - `declareRuntimeFunctions`:** Add extern declarations for all 8 dict functions.

#### 2. Implement `DictExpr` visitor + dict wiring

**`DictExpr` visitor** (replace stub at line ~1563):
- Call `dragon_dict_new(max(entries.size, 4))`
- For each entry: evaluate key/value, convert value to i64 (zext/bitcast/ptrToInt), call `dragon_dict_set`
- Set `lastValue = dict`

**Wire dict into `print`:** Detect `VarKind::Dict` → call `dragon_print_dict`
**Wire dict into `len`:** Detect `VarKind::Dict` → call `dragon_dict_len`
**Wire dict into `SubscriptExpr`:** Detect `VarKind::Dict` → call `dragon_dict_get(dict, key)`
**Wire dict subscript assignment in `AssignStmt`:** `d["x"] = 5` → detect `SubscriptExpr` target on dict → `dragon_dict_set`
**Wire dict methods in `CallExpr`:** `.get(key, default)` → `dragon_dict_get_default`, `.keys` → `dragon_dict_keys`, `.has_key(key)` → `dragon_dict_has_key`

#### 3. Implement for-in on list and string

Replace early `return` for non-range iterables in `ForStmt` visitor.

**List iteration** (VarKind::List): Index loop using `dragon_list_len` / `dragon_list_get`:
```
__iter = evaluate(iterable); __i = 0
loop: if __i >= dragon_list_len(__iter) goto end
 target = dragon_list_get(__iter, __i); <body>; __i++; goto loop
end:
```

**String iteration** (VarKind::Str): Index loop using `dragon_str_len` / `dragon_str_index`:
```
__iter = evaluate(iterable); __i = 0
loop: if __i >= dragon_str_len(__iter) goto end
 target = dragon_str_index(__iter, __i); <body>; __i++; goto loop
end:
```

Detection: check VarKind of iterable (NameExpr lookup / StringLiteral → Str). Default to list for unknown.

#### 4. Implement `ListCompExpr` visitor

Range-only (matching CEmitter). Steps:
1. `dragon_list_new(8)` → temp
2. Detect `range` on iterable, extract start/end/step
3. Generate for-range loop, create alloca for `node.varName`
4. In body: evaluate `node.element`, optionally guard with `node.condition`
5. `dragon_list_append(list, element)` - convert to i64
6. `lastValue = list`

#### 5. Implement `DictCompExpr` visitor

Range-only. Same pattern as ListCompExpr but uses `dragon_dict_new(8)` + `dragon_dict_set(dict, key, value)`. Uses `node.varNames[0]` as loop variable.

#### 6. Implement `LambdaExpr` visitor

Add `int lambdaCounter = 0` to `Impl`.

1. Generate unique name: `__dragon_lambda_N`
2. Determine return/param types from annotations via `typeExprToLLVM`
3. Create `llvm::Function` with `InternalLinkage`
4. Save current function/block state
5. Generate body (expression or block form)
6. Restore state
7. `lastValue = lambda function pointer`

No closure capture - lambda can only reference own params and globals.

#### 7. Phase 3 Tests

**IR-level tests (11):**
- Dict: DictExprEmpty, DictExprEntries, DictSubscriptGet, DictLen, DictPrint
- For-in: ForInList, ForInString
- Comprehensions: ListCompRange, ListCompWithCond, DictCompRange
- Lambda: LambdaSimple

**E2E test (1):**
- `ForInAndListComp`: for-in list + list comprehension → verify output

**What does NOT change:** Lexer, Parser, Sema, TypeChecker, CEmitter - all untouched.

**Output:** Programs with dicts, for-in on list/string, comprehensions, and lambdas compile through `--backend llvm`.

### Phase 4: Classes

**Goal:** Dragon classes compile to LLVM IR with struct types and static method dispatch.

**Files modified (2):**

| File | Change |
|------|--------|
| `src/CodeGen.cpp` | Class infrastructure, ClassDecl visitor, field access, constructor/method dispatch |
| `test/CodeGenTest.cpp` | ~7 IR tests + 2 E2E tests for class features |

**Detailed changes:**

#### 1. Add class infrastructure to `Impl`

```cpp
std::set<std::string> classNames; // Known class names for constructor dispatch
std::string currentClassName; // Set when emitting class methods
std::unordered_map<std::string, llvm::StructType*> classStructTypes; // className → LLVM struct
std::unordered_map<std::string, std::unordered_map<std::string, unsigned>> classFieldIndices; // className → {field → idx}
std::unordered_map<std::string, std::unordered_map<std::string, llvm::Type*>> classFieldTypes; // className → {field → type}
std::unordered_map<std::string, std::string> varClassNames; // varName → className
```

#### 2. Implement `ClassDecl` visitor

Replace stub at line ~2065. Pattern matches CEmitter lines 2088-2287.

**Step 2a: Extract fields from `__init__` body.**
Walk `node.body` to find `FunctionDecl` named `__init__`. Scan its body for `AssignStmt`/`AnnAssignStmt` where target is `AttributeExpr(NameExpr("self"), fieldName)`. Collect field names + infer LLVM types from type annotation (default i64).

**Step 2b: Create LLVM struct type.**
```cpp
auto* structType = llvm::StructType::create(context, fieldTypes, className);
classStructTypes[className] = structType;
// Store field→index and field→type mappings
```

**Step 2c: Emit `ClassName___init__` function.**
Signature: `void @ClassName___init__(ptr %self, params...)`. Skip `self` from Dragon params if `hasImplicitSelf`. `self.field = val` in body → GEP + store.

**Step 2d: Emit `ClassName_new` constructor.**
Signature: `ptr @ClassName_new(params...)`.
- Declare `@malloc(i64) → ptr` extern
- Call `malloc(sizeof(struct))` - compute size from struct type
- Call `ClassName___init__(self, params...)`
- Return self

**Step 2e: Emit regular methods.**
For each non-`__init__` `FunctionDecl`: `RetType @ClassName_methodName(ptr %self, params...)`. Track `currentClassName` during emission for `self.field` resolution.

**Step 2f: Register class name** in `classNames` set.

#### 3. Forward-declare class functions in `generate`

In the first pass (line 400-412), add a scan for `ClassDecl` nodes. Forward-declare `ClassName_new`, `ClassName___init__`, and all methods so that call order doesn't matter.

#### 4. Wire `self.field` access via GEP

**`AttributeExpr` visitor:** If object is `self` (and `currentClassName` set) or a known instance (via `varClassNames`): look up field index → `CreateStructGEP` + `CreateLoad`. For non-field attributes, fall through.

**`AssignStmt` visitor:** When target is `AttributeExpr` with `self` or known instance: evaluate value → `CreateStructGEP` + `CreateStore`.

#### 5. Wire constructor and method calls

**Constructor calls** - in `CallExpr` NameExpr branch, before user function lookup: if name is in `classNames` → call `ClassName_new(args)`. Track result variable in `varClassNames` when part of `AssignStmt`.

**Method calls** - in `CallExpr` AttributeExpr branch, after string/list/dict dispatch: if object is `self` or known instance → call `ClassName_methodName(obj, args)` with obj as first arg.

#### 6. Phase 4 Tests

**IR-level tests (7):**
- ClassDeclStruct: verify struct type + `_new` + `___init__` functions
- ClassDeclMethod: verify `ClassName_methodName` function
- ClassFieldAccess: verify GEP + load
- ClassFieldAssign: verify GEP + store
- ClassConstructorCall: verify call to `ClassName_new`
- ClassMethodCall: verify call to `ClassName_methodName` with obj as first arg
- ClassSelfAccess: verify self.field inside method resolves to GEP

**E2E tests (2):**
- `DictBasic`: `d: dict[str, int] = {"a": 1, "b": 2}; print(d["a"]); print(len(d))` → `1\n2\n`
- `ClassBasic`: Point class with __init__ and sum method → `7\n`

**What does NOT change:** Lexer, Parser, Sema, TypeChecker, CEmitter - all untouched.

**Output:** Programs with classes, constructors, methods, and field access compile through `--backend llvm`.

### Phase 5: Exception Handling

**Goal:** try/except/finally/raise works in LLVM backend using setjmp/longjmp.

**Strategy:** Use `setjmp`/`longjmp` matching CEmitter's approach. Native LLVM `invoke`/`landingpad` deferred to future work.

**Key constraint:** `setjmp` must be called directly from LLVM IR (not wrapped in a runtime function) because it saves the caller's stack frame. The runtime provides `dragon_exc_push_frame` which returns a `jmp_buf*`, then CodeGen calls `@setjmp` on it with `returns_twice` attribute.

**Files modified (3):**

| File | Change |
|------|--------|
| `lib/Runtime/runtime.cpp` | Add `#include <csetjmp>`, exception globals, 5 new functions |
| `src/CodeGen.cpp` | Exception declarations, `excTypeCode` helper, TryStmt visitor, RaiseStmt visitor |
| `test/CodeGenTest.cpp` | ~8 IR tests + 3 E2E tests |

**Detailed changes:**

#### 1. Runtime exception functions (`lib/Runtime/runtime.cpp`)

Add setjmp/longjmp machinery after existing exception section:

```cpp
#include <csetjmp>
#define DRAGON_EXC_STACK_SIZE 32
static jmp_buf __dragon_exc_stack[DRAGON_EXC_STACK_SIZE];
static int __dragon_exc_sp = -1;
static int __dragon_exc_type = 0;
static const char* __dragon_exc_msg = "";
```

Exception type codes (matching CEmitter): `EXCEPTION=1, VALUE=2, TYPE=3, RUNTIME=4, INDEX=5, KEY=6, ZERO_DIV=7`.

Five functions:
- `void* dragon_exc_push_frame` - increment sp, return `&__dragon_exc_stack[sp]`
- `void dragon_exc_pop_frame` - decrement sp (guarded)
- `int64_t dragon_exc_get_type` - return current exception type
- `const char* dragon_exc_get_msg` - return current exception message
- `void dragon_raise_exc(int64_t type, const char* msg)` - set type/msg, `longjmp` if handler active, else `fprintf(stderr) + exit(1)`

Keep existing `dragon_raise(const char*, const char*)` for backward compatibility with non-exception fatal errors.

#### 2. LLVM declarations + Impl additions (`src/CodeGen.cpp`)

**Impl struct:** Add `int excCounter = 0` and `int64_t excTypeCode(const std::string& name)` helper mapping `ValueError→2, TypeError→3, RuntimeError→4, IndexError→5, KeyError→6, ZeroDivisionError→7, default→1`.

**declareRuntimeFunctions:** Add 6 declarations:
- `dragon_exc_push_frame → ptr`
- `dragon_exc_pop_frame → void`
- `dragon_exc_get_type → i64`
- `dragon_exc_get_msg → ptr`
- `dragon_raise_exc(i64, ptr) → void`
- `setjmp(ptr) → i32` with `returns_twice` attribute

#### 3. RaiseStmt visitor

Three forms:
- `raise ValueError("msg")` - detect `CallExpr(NameExpr, args)`, map name to type code, evaluate msg, call `dragon_raise_exc`, emit `unreachable`
- `raise someExpr` - generic exception
- Bare `raise` - `dragon_raise_exc(1, "Exception")` + `unreachable`

#### 4. TryStmt visitor

LLVM IR control flow:
```
push_frame → setjmp → branch(normal/exception)
 try.body: <stmts> → pop_frame → try.else/try.finally/try.end
 try.dispatch: pop_frame → get_type → handler dispatch chain
 handler.N: [get_msg if named] → <stmts> → try.finally/try.end
 unmatched: re-raise via dragon_raise_exc → unreachable
 try.else: <stmts> → try.finally/try.end
 try.finally: <stmts> → try.end
 try.end: continue
```

Handler dispatch: typed handlers use `icmp eq` on exception type code, forming an if/elif chain. Bare `except` (no type) is catch-all - direct branch, no check. Unmatched block re-raises.

#### 5. Tests

**IR-level tests (8):**
- `TryExceptBasic`, `TryExceptTyped`, `TryExceptFinally`, `TryExceptElse`
- `TryMultipleHandlers`, `RaiseValueError`, `RaiseBare`, `TryExceptNamedHandler`

**E2E tests (3):**
- `TryCatchBasic`: raise + catch + print message
- `TryFinallyExec`: handler + finally both run
- `TryCatchElse`: no exception + else runs

**Known limitation:** `return`/`break` inside a try body skips `dragon_exc_pop_frame`, leaving the exception stack pointer off by one. Same limitation as CEmitter. Mitigation deferred.

**Output:** try/except/finally/raise works through LLVM. Programs that catch exceptions compile and run correctly.

### Phase 6: Multi-File & CEmitter Deletion - COMPLETE

**Goal:** LLVM handles everything. CEmitter is deleted. The codebase shrinks by ~4,500 LOC.

**Result:** 10 test suites, 118 CodeGen tests, 7 InteropTests - all passing via LLVM. CEmitter deleted. `--backend` flag accepted but ignored (backward compat).

**Files deleted (5 files, ~4,534 LOC):**

| File | Lines | Purpose |
|------|-------|---------|
| `src/CEmitter.cpp` | 2,305 | C code generation backend |
| `src/CEmitterRuntime.cpp` | 782 | Embedded C runtime as string literals |
| `include/dragon/CEmitter.h` | 95 | CEmitter class interface |
| `include/dragon/CEmitterRuntime.h` | 17 | Runtime generation declaration |
| `test/CEmitterTest.cpp` | 1,335 | 42 CEmitter tests |

**Files renamed (preserved, backend-agnostic):**

| From | To |
|------|----|
| `include/dragon/CEmitterStdlib.h` | `include/dragon/StdlibRegistry.h` |
| `src/CEmitterStdlib.cpp` | `src/StdlibRegistry.cpp` |

**Files modified (6):**

| File | Change |
|------|--------|
| `include/dragon/CodeGen.h` | Add multi-file `generate` overload |
| `src/CodeGen.cpp` | Multi-file generate implementation, helper refactoring |
| `include/dragon/Driver.h` | Remove `Backend` enum and `backend` field |
| `src/Driver.cpp` | Remove CEmitter include, `--backend` parsing, C emission path; make LLVM sole path |
| `test/InteropTest.cpp` | Port from CEmitter to CodeGen for compilation |
| `CMakeLists.txt` + `test/CMakeLists.txt` | Remove CEmitter sources and test target |

**Detailed changes:**

#### 1. Rename CEmitterStdlib → StdlibRegistry

`StdlibRegistry` is backend-agnostic (maps Python stdlib names to C function names). CodeGen.cpp depends on it for `import math` etc. Rename before deletion so no dependency breaks.

- `git mv` both files
- Update include guards: `DRAGON_CEMITTER_STDLIB_H` → `DRAGON_STDLIB_REGISTRY_H`
- Update `#include` in CodeGen.cpp, StdlibRegistry.cpp
- Update `CMakeLists.txt` source list

#### 2. Add multi-file CodeGen support

CEmitter has `emitMultiModule` for multi-file compilation. CodeGen needs equivalent before CEmitter can be deleted. `ModuleResolver` already handles dependency resolution and topological sorting.

**Approach:** Single LLVM module - all dependency modules' functions and classes are forward-declared and emitted into the same module as the entry module.

**`CodeGen.h`** - add overload:
```cpp
bool generate(dragon::Module& entryModule,
 const std::vector<dragon::Module*>& depModules);
```

**`CodeGen.cpp`** - implementation:
1. Extract forward-declaration logic into private helpers: `forwardDeclareFunctions(Module&)` and `forwardDeclareClasses(Module&)`, refactored from existing generate lines 483-553
2. Refactor single-arg `generate` to delegate to multi-file version with empty deps
3. New multi-file generate:
 - Forward-declare functions from ALL modules (deps first, then entry)
 - Forward-declare class functions from ALL modules
 - For each dep: visit FunctionDecl, ClassDecl, ImportStmt, FromImportStmt only (skip top-level expressions - dep modules define functions, entry module runs them)
 - Create `main`, visit entry module statements (all, including top-level code)
 - Verify module
4. Guard against duplicates: check `module->getFunction(name)` before creating declarations

**`Driver.cpp`** - wire multi-file:
- Replace LLVM multi-file error with: `codegen.generate(*module, depModules)`

#### 3. Delete CEmitter & remove Backend flag

**`CMakeLists.txt`:** Remove CEmitter sources from `DRAGON_SOURCES`
**`test/CMakeLists.txt`:** Remove CEmitter test target

**`Driver.h`:**
- Delete `Backend` enum
- Delete `backend` field from `DriverOptions`

**`Driver.cpp`:**
- Delete `#include "dragon/CEmitter.h"`
- Delete `--backend` parsing in `parseArgs`
- Make LLVM path unconditional in `buildFile` (remove `if (backend == LLVM)` guard)
- Delete C emission code block (CEmitter creation, temp C file, `cc` invocation)

#### 4. Port InteropTest to LLVM backend

**`InteropTest.cpp`:**
- Replace `#include "dragon/CEmitter.h"` with `#include "dragon/CodeGen.h"` + LLVM init
- Rewrite `compileAndRun` helper: CEmitter emit → CodeGen generate + compileToObject + linkExecutable
- Delete `EmitMultiModuleProducesValidC` test (CEmitter-specific, tests C output format)
- 7 remaining tests now compile through LLVM backend

**`test/CMakeLists.txt`:** Add `DRAGON_RUNTIME_LIB` compile definition to interop test target

#### 5. Deferred to future work

- **Debug info** - `DIBuilder` metadata mapping LLVM IR to Dragon source locations
- **JIT for `dragon run`** - LLJIT for in-process execution (no temp files)
- **Native LLVM exceptions** - `invoke`/`landingpad` replacing setjmp/longjmp

**Output:** `dragon build file.dr` uses LLVM (no `--backend` flag needed). Multi-file compilation works. CEmitter is gone. Codebase shrinks by ~4,500 LOC.

---

## Migration Strategy

### For the codebase

CEmitter is **deleted at graduation** (Phase 6). During Phases 1-5, both backends coexist so we can verify correctness by comparing output. Once CodeGen passes all tests, CEmitter and its associated files are removed:

- `src/CEmitter.cpp` (~2,155 LOC)
- `src/CEmitterRuntime.cpp` (~673 LOC)
- `src/CEmitterStdlib.cpp` (~140 LOC)
- `include/dragon/CEmitter.h`
- `include/dragon/CEmitterRuntime.h`
- `include/dragon/CEmitterStdlib.h`
- `test/CEmitterTest.cpp` (~164 tests)

That's ~3,000 LOC of dead code and 164 tests that no longer serve a purpose. Good riddance.

### For tests

Each phase ports relevant CEmitter tests to CodeGen. At Phase 6, CEmitter tests are deleted:

| Phase | Tests to port |
|-------|--------------|
| Phase 1 | Basic literals, operators, if/while/for, functions (already exist in CodeGenTests) |
| Phase 2 | String method tests, stdlib tests, f-string tests, aug-assign tests |
| Phase 3 | Dict tests, comprehension tests, lambda tests, for-in tests |
| Phase 4 | Class tests (constructor, methods, field access) |
| Phase 5 | Exception tests (try/except/finally/raise) |
| Phase 6 | Multi-file tests, interop tests - then delete `CEmitterTest.cpp` |

### For the Driver

```
# Current (Phase 0)
dragon build file.dr → CEmitter → cc → binary

# Phase 1
dragon build file.dr → CEmitter → cc → binary (default unchanged)
dragon build --backend llvm file.dr → CodeGen → .o → cc link → binary

# Phase 6 (graduation)
dragon build file.dr → CodeGen → .o → link → binary (only backend)
```

---

## Risk Assessment

| Risk | Likelihood | Impact | Mitigation |
|------|-----------|--------|------------|
| Classes require complex LLVM struct management | High | Medium | Start with flat structs (no inheritance), iterate |
| Exception handling in LLVM is notoriously complex | High | Medium | Use setjmp/longjmp first (proven in CEmitter), native EH later |
| LLVM API changes between versions | Low | Medium | Pin to LLVM 19.x; use stable C++ API subset |
| Performance regression vs CEmitter + `cc -O2` | Low | Low | LLVM's optimization passes are the same ones `clang -O2` uses |
| CodeGen grows too large (monolithic file) | Medium | Low | Split into CodeGenExpr.cpp, CodeGenStmt.cpp, CodeGenDecl.cpp at ~3000 LOC |
| Multi-file linking is complex with LLVM | Medium | Medium | Use `llvm::Linker::linkModules` - well-documented API |

---

## What This Enables

Once LLVM is primary, Dragon gains capabilities impossible with C emission:

1. **JIT compilation** - `dragon run` executes instantly via LLJIT, no temp files
2. **Source-level debugging** - DWARF metadata maps breakpoints to Dragon source lines
3. **Dragon-specific optimizations** - escape analysis for arena allocation, devirtualization for monomorphic calls, string constant interning
4. **Cross-compilation** - target ARM/RISC-V/WebAssembly from any host
5. **Profile-guided optimization** - LLVM's PGO infrastructure works out of the box
6. **Link-time optimization** - LTO across Dragon modules for whole-program optimization
7. **Sanitizers** - AddressSanitizer, UBSan, ThreadSan for Dragon programs via LLVM's sanitizer passes

---

## Decision

1. **Expand `CodeGen.cpp` to feature parity with CEmitter** following the phased plan above.
2. **Add `--backend llvm` flag to the Driver** in Phase 1 (temporary, for transition testing).
3. **Delete CEmitter entirely** after Phase 6 (feature parity + multi-file). Remove all CEmitter source, headers, runtime shims, and tests.
4. **Remove `--backend` flag** after deletion - one backend, no flag needed.
5. **Decision 004 is superseded.** The "freeze CodeGen" and "LLVM text emitter" recommendations are rescinded.
6. **No new features land in CEmitter.** From this point forward, CodeGen is the only development target.

The path: **Expand CodeGen → Wire Driver → Reach Parity → Delete CEmitter.**
