# Decision 045: Enforced Member & Module Privacy via Leading-Underscore Convention

Done. Leading underscore actually means something in Dragon - compile-time access control, not advisory naming. Python can't enforce this (`_x` is a hint, `__x` is mangling you can bypass, `__x__` is protocol). Dragon can. Same move as mandatory types: Python surface, compiler makes the discipline real. All in Sema/TypeChecker, zero codegen/runtime cost.

Covers what privacy means here, why Dragon diverges from Python, and the implementation spec (rule table, three enforcement points, reserved dunders, errors, stdlib migration, tests).

The rule, in one table (applies identically in `.dr` and `.py` - surface ≠ semantics):

| Name shape | Class member | Module-level name |
|---|---|---|
| `name` | **public** | **public** (exportable / importable) |
| `_name` | **protected** - any code in the **same package**, **plus subclasses anywhere** | **package-internal** - importable within the package, not from another package |
| `__name` (no trailing `__`) | **private** - declaring class **only** (same-package code and subclasses cannot see it) | **file-private** - not importable even by sibling modules in the same package |
| `__name__` (trailing dunder) | **reserved** - must be a recognized special method (else compile error); recognized ones are **public** | **reserved** - must be recognized metadata (`__all__`, `__version__`, `__doc__`, `__author__`, …); recognized ones are **public** |

The two tiers are deliberately *soft* (`_`) and *hard* (`__`). **The package is the encapsulation unit
for `_`** (Go's model: tightly-coupled types in a package collaborate through their internals without
exposing them to the world). `__` is the *strict* tier - declaring-class-only for members,
declaring-file-only for module names - with **no escape hatch** (decision 3 below applies to `__`).

Three decisions were locked by the maintainer and are **not** re-litigated below:

1. **Trailing `__x__` is reserved + public, and unrecognized dunders are forbidden.** A leading `__`
 triggers privacy **only when there is no trailing `__`**. `__x__` is a *different namespace* - the
 special-method + metadata surface (`__init__`, `__enter__`, `__str__`, `__doc__`, `__all__`, …) - and
 that surface must stay visible or the language's own syntax breaks (see Decision §"Why dunders are
 public"). To prevent a "private-looking but actually public `__secret__`" wart, **defining an
 unrecognized `__x__` is a compile error** - Python *advises* "never invent dunder names"; Dragon
 *enforces* it. Net: the only `__x__` names that exist are the language's own, which are legitimately
 public.
2. **Classes + modules enforced together**, in one pass.
3. **`__` is strict - no escape hatch.** The *private* tier has no `friend`, no reflection back door, no
 same-package relaxation: a `__x` member is reachable only from inside its declaring class, a `__x`
 module name only from inside its declaring file. This is *more* restrictive than Python and than
 C++/Java `private`, by design. **`_` is the soft tier**: a `_x` member is reachable from any code in
 the same package (regardless of class) and from subclasses anywhere - this is what lets collaborating
 types in a package (a server reaching into its router/response) share internals *without* exposing
 public API. The escape-hatch question is therefore answered per-tier, not globally: `_` *is* the
 "package-visible" relaxation; `__` has none.

## Context / Motivation

Today Dragon enforces **nothing**. Verified across every surface:

| Surface | Today |
|---|---|
| `obj._x` / `obj.__x` from outside the class | allowed - `TypeChecker::visit(AttributeExpr)` does a bare `cls->fields.find` / `methods.find`, no visibility guard |
| `__x` → `_Class__x` name mangling | not done - the only mangling in the compiler is `main` → `_dragon_user_main` |
| `from mod import _helper` | allowed - `getExports` exports everything; no `__all__`, no filter (stdlib ships importable `_encode_into`/`_decode_with` in `base64.dr`) |
| `__x__` dunders | special-cased for *dispatch* (`__init__`, `__enter__`, `__exit__`, `__call__`, `__doc__`) but never privatized, mangled, or validated against a recognized set |

So leading underscores are pure documentaton. Dragon's entire identity is "Python where the compiler
enforces the discipline the convention only gestures at" - and encapsulation is the same move as
mandatory typing. Because the check is static-only it carries no speed cost, so the only real question
is *parity vs. principled divergence*, and the project has already chosen principled divergence on the
same axis (block scoping, `:` declares / `=` reassigns, mandatory types).

## Options considered

| Option | Verdict |
|---|---|
| **A. Convention only (status quo / Python).** Underscores are advisory; the compiler enforces nothing. | Rejected. Wastes Dragon's static nature; "consenting adults" is a *dynamic*-language compromise, not a design goal. |
| **B. Name mangling (Python `__x` → `_Class__x`).** | Rejected. Mangling is *obfuscation, not enforcement* - `obj._Class__x` still reaches the field. It also pollutes symbol names for no speed benefit. The maintainer asked for "full private, not name mangling." |
| **C. Keyword modifiers (`private`/`protected`, C++/Java style).** | Rejected. Adds surface syntax and forks `.dr`/`.py`; Dragon already *has* a privacy signal (the convention) - enforce that instead of bolting on keywords. |
| **D. Enforced underscore convention (this ADR).** Static-only, no new syntax, no mangling, no runtime cost. | **Chosen.** Reuses the signal Python programmers already write, turns it into a real guarantee, and stays free under #1. |
| **D′. …with `__x__` also private (maintainer's first instinct).** | Rejected after analysis - see "Why dunders are public." Folded into D with the reserved-namespace carve-out + forbid-unknown rule. |

## Decision

### The rule, precisely

**Access context.** Privacy is evaluated against two facts about the access site: its **enclosing class**
(if any) and its **package**.
- *Enclosing class*: inside a method/constructor/`@staticmethod`/`@classmethod` of class `C`, the context
 class is `C`; a free function or module top-level body has **no** context class.
- *Package*: the package directory the access site's source file belongs to (Dragon packaging:
 `name/name.dr` + sibling modules under `name/`). A flat single-file module is its own singleton
 package. This is the unit for the `_` tier.

**Member access (`base.member`, both read and write).** Let `D` = the class that *declares* `member`,
defined in package `P_D`. Let the access site be in package `P_use` with optional context class `C`.
- `member` is public → always allowed.
- `member` is `_protected` → allowed iff **`P_use == P_D`** (same package - no context class required, so
 a free function or a *sibling* class in the package qualifies) **or** `C` exists and `C == D` or `C` is
 a subclass of `D` (subclass anywhere, walking 's class table).
- `member` is `__private` → allowed iff context class `C` exists and `C == D` **exactly**. Same-package
 code and subclasses **cannot** see a base class's private member (stricter than Java `private`'s
 package-default, by design - decision 3). A subclass redeclaring `__x` gets its *own* distinct private
 slot - no collision, no override.
- `member` is reserved `__x__` → public (allowed); see below.

**Module member access** - both `from mod import name` *and* qualified `mod.name`:
- `name` public or reserved-metadata `__x__` → allowed.
- `name` is `_x` → allowed iff the access site is **in the same package** as `mod`; another package is a
 compile error.
- `name` is `__x` → allowed **only within `mod`'s own file**; even a sibling module in the same package
 importing it is a compile error.
- (Within-file use of either is unaffected - only the cross-file / cross-package boundary is policed.)

**Self/cls.** `self._x` / `self.__x` / `cls.__y` inside a method of `C` resolve with context `C`, so
they always pass for members `C` declares (and protected members `C` inherits). This is the common case
and costs one hierarchy check.

### Why dunders are public (the carve-out)

`__x__` is not "extra-private" - it is the language's **reserved protocol + metadata namespace**, the
direct analog of C++ `operator==` / `operator` / `~T`: *special* members that are nonetheless
**public interface**. Privatizing them breaks the language's own syntax and produces an incoherent
caller asymmetry:

```python
with conn: # compiler synthesizes conn.__enter__/__exit__ - works
 ...
s: str = str(obj) # compiler synthesizes obj.__str__ - works
t: str = obj.__str__ # would be a compile error if __str__ were private - but it is the SAME method
print(obj.__doc__) # introspection - would break if __doc__ were private
```

The compiler can always call protocol dunders (it emits the call from *inside* the protocol), so making
them "private" would mean *the compiler may call them but the user may not* - privacy the enforcer
itself ignores. Therefore protocol/metadata dunders are public. The wart the maintainer rightly worried
about ("then is `__secret__` silently public?") is closed by **forbidding unrecognized dunders**: you
cannot *define* a non-protocol `__x__`, so no private-looking-but-public name can exist.

### The reserved-dunder set (`isReservedDunder`)

A single canonical predicate, consolidated from the existing dispatch sites
(`codegen/ImplInit.cpp` dunder detection, `codegen/CallExpr.cpp` protocol calls,
`TypeChecker.cpp:1283` `__doc__`). Defining a `__x__` name (class member **or** module-level) not in
this set is a compile error: *"`__foo__` is not a recognized special method or metadata name;
double-underscore-dunder names are reserved ."* The set = **exactly the dunders Dragon actually
dispatches or exposes** - extending it is a deliberate edit, not an accident:

- Lifecycle / construction: `__init__`
- Representation: `__str__`, `__repr__`
- Comparison: `__eq__`, `__ne__`, `__lt__`, `__le__`, `__gt__`, `__ge__`, `__hash__`
- Arithmetic / bitwise (only those with codegen): `__add__`, `__sub__`, `__mul__`, `__truediv__`,
 `__floordiv__`, `__mod__`, `__pow__`, `__neg__`, `__and__`, `__or__`, `__xor__`, `__lshift__`,
 `__rshift__`, `__invert__` (+ in-place/reflected variants **iff** Dragon dispatches them - gate on the
 actual table, do not pre-list aspirational ones)
- Container protocol: `__len__`, `__getitem__`, `__setitem__`, `__delitem__`, `__contains__`,
 `__iter__`, `__next__`
- Callable / context: `__call__`, `__enter__`, `__exit__`
- Conversion: `__bool__`, `__int__`, `__float__`
- Introspection (compiler-provided): `__doc__`, `__class__`, `__name__`
- Module metadata: `__all__`, `__version__`, `__author__`, `__doc__`

> **Single source of truth.** `isReservedDunder` must be derived from / kept in lockstep with the
> dispatch tables - a dunder that codegen handles but the predicate rejects would make valid programs
> uncompilable, and vice-versa. Add a test that asserts every dispatched dunder is reserved.

### Where it is enforced (three points, all pre-codegen)

1. **Member access - `TypeChecker::visit(AttributeExpr)`** (`src/TypeChecker.cpp:1276`). After the
 existing field/method resolution succeeds, before returning the member type, apply the member-access
 rule using (a) the declaring class `D` and *its* package `P_D` (the package of `D`'s defining
 module - already resolvable from the class's module), (b) the access site's package `P_use` and
 context class `C` (thread a `currentClass` + the current module's package through the TypeChecker),
 and (c) for a module base, the "`_x` ⇒ same-package, `__x` ⇒ same-file" branch. Covers reads, writes,
 and qualified `mod.name`. Each check is a package-equality test plus, for `_`, an optional
 subclass-walk - all O(hierarchy depth), compile-time.
2. **Imports - `TypeChecker::visit(FromImportStmt)` / `visit(ImportStmt)`** (`src/TypeChecker.cpp:2551`).
 Reject importing a `_x` name from a *different package*, or a `__x` name from a *different file*,
 before binding it (reserved `__x__` metadata always allowed).
3. **Declaration-time dunder validation - Sema (or TypeChecker) at class-member and module-top-level
 declaration.** Reject any `__x__` not in `isReservedDunder`.

`getExports` (`src/TypeChecker.cpp:384`) may *additionally* drop module-private names from the export
map as a defense-in-depth measure, but the authoritative rejection is at the import site (so the error
points at the offending `import`, not silently hides the name).

> **Scope of the rule.** It applies to **class members** and **module top-level names** only. Locals and
> parameters already have lexical (block) scoping; a leading underscore there is just an "unused"
> marker and is untouched. Single `_` (wildcard) is unaffected.

### Error messages (Dragon style - name the rule, the kind, and the context)

- `"_routes_by_sd" is protected; accessible only within Router and its subclasses (accessed from free function dispatch_request) []`
- `"__compress" is private to TarFile; subclasses and outside code cannot access it []`
- `"_encode_into" is module-private to base64; it cannot be imported (remove the leading underscore to export it) []`
- `"__frobnicate__" is not a recognized special method; double-underscore-dunder names are reserved []`

## What users and stdlib authors get

**Positive.**
- Real encapsulation Python structurally cannot provide - a true selling point in the same family as
 mandatory typing, at **zero runtime cost** (static-only).
- Refactor saftey: renaming/removing a `__private` field is now provably local to its class.
- `_`/`__` module helpers become *guaranteed* internal - `__all__` becomes unnecessary as an export
 filter (it remains valid metadata).
- Identical in `.dr` and `.py` (surface ≠ semantics). Note this *does* mean a `.py` file that pokes
 `obj._x` cross-class compiles under CPython but errors under Dragon - consistent with Dragon's "typed
 Python subset, stricter" positioning, not a regression.

**Costs / migration (near-zero for today's stdlib - the package-visible `_` tier is why).** The audit
found **no** cross-package `from X import _y` leaks. The intra-module member accesses it *did* find are
all **single-underscore, same-package** - and therefore **legal under ** (the `_` tier is exactly
package-visible). They need no change:

| Site | Access | Status under |
|---|---|---|
| `stdlib/tarfile.dr:505,514` | `tf._compress = …`, `tf._load_members(raw)` from the module-level `open` factory (same file) | done legal - `_x` is package-internal |
| `stdlib/tomllib.dr:322` | `doc._set(full_key, value)` (same file) | done legal |
| `stdlib/http/server.dr:1089-1481` | dispatch loop reads `router._routes_by_sd`, `router._before_by_sd`, `res._deferred`, `res._aborted` (same package) | done legal - *this* is the case the package tier exists for; no public-accessor bloat, no `friend` |

This validates the package-as-encapsulation-unit choice: the model the convention already implied is
also the one that keeps tightly-coupled stdlib types working without leaking internals to the world.

**Residual audit to run before landing.** The only thing that *can* break is a **cross-class
double-underscore (`__x`) access** or a **cross-package `_x` import** - neither showed up, but the
member-access grep (`\.[_][A-Za-z]`) does not catch `.__x`. Re-audit with a `__`-aware sweep across
`stdlib/` and `test/dr/` and fix any genuine hits at the root (#2 - public API or relocate, never loosen
the rule). Expectation from the data so far: a handful at most.

**Sharp edges to document for users.**
- `__x` (private) is **declaring-class-only** - neither a subclass nor a sibling class in the same
 package can touch it. Use `_x` (protected) when same-package code or subclasses need it.
- The two tiers split along a clear line: **`_x` = "internal to this package"**, **`__x` = "internal to
 this class/file."** Reach for `_` by default for cross-class collaboration within a unit; reach for
 `__` only when even same-package code must be locked out.
- Module names: `_x` is shared across the package's files but never importable from another package;
 `__x` is locked to its own file even from package siblings. Promote to a public name to cross a package
 boundary.

## Test plan

- **EnforcerTests / SemaTests / TypeCheckerTests** (IR-free, fast):
 - protected (`_x`): same-package free-function and sibling-class access OK; subclass-in-another-package
 access OK; unrelated class in another package rejected.
 - private (`__x`): declaring-class OK; subclass, same-package sibling class, and outside all rejected;
 a subclass redeclaring `__x` is a distinct slot.
 - self/cls access of own + inherited-protected members passes.
 - module names: `from sibling import _x` within the package OK; from another package rejected; `__x`
 rejected even from a package sibling; `m._x` qualified access mirrors the same rules.
 - reserved dunder: defining `__frob__` rejected; every dispatched dunder (`__init__`, `__str__`,
 `__enter__`, …) accepted; `obj.__doc__` / `obj.__str__` explicit access allowed.
 - regression: assert `isReservedDunder` ⊇ every dunder codegen dispatches.
- **test/dr/*.dr** behavioral suite (auto-registered ctest): one `unittest`-style file proving a
 `__balance` field is unreachable from outside, a `_protected` helper reaches subclasses, a
 module-private helper isn't importable, and protocol dunders still drive `with`/`str`/`len`.
- **Stdlib green-build gate:** the tarfile/tomllib/http-server migrations above land in the same change;
 `ctest` must stay green.
