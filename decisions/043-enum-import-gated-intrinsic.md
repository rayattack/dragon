# Decision 043: Class-Based `enum` as an Import-Gated Compiler Intrinsic

Done. Native `enum Name { A, B, C }` already exists - fast int constants, no `.name`/`.value`/iteration. Python's `from enum import Enum` wants member *instances*. I'm adding class-based `Enum` / `IntEnum` / `StrEnum` + `auto` as an import-gated compiler intrinsic, same model as `from threading import Lock`. No import, no `Enum`.

The native `enum` keyword stays unchanged as the faster int-constant form. Two surfaces, both useful: keyword for hot-path/self-hosting token tags, class form for Python parity.

## Decision

- **Surface:** `class Color(Enum) { RED: int = 1; GREEN: int = 2; BLUE: int = auto }`
 (`.py` is indentation-equivalent). Members become singleton instances with `.name` and
 `.value`; `str(Color.RED)` → `"Color.RED"`; `for c in Color` iterates in definition
 order; `Color(2)` returns the member with that value (raises `ValueError` if none).
 `IntEnum`/`StrEnum` members compare by value (`Priority.LOW == 1`, `Mode.READ == "r"`,
 ordering); plain `Enum` compares by identity.
- **Import-gating:** `Enum`/`IntEnum`/`StrEnum`/`auto` are real (inert) declarations in
 `stdlib/enum.dr` and are **not** seeded as builtins, so omitting the import is a compile
 error (`undefined name 'Enum'`) - exactly like `Lock`.
- **Implementation = frontend synthesis (no runtime cost).** `synthesizeEnumMethods`
 (`src/codegen/Classes.cpp`), run before `synthesizeDataclassMethods` in
 `forwardDeclareClasses`, rewrites the class: drops the marker base, turns each member
 into a `static NAME: Color = Color("NAME", v)` singleton built once in `main`'s preamble,
 synthesizes `__init__(name, value)`/`__str__`/`__repr__`, a `__members__` list, and a
 `_lookup(value)` static method. Member access is then a global load; `==` is a pointer
 compare (plain) or an int/str compare (Int/Str). This mirrors the proven
 `@dataclass`/`NamedTuple` pathway and ports to `.dr` when the frontend self-hosts .

### Where each piece lives
- `stdlib/enum.dr` - inert marker classes + `auto` sentinel.
- `src/codegen/Classes.cpp` - `synthesizeEnumMethods` (the rewrite).
- `src/TypeChecker.cpp` + `include/dragon/TypeChecker.h` - `ClassType::isEnum`; members
 typed as `InstanceType(self)` with `.name: str` / `.value: int|str` (CodeGen trusts these
 types, so without this `Color.RED` mis-lowers to an int read).
- `src/codegen/ForLoop.cpp` - `for c in Color` rewrites to iterate `Color.__members__`.
- `src/codegen/CallExpr.cpp` - `Color(v)` (arity 1) redirects to `Color._lookup(v)`.
- `src/codegen/Expressions.cpp` - IntEnum/StrEnum comparison wraps an enum operand to
 `operand.value`, so the normal int/str compare handles member-vs-scalar and
 member-vs-member with no boxing.

## Options Considered

- **Pure `.dr` (`class Color(Enum)` with a runtime metaclass).** Rejected: Dragon has no
 metaclass / class-body-namespace hook, so members can't be turned into instances from
 `.dr`. Would need a runtime metaclass - a dynamic-typing poison pill against #1.
- **Only enhance the native keyword.** Rejected: breaks line-for-line Python ports
 (`from enum import Enum`, `class X(Enum)` wouldn't work).
- **Frontend synthesis behind an import gate (chosen).** Same category as the shipped
 `@dataclass`/`NamedTuple` synthesis; compile-time, zero runtime cost, full parity.
- **Synthesized `__eq__(self, other: int)` for IntEnum.** Rejected: the binary-op dispatch
 has no rhs-type selection, so member-vs-member would pass a `Color*` where `int` is
 expected and corrupt the compare. The operand-wrap-to-`.value` rewrite is correct for both.

## What shipped (and what's still missing)

- `enum` (class form) unblocks the Python-parity surfaces that wanted real enums (`logging`
 levels, `http` status, `signal` numbers, `socket` families).
- Member access/`==`/iteration are loadss + pointer/int compares - no allocation per access,
 no boxing (#1 satisfied).
- Two enum surfaces coexsist by design (keyword = fast int constants; class = Python parity).
- **Known parity gaps (deferred, not bugs):** reflected scalar-on-left comparison
 (`1 == Color.RED`) does not dispatch (the binary-op path requires a pointer LHS);
 `Flag`/`IntFlag` (bitwise) are out of scope. Both documented; the common directions work.

## Verification

`test/dr/test_enum.dr` (auto-registered, 11 tests): member `.name`/`.value`, `str`,
`auto` incl. explicit-reset (`A=auto→1, B=10, C=auto→11`), iteration order,
`Color(2)` lookup + `ValueError`, identity equality, IntEnum int-compare + ordering,
StrEnum str-compare, native-keyword coexistence. `.py`-mode and import-gating
(`undefined name 'Enum'` without the import) verified manually. Full `ctest` 42/42 green -
no regressions.
