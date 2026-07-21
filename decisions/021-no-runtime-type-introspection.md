# Decision 021: No Runtime Type Introspection - the Static Type System *Is* the Type System

> **Status:** Approved. Slot 021 used to be stdlib dogfooding - recycled for this doctrine.

Dragon has **no runtime type-introspection layer, and never will.** No first-class type objects, no `type(x)` value, no `getattr`/`setattr`/`hasattr`/`dir`/`__dict__`, no metaclasses, no monkeypatching, no `eval`/`exec`. Python has these because a dynamic language throws away type info at runtime and needs machinery to get it back. **Dragon never throws it away**. The static type system already has the answer at compile time. A runtime mechanism to re-ask the question is redundant by construction, not merely slow.

I drew the line after watching someone try to `type()` their way through a hot loop. Features that *recover* type information are cut; features that handle genuinely deferred types (`isinstance` over `Any`/`Union`), control flow (exceptions), or cold-path diagnostics stay. Commandment #3 - familiarity must earn its place - backs this; "Python offers it" isn't an argument for bringing it back.

### Why Python has `type` - and why that reason is absent in Dragon

In a dynamically typed language the **value** is the only place a type lives at runtime; the
binding carries none. So Python provides runtime machinery: `type(x)`,
`x.__class__`, `isinstance`, `getattr`, `dir`, metaclasses. This machinery is *essential* in
Python - dispatch, serialization, frameworks, ORMs are built on it.

Dragon attaches the type to the **binding**, fixed at declaration (; zen.md
"Variable Declarations & Scoping"). The compiler knows the static type of every expression. Canonical uses of `type` go away:

| Python use of `type` | Dragon's static answer |
|---|---|
| Dispatch - `if type(x) == C` | Known at compile time; subtype dispatch is vtables/devirtualization |
| Construction - `type(name, bases, dict)`, metaclasses | Dynamic class synthesis - rejected outright |
| Identity - `type(x) is type(y)` | Meaningless when types are compile-time facts |
| Debug - print what something is | Compile-time-known **name string**, not a runtime object |

`type(x)` asks a question whose answer was sealed at compile time. It doesn't need careful
implementation; it needs **not to exist**. The static type system is Dragon's type-introspection
facility - compile time, not runtime.

### The same logic kills a whole family

`type` is one member of a family whose job is "interrogate a value's type or shape at
runtime." All die for the same reason. Dragon **already enforces this**: undefined-member access
is a *compile error* ("has no attribute") - literally "you may not discover members at runtime."
Cutting `type` is the same posture applied consistently.

### Why this is a #1 (speed) decision too, not only purity

Runtime introspection isn't free to *offer*. A real `type`/`inspect` layer forces the runtime
to keep type metadata reachable from every value, which (a) leaves no room in native `i64`/`f64`/
`i1` without boxing, and (b) defeats devirtualization and dead-code stripping for
**every program, including those that never use it.** Class-(A) whole-program tax
(Watchlist). The doctrine serves commandment #1: fastest emitted code carries no ambient type metadata.

---

## The Test

For any proposed feature, ask: **what question does it answer, and did the compiler already answer
it?** Three outcomes:

1. **The compiler already knows the answer** → redundant by construction → **cut.**
2. **The compiler genuinely cannot know** - type deferred *by design* (`Union`/`Any`
 member), or the fact is a runtime event → **legitimate, keep.**
3. **Not about types at all** (control flow, computation, diagnostics) → orthogonal; judge on its
 own merits (speed, utility).

This test, not a vibe about "staticness," is the arbiter.

---

## Decision

### Cut - runtime type/shape interrogation (outcome 1)

Permanent non-goals - the language design makes them answer an already-answered question:

| Cut | What it would do | Static replacement |
|---|---|---|
| `type(x)` as a value / first-class type objects | runtime type discovery | the binding's static type; dispatch |
| `x.__class__` | same | same |
| `getattr`/`setattr`/`hasattr` (dynamic name), `__getattr__`/`__setattr__` | runtime attribute access by string | static member access (undefined member = compile error) |
| `dir(x)`, `vars(x)`, `x.__dict__` | runtime structure listing | declared type's fields, known at compile time |
| metaclasses, `type(name, bases, dict)`, dynamic subclassing | synthesize classes at runtime | classes are compile-time entities |
| monkeypatching (rebinding methods/attrs at runtime) | mutate dispatch tables | dispatch is fixed ; enables devirtualization |
| `eval` / `exec` of dynamic source | run code built at runtime | - (out of scope) |
| duck-typed structural protocols ("has `.read` ⇒ file-like") | implicit runtime conformance | declared types / explicit interfaces |

### Keep - the deliberate exceptions (outcomes 2 and 3)

So the line is principled, not reflexive:

- **`isinstance(x, T)` - KEEP.** `Union[A, B]` and `Any` are the *one* place Dragon deliberately
 *defers* a type to runtime . Asking at runtime is sound; narrowing keeps it
 static afterward. `isinstance` is the **bounded, type-directed** form of the question `type`
 asks unboundedly. Keep the bounded one, drop the open-ended one. **This is the dividing line.**
- **Exceptions + cold-path tracebacks - KEEP (diagnostics, not types).** Where an error
 happened is control flow + diagnostics, same as C/Rust/Go; no type-introspection
 content. Speed gate: capture must be cold-path / raise-unwind, never hot-path shadow stack.
 Traceback *frame objects* with `f_locals` introspection do **not**
 belong; formatted *string* does.
- **Compile-time reflection - KEEP.** Reflection in the *compiler* that emits plain
 code is fast and welcome (e.g. `@dataclass` emitting ordinary fields). Forbidden is a *runtime mirror*.
 is the static form; this ADR forbids the dynamic form.
- **`enum` / generics - KEEP.** Type-directed, fully static ; no runtime type objects.

### Application to the open backlog (`missing.md`)

- **`type(x)` real type objects - CUT, on principle.** Not deferred - removed from
 scope. Only permissible sliver: `type(x)` returning a compile-time **name string**
 (`"int"`) for debug ergonomics, no runtime type object.
- **`with` / `__exit__` suppression - KEEP the capability, REDESIGN the signature.** Truthy return to suppress is sound control flow. But Python's `__exit__(self, exc_type, exc, tb)`
 passes a **type object** (cut here) and a **traceback object** (cold/optional). Dragon adopts
 **native signature** - `__exit__(exc: Exception?) -> bool` (truthy return suppresses; live exception
 instance passed, statically typed, no type object). Documented divergence from Python (#3 yields to #1/#2 when parity would drag in cut machinery).

Everything else in `missing.md` (list `+`/set ops, `filter`, wrapping decorators, `intc`) passes
the Test as pure static computation.

---

## Options Considered

### Option A - Implement `type` and a reflection layer for Python parity

Maximizes #3 (parity). Rejected: answers a question the compiler already answers
(redundant), and honest implementation carries ambient type metadata taxing **every**
program (#1 violation, Class-A). Parity isn't worth a whole-program speed regression.

### Option B - Block reflection in the stdlib only, stay silent on the language

Pre-existing state: Watchlist blocks `inspect`/`types`/`pickle`, but nothing
records *why* at language level, so every `type`/`getattr` proposal reopens debate.
Rejected: recurring decision with no citable root is a process leak.

### Option C - State the doctrine once, as a language-semantics ADR (chosen)

Record the Test and cut/keep line here; Watchlist and cite it. One citable
"rejected by " closes the family.

---

## What we're giving up permanently

- **Whole family of Python features is a permanent non-goal:** `type`, `__class__`, `getattr`/`setattr`/`hasattr`, `dir`/`vars`/
 `__dict__`, metaclasses, monkeypatching, `eval`/`exec`, duck-typed protocols.
- **`isinstance`, exceptions/cold tracebacks, compile-time reflection, `enum`, generics
 remain** - the line is the Test.
- **'s reflection Watchlist (Class A) is now grounded** - stdlib consequence of this doctrine.
- **`with`/`__exit__` adopts Dragon-native signature** rather than Python's `(exc_type, exc, tb)`.
- **Commandment #3 yields here.** Where Python API would require runtime type
 introspection, Dragon diverges - parity is tiebreaker, never driver.

---

## Motto Check

1. **Speed is king.** No ambient runtime type metadata → devirtualization and native
 values preserved for *every* binary.
2. **Efficiency over quick wins.** One doctrine closes a whole family at the
 root, instead of re-litigating each proposal one workaround at a time.
3. **Python API parity.** Honored where expressible (`isinstance`, `enum`, exceptions). Where parity
 would require cut machinery (`type`, full `__exit__` signature, `inspect`), it **yields**.

Occassionally someone will ask for `getattr` "just for plugins" - same Test, same answer.
