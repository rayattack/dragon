# Decision 005: Dragon REPL

Proposed (revised). Command: `dragon repl`.

Every serious language needs a REPL and Dragon doesn't have one yet, which feels wrong. Python's interactive interpreter is half how people learn the language - try an expression, see the result, iterate. I want that for Dragon: explore without files, debug a function in isolation, sketch logic before committing, all the usual reasons. A language without a REPL feels incomplete no matter how fast `dragon build` is.

```
$ dragon repl
Dragon <version> - The Snake That Became a Dragon
Enter adds a line. Alt+Enter (or Esc, Enter) runs the cell. :help for commands.

>>> x: int = 10
... print(x)
...
... x = x * 5
... x                      # Alt+Enter
10
50
>>> def greet(name: str) -> str {
...     return "Hello, " + name + "!"
... }
... greet("Dragon")        # Alt+Enter
'Hello, Dragon!'
```

One deliberate departure from Python's REPL: the unit of input is a **cell**, not a line. Enter never executes anything; it inserts a newline. Execution is an explicit keystroke. The rationale and the terminal mechanics are in the design section.

Commandment #1 applies: prompt-to-output has to feel instant. Spawning the system linker on every run (100-300ms) is a non-starter when the whole frontend runs in single-digit milliseconds.

## Current Architecture

Dragon's `dragon run file.dr` flow today:

```
source → Lex → Parse → [TypeHintEnforcer (.py)] → Sema → TypeChecker → CodeGen (LLVM IR) → object → cc link → execute
```

The full frontend (Lex → Parse → Sema → TypeChecker) runs in under 5ms. CodeGen lowers to LLVM IR in single-digit milliseconds. The bottleneck is the object emit plus system-linker tail: 100-300ms per invocation, dominated by linker setup.

Everything upstream of the linker is reusable verbatim for a REPL:

- The lexer and parser already carry the mode switches a REPL needs (brace vs indent input, relaxed type requirements).
- Sema and the TypeChecker already support registering symbols from other compilation units, which is exactly the shape of "names defined in earlier turns".
- CodeGen produces an in-memory LLVM module; nothing about it assumes the module is headed for an object file.
- The runtime is a single static archive of C-ABI entry points, the same archive every compiled program links.

One correction from the first draft of this document: the build deliberately links only the AOT slice of LLVM, and the JIT (ORC) components are **not** currently in the component list. Adding them is a one-line build change, but it is a real addition, not a freebie: it grows the compiler binary and its link time. That cost is accepted and priced in below; it buys a REPL with zero new runtime semantics.

---

## Decision

**Implement the REPL as a thin shell over LLVM's ORC JIT (LLJIT), reusing CodeGen verbatim.** Input follows a cell model: Enter edits, an explicit keystroke executes. Each executed cell becomes a new LLVM module added to a persistent JIT instance; cross-turn state (variables, functions, classes) persists as JIT-resident globals and symbols.

Four structural commitments come with this, each covered in the design:

1. **Every turn is transactional.** Each turn's module is registered under its own removable resource tracker, so a turn that fails at any stage (parse, type check, JIT, or a runtime exception mid-execution) is rolled back completely and leaves the session exactly as it was.
2. **Names are versioned, not overwritten.** Rebinding a name (including at a different type) mints a new versioned symbol; the persistent type environment always points at the live version. Redefinition never collides with a dead symbol.
3. **The shell owns a catch-all exception frame.** An uncaught Dragon exception unwinds to the shell, prints, and returns to the prompt. It never exits the process.
4. **REPL globals are module globals.** Top-level variables get the same ownership and sharing semantics as module globals in a compiled program: released on reassignment, never at entry-function exit, and stored through the same write barrier that makes globals safe to share with fired vthreads.

No tree-walking interpreter. No accumulating source buffer. No system linker in the loop.

---

## Options Considered

### Option A: ORC JIT over existing CodeGen (chosen)

Persist a JIT instance for the session. Per input: parse → typecheck → codegen into a fresh module → add to the JIT → look up that turn's entry function → call it. Variables become JIT globals; functions and classes become JIT-resident symbols.

**Good:**
- Native machine-code speed at the prompt (target under 10ms per input including codegen).
- 100% feature parity with `dragon run`, because it is the same CodeGen. No second implementation of any language feature.
- Links the same runtime archive compiled programs link, so REPL behavior and compiled behavior cannot drift at the runtime level either.
- No linker dependency at the prompt, no temp files, no process fork.

**Bad:**
- The JIT components must be added to the LLVM link (binary size and link time; a one-line change, but not free).
- Variable persistence requires an emit-mode toggle in CodeGen (top-level vars as globals rather than stack slots), and that toggle carries real ownership semantics, detailed below.
- Per-turn module management: a long session accumulates modules and versioned symbols. ORC handles thousands of modules; the design keeps the growth inert (dead versions hold no live references).

### Option B: Tree-walking AST interpreter

Build a parallel boxed-value runtime plus an interpreter that evaluates the AST directly.

**Bad, and rejected:**
- **Duplicates the runtime.** Every container, string method, exception path, and concurrency primitive would need a second implementation against interpreter values. The runtime is ~9k LOC; the interpreter would shadow most of it.
- **Drift risk.** Compiled semantics and REPL semantics will diverge - exactly the bug pattern the "no workarounds" rule exists to prevent.
- Violates dogfooding: a parallel interpreter is a workaround for not linking a JIT that LLVM already ships.
- Misses commandment #1: users will feel the gap the moment they paste a real loop.

### Option C: Accumulating C-text + recompile per turn

**Rejected:** predicated on a C-text emitter that no longer exists. CodeGen emits LLVM IR; reviving a C backend just for the REPL forks the entire codegen path, and still pays ~150ms per input.

### Option D: Object file per input + dynamic loader

**Rejected:** object emission alone is 50-100ms and dynamic loading adds more, so it is no faster than the current linker path, while taking on position-independent-code and symbol-versioning quirks the in-process JIT handles for free.

---

## Design

### Session shape

One long-lived session object owns:

- the JIT instance and a per-turn list of resource trackers (for rollback);
- the persistent frontend state: a symbol table, a type environment, and the AST nodes of previously defined functions and classes, so each turn's type check sees everything earlier turns defined;
- the name-version map: for every user-visible name, which versioned JIT symbol is live;
- the cell buffer currently being edited.

### Per-turn module layout

Each executed cell compiles into a fresh module named `repl.N` containing:

1. External declarations for every prior global, function, and class the new input references (the same forward-declare-then-resolve pattern multi-file compilation already uses).
2. Definitions for any new (or rebound) variables, functions, and classes introduced this turn.
3. An entry function for the turn, wrapping the cell's top-level statements. Bare expressions get echo handling (below).

After the module is added, the shell looks up the turn's entry function and calls it through the catch frame.

### Names: rebinding and versioning

Rebinding is an everyday REPL action and must work, including at a different type:

```
>>> x: int = 1
>>> x = "hello"
```

A JIT dylib will not accept two definitions of the same symbol, and the second `x` may not even have the same layout as the first. So user names never map 1:1 onto JIT symbols. Each binding of `x` mints a fresh versioned symbol (conceptually `x@1`, `x@2`, ...), and the persistent type environment records which version is live along with its type. Later turns that mention `x` compile against the live version.

Rebinding releases the old version's value (the ownership rules below), after which the old global is dead metadata: it holds no live reference and costs a few bytes of JIT memory. Nothing scans it, nothing frees it, nothing collides with it.

Whether `x = "hello"` after `x: int = 1` is *allowed* is a language-surface question, not a JIT question: the prompt follows the same typing rules as a file, where a name's type is fixed by its first binding. Re-annotating (`x: str = "hello"`) reads as a deliberate new binding and is accepted; a bare re-assignment at a mismatched type is the same type error a file would produce. The versioning machinery exists so that the *accepted* cases never fight the symbol table.

### Failed turns: rollback

"Errors never corrupt the session" has to hold at every stage, including after code has started running:

```
>>> y: str = load_config()     # raises IOError halfway through
>>> y                          # must be an undefined-variable error, not a null deref
```

By the time the exception fires, the turn's module is already in the JIT and `y`'s global already exists, zero-initialized. If the session forgot the turn but kept the symbol, the next `y: str = ...` would collide with the dead definition; if it kept the binding, `y` would be a null string. Both are wrong.

The fix is transactional: every turn's module is registered under its own removable resource tracker. On failure at any stage - parse, type check, JIT materialization, or a runtime exception during execution - the tracker is removed, which unloads the turn's code and symbols wholesale, and the persistent frontend state is left untouched (it is only committed after the entry function returns cleanly). The session after a failed turn is bit-identical to the session before it.

One subtlety: a turn that partially executed may have already mutated *pre-existing* state (`xs.append(1)` before raising). That is not rolled back, matching Python and every other REPL: executed effects are effects. Rollback guarantees are about the turn's *definitions*, not about undoing its side effects.

### Variable persistence and ownership

Top-level variables in REPL mode are emitted as module-scope globals rather than stack slots in the entry function, behind a single emit-mode flag. Subsequent modules see them as external and the JIT resolves them across modules, the same way multi-file compilation already resolves cross-module globals.

The flag is small; the ownership semantics it drags in are not, and this is where the REPL touches the codebase's most audited territory. Stated explicitly, a REPL global must behave exactly like a module global in a compiled program:

- **Released on reassignment, never at scope exit.** A local's owned value is released when the entry function ends; a global's value must survive the turn and be released only when a later turn rebinds the name (or on `:reset`). The emit mode changes where the release happens, and getting that wrong is a leak or a use-after-free per assignment.
- **Stored through the shared-global write barrier.** A compiled program marks values stored into module globals as shared, so vthreads spawned with `fire` can read them without racing the refcount. A REPL global assigned in turn 1 and read by a vthread fired in turn 4 is exactly that scenario, so REPL globals take the same barrier, not a private variant.
- **Per-name codegen metadata is re-seeded, never accumulated.** CodeGen keeps name-keyed metadata (element kinds, class bindings) that in a single compilation is written once. A REPL compiles many small programs that reuse names. Each turn's CodeGen starts from metadata seeded off the persistent type environment - the one source of truth - rather than inheriting a previous turn's map. Stale name-keyed metadata has caused real miscompiles in this codebase before; the REPL must be structurally unable to reproduce that pattern.

Per this repo's memory-safety policy, the adversarial tests for this emit mode (assign-and-rebind churn, globals captured by fired vthreads, exception mid-assignment) are written and run under ASan **before** the emit mode is adopted, not after. They are the first artifact of that step, not the last.

### Functions, classes, vtables

Functions and classes already emit as module-level symbols; the REPL keeps their AST nodes so later turns type-check against them, and the JIT's symbol table handles linkage. Each class's vtable emits into the module that defines it and later turns reference it externally - the standard cross-module pattern, nothing REPL-specific. Redefining a function or class is the same versioning story as variables: new version becomes live, old one goes inert.

### Exceptions: the shell catch frame

The runtime's unwinding machinery is in-process and per-thread, so exceptions inside JIT'd code propagate exactly as in a compiled program. What a compiled program does with an *uncaught* exception, though, is print and exit - correct for `dragon run`, fatal for a REPL.

So the shell pushes its own catch-all exception frame around every entry-function call, using the same frame push/pop machinery `try` compiles to. An uncaught raise unwinds to the shell's frame (running the same unwind cleanup any handler gets, so owned temporaries on the skipped frames are released, not leaked), the shell prints the error, pops the frame, rolls back the turn's definitions, and prompts again:

```
>>> 1 / 0
DRAGON SCALE ERROR: ZeroDivisionError: integer division by zero
>>> x
DRAGON SCALE ERROR: undefined variable 'x'
>>> x: int = 42
>>> x
42
```

### Runtime linkage

The first draft proposed resolving runtime symbols by searching the compiler's own process image. That does not survive contact with how the binary is built: a normal executable does not export its statically linked symbols for runtime lookup, and the linker only keeps the archive members the compiler itself uses. Making it work would mean exporting and whole-archive-linking the entire runtime into the compiler binary - bloat in service of the wrong mechanism.

The right mechanism is sitting in plain sight: the compiler already knows the path to the runtime archive, because it passes that archive to the system linker for every `dragon build`. ORC can load static archives directly into the JIT's symbol table. So the REPL loads **the same runtime archive compiled programs link**, plus the same bundled dependency archives under the same "only if the program needs them" gating the AOT link uses. Process-level lookup remains only as a fallback for libc/libm, which are genuinely in-process.

This is strictly better than the original plan: no build-flag changes, no binary bloat, and the REPL's runtime is byte-for-byte the archive a compiled program gets, which upgrades "no drift" from a goal to a property of the link.

### Imports

`import math` in a REPL turn resolves through the same compile-time module resolver as a file build, compiles the module's IR into the JIT, and runs its module-level initialization. Two REPL-specific rules:

- **Idempotence.** The session tracks which modules are already resident; a second `import math` (or an import reached transitively twice) neither re-JITs nor re-runs initialization.
- **Initialization is a turn effect.** Module init runs inside the importing turn's catch frame, so a module whose init raises rolls back like any other failed turn.

### Input model: cells, not lines

A deliberate departure from Python's REPL: the unit of input is a **cell**, a small multi-line buffer, and execution is an explicit keystroke. Enter always inserts a newline; nothing runs until the user says run.

```
>>> def fibonacci(n: int) -> int {
...     if n <= 1 {
...         return n
...     }
...     return fibonacci(n - 1) + fibonacci(n - 2)
... }
... fibonacci(10)          # Alt+Enter
55
```

The `>>>` / `...` prefixes are display decoration on the cell's lines, not modes; the whole buffer is one editable unit.

This kills the worst part of line-based REPLs at the root. A line REPL has to guess whether input is complete after every Enter, which is what continuation prompts, depth-tracking classifiers, and blank-line-submits rules exist to approximate - and the guess is wrong exactly where those REPLs hurt most, editing multi-line definitions. With an explicit run keystroke there is nothing to guess: comments, blank lines, and half-written braces are just text until the user runs the cell. The lex-only classifier from the first draft is deleted outright, and `--py` mode needs no special submission rules either, since indentation is just text too.

**The run keystroke: Alt+Enter, one binding everywhere.** The obvious first instinct is Ctrl+Enter, and it has a terminal problem: in a plain terminal, Ctrl+Enter and Enter arrive as the same byte, so most terminals cannot even report the difference without an enhanced keyboard protocol, which would mean protocol detection and a banner that names a different chord per terminal. Alt+Enter has none of that: every terminal and multiplexer delivers it as an escape-prefixed Enter. So Alt+Enter is the one documented run key, implemented as that two-byte sequence - which makes **Esc followed by Enter** the exact same input for free. That equivalence is not a footnote; it is the escape hatch for the two real-world snags: macOS terminals where Option is not configured as Meta (Alt+Enter never reaches the program), and desktops or terminals that grab Alt+Enter for fullscreen before the shell sees it. Esc, Enter cannot be intercepted by anything. One key, one parser, no detection code. If enhanced-keyboard Ctrl+Enter is ever wanted as a courtesy alias, it can be added later without changing anything documented.

**Editing.** The reader is a small multi-line buffer editor: arrows move within the cell, Ctrl+C discards the cell, Ctrl+D on an empty cell exits. Enter auto-indents: the new line copies the previous line's leading whitespace, plus one indent unit when that line opened more `{`/`(`/`[` than it closed, and typing `}` as the first character of a line dedents that line as it is typed. Auto-indent is a hint, never authority: braces carry the structure and indentation stays cosmetic exactly as in a file, so a wrong guess costs one keystroke to fix and can never change meaning. (This is the depth tracker the first draft used for submission guessing, relocated to the one place where guessing wrong is harmless.) History stores whole cells, so recalling a function definition brings back the entire definition, ready to edit and re-run - the single biggest quality-of-life win over line-based REPLs.

**Commands.** A cell whose first non-blank character is `:` is a shell command, not a program. Commands are one-liners and run on plain Enter; they never reach the compiler.

**Statement separators.** None needed. Statements end at newlines, exactly as in a file; since Enter no longer means execute, there is no reason for trailing semicolons.

### REPL commands

| Command | Action |
|---------|--------|
| `:quit` / `:q` | Exit |
| `:reset` | Tear down the JIT and all session state, start fresh |
| `:type <expr>` | Type-check expr and print its inferred type, no execution |
| `:ast <expr>` | Dump AST |
| `:ir` / `:ir <N>` | Dump accumulated LLVM IR / the Nth turn's IR |
| `:save <file.dr>` | Export the session as a valid Dragon source file |
| `:load <file.dr>` | Read and execute a file in this session |
| `:vars` | List bound names with types |
| `:fns` | List defined functions |
| `:time <expr>` | Time the expression's execution |
| `:help` | List commands |

`:reset` has one guard: it must not tear down the JIT while fired vthreads are still running, because destroying the JIT unmaps machine code a live thread may be executing. `:reset` waits for (or refuses under) live vthreads and says so.

### Echo

Top-level bare expressions echo their value, repr-style. A cell can hold several statements, so this needs a rule, and the rule is: **every** top-level expression statement of non-`None`, non-`void` type echoes, in source order. (Jupyter's echo-only-the-last rule was considered and rejected: an expression that silently vanishes mid-cell is a surprise, and echoing in order means a one-expression cell behaves exactly like Python's prompt.)

| Input | Behavior |
|-------|----------|
| `2 + 2` | Print `4` |
| `"hello".upper()` | Print `'HELLO'` |
| `x = 42` | Silent (assignment) |
| `print("hi")` | Print `hi` (explicit print) |
| `[1, 2, 3]` | Print `[1, 2, 3]` |
| `None` literal | Silent (Python parity) |
| `def foo() {...}` | Silent (declaration) |
| class instance | Repr-style, same formatting the runtime already produces for nested values |

Implementation: the entry synthesizer wraps each qualifying top-level expression statement in a repr-printing call dispatched on the static type. The runtime already formats every value repr-style for nested container printing; echo reuses that formatting at the top level rather than inventing a second one.

### Types at the prompt

The prompt follows the file rules: first binding annotated, exactly as the compiler enforces everywhere else.

The first draft proposed relaxing annotations at the prompt and leaning on inference (`x = 42` infers `int`). Rejected on reflection: the same draft's risk table promised "errors identical to `dragon check`", and a mode where a file-mode error is a REPL success is drift by definition, in the one place (the interactive teaching surface) where users form their model of the language. It would also make `:save` emit files that don't compile, or force `:save` to rewrite the user's own input. A REPL that quietly waives the language's rules teaches a different language.

```
>>> x = 42
DRAGON SCALE ERROR: first binding of 'x' needs a type: x: int = 42
>>> x: int = 42
>>> x
42
```

The annotation cost at the prompt is one token, the error is instructive rather than obstructive, and `:save` output compiles by construction. If real-world use shows the annotation requirement genuinely hurts at the prompt, relaxing it is a one-flag experiment *on top of* a working REPL, decided then with usage evidence rather than now with none.

---

## Implementation Plan

### Step 1: Build + driver wiring

- Add the ORC JIT components to the LLVM link.
- Add a `repl` action to the driver and argument parsing; usage text.

### Step 2: JIT bring-up + runtime linkage

- Construct the session's JIT instance.
- Load the runtime archive (and gated dependency archives) into the JIT's symbol table; process-lookup fallback for libc/libm.
- Smoke test: JIT a hand-built module that calls into the runtime.

### Step 3: Cell editor

- Multi-line buffer editor: Enter inserts a line, arrows move within the cell, `>>>` / `...` render as line decoration.
- Auto-indent on Enter (copy leading whitespace, one extra unit after a net-opening line; a leading `}` dedents live); a hint only, never structure.
- Run keystroke: the escape-prefixed-Enter sequence, so Alt+Enter and Esc, Enter are one code path; no protocol detection.
- Command cells (leading `:`) dispatch on plain Enter; Ctrl+C discards the cell, Ctrl+D on an empty cell exits.

### Step 4: CodeGen REPL emit mode (tests first)

- **First artifact: the adversarial ASan test suite** for the ownership semantics - assign/rebind churn across turns, globals captured by fired vthreads, exception mid-assignment, container globals mutated across turns. Written and running (red) before the emit mode lands.
- Emit-mode flag: top-level vars as globals; external references for names from prior turns; versioned symbol naming.
- Globals released on reassignment, never at entry-function exit; stores go through the shared-global write barrier.
- Per-turn entry-function synthesis.
- Per-name codegen metadata seeded from the persistent type environment each turn.

### Step 5: Per-turn pipeline, rollback, catch frame

- Parse → typecheck against persistent state → codegen → add module under a fresh resource tracker → call entry through the shell's catch frame.
- Commit persistent state only on clean return; remove the tracker on any failure.
- Uncaught-exception path: print, pop frame, roll back, re-prompt.
- Import residency tracking.

### Step 6: Echo

- Per-expression echo in the entry synthesizer (every qualifying top-level expression, in order); repr-print dispatch on static type, reusing the runtime's existing repr formatting.

### Step 7: Polish

- Cell history: whole cells as history entries, persisted to `~/.dragon_history`; up-arrow at the top of a cell recalls the previous one.
- `:vars`, `:fns`, `:type`, `:ir`, `:save`, `:load`, `:time` implementations.

### Step 8: Tests (beyond Step 4's suite)

| Test | Validates |
|------|-----------|
| Expression echo per type | Bare `2 + 2` prints `4`; strings quote; `None` silent |
| Echo order | A cell with several bare expressions echoes each, in source order |
| Variable persistence | `x: int = 1` then `x + 1` → `2` |
| Rebinding | Re-annotated rebind works; mismatched bare re-assignment errors; no symbol collision either way |
| Failed-turn rollback | A turn that raises mid-execution leaves no binding, no symbol, no leak; the name is reusable |
| Uncaught exception | Raise at the prompt prints and returns to a working prompt |
| Function/class definition + use across turns | Including vtable dispatch through a dynamically bound class |
| Cell editing | Enter never executes; the run keystroke does; Esc, Enter equals Alt+Enter; Ctrl+C discards; command cells run on Enter |
| REPL commands | `:quit`, `:reset`, `:type`, `:vars`, `:ir`, `:save` round-trip (saved file compiles) |
| Import | `import math` then use; repeated import is a no-op; failing module init rolls back |
| Concurrency | `fire f(x)` where `x` is a REPL global; `:reset` under a live vthread is guarded |

The harness drives the session object directly and captures stdout per turn. The whole suite also runs under ASan/LSan in CI, since half of what the REPL exercises is ownership across turn boundaries.

---

## Risks

| Risk | Mitigation |
|------|------------|
| Per-turn module and dead-version accumulation over long sessions | Dead versions hold no live references (rebind releases the value) and cost bytes; ORC scales to thousands of modules. If lookup cost ever shows up, coalesce or prune on `:reset` |
| Emit-mode globals diverge from compiled-program global semantics | One emit path, exercised by running identical source as a file and as a session and comparing observable behavior; the Step 4 ASan suite is written before the emit mode lands |
| Rollback misses a stage (state committed before execution finishes) | Single commit point after clean entry return; everything before it lives under the turn's resource tracker; the failed-turn tests assert bit-identical session state |
| Shared-global barrier missed for REPL globals → vthread refcount race | REPL globals compile through the same store path as module globals, not a REPL-specific one; the fired-vthread test runs under ASan |
| A runtime symbol the JIT can't resolve | The archive loaded is the same one the AOT link uses, so any gap is a pre-existing AOT gap; the JIT reports the missing symbol by name at first use, no fallback |
| Uncaught exception leaks the skipped frames' temporaries | The shell's catch frame runs the same unwind cleanup as any handler; covered under LSan |
| `:reset` while fired vthreads still run JIT'd code | Guarded: join or refuse with a message, never unmap live code |
| Alt+Enter grabbed by the desktop or terminal (fullscreen bindings) before the shell sees it | Esc, Enter is the same byte sequence and cannot be intercepted; the banner names both |
| LLVM error during module add corrupts the shell | Errors surface as diagnostics; the turn rolls back; on a hard JIT error, offer `:reset` |

---

## Why One Phase

The original draft proposed three phases (accumulating C-text → tree-walking interpreter → LLVM JIT) because at the time CodeGen targeted C source text and a JIT looked blocked on feature parity. Both premises are obsolete: CodeGen targets LLVM IR directly, and the runtime ABI the JIT would consume is the stable, shipping ABI every compiled program already uses. The only remaining gap is linking LLVM's JIT components, which is a build-list change, not a phase.

So: no transitional phase 1, no parallel phase 2 interpreter. The JIT approach is the only one that satisfies commandment #1 (native speed), commandment #2 (no parallel runtime to drift against compiled output), and the dogfooding policy (reuse, don't reimplement). People occasionally ask for the C-text REPL anyway - still no.

---

## Effort Summary

| Step | Effort | Notes |
|------|--------|-------|
| 1: Build + driver wiring | 0.5 day | Mechanical |
| 2: JIT bring-up + archive linkage | 1.5 days | Standard ORC patterns |
| 3: Cell editor + run keystroke | 2.5 days | Multi-line buffer editing; no classifier to build |
| 4: Emit mode + ownership (tests first) | 4-5 days | The substantive piece; the ASan suite is written before the emit mode |
| 5: Per-turn pipeline, rollback, catch frame | 3 days | Resource trackers, versioning, commit point, exception path |
| 6: Echo | 1 day | Reuses existing repr formatting |
| 7: Cell history + commands | 2 days | Polish, deferrable |
| 8: Remaining tests | 1.5 days | On top of Step 4's suite |
| **Total** | **~3 weeks** | One engineer, no blocking dependencies |

Latency budget: parse + typecheck (~3ms) + codegen (~2ms) + JIT compile of one small module (~1-3ms) + execute = **under 10ms typical**, well below the 50ms perceptual threshold and dramatically below Python's REPL.

---

## Recommendation

Ship it. The pipeline reuse is real - this is driver glue, one carefully audited emit mode, and a transactional turn loop around components that already exist. The two places that deserve the most care are exactly the two this revision redesigned: the ownership semantics of cross-turn globals (Step 4, tests first) and turn rollback (Step 5). Everything else is mechanical.
