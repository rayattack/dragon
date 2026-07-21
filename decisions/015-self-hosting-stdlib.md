# Decision 015: Self-Hosting the Compiler - Writing Dragon's Frontend in Dragon

> **Status:** Proposed

This ADR used to be the *stdlib* self-hosting roadmap (four-layer model, module catalogs, what stays in C). That library sense now lives elsewhere. I repurposed this slot for the *strict* sense: **writing Dragon's compiler in Dragon.** Two different "self-hosting" words - don't mix them up.

I want to rewrite the compiler **frontend** - lexer, parser, Sema, type checker - in `.dr`, stage by stage, diffing against the C++ frontend as oracle. LLVM backend stays. Runtime stays C++. Makes the compiler the ultimate dogfooding target so contributors can hack the frontend without touching C++. Readiness audit says the language is already expressive enough; `stdlib/drs.dr` is the proof I keep pointing at when someone asks "can Dragon parse Dragon yet?"

### "Self-hosting" is overloaded. This is the compiler sense

| Sense | Meaning | Home |
|---|---|---|
| **Stdlib dogfooding** | the *library* shipping with Dragon is written in `.dr` over a C runtime | (rollout), (boundary) |
| **Compiler bootstrapping** | the *compiler* that translates `.dr` is itself written in `.dr` | **this ADR** |

That's the Rust/Go/Zig/TypeScript milestone. Orthogonal to two other axes people conflate:

### Three axes (kept separate on purpose)

1. **What language is the compiler written in?** → *this ADR* (move `src/*.cpp` frontend to `.dr`).
2. **What backend lowers Dragon to machine code?** → **LLVM, kept** . Hand-rolled backend (Go model) forfeits commandment #1. LLVM is bundled and invisible. **Out of scope.**
3. **What links the final binary?** → toolchain/`cc` question; separate concern.

Self-hosting the frontend (axis 1) doesn't remove toolchain deps by itself - still calls LLVM, still links. Rust is self-hosted and still shells out to the linker. I'm doing axis 1 for dogfooding and contributor ergonomics, not to shed LLVM.

### Why now - evidence is already in the repo

A compiler frontend is a character scanner + tree builder + symbol table + type walker. All demonstrated in shipping `.dr`:

| Frontend need | Already done in Dragon |
|---|---|
| Tokenizer with a token-kind set | `stdlib/drs.dr` (1649 lines) - `TK_EOF`/`TK_LBRACE`/… + char scanner + `_tk(tokens,pos)` peek + precedence climbing |
| Recursive-descent parser | `stdlib/json.dr` (819 lines) - `_skip_ws`, `_parse_string_value`, `_find_number_end`, `detect_type` |
| Enum-like tag sets | `const X: int = N` idiom (`TK_*`, `JSON_NULL=0…`) |
| AST as class hierarchy + dispatch | classes + inheritance + `isinstance` (58 stdlib call sites) + virtual dispatch |
| Symbol tables | `dict[str, T]` |
| Error reporting | user-defined exceptions + try/except |
| Reading source | `io` / `fileio` |

18,904 lines of `.dr` stdlib (crypto, TLS, HTTP server, Postgres driver) prove the language carries programs harder than a frontend. **No missing language feature blocks the start.** Dragon lexer = `drs.dr` tokenizer with a bigger table; Dragon parser = `json.dr` scaled up.

---

## Options Considered

### Option A - Leave the frontend in C++ permanently

- **Pro:** zero work.
- **Con:** frontend is user-space business logic (string scanning, tree building) that / say belongs in `.dr`. Keeping it C++ contradicts dogfooding with no speed justification a benchmark could defend.

### Option B - Big-bang rewrite (all stages at once, flip over)

- **Pro:** done in one cut.
- **Con:** no oracle during rewrite, no incremental validation, huge merge risk. Rejected on commandment #2.

### Option C - Incremental stage-by-stage bootstrap with differential testing (**chosen**)

Build each frontend stage in `.dr` as a *parallel* implementation, validate against C++ on the real corpus before moving on, lexer first. C++ frontend is the oracle the whole time. No stage ships until it (a) matches oracle output and (b) passes the speed gate.

- **Pro:** every stage proven correct on real code; no big-bang; speed gated at each step.
- **Con:** two frontends coexist during transition; stage0 seed maintained forever. Normal cost of self-hosting.

---

## Decision

Adopt **Option C**. Concretely:

1. **Bootstrap seed (stage0).** Current C++ `dragon` is stage0 - compiles the `.dr` frontend. Self-hosted languages always keep a seed (Rust ships stage0, Go keeps a bootstrap chain). Stage0 never deleted.

2. **Stage order:** `lexer.dr` → `parser.dr` → `sema.dr` → `typechecker.dr`. **Codegen stays C++ longest** - LLVM-IR-binding layer . If ever moved, LLVM-C API over FFI, final optional step.

3. **Differential testing is the correctness proof.** C++ Driver exposes `--dump-tokens` and `--dump-ast`. For each stage, run *both* over `stdlib/` (18.9k lines) + test corpus and **diff**; empty diff = correct. Gate to start next stage.

4. **Speed gate (commandment #1).** Each `.dr` stage benchmarked wall-clock vs C++ over the corpus. **Lexer is the canary** - per-character byte loop, same shape measured at 6-30× C for byte work. Mitigant: frontend runs **once per build over KB-MB of source**, not in a hot loop, so small constant factor OK where it wouldn't be in scrypt. **But measured, not assumed:** if lexer regresses badly, builder primitive lands first before `lexer.dr` ships.

5. **Integration model = parallel/standalone, not mid-pipeline splice.** Stages built standalone, validated alongside C++, flipped in groups - *not* spliced mid-pipeline (marshalling Dragon AST ↔ C++ AST mid-stream is harder than writing the next stage). Final state: whole frontend is `.dr`, typed AST to codegen, LLVM via C API.

### Prerequisites to de-risk (small, not blockers)

- **`enum` ergonomics.** Token/AST kinds work via `const int` (proven in `drs.dr`); native `enum` would be nicer. Phase 2 - land before `lexer.dr` if cheap; else `const int` fallback.
- **`match` class-patterns.** Helpful for AST walking; if weak, `isinstance` (58 sites) or virtual dispatch (visitor pattern C++ already uses).

---

## Implementation Phases

| Phase | Deliverable | Validation |
|---|---|---|
| **1 - Lexer** | `lexer.dr` mirroring C++ token set + diff harness | `--dump-tokens` diff over corpus == empty; wall-clock vs C++ within gate |
| **2 - Parser** | `parser.dr` + AST node hierarchy in `.dr` | `--dump-ast` diff == empty |
| **3 - Sema** | `sema.dr` (name resolution, scopes) | symbol-resolution diff / error-parity over corpus |
| **4 - TypeChecker** | `typechecker.dr` | type-error parity (same diagnostics on same inputs) |
| **5 - Codegen (optional, long-horizon)** | frontend hands AST to codegen; codegen → LLVM via C-API FFI | full E2E: self-built compiler reproduces stage0's binaries |
| **Fixpoint** | stage1 (Dragon frontend, built by stage0) compiles itself → stage2; `stage1 == stage2` | byte-identical reproduction = self-hosted |

Phase 1 alone derisks the effort: proves correctness (diff) **and** viability (speed) before a second file.

---

## Long-term trade-offs

### Positive
- Compiler becomes largest dogfooding program - catches codegen/typechecker bugs synthetic tests miss.
- Contributors hack frontend in `.dr` instead of C++.
- Differential testing keeps transition low-risk.

### Negative
- Two frontends coexist during transition (sync cost).
- Stage0 seed maintained indefinitely.
- Lexer byte-loop may force builder primitive first (sequencing dependency).

### Neutral
- LLVM and C++ runtime unaffected - frontend-language change only. Axes 2 and 3 out of scope.

---

## Motto Check

1. **Speed is king.** Every stage passes wall-clock gate vs C++; lexer is measured canary; byte-builder blocks lexer if gate fails.
2. **Efficiency over quick wins.** Incremental bootstrap, not reckless big-bang; not shipping slow `.dr` frontend to claim the milestone early.
3. **Python API parity.** Orthogonal - frontend *output* (tokens/AST/types) validated to match existing compiler; parity of the language it compiles unchanged.

---

## Open Questions

- **Codegen handoff (Phase 5):** serialized typed-AST vs in-memory FFI vs full LLVM-C from `.dr` - resolve when Phases 1-4 proven.
- **AST representation in `.dr`:** one class hierarchy + visitor (matches C++) vs tagged-union/`enum`-driven node - depends on `enum`/`match` outcome.
- **Diagnostic byte-parity:** must `.dr`-stage error messages match C++ character-for-character, or only semantically? Leaning semantic, with DiagnosticFormatter shared/ported. Occassionally we'll want byte-parity for diff tooling - TBD.
