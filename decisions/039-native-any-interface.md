# Decision 039: Native `Any` - Dragon's `interface{}`

**Status:** Approved (Phases 1-10 implemented)

**Builds on:** D030 (native value model. This decision completes Phase 4's plumbing across remaining call sites), D023 (Dragon Script - primary consumer of `dict[str, Any]`), D014 (Exceptions. TypeError on unbox mismatch)

Phase 4 gave me `%dragon.box = { i64 tag, i64 payload }` and tagged dict storage, but half the call sites still didn't know what to do with a box. Config files want `dict[str, Any]`, JSON decode wants heterogeneous values, and I got tired of explaining "Union works in params but not in returns yet." This finishes the plumbing, basically Go's `interface{}` with stricter static rules.

## Summary

Dragon's `Any` is basically Go's `interface{}`: same speed story, tighter static rules.
I need `dict[str, Any]`, `list[Any]`, `Any` locals/returns/fields, and
`T | None` / `Union[A, B, ...]` working end-to-end as real typed values.
Same job as Go's `map[string]interface{}`, Rust's `HashMap<String, serde_json::Value>`,
Java's `Map<String, Object>`, C#'s `Dictionary<string, object>`.

 Phase 4 (`230c7b4`) gave us the foundation: `%dragon.box = { i64 tag, i64 payload }`,
tagged dict storage, and `isinstance` narrowing for params/locals via `unionTagAllocas`.
This decision finishes the rest of Phase 4's plumbing: subscript reads, `print`
dispatch, narrowing-through-subscripts, function returns, class fields,
iteration, equality, and the box ABI for inter-function flow.

It also folds in what an interim review of boxed calling conventions
proposed as a separate decision - the box ABI section below now lives here
to keep the surface coherent in one document.

### Speed model: Dragon's box vs Go's interface{}

| Mechanic | Go | Dragon (this ADR) | Match? |
|---|---|---|---|
| Heterogeneous map value | 16 bytes: `{*_type, *data}` | 16 bytes: `%dragon.box = { i64 tag, i64 payload }` | ok |
| Register-passable (AMD64) | 2 registers | 2 registers (i64 + i64) | ok |
| Type-check cost | 1 load + 1 cmp on `*_type` ptr | 1 load + 1 cmp on `tag` (i64 vs constant - cheaper than ptr cmp) | ok better |
| Hot path stays monomorphic | `map[string]int` is concrete; never widened | `dict[str, int]` is monomorphic ; never widened | ok |
| Post-narrowing devirt | `v, ok := m["k"].(int)` produces native int branch | `if isinstance(v, int) { ... v as native int ... }` | ok |
| Print dispatch | via type-descriptor method table | via tag-switch in `dragon_print_box` | ok |
| Static-source coercion | requires `.(int)` - never silent | Phase 7b: compile-time error for Union demanding narrowing | **stricter** |

Speed stays intact. Sixteen-byte box in registers, one tag compare, monomorphic hot path untouched.
Dragon matches Go on speed and beat it on static strictness when the compiler can catch the bug at compile time (#2).

---

## Context / Motivation

Any language that parses JSON/TOML/YAML/etc. eventually needs a map whose values can be anything:

| Language | Primitive |
|---|---|
| Go | `map[string]interface{}` |
| Rust | `HashMap<String, serde_json::Value>` |
| Java | `Map<String, Object>` |
| C# | `Dictionary<string, object>` |
| TypeScript | `Record<string, unknown>` / `Record<string, any>` |
| Python (typed) | `dict[str, Any]` |
| Dragon (today) | **broken** |

`stdlib/drs.dr` already worked around the missing primitive: every leaf became
`{"_tag": 1, "_str": "foo"}`, so users did `cfg["k"]["_str"]` instead of `cfg["k"]`.
That's user code papering over a runtime hole. Dragon doesn't do that (#2).
Every format parser (JSON, TOML, YAML, msgpack, REST, Dragon Script) needs this fixed once.

Smoke tests against curent Dragon document the gaps:

```dragon
# Test 1 - bare Any reassignment
x: Any = 42 # works
x = "hello"
print(x) # → 99230782623748 (raw ptr-as-int) - BUG

# Test 2 - Union through subscript
cfg: dict[str, int | str] = {}
cfg["port"] = 8080
v: int | str = cfg["port"]
if isinstance(v, int) { print(v + 1) } # compile error - narrowing lost

# Test 3 - dict[str, Any]
cfg: dict[str, Any] = {}
cfg["name"] = "my-app"
v: Any = cfg["name"] # TypeError: 'name' is str, not int
 # codegen dispatched as TAG_INT-checked
```

### Design Principles

- **Hot path stays monomorphic.** `dict[str, int]`, `list[str]`, etc. continue
 to use 's typed runtime ops - zero overhead, no box, no tag dispatch.
 This decision adds `Any` as a **second tier** for heterogeneous data, not a
 replacement.
- **Use existing infrastructure.** Phase 4 already shipped
 `%dragon.box`, tagged dict entries, isinstance narrowing. This decision
 threads them through the missing call sites.
- **One semantic, three carriers.** Whether you write `Any`, `int | str`, or
 `Optional[int]`, the runtime representation is the same `{i64 tag, i64
 payload}` box. Narrowing, dispatch, and round-trip are unified.

---

## Design

### Runtime representation

All `Any` / `Union[...]` / `Optional[T]` values use the existing Phase 4
box:

```
%dragon.box = type { i64, i64 } ; { tag, payload }
```

Tag taxonomy (existing `DragonValueTag`):
- `0` TAG_INT, `1` TAG_STR, `2` TAG_FLOAT, `3` TAG_BOOL, `4` TAG_NONE,
 `5` TAG_LIST, `6` TAG_DICT, `7` TAG_BYTES/CLASS, `8` TAG_GENERATOR,
 `9` TAG_TYPE, `10` TAG_CLOSURE, `11` TAG_ENV, `12` TAG_DEQUE.

### Three new runtime entry points

```c
// Returns a box-by-value (LLVM lowers to {i64, i64})
dragon_box dragon_dict_get_box(DragonDict* d, const char* key);
dragon_box dragon_list_get_box(DragonList* l, int64_t idx);

// Print dispatch on box tag (string/int/float/bool/None/nested via existing printers)
void dragon_print_box(dragon_box b);
```

Storage already supports per-entry tags (Phase 4); these are read-side
companions to the existing `dragon_dict_set_tagged` / `dragon_list_set_ptr +
tag`.

### Codegen dispatch table

| Source pattern | Today | After |
|---|---|---|
| `cfg[k]` where `cfg: dict[str, Any]` | `dragon_dict_get_checked(TAG_INT)` (wrong) | `dragon_dict_get_box` → `%dragon.box` value |
| `lst[i]` where `lst: list[Any]` | falls through to int dispatch | `dragon_list_get_box` |
| `print(box)` | prints raw i64 payload | `dragon_print_box` (tag-dispatch) |
| `if isinstance(x, T) { ... use x ... }` where `x` is a box | works for params/locals | also works when `x` came from a subscript read |
| `x: int = cfg["k"]` where `cfg: dict[str, Any]` | dispatches as TAG_INT-checked (works by luck if tag matches) | explicit box → unbox with TypeError on mismatch |
| `f: Optional[int]` in a class | reads as raw i64 (no tag) | reads as box; `if instance.f is None` works via TAG_NONE |

### TypeChecker fixes

1. `typeExprToTypeKind` (`src/CodeGenImpl.h:2173-2206`) - add explicit case for
 `NamedTypeExpr{name: "Any"}` returning `Type::Kind::Any`. Today it falls
 to `return Type::Kind::Int` at line 2188.
2. **Narrowing propagation through subscript reads.** When the typechecker
 sees:
 ```dragon
 v = cfg["k"] # cfg: dict[str, Any]
 if isinstance(v, int) {
 v + 1 # must typecheck - v is narrowed to int here
 }
 ```
 the narrowed scope must apply even though `v`'s declared type traces back
 to a subscript expression on an Any-valued container. Today narrowing
 propagates for params and direct locals but not for "local assigned from
 subscript".

### Codegen sites that need box-aware dispatch

| File | Site | What changes |
|---|---|---|
| `src/codegen/Attributes.cpp:680-728` | dict subscript read | when value type is `Any` / Union / Optional, emit `dragon_dict_get_box` returning `%dragon.box` |
| `src/codegen/Attributes.cpp` (list read path) | list subscript read | when element type is Any/Union/Optional, emit `dragon_list_get_box` |
| `src/codegen/Assign.cpp:1448-1469` | annotated assignment from dict subscript | when LHS annotation is `Any` / Union / Optional, skip `pendingDictCheckTag` setup (no checked-get); when LHS is concrete and RHS is Any, emit box → unbox with TypeError trampoline |
| `src/codegen/Expressions.cpp` (`print` lowering) | `print(value)` where value is `%dragon.box` | dispatch via `dragon_print_box` rather than printing the payload as i64 |
| `src/codegen/Classes.cpp` | class field access where field is Optional/Union | load as box; comparison-to-None uses TAG_NONE check |

### `None` and `Optional[T]`

`None` is a singleton box `{tag=4, payload=0}`. `Optional[T]` is sugar for
`T | None`.

**Niche-ptr preservation (important).** When `T` is pointer-shaped
(class instance, str, list, dict, bytes, set), `T | None` already collapses
to a single nullable pointer via `unionNicheMember`
(`src/CodeGenImpl.h:2290-2318`) - zero-cost encoding, no box, no tag, just a
null check. Live consumers include `stdlib/http/server.dr:1195, 1248, 1352`.

**This decision MUST NOT regress the niche-ptr fast path.** Box representation
applies only when:
- `T` is value-shaped (`int | None`, `float | None`, `bool | None`, etc.), OR
- The union has 3+ members (`int | str | bool`), OR
- The user explicitly annotates `Any`.

Pointer-shaped 2-member optionals stay niche-encoded. `x is None` lowers to
`x.tag == 4` for box variants, or `x == nullptr` for niche-encoded variants.
The dispatch is selected by `unionNicheMember` returning the niche member
(box path bypassed) or null (box path taken).

This is a speed-positive optimization, not a workaround - it is the existing
zero-cost encoding that this decision must explicitly preserve.

---

## Box ABI - How a Box Flows Through the Program

Every codegen site needs the same rules written down once. Otherwise each site reinvents them and things drift.

### Box value layout

```
%dragon.box = type { i64 tag, i64 payload }
; tag uses DragonValueTag taxonomy
; payload holds primitive value in-place (int/float bits/bool 0|1)
; OR holds a pointer cast to i64 (str/list/dict/class/bytes)
```

Passed by value (16 bytes, register-fit on AMD64). Constructed via
`makeBox(tag, payload)` (already exists from Phase 4).

### Calling conventions

| Site | Today | After |
|---|---|---|
| **`Any` / Union param** | Phase 4: box value passed in 2 registers | Unchanged - already correct |
| **`Any` return type** | `typeExprToLLVM("Any") = i64Type` → returns raw i64, tag info LOST | Returns `%dragon.box` (2 registers) |
| **`Any` local var** | i64 alloca (8 bytes) - second assignment with different type loses tag | `boxType` alloca (16 bytes) |
| **`Any`-valued class field** | i64 slot - same issue as local | 16-byte slot; field-load returns box value |
| **`Any`-valued dict entry** | Tagged (per-entry tag in `DragonDict`) ok already correct | Unchanged |
| **`Any`-valued list element** | Stored as ptr in `DragonListPtr`; tag lost on read | New `dragon_list_get_box` returns box value |
| **Box-typed tuple slot** | Tuple slots are i64 - tag lost | When tuple element type is `Any`, store box payload + slot's own tag byte |

### Refcount ownership (important)

When a runtime function returns a box whose payload is a refcounted pointer
(TAG_STR / TAG_LIST / TAG_DICT / TAG_BYTES / TAG_CLASS), the caller has two
contract options:

| Contract | Pros | Cons |
|---|---|---|
| **Returns borrow** | Cheap on the get side (no incref); read-only access is free | Caller must incref before storing into a longer-lived slot |
| **Returns owned (+1 refcount)** | Caller stores without extra incref | Read-only access costs an unnecessary incref/decref pair |

**Decision: returns borrow** - matches the existing `dragon_dict_get` contract
(returns borrows; the dict still owns the +1) and the existing decref-on-overwrite
discipline in `Assign.cpp`. Box-returning ops in this decision (`dragon_dict_get_box`,
`dragon_list_get_box`) **return borrows**: payload is a non-owning pointer
into the container's storage.

Codegen rule:
- **Storing a box into a longer-lived slot** (local alloca, class field, dict
 value, list element): emit incref on the box's payload at store time if the
 tag is refcounted.
- **Discarding a box** (last use in expression): no action; the container
 still owns the +1.
- **Overwriting a slot that previously held a refcounted box**: decref the
 OLD payload (using its tag) before storing the new box.

This mirrors 's existing Model-B pattern for `dragon_list_set_ptr` and
`dragon_dict_set_str_ptr` - extends it consistently to box-returning ops.

### Equality

`==` on boxes:
- Different tags → false (no implicit type coercion, matches Python).
- Same tag → compare payloads using the tag-appropriate operation:
 - TAG_INT, TAG_BOOL: i64 equality
 - TAG_FLOAT: f64 equality (no epsilon - matches Python `==` on floats)
 - TAG_STR: `dragon_str_eq`
 - TAG_LIST/DICT/BYTES/CLASS: pointer identity (matches Python `is`-style for
 container `==` short-circuit), then recurse via runtime list/dict equality
 - TAG_NONE: always true (None == None)

Emitted via a new `dragon_box_eq(a, b) -> i1` runtime helper. Used by the
codegen `==` operator when either operand is a box.

### Hashing (deferred non-goal for v1)

`hash(box)` would be required for `dict[Any, V]`. This decision deliberately
scopes to `dict[str, Any]` only. `dict[Any, V]` is a non-goal for v1 - a
follow-up ADR can add tag-dispatched hashing if a use case demands it.

---

## Implementation Phases

Phases are ordered so each is independently smoke-testable: Phase N's test
only depends on Phases 1..N-1 working.

### Phase 1: `Any` as box throughout (NOT a one-liner)

The original draft framed this as a single-line case in `typeExprToTypeKind`.
That was wrong. Two distinct problems:

1. `typeExprToTypeKind("Any")` (`src/CodeGenImpl.h:2173-2206`) returns
 `Type::Kind::Int` via fallthrough at line 2188. Fix: add explicit Any case
 returning `Type::Kind::Any`.
2. `typeExprToLLVM("Any")` (`src/CodeGenImpl.h:2448`) returns `i64Type`. So
 `Any` locals are 8-byte i64 today - a second assignment with a different
 type loses tag info. Fix: return `boxType` for Any.

The second change cascades through every Any use site:
- `Any` allocas grow from 8 → 16 bytes
- Loads/stores against Any allocas use box-aware codegen
- Refcount management at Any-local overwrite sites
- IfExpr ternary narrowing on Any operands
- Function param/return ABI for `Any` (return type lowering)
- Parser annotation `x: Any = expr` → box construction at the assignment

`typeKindToTag(Type::Kind::Any)` returns `-1` (no specific tag - defer to
runtime). Already the default; make it explicit.

**Test:** `x: Any = 42; print(x); x = "hello"; print(x); x = 3.14; print(x)`
outputs `42 / hello / 3.14` (requires Phase 3 print).

**Scope:** ~150 LOC. CodeGenImpl.h + Assign.cpp + Classes.cpp + Expressions.cpp.

### Phase 2: `dict[str, Any]` read returns box

New runtime `dragon_dict_get_box(d, key) -> %dragon.box`. Codegen dispatch in
`src/codegen/Attributes.cpp:680-728` when reading from a dict whose value-kind
is `Any` (tracked via existing `varDictValueKinds` map).

`dragon_dict_get_box` **returns a borrow** (see Box ABI). The codegen at
store-into-longer-lived-slot sites emits incref on the box payload as
needed.

**Test:** `cfg: dict[str, Any] = {}; cfg["s"] = "hi"; cfg["n"] = 5; v1: Any =
cfg["s"]; v2: Any = cfg["n"]` - both work, both tags preserved (visible once
Phase 3 prints them).

**Scope:** ~150 LOC across `runtime_dict.cpp` + `Attributes.cpp` + a new
`runtime_box.cpp` for box helpers (also used by later phases).

### Phase 3: `print` / `str` / format dispatch on box tag

`src/codegen/CallBuiltins.cpp:170-241` already has a partial switch-on-tag
for `print(NameExpr)` where the local is `VarKind::Union`. This phase
**generalizes that into a runtime helper** and routes every print site
through it:

- `print(boxValue)` (positional)
- f-string interpolation of a box (`CallBuiltins.cpp:163`)
- `str(boxValue)` lowering
- Implicit print of dict/list-of-box during print of the container

New runtime: `dragon_print_box(box) -> void` and `dragon_box_to_str(box) ->
char*`. Tag dispatch covers ALL tags (the existing switch misses TAG_NONE,
TAG_BYTES, TAG_TUPLE/SET - those become explicit cases). Default falls
through to a generic `"<box tag=N>"` debug print.

**Test:** All Phase 1 and Phase 2 smoke tests now produce visible output.

**Scope:** ~350 LOC . Generalization + dead-path
removal + all print sites threaded through the new helper.

### Phase 4: `list[Any]` read returns box

Companion to Phase 2 for lists. New `dragon_list_get_box(list, idx) ->
%dragon.box`. Codegen dispatch in `src/codegen/Attributes.cpp` list-read
path when element kind is `Any` (tracked via existing `varListElemKinds`).

Same borrow-return contract as Phase 2.

**Test:** `xs: list[Any] = []; xs.append(1); xs.append("two"); for x in xs
{ print(x) }` outputs `1 / two`.

**Scope:** ~120 LOC.

### Phase 5a: Statement-level `if isinstance` narrowing

Today, only `IfExpr` (the ternary `a if c else b`) does isinstance narrowing
(`src/TypeChecker.cpp:1547-1555`). Statement-level `if isinstance(v, T) {
... use v ... }` does NOT narrow `v` inside the branch. This is a separate,
pre-existing gap that this decision must fix to make testable end-to-end.

Add the same narrow/popScope plumbing to `visit(IfStmt&)` in TypeChecker.cpp
and Sema.cpp. Both `isinstance(x, T)` and `not isinstance(x, T)` produce a
narrowed (and complementary anti-narrowed) scope for the then/else branches.

**Test:**
```dragon
v: int | str = 42
if isinstance(v, int) { print(v + 1) }
else { print(v) }
```

**Scope:** ~150 LOC (extension of existing IfExpr narrower to IfStmt).

### Phase 5b: Narrowing through subscript reads (and field reads)

After 5a, narrowing on a `NameExpr` whose declared type is a Union/Any works
inside the branch. This phase ensures the narrowing also applies when the
local was assigned from a subscript or field read whose returned type was
Union/Any.

Concretely: when typechecker sees `v = cfg["k"]` where `cfg: dict[str, Any]`,
`v` has declared type `Any`. The isinstance check at the next use of `v`
should narrow `v`'s type in the branch. No new mechanism - same scoped
narrowing - but the typechecker must track `v`'s static type as `Any` rather
than collapsing it (today's fallback) to `Int` (the typeExprToTypeKind bug
from Phase 1).

**Subtleties** (flagged in review):
- **Aliasing:** `v = cfg["k"]; w = v; if isinstance(v, int) { w + 1 }` -
 narrowing applies only to `v`. `w` is a separate binding. Match Python.
- **Re-reads:** `if isinstance(cfg["k"], int) { cfg["k"] + 1 }` - Python
 does NOT narrow subscript reads (the value could be mutated between
 reads). Dragon doesn't either. The pattern requires an intermediate
 `v = cfg["k"]` local.
- **Mutation invalidation:** `v = cfg["k"]; if isinstance(v, int) { cfg["k"]
 = "x"; v + 1 }` - `v` is a copy of the box at assign time; narrowing on
 `v` remains valid even if `cfg["k"]` is overwritten in the branch.

**Test:** `cfg: dict[str, Any] = {}; cfg["port"] = 8080; v: Any =
cfg["port"]; if isinstance(v, int) { print(v + 1) }` - must compile and
print `8081`.

**Scope:** ~150 LOC in TypeChecker.cpp.

### Phase 6: `Optional[T]` / `T | None` - box for non-niche, niche-ptr preserved

**Hard rule (important):** pointer-shaped `T | None` (class instance,
str, list, dict, bytes, set) continues to use the existing niche-ptr
encoding from `unionNicheMember`. Zero-cost. Live consumers at
`stdlib/http/server.dr:1195, 1248, 1352` must not regress.

This phase adds the box variant only for:
- Value-shaped optionals: `int | None`, `float | None`, `bool | None`
- 3+-member unions: `int | str | bool`

Sites:
- `Optional[T]` class field declarations - emit box layout when not niche-able
- Field access - load as box for non-niche; load as nullable pointer for niche
- `x is None` - `x.tag == 4` (box) OR `x == null` (niche)

**Test:** `class C { x: int | None }; c = C; c.x = 5; if c.x is None {
print("none") } else { print(c.x + 1) }` works correctly.

**Scope:** ~250 LOC in `Classes.cpp` + small TypeChecker changes for the
niche/box dispatch decision.

### Phase 7a: `Any` unbox-on-assign - runtime TypeError

When `x: int = cfg["k"]` and `cfg: dict[str, Any]`, the typechecker cannot
statically prove the box's tag. Codegen emits:

1. Box read.
2. Compare `box.tag` to `TAG_INT`.
3. If equal, store payload into `x`.
4. If not, `dragon_raise_exc(80, "TypeError: expected int, got <tag>")`.

Mirrors the existing `dragon_dict_get_checked` runtime contract. The
TypeError longjmps to the nearest handler frame . No new exception
machinery required.

**Test:** `cfg["s"] = "hi"; x: int = cfg["s"]` raises `TypeError`.

**Scope:** ~150 LOC in `Assign.cpp` + a small helper in `runtime_box.cpp`.

### Phase 7b: Union unbox-on-assign - COMPILE-TIME error demanding narrowing

When the source's static type is a Union (e.g., `int | str`) and the target
is one of its members (e.g., `int`), the typechecker has the information to
demand `isinstance` narrowing **statically**. This is stricter than Go (Go
would runtime-panic on `.(int)` for the wrong tag) and aligned with
motto #2 - surface bugs at compile time when we can.

```dragon
v: int | str = 42
x: int = v # COMPILE ERROR - requires `if isinstance(v, int) { x = v }`
```

Error message:
```
error: cannot assign 'int | str' to 'int' without narrowing.
 use `if isinstance(v, int) { ... }` to narrow before assigning.
```

**Test:** the snippet above must compile-error; the narrowing form must
work.

**Scope:** ~100 LOC in TypeChecker.cpp (assignment type-compat rule).

### Phase 8: `Any` as function return type

`def f -> Any` - return type lowers to `%dragon.box`. Call sites receive a
box value. Box flows through return register pair on AMD64.

Touches `typeExprToLLVM` (Phase 1 helper), function-emission in
`ImplInit.cpp`, and call-site lowering in `CallExpr.cpp`.

**Test:** `def make -> Any { return 42 } x: Any = make; print(x)` prints
`42`.

**Scope:** ~120 LOC.

### Phase 9: Iteration over `dict[str, Any]` and `list[Any]`

`for k, v in cfg.items` / `for x in lst` where the value/element type is
Any - loop variable is a box. Same dispatch as the subscript path, but in
the iteration codegen at `ForLoop.cpp`.

**Test:** `cfg: dict[str, Any] = {}; cfg["s"] = "hi"; cfg["n"] = 5; for k, v
in cfg.items { print(k); print(v) }` prints `s / hi / n / 5` (order
depends on dict insertion).

**Scope:** ~150 LOC in `ForLoop.cpp` + matching items lowering.

### Phase 10: Box equality

`==` between two boxes lowers to `dragon_box_eq(a, b) -> i1`. Tag-first
comparison; different tags → false; same tag → tag-appropriate compare (see
Box ABI § Equality). `!=` is the negation.

**Test:** `a: Any = 1; b: Any = 1; print(a == b)` → `True`. `a: Any = 1; b:
Any = "1"; print(a == b)` → `False`.

**Scope:** ~80 LOC.

### Combined scope estimate

**~1,500-1,750 LOC** across:
- `src/CodeGenImpl.h` (Phase 1 - typeExprToTypeKind + typeExprToLLVM)
- `src/codegen/Assign.cpp` (Phases 1, 7a)
- `src/codegen/Attributes.cpp` (Phases 2, 4)
- `src/codegen/CallBuiltins.cpp` (Phase 3 generalization)
- `src/codegen/CallExpr.cpp` (Phase 8 return)
- `src/codegen/Classes.cpp` (Phases 1, 6)
- `src/codegen/Expressions.cpp` (Phase 1, 10)
- `src/codegen/ForLoop.cpp` (Phase 9)
- `src/codegen/ImplInit.cpp` (Phase 8 function emission)
- `src/TypeChecker.cpp` + `src/Sema.cpp` (Phases 1, 5a, 5b, 7b)
- `lib/Runtime/runtime_dict.cpp` (Phase 2)
- `lib/Runtime/runtime_list.cpp` (Phase 4)
- `lib/Runtime/runtime_box.cpp` (new - Phases 2, 3, 4, 7a, 10)

Earlier "~1,100 LOC" estimate was optimistic; this is the realistic figure
after factoring the Phase 1 cascade, Phase 3 generalization, statement-level
narrowing (Phase 5a was missing from the original phase list), and box
return/equality work folded in from the interim boxed-calling-conventions
proposal.

---

## Motto Check

| Decision | Speed (#1) | No workarounds (#2) | Parity (#3) |
|---|---|---|---|
| Keep monomorphic hot path | Existing `dict[str, int]` etc. stay native - zero overhead | n/a | n/a |
| Add box-returning second tier | Boxes are 16 bytes returned by-value; one extra load + tag-check per Any access - cheap | Yes, this IS the root cause fix that closes the workaround in `stdlib/drs.dr` | Matches Python `dict[str, Any]`, Go `map[string]interface{}` |
| `isinstance` narrowing through subscripts | After narrowing, access is native int/str/etc. - same cost as monomorphic | Yes, completes Phase 4's narrowing story | Matches Python typing's narrowing |
| Unbox-on-assign TypeError | One branch per assignment; predictable, well-cached | Yes, no silent miscompile | Matches Pydantic/serde "fail fast on type mismatch" |

Not doing this means permanent boilerplate in every parser and a two-tier API forever.
Doing it is a bounded one-time compiler/runtime pass. Obvious call under #2.

---

## What this unlocks

### Positive

- `dict[str, Any]` works end-to-end. (Dragon Script) can return a normal
 dict like Python's `json.loads`. Same for any future TOML/YAML/msgpack
 parser.
- `Optional[T]` becomes usable in class fields and dict values.
- `isinstance` narrowing is consistent across params, locals, subscripts, and
 field reads.
- One canonical heterogeneous-value carrier (`%dragon.box`) instead of
 per-stdlib invention.
- Unblocks 's natural API (`drs.loads(body) -> dict`), 's manifest
 schema flexibility, and any future REST/RPC client work.

### Negative

- Adds ~1,500-1,750 LOC across the compiler and runtime. One-time cost.
- The `%dragon.box` representation is twice the size of a raw i64 - but
 this only applies to Any-typed slots; monomorphic dicts/lists/locals stay
 at native sizes.
- Codegen at every subscript / field / iteration site grows a compile-time
 branch ("is the container value-type Any/Union?"). Runtime hot path is
 unchanged for monomorphic accesses; codegen complexity is real.
- `Foo | None` users (`stdlib/http/server.dr` and others) keep their
 zero-cost niche-ptr encoding - this decision must preserve it explicitly
 in Phase 6.

### Neutral

- The decision deliberately keeps the hot path (monomorphic typed dicts/lists)
 unchanged. Existing code is unaffected at runtime.

---

## Open Questions

1. Should `Any` print include a type prefix (e.g., `Any(42)` vs just `42`)?
 **Resolved:** No - match Python's `print(x)`.
2. ~~Should `Any` participate in `==` comparisons by tag-then-payload?~~
 **Resolved:** Yes, made a phase (Phase 10). Tag-first, then payload by
 tag-appropriate operation. Different tags → false.
3. Should `dict[str, Any]` writes also accept any source type without
 annotation gymnastics? (E.g., `cfg["x"] = some_class_instance`.)
 **Tentative answer:** Yes - the existing tagged-set path already handles
 ptr-typed sources by `inferPtrValueTag`. Verify it covers class instances
 during Phase 2 implementation.
4. Backward compatibility: are there existing tests that depend on
 `typeExprToTypeKind("Any") -> Int`?
 **Action item:** grep + run full test suite as part of Phase 1.
5. `dict[Any, V]` and `dict[Any, Any]` - explicit non-goal for v1.
 Requires `hash(box)` and `__eq__(box)` plus the int-keyed dispatch path.
 **Action item:** add the non-goal note to Phase boundaries; revisit in a
 follow-up ADR if a use case emerges.
6. (generational GC) is not in-tree (`GCMode` is `None | RC` only -
 `Driver.cpp:585`). When tracing GC lands, box payloads that hold
 pointers (TAG_STR/LIST/DICT/CLASS/BYTES) must be visible to the root
 scanner.
 **Action item:** mentions this in Box ABI; the actual GC root
 metadata emission is 's problem to solve. Reference 's tag
 taxonomy when designs the masking.

---

## Relationship to

 (Dragon Script) was the originating use case - its initial draft assumed
`dict[str, Any]` worked and had to retreat to schema-class binding when smoke
tests showed it didn't. Once lands, 's revision will restore the
natural `drs.loads(body) -> dict` API alongside the `drs.parse(Schema, body)`
fast path. The schema-bound form remains a strict speedup for code that knows
the shape; the dict form is the Pythonic default.
