# Decision 004: C Emission vs LLVM Backend Strategy [Superseded]

Superseded. See [008-llvm-graduation.md](008-llvm-graduation.md). We eventually graduated off C emission; this doc is why I didn't do that sooner.

People keep asking "why are we emitting C? I thought the goal was LLVM." I spent a stupid afternoon writing this so I'd stop re-explaining it in every PR comment and Slack thread. Fair question. Embarrassing answer.

At the time Dragon had two backends and one of them was clearly winning despite not being the plan. CEmitter was ~3k lines and covered maybe 85% of the language. CodeGen was ~1.6k lines and covered maybe 35-40%. CEmitter wasn't supposed to be primary. It started as a quick prototyping thing, print some C strings, call `cc`, go home. Then I needed classes, then exceptions, then dict comprehensions, and every time I tried the same feature in LLVM I'd lose half a day on phi nodes and basic blocks while the C path was basically "add 50 lines of string concat."

The other stuff matters too and I'm not gonna pretend it doesn't: gcc/clang give you `-O2` for free, `-g` debug info for free, any platform with a C compiler for free. Exceptions via setjmp/longjmp are fragile and I hate them, but LLVM landing pads are worse when you're just trying to ship try/except. Stdlib integration is `#include <math.h>` instead of declaring externs and wrestling calling conventions. None of this is elegant. All of it worked while LLVM didn't, and syntax parity was already painful enough without implementing every feature twice.

So yeah. C emission "won" because the bootstrap hack outran the real backend. The tables below are the receipts from when I actually measured LOC and feature gaps. The recommendation at the bottom was keep CEmitter, freeze CodeGen, maybe do LLVM IR text emission later, full C++ API at v1.0. We flipped that eventually (008) but the grumpy math on *why* C dominated for so long still holds.

## Current State

Dragon has two backends:

| | CEmitter (Primary) | LLVM CodeGen (Secondary) |
|---|---|---|
| **LOC** | 2,155 + 673 + 140 = **2,968** | **1,608** |
| **Feature coverage** | ~85% of Dragon language | ~35-40% |
| **Files** | CEmitter.cpp, CEmitterRuntime.cpp, CEmitterStdlib.cpp | CodeGen.cpp |
| **Output** | C source code → `cc` → native binary | LLVM IR → LLVM backend → object file |

### What CEmitter Supports That CodeGen Doesn't

| Feature | CEmitter | CodeGen |
|---------|----------|---------|
| Classes (struct + methods + constructor) | done | no |
| Exception handling (try/except/finally) | done | no |
| Dict type + operations | done | no |
| List comprehensions | done | no |
| Dict comprehensions | done | no |
| Lambda functions | done | no |
| String methods (38 of them) | done | no |
| F-strings | done | no |
| Stdlib module imports | done (5 modules) | no |
| Multi-file compilation | done | no |
| Augmented assignment operators | done | no |
| Slice operations | done | no |

### What CodeGen Supports

| Feature | Status |
|---------|--------|
| Integer/float/bool/string literals | done |
| Binary/unary operators | done |
| If/elif/else | done |
| While loops | done |
| For-range loops | done |
| Functions (def, call, return) | done |
| Print (type-dispatched) | done |
| Variables (local, nested scopes) | done |
| List basics (new, append, get, len) | partial |
| Break/continue | done |
| Comparison operators | done |
| Logical operators (and/or/not) | done |

### CodeGen Technical Debt

1. **Uses Legacy PassManager** - deprecated in LLVM 17+, will be removed
2. **No optimization passes configured** - generates unoptimized IR
3. **Requires separate `libdragon_runtime.a`** - must be pre-compiled and linked
4. **No debug info** - LLVM's debug metadata not used
5. **No string interning or constant pooling**
6. **No struct/class support at all**

---

## Why C Emission Became Primary

Yeah I know. CEmitter wasn't supposed to win. It was a quick prototyping tool and then it ate the project because:

1. **Rapid iteraton** - emitting C strings is much simpler than constructing LLVM IR. Adding a new feature to CEmitter takes ~50 lines; the same in CodeGen takes ~200+ lines with type management, basic block handling, and phi nodes.

2. **Free optimizaton** - the system C compiler (`gcc`/`clang -O2`) provides decent `-O2` optimization for free. CEmitter doesn't need to implement any optimization passes.

3. **Free debuggability** - `cc -g` produces debug info. Users can use `gdb`/`lldb` on the intermediate C file. The generated C is readable.

4. **Free portability** - any platform with a C compiler can run Dragon. No LLVM dependency needed at runtime.

5. **Stdlib integration** - C-shimming Python stdlib functions is trivial (wrap `<math.h>`, `<string.h>`, etc.). In LLVM, each shim requires declaring external functions, managing calling conventions, and handling platform differences.

6. **Exception handling** - `setjmp`/`longjmp` is a well-understood C pattern. LLVM exception handling (landing pads, personality functions) is notoriously complex and platform-dependent.

---

## Pros and Cons

### C Emission

| Good | Bad |
|------|------|
| done Fast to develop (string concatenation) | no No control over code generation quality |
| done Free optimization via `cc -O2` | no Extra compilation step (Dragon → C → binary) |
| done Readable intermediate representation | no C compilation adds latency (~100-300ms per file) |
| done Any C compiler works (gcc, clang, tcc, MSVC) | no Cannot do Dragon-specific optimizations (e.g., escape analysis, devirtualization) |
| done Free debug info via `cc -g` | no Debug info refers to generated C, not Dragon source |
| done Easy stdlib integration (C headers) | no Generated C can be large and repetitive |
| done No LLVM dependency (smaller binary, simpler build) | no Cannot generate machine code directly |
| done setjmp/longjmp for exceptions is simple | no setjmp/longjmp is fragile and breaks some optimizations |

### LLVM CodeGen

| Good | Bad |
|------|------|
| done Direct machine code (no intermediate step) | no ~2x more code per feature vs CEmitter |
| done LLVM optimization passes (inlining, LICM, GVN, etc.) | no Must configure pass pipeline manually |
| done Source-level debug info (DWARF/CodeView) | no Complex debug metadata API |
| done JIT compilation possible (LLJIT) | no Large dependency (~200MB LLVM libraries) |
| done Dragon-specific optimizations possible | no Platform-specific target machine setup |
| done Professional compiler infrastructure | no LLVM API changes between versions |
| done Can target any LLVM backend (x86, ARM, RISC-V, WASM) | no Must handle calling conventions per platform |
| done Proper exception handling (invoke/landingpad) | no Exception handling is very complex in LLVM |

### Alternative: Emit LLVM IR Text (Hybrid)

A middle ground: emit `.ll` (LLVM IR text) instead of C, then feed it to `llc`/`opt`:

| Good | Bad |
|------|------|
| done Same simplicity as C emission (string concat) | no LLVM IR is less readable than C |
| done Access to LLVM optimization passes via `opt` | no Still requires LLVM toolchain installed |
| done Can target any LLVM backend | no LLVM IR text is verbose |
| done No LLVM C++ API dependency in Dragon binary | no Error messages from `llc` are cryptic |
| done Smaller Dragon binary | no Cannot do JIT |

---

## Recommendation

### Short term (v0.2-v0.4): Keep CEmitter

CEmitter is ~2.4x ahead on features. New stuff goes there first. Why:

1. **Syntax parity** ([002-syntax-parity.md](002-syntax-parity.md)) requires parser + CEmitter work. Duplicating to CodeGen doubles the effort for nothing.
2. **Memory management** ([003-memory-management.md](003-memory-management.md)) is a runtime concern that must work in C first.
3. **Object model** (dunders, inheritance, protocols) will be implemented in C first, then ported.
4. CEmitter produces correct, fast binaries today. Users don't care whether the backend is C or LLVM - they care that it works.

### Medium Term (v0.5): LLVM IR Text Emission

Instead of building up the C++ LLVM API CodeGen, emit LLVM IR as text (`.ll` files):

1. Create `LLVMTextEmitter` - same visitor pattern as CEmitter but emits LLVM IR text
2. Reuse the C runtime by compiling it to `.bc` and linking: `llvm-link dragon_runtime.bc program.bc -o out.bc`
3. Use `opt -O2` for optimization, `llc` for code generation
4. This gives us LLVM's optimization power without the LLVM C++ API complexity

**Note:** CEmitter is just string-concatenating C. An LLVM text emitter would be the same idea, different target syntax. Comparable effort, LLVM passes for free.

### Long Term (v1.0+): Full LLVM C++ API CodeGen

Once the language is stable and the feature set complete, invest in a proper LLVM CodeGen:

1. Port features from LLVMTextEmitter to use LLVM C++ API for in-process compilation
2. Enable JIT compilation for `dragon run` (instant execution, no temp files)
3. Add Dragon-specific optimization passes:
 - String constant pooling and interning
 - Escape analysis for arena allocation decisions
 - Devirtualization for monomorphic call sites
 - Loop-invariant allocation hoisting
4. This is a v1.0 effort - don't invest until the language semantics are frozen

### Action on Existing CodeGen

The current `CodeGen.cpp` (1,608 LOC) should be **maintained but not actively expanded**. It serves as:
- A working proof-of-concept that Dragon → LLVM IR works
- A reference for the future LLVMTextEmitter
- A test target for LLVM integration tests

Do NOT delete it. Do NOT invest feature development time in it until the CEmitter has achieved feature-completeness.

---

## Comparison with Other Language Compilers

| Language | Backend Strategy | Notes |
|----------|-----------------|-------|
| **Zig** | Custom LLVM API codegen + C backend | C backend for bootstrapping, LLVM for optimized builds |
| **Nim** | C emission (primary) + JS backend | C emission is the default; works on any platform with a C compiler |
| **Crystal** | LLVM API codegen | Requires LLVM; slower compilation |
| **V** | C emission (primary) | Very fast compilation; uses system C compiler for optimization |
| **Cython** | C emission | Python superset → C → native. Same architecture as Dragon CEmitter |
| **Rust** | LLVM API codegen | Requires LLVM; slow compilation is a known pain point |
| **Go** | Custom codegen | Own backend; no LLVM dependency; fast compilation |
| **Mojo** | MLIR/LLVM | Built on LLVM infrastructure from day one |

**Dragon's CEmitter approach is the same as Nim, V, and Cython** - all successful languages that emit C as their primary backend. This is a proven strategy.

---

## Build Time Analysis

| Step | Time | Notes |
|------|------|-------|
| Dragon compile (AST → C) | ~5-15ms | Very fast (string concatenation) |
| C compile (`cc -O2`) | ~100-300ms per file | Bottleneck, but gcc/clang are well-optimized |
| LLVM compile (AST → IR → object) | ~200-500ms per file | Comparable or slower than C compile |
| LLVM JIT (future) | ~50-100ms per file | Fastest for `dragon run` (no temp files) |

For `dragon run` (development), compilation speed matters. For `dragon build` (release), optimization quality matters. The CEmitter serves both adequately today, and LLVM JIT would improve the dev experience later.

---

## Decision Summary

1. **CEmitter remains primary backend** for all new feature development
2. **CodeGen.cpp is frozen** - maintained but not expanded
3. **Future LLVM work** takes the form of LLVM IR text emission (`.ll` files), not C++ API
4. **LLVM C++ API CodeGen** is a v1.0 project, after language semantics stabilize
5. **No backend should block syntax parity** - parser work is backend-independent

Plan: **CEmitter → LLVMTextEmitter → Full LLVM CodeGen**. Each step builds on the previous. Don't skip ahead.
