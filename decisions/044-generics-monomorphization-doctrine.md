# Decision 044: Generics - Monomorphize by Default, Erase When Uniform, Box Only Heterogeneous

Done. v1 implemented (see Implementation note below). How should a speed-first static language do generics? "Just use `Any`" (Go pre-1.18 `interface{}`, Java raw `Object`) is the wrong default: it brings back exactly the boxing cost the native value model was written to kill.

v1 ships `def f[T]` in both `.dr` and `.py`, the monomorphization engine (`AstClone` deep-clone + substitution, `TypeCheckerGenerics` worklist at end of check), and native codegen of stamped instantiations - `Box[int]` lowers to `i64`, `Box[str]` to `ptr`, zero boxing. Generic methods with their own type param (`def m[T]`) work via double monomorphization on generic classes. Bounded `T: B` landed later without a separate ADR. Still deferred cleanly: subclassing a generic instantiation, cross-module generics.

This doc covers the doctrine (which mechanism, why) and the engine spec (parser/Sema/TypeChecker/codegen, worklist, mangling, interaction matrix, v1 bounds, tests).

Default rule:

> **Monomorphize by default. Erase to `ptr` only when the type parameter is provably
> representation-uniform _and the generic never owns or refcounts a `T`-typed value_. Box
> (`Any`/`Union`) only genuinely heterogeneous runtime data.**

The ownership bit on erasure matters: an *owning* generic erased to `ptr` is a miscompile
(leak/double-free), not a missed optimization. See soundness note below.

The buildable consquence: a **monomorphization (instantiation) engine** - a new compiler pass
that stamps a concrete, native-typed copy of each user-defined generic per distinct type
argument, then feeds those copies into the existing class/function machinery. This is the same
strategy [[]] applied *by hand* to `list`/`dict`; makes it *programmable* for user code
(`class Stack[T]`, `def first[T](xs: list[T]) -> T`). It introduces **no new runtime
representation**: after substitution, an instantiation is an ordinary monomorphic class/function
and reuses every existing codegen, refcount, vtable, and box path unchanged.

Take Python's generic *syntax* (PEP 695 `class Foo[T]`, `def f[T]`). Reject Python's erasure semantics. Python erases type parameters because every
value is already a boxed `PyObject*` - there is no representation to specialize. Dragon's entire
reason for existing is native, unboxed values, so erasure-to-box would be a #1
regression. **Take Python's syntax; take C++/Rust's implementation.**

**v1 scope is unbounded `T` only** (no `T: Bound`): inside a generic body a `T` value may be
stored, passed, returned, compared, and placed in containers, but **methods/operators may not be
called on it** (the checker can't prove they exist for every `T`). This covers the motivating
container case (`Stack`, `Queue`, `Pair`, `Result`, `Cache`) - exactly what built-in `list`/`dict`
already prove useful - and is fully sound. Bounds shipped later .

## Context / Motivation

Every statically-typed language must answer "how do I write a container/algorithm that works
over many element types without rewriting it per type?" There are exactly three machine-level
answers, and a language's speed character is largely decided by which one is the *default*:

| Strategy | Representative | Mechanism | Element cost |
|---|---|---|---|
| **Box everything** | Go `interface{}` (pre-1.18), Java raw types, Python (all values) | one runtime carrier; type recovered via tag/descriptor | indirection + tag dispatch on **every** access |
| **Monomorphize** | C++ templates, Rust generics, Go 1.18+ (GCShape stencils) | stamp a specialized copy per concrete type argument | **zero** - native representation per instantiation |
| **Erase** | Java generics, Python typing | parameter exists only for the checker; one runtime shape | zero *if* the shape is already uniform; else forces boxing |

**The Go lesson.** Go shipped for ~a decade with only `map[string]interface{}` and code
generation, then added real generics in 1.18 - precisely because `interface{}` was *both* unsafe
(static types lost; runtime type-asserts) *and* slow (boxing, heap escapes, indirection that
defeats inlining/vectorization). Go's chosen implementation is GCShape stenciling - a pragmatic
monomorphization. The takeaway: **do not ship the mistake Go spent a decade regretting.** "Use
`Any`" is the decade-long wrong default.

**The Python lesson.** Python has `T` semantics: `TypeVar('T')`, `Generic[T]`, and since 3.12
(PEP 695) the clean `class Foo[T]:` / `def f[T]` syntax. But Python's generics are **pure
erasure for the static checker** (mypy/pyright) and do *nothing* at runtime - affordable only
because everything is already a boxed `PyObject*`. Dragon cannot inherit the semantics without
inheriting the box. The correct inheritance is the **surface**, paired with a monomorphizing
backend.

The motivating demand: the [[]] `.dr` frontend and stdlib modules want user-level generic
containers the built-ins can't express (a `Result[T, E]`, a typed `Stack[T]`, a `Cache[K, V]`),
and the no-workarounds rule (#2) forbids telling them to box into `list[Any]`.

## Current state (verified in code)

Dragon today has **zero user-defined generics** but three working built-in mechanisms - one per
row of the table above:

1. **Monomorphization (the default, [[]]).** `list[T]` / `dict[K,V]` are hand-stamped into
 `DragonListI64` / `DragonListF64` / `DragonListPtr` / `DragonListBox` and typed dict entry
 points (`dragon_dict_get_str_f64`, `dragon_dict_int_set_*`, …). Codegen picks the variant
 from the element type at allocation. Native `i64[]` / `double[]` / `void**` storage, no box.

2. **Erasure (the `Task[T]` precedent, [[]]).** `TaskType` carries the result type `T`
 through the type system (`src/TypeChecker.cpp:101,710-711`, methods at `:1343-1356`,
 `await` unwrap at `:1892-1903`) but **lowers to a bare `ptr`** - `if (named->name == "Task")
 return i8PtrType;` (`src/CodeGenImpl.h:3155`). `T` never affects layout *and a Task never owns
 its `T`*, so erasure is sound here (see the soundness note).

3. **Boxing (the bounded second tier, [[]]).** `Any` / `Union[...]` / non-niche
 `Optional[T]` use `%dragon.box = { i64 tag, i64 payload }` - Dragon's `interface{}` analogue,
 deliberately scoped to heterogeneous data and never the hot path.

**Scaffolding that proves the gap:** `TypeVarType` / `Type::Kind::TypeVar` exist
(`include/dragon/TypeChecker.h:191-196`) but are **dead** - never constructed anywhere
(`src/TypeChecker.cpp` defines only `equals`/`toString`; no `make_shared<TypeVarType>` exists).
`resolveType` is a **hardcoded whitelist** (`src/TypeChecker.cpp:685-714`): `list`, `dict`,
`tuple`, `set` (currently aliased to `list[T]`), `Task`. Any other `Foo[T]` is a hard error -
`"unknown generic type '...'"` (`src/TypeChecker.cpp:713`). There is no PEP 695 parser support.
So a user cannot write `Stack[T]`; the engine below is what closes the gap.

## Options Considered

### Option A - `Any`/box as the generics story

Tell users to write `list[Any]` / `dict[str, Any]` and `isinstance`-narrow at use sites; never
add real generics. This is Go-before-1.18 / `interface{}`.

- **Pros:** No new compiler machinery; [[]] already ships the box.
- **Cons:** Every element access pays a load + tag dispatch; values heap-escape; the optimizer
 loses provenance and cannot vectorize or inline through the box. This is the exact cost model
 [[]] was written to *delete* - adopting it as the generics default would be a direct
 #1 violation, and it discards static type safety (#2). **Rejected as the default.**

### Option B - Erase all generics to `ptr`

Generalize the `Task[T]` trick: every generic lowers to `ptr`, `T` lives only in the checker.
This is Java/Python erasure.

- **Pros:** Cheap to implement; one runtime shape; zero code bloat.
- **Cons:** Only sound when `T` is representation-uniform *and unowned*. A `Stack[int]` erased to
 `ptr` would have to **box** its `int`s - collapsing back into Option A's cost; an owning
 `Stack[str]` would lose the decref dispatch and leak/double-free (soundness note below).
 **Rejected as the default; retained as a conditional fast path** (Decision, tier 2).

### Option C - Monomorphize by default; erase when uniform+unowned; box only heterogeneous (chosen)

Stamp a specialized instantiation per concrete type argument (C++/Rust/Go-1.18 model), falling
back to erasure only when `T` provably doesn't affect representation and isn't owned, and to the
box only for data that is heterogeneous at runtime.

- **Pros:** Native representation per instantiation → zero per-element overhead, full
 inlining/vectorization, preserved pointer provenance. The **only** option compatible with
 #1, and it *generalizes the strategy Dragon already proved* for containers
 ([[]]). Static safety is maximal (#2).
- **Cons:** Code bloat (one copy per distinct type argument) and real implementation cost - the
 instantiation engine specified below.

## Decision

**Adopt Option C**, in strict priorty order matching the three commandments:

1. **Monomorphize by default.** A generic instantiated at a concrete type gets a specialized copy
 with native representation - the [[]] container strategy, generalized. This is the default
 for every user-defined `class Foo[T]` / `def f[T]`.

2. **Erase to `ptr` when `T` is provably representation-uniform _and the generic never owns or
 refcounts a `T`-typed value_.** Both conditions required. If every `T` lowers to the same
 `ptr` shape *and* the generic only passes `T` through / hands it back without owning the
 refcount (the `Task[T]` case), erase: no specialization, no bloat.

 **Why the ownership clause is not optional (soundness, not optimization).** Dragon has no
 single `decref(ptr)`. `ptr`-shaped types do *not* share a destructor: a `str` is freed by
 `dragon_decref_str_dispatch`, a `list`/`dict`/`bytes` by the generic refcount decref, and an
 `int`/`bool` by nothing. The runtime picks among them by a stored **tag** - `DragonListPtr`
 carries a per-list `elem_tag` and `runtime_list.cpp:47-88` branches on it (`TAG_STR →
 dragon_decref_str_dispatch`, `TAG_LIST/DICT/BYTES → decref`, ints → no-op);
 `runtime_builtins.cpp:1268` documents the same split. A generic that *owns* a `T` and is erased
 to a bare `ptr` has discarded exactly the information needed to free it - it leaks, double-frees,
 or calls the wrong destructor the first time someone writes an owning `Box[T]`/`Stack[T]`/
 `Cache[K,V]`. The only sound ways to own a `T` behind erasure are (a) carry the tag at runtime
 (which *is* runtime monomorphization-by-tag - the `DragonListPtr` design, not true erasure), or
 (b) box it ([[]]). So: **owning generic ⇒ monomorphize (tier 1). Erasure is reserved for
 the non-owning, representation-uniform case.**

3. **Box only genuinely heterogeneous runtime data.** `Any` / `Union[...]` / non-niche
 `Optional[T]` use the [[]] box - a *second tier for heterogeneity*, explicitly **not** the
 generics mechanism.

**Surface = Python; mechanism = C++/Rust.** The surface is PEP 695 (`class Foo[T]:`,
`def f[T](...)`, `.py` indentation-equivalent) per #3 - backed by tier-1
monomorphization, never Python's runtime erasure.

**Doctrine-to-mechanism map:**

| Situation | Mechanism | Why |
|---|---|---|
| `list[int]`, `dict[str, float]`, `Stack[int]`, **any owning `Box[T]`/`Cache[K,V]`** | **Monomorphize** | native storage; the #1 path; already real for containers. *Owning a `T` forces this tier.* |
| `Task[T]`, and only generics that **pass `T` through without owning it** where every `T` is `ptr`-shaped | **Erase to `ptr`** | `T` doesn't change layout *and* there's no destructor to dispatch |
| `dict[str, Any]`, `list[Any]`, `int \| str \| bool`, JSON/TOML leaves | **Box** ([[]]) | data is heterogeneous at runtime; one carrier is correct |

---

## The Instantiation Engine (implementation spec)

### Surface - parser / Sema

Adopt **PEP 695 bracket form, both modes. Decline the `TypeVar('T')` assignment form for v1.**

```dragon
// .dr - brace mode
class Stack[T] {
 items: list[T] = []
 def push(self, x: T) { self.items.append(x) }
 def pop(self) -> T { return self.items.pop }
}
def first[T](xs: list[T]) -> T { return xs[0] }
```
```python
# .py - indentation mode (identical semantics)
class Stack[T]:
 items: list[T] = []
 def push(self, x: T) -> None: self.items.append(x)
 def pop(self) -> T: return self.items.pop
```

**Why bracket-only, not `TypeVar('T')`:** PEP 695 binds `T` *lexically to the declaration* - no
runtime sentinel object, clean scope entry/exit, and it is the form new Python code uses
(#3 targets current Python). `TypeVar('T')` is a runtime-object pattern that clashes
with Dragon's no-runtime-type-objects model; if demand appears it can later desugar to the
bracket form. Decline it now (#2: don't build the second surface speculatively).

**AST changes** (`include/dragon/AST.h`):
- Add `std::vector<TypeParam> typeParams` to `ClassDecl` and `FuncDecl`.
 `TypeParam = { std::string name; std::unique_ptr<TypeExpr> bound; /* bound==nullptr in v1 */ }`.
- A decl with non-empty `typeParams` is a **template**: parsed and Sema/checked once abstractly,
 but **never lowered to LLVM directly** (it has free type vars). Only its stamped instantiations
 reach CodeGen.

**Parser** (`src/Parser.cpp`, both `isDragonFile` and `.py`): after the class/def *name*, parse an
optional `[ Ident (Ident)* ]` into `typeParams`. Unambiguous at a definition site (a name
immediately followed by `[` in declaration position is a type-param list, never a subscript).

**Sema** (`src/Sema.cpp`): on entering a generic class/function scope, bind each
`typeParams[i].name` as a **type symbol** so body name-resolution sees `T` as a known type; pop on
scope exit. This is the first real consumer of `TypeVarType`.

### Pipeline placement

```
Lexer → Parser → [TypeHintEnforcer] → Sema → TypeChecker → ⟦MONOMORPHIZE (new)⟧ → CodeGen → cc
```

The new pass sits **between TypeChecker and CodeGen, before `forwardDeclareClasses`.** This is
forced by the single-LLVM-module, forward-declare-all-then-emit model (compile-time import model): every class/function must exist concretely *before* the forward-declare sweep, so
all instantiations must be stamped first. A generic with free type vars can never reach codegen.

### Two-phase: collect, then stamp to fixpoint

**Phase A - collect requests (during TypeChecking).** TypeChecker type-checks each generic body
**once, abstractly**, against its `TypeVarType`s (catches `T`-independent errors and enforces the
unbounded-`T` restriction below). At every *use site* that pins concrete arguments - `Stack[int]`,
`x: Stack[int]`, a param/return/field of generic type, an explicit `first[int](...)` or an inferred
`first(xs)` where `xs: list[int]` - emit an **instantiation request** `(genericDecl, [concrete type
args])`. Extend `resolveType`'s `GenericTypeExpr` branch: after the existing `list`/`dict`/`tuple`/
`set`/`Task` whitelist, if `baseName` resolves to a generic `ClassDecl`, record the request and
return the (to-be-stamped) `InstanceType` instead of erroring at `:713`.

**Phase B - stamp the transitive closure (worklist).** Seed a worklist with Phase A's requests.
Pop `(G, args)`:
1. If its cache key is already present, skip (dedup).
2. Clone `G`'s AST, substituting each type param with its concrete arg throughout (params,
 returns, fields, locals, nested `GenericTypeExpr`s). Substitution is a structural AST/Type
 rewrite: `TypeVarType("T") → concrete`.
3. The clone is a normal monomorphic `ClassDecl`/`FuncDecl` with the mangled name (below).
 Register it exactly like a hand-written class.
4. **Enqueue transitively discovered instantiations:** if the substituted body references
 `Inner[int]`, push `(Inner, [int])`. Iterate to fixpoint.

Fixpoint terminates for any non-polymorphically-recursive program (finite set of distinct
`(generic, concrete-args)`). **Polymorphic recursion** (`Foo[T]` instantiates `Foo[list[T]]`,
unbounded) is detected by an instantiation-depth cap and reported as a compile error - same
posture as C++ `-ftemplate-depth` and Rust's recursion limit. Do **not** silently truncate.

After Phase B, `forwardDeclareClasses` and emission run normally over the original monomorphic
classes **plus** the stamped instantiations. No generic template is emitted.

### Cache key + name mangling

- **Cache key:** `genericName + "[" + join(argCanonicalStrings, ",") + "]"`, where each arg's
 canonical string is the existing `Type::toString` (already yields `int`, `list[int]`, `Foo`,
 …). Stable, structural, collision-free. `Stack[int]` requested twice → one stamp.
- **LLVM symbol/type names:** use the canonical string verbatim as the internal name -
 `%"Stack[int]"` for the struct type, `"Stack[int].push"` for the method. Legal because (a) LLVM
 permits arbitrary quoted identifiers, and (b) every instantiation has **internal linkage**
 inside the one module and never crosses the C ABI, so no C-symbol sanitization is needed. `[`,
 `]`, `,` cannot appear in a user identifier, so collision with hand-written names is impossible.
 Reuses the existing per-class method-mangling by feeding it the stamped class name.

### Interaction matrix

| Subsystem | How a generic instantiation behaves | New work? |
|---|---|---|
| **Refcount / decref (the soundness rule)** | After substitution a field of type `T` *is* concrete - `Stack[str].items` is `list[str]`, freed by the existing tag-correct decref. This is the whole reason owning generics monomorphize. | **None** - reuses existing decref-by-type. |
| **Vtables ([[]])** | Each instantiation is a distinct concrete class with its **own vtable**. Whole-program devirtualization works per-instantiation (the receiver's concrete type is known). Generic methods are ordinary methods on the stamped class. | None beyond emitting the stamped class. |
| **`Any` / box ([[]])** | `Stack[Any]` is **a legal instantiation** (T = `Any`): the stamped field is a `%dragon.box`. `Stack[int\|str]` likewise. "Generic over a heterogeneous type" composes: monomorphize the container, box the element. No surprise degradation. | None - box path already exists. |
| **Closures ([[]] typed env)** | A closure inside a generic body capturing a `T`: after substitution the env field is concrete; existing typed-env packing applies. | None. |
| **Fields / layout** | Per-instantiation layout: `Stack[int]` has `list[i64]` storage; `Stack[str]` has `list[ptr]`. Falls out of substitution. | None. |
| **GC root scanning ([[]], when it lands)** | Stamped slots are concretely typed, so the scanner sees real pointer/value slots - strictly easier than scanning a `T`. | None ('s concern). |

The payoff is uniform: **substitution turns every generic into something the compiler already
knows how to handle.** The engine adds the stamping pass; it adds *no* new lowering.

### Bounds - v1 restriction, made explicit

**v1 = unbounded `T` only.** Inside a generic body, a value of type `T` may be: stored (field,
local, or container), passed as an argument, returned; compared with `==`/`!=`/`is`; placed into /
read from built-in containers (`list[T]`, `dict[K, T]`). It may **not** have methods or operators
called on it (`t.foo`, `t + t`, `t < t`, `len(t)`, subscript) - the checker cannot prove those
exist for every `T`. Such a use is a **compile error**: `"cannot call method 'foo' on unbounded
type parameter 'T'; method calls on a type parameter require a bound"`. Sound, and exactly enough
for the container use case (built-in `list`/`dict` never call methods on their element type either).

Bounded type parameters (`T: SomeClass` - method calls typechecked against the bound and
dispatched via [[]] vtables) shipped later .
The `TypeParam.bound` AST slot is added now (always `nullptr` in v1) so the surface doesn't churn
when bounds land.

### `set[T]` is out of scope

`resolveType` currently aliases `set[T]` to `ListType` (`src/TypeChecker.cpp:706-708`) - a
**pre-existing built-in-container defect** (set semantics collapsed to list), not a user-generics
problem. **Not gated on this work and not fixed here.** Tracked separately as a focused
runtime+codegen task (a real `DragonSet` + typed ops) in the [[]] container family. Noted only
so the implementer doesn't conflate it with the generic engine.

### Verification plan

Behavioral E2E in `test/dr/` (auto-registered ctest); IR-shape/internal checks in GoogleTest
(`CodeGenTest`), per project test conventions:

| Test | Kind | Asserts |
|---|---|---|
| `test/dr/test_generics_basic.dr` | E2E | `Box[int]`/`Box[str]`/`Box[float]` round-trip a value at native type; `Stack[int]` push/pop. |
| `test/dr/test_generics_function.dr` | E2E | `def first[T](xs: list[T]) -> T` over `list[int]` and `list[str]`. |
| `test/dr/test_generics_transitive.dr` | E2E | `Outer[T]` holding an `Inner[T]`; instantiating `Outer[int]` auto-stamps `Inner[int]`. |
| `test/dr/test_generics_any.dr` | E2E | `Stack[Any]` accepts `int`+`str`, prints via box dispatch - composition with [[]]. |
| `test/dr/test_generics_py.dr` | E2E | The basic suite in `.py` indentation mode - surface≠semantics parity. |
| `Generics_MonomorphizedFieldIsNative` | GoogleTest IR | `Box[int]`'s stamped field is `i64`, **not** `%dragon.box` - proves monomorphization, not boxing. |
| `Generics_InstantiationDedup` | GoogleTest IR | Two `Box[int]` uses emit **one** `%"Box[int]"` struct def. |
| Owning-generic refcount | E2E + leak harness | A loop creating/dropping `Box[str]` runs `dragon_decref_str` and **does not leak** (the soundness scenario; run under the ASan recipe). |
| Unbounded-`T` method-call rejection | GoogleTest (compile-error) | `def f[T](t: T) { t.foo }` errors cleanly with the declare-a-bound message . |
| Polymorphic-recursion cap | GoogleTest (compile-error) | `class Foo[T] { x: Foo[list[T]] }`-style infinite instantiation hits the depth cap and errors cleanly (no hang). |

### Implementation order (each step independently testable)

1. **Surface:** AST `typeParams`, parser (both modes), Sema type-param binding, `TypeVarType`
 construction. Generic decls parse + resolve `T`; no instantiation yet.
2. **Collect:** TypeChecker abstract body-check + unbounded-`T` restriction + instantiation-request
 recording in `resolveType`'s generic branch.
3. **Stamp:** the monomorphization pass - clone+substitute, cache, worklist fixpoint, depth cap,
 mangling. Wire it before `forwardDeclareClasses`.
4. **Emit + verify:** stamped classes/functions flow through existing codegen; run the test matrix.
5. **Cleanup:** the dead-`TypeVarType` half-state is now fully live (the substitution pivot).

## Why monomorphize, concretely

**Positive**
- A single, #1-aligned answer to "how do generics work": specialize, don't box.
- User-defined generic containers exist at native speed, reusing every existing lowering path -
 the engine is *pure front-of-codegen stamping*, no new runtime representation.
- Generalizes [[]]'s proven container strategy; the `Task[T]` erasure becomes a principled
 tier rather than a special case.
- PEP 695 surface keeps Python parity (#3) without importing Python's runtime cost.
- Unblocks the [[]] `.dr` frontend and stdlib modules that want `Result`/`Stack`/`Cache`.
- The dead `TypeVarType` scaffolding becomes important (the substitution pivot).

**Negative / costs**
- Code bloat: one stamped copy per distinct type argument (the standard C++/Rust trade, accepted
 for #1). The depth cap bounds pathological cases.
- A new compiler pass + AST/parser/Sema/TypeChecker surface; non-trivial but bounded by the steps
 above.
- v1's unbounded-`T`-only limit means no method calls on `T` until bounds shipped - an
 honest, documented first cut, not a silent gap.

**Neutral**
- Built-in `list`/`dict`/`tuple` are unaffected - they keep their hand-written [[]] monomorphic
 paths; this work is purely additive for *user* generics.

## Open Questions

1. **Type-argument inference for generic *functions*.** v1 may require explicit `first[int](xs)` if
 full inference is hard; the test matrix assumes the common case works. Decide during step 2
 whether to ship inference in v1 or require explicit `[T]` at the call site initially. Note that
 Dragon's **mandatory binding annotation makes inference *bidirectional* and unusually
 tractable**: the declaration `xs: list[int] = ...` always supplies an *expected type*, so the
 engine can unify it against the function's return type. This solves cases that are unsolvable in
 Python - including a `T` that appears *only* in the return position (`def make[T] -> list[T]`;
 `xs: list[int] = make` solves `T=int` from the annotation, with no argument to match). So the
 two inference sources are (a) argument types vs declared param types, and (b) the binding's
 annotation vs the declared return type. Container *class* instantiation is never affected - the
 mandatory annotation (`s: Stack[int] = Stack`) always pins `T` directly, so inference only
 ever matters for free functions.
2. **Bounds .** Bounded `T: SomeClass` -
 method calls on `T` typechecked against the bound and dispatched via [[]]. The
 `TypeParam.bound` slot was reserved by and is now populated.
3. **Per-generic opt-in erasure** as a bloat valve for non-owning `ptr`-shaped generics (mirroring
 Go's GCShape sharing). The soundness rule already permits it for non-owning generics; the
 question is whether to surface it as an annotation. Deferred until a measurement demands it -
 speculative knobs violate #2.
