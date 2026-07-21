# Decision 009: `const`, `static`, and `self` Constructors

Done.

Three `.dr`-only features that make Dragon feel like Dragon instead of Python with braces stapled on: **`const`** for compile-time immutable bindings, **`static`** for explicit static fields/methods on classes, and **`self` constructors** for named constructors with overloading. Python's `SCREAMING_CASE` constants are convention-only and unenforced; `@staticmethod` is ceremony for a common pattern; you can't overload `__init__` without factory hacks. `.dr` already has braces and implicit self - these three finish the picture.

## Design

### `const`
```dragon
const MAX_HP: int = 100
const pi: float = 3.14159 // any casing allowed
```
- Compile error on reassignment (Sema-enforced)
- Must have initializer
- Works at all scopes: module, function, class (with `static`)
- Shallow immutablity (rebinding prevented, not deep freeze)

### `static`
```dragon
class Config {
 static count: int = 0
 static const VERSION: int = 1

 static def create -> Config {
 return Config
 }
}
```
- Static fields → LLVM global variables
- Static methods → functions without implicit self
- Accessed via `ClassName.field` / `ClassName.method`

### `self` constructors
```dragon
class Point {
 self(x: int, y: int) {
 self.x = x
 self.y = y
 }

 self(xy: int) { // overloaded by arity
 self.x = xy
 self.y = xy
 }
}
```
- `self` replaces `def __init__` in `.dr` mode
- Multiple `self` blocks → overloaded constructors
- Resolved by argument count at compile time
- `def __init__` still works (backward compatible)

## `.py` Mode

Completely unaffected. `const` and `static` lex as identifiers. Python syntax preserved:
- `__init__` with explicit `self`
- `@staticmethod` decorator
- `SCREAMING_CASE` convention for constants

## Implementation

Three phaes, each shippable on its own:
1. Phase 1: `const` - Token + AST flag + Sema enforcement (~10 tests)
2. Phase 2: `static` - Token + AST flags + CodeGen globals/methods (~10 tests)
3. Phase 3: `self` - Parser soft keyword + CodeGen multi-constructor dispatch (~12 tests)

See plan file for full implementation details.
