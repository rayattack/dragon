# Decision 047: General Call-Site Spread (`*tuple` / `*list` / `**dict`)

Done. `*tuple`/`*list`/`**dict` into free functions, class constructors, and instance/static methods, incl. virtual dispatch and vararg-param forwarding. C9-A rejected non-TypedDict spread at check time; this is C9-B, the real arg-lowering work. Constraint I cared about: spread must not slow normal calls; pay only when you write `*`/`**`.

`f(*positional, **keyword)` spread now lowers into calls. Three tiers; cost is explicit (normal calls stay byte-identical IR):

| Form | Lowering | Cost | Failure mode |
|---|---|---|---|
| `*tuple` (arity known - `TupleType` carries length + per-elem types) | compile-time arity match; per-element typed `dragon_tuple_get` loads straight into the arg slots | ~free; heterogeneous element types OK | **compile-time** arity / element-type error |
| `*list[T]` (runtime length) | one `dragon_list_len == R` check + raise; R typed `dragon_list_get_<variant>` loads | O(R) loads + 1 length check | **runtime** `TypeError` on length mismatch |
| `**dict[str,V]` | one `dragon_dict_has_key` + typed getter per bindable param + an unexpected-key validation pass | O(params) hash lookups | **runtime** `TypeError` on missing-required / unexpected key |

## Context / Motivation

C9-A rejected non-TypedDict spread at check time. C9-B is the real work: arg lowering on every call site.
Constraint: spread must not slow normal calls; pay only when you write `*`/`**`.

## Options considered

1. **Runtime-arity calling convention** (Python-style `PyObject* argv[]` + a trampoline). Rejected: it
 would impose an indirection and an arg-array build on *every* call to a spreadable function, or
 bifurcate every function into a fast and a slow entry - a standing tax that violates #1 and fights
 's monomorphization doctine.
2. **Route `*tuple` through the dynamic `*list` path** (one lowering for both). Rejected: a tuple's
 arity and per-element types are static, so it gets compile-time checking and zero runtime cost.
 Collapsing it into the list path would add a needless length check and erase the heterogeneous-type
 capability. `*tuple` keeps its own fast lowering.
3. **Fixed-arity expansion, opt-in cost (chosen).** Spread fires only on explicit `*`/`**` syntax. Each
 spread expands into a fixed number of LLVM args at compile time; a `*list`'s runtime length must
 match that fixed count (`len == R`, else raise). This keeps the call ABI unchanged and the cost
 local to spread sites.

## Decision

Implement option 3. A single expansion routine (`CodeGen::Impl::expandSpreadCallArgs`) is shared by the
free-function/ctor path (`emitSpreadCall` → `emitSpreadDispatch`) and both class-instance method
dispatch paths, so refcount discipline lives in exactly one place.

**Refcount:** spread elements are **borrowed** from their source container (the list/tuple/dict keeps
its `+1`); they are never added to `argTemps`. The callee increfs whatever it retains, exactly as for a
borrowed local passed as an argument. Verified UAF/double-free-free under AddressSanitizr with a
2000-iteration heap-string (`*tuple`/`*list`/`**dict`) stress into retaining and dropping callees.

**Vararg-param typing:** a `*args: T` parameter binds inside the body as `list[T]` and `**kw: T` as
`dict[str, T]` (their actual runtime shape - codegen packs them), enabling `len(args)`, iteration, and
list-only forwarding `inner(*args)`.

**Capability boundaries :**
- A **positional argument after `*`** (`f(*xs, y)`) - Python makes those keyword-only; rejected at
 `check`.
- `*list` **combined with keyword / `**dict` binding into a fixed-arity callee** - the list/dict split
 point is runtime-indeterminate, so a fixed arg count can't be emitted.
- Spread into a **`*args`/`**kwargs` (vararg) target**, a **closure/decorated callable**, or a
 **dynamic constructor** - deferred; each errors with "spread into this callable is not supported"
 rather than miscompiling.

## What callers should know

- Normal (non-spread) calls are byte-identical IR - verified by diffing pre/post-change LLVM IR for
 free-function, method, and ctor calls.
- The cost is documented (the table above is mirrored in the language reference) so writing `*`/`**` is
 an informed opt-in. Dragon has no warning channel today, so docs are the agreed mechanism; a
 non-fatal compiler note at `*list`/`**dict` sites is a possible future addition.
- `**dict` generalizes the `T(**row)` path; that TypedDict fast path (a single `dragon_dict_copy`)
 is unchanged and still wins for typed-row construction.
- Follow-ups (not blocking): vararg→vararg passthrough forwarding (pass the packed list/dict straight
 into the target's `*args`/`**kwargs`), and spread into closures/decorated callables (depends on the
 uniform-callable representation).
