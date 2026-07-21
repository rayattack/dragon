# Decision 006: Implicit `self` in Dragon [Done]

Done. Scope: AST, Parser, Sema, TypeChecker, CEmitter (and LLVM path where applicable).

Python makes you write `self` on every method and I got tired of it in `.dr` files where we're already doing Dragon things not Python things:

```python
class Point:
 def __init__(self, x: int, y: int) -> None:
 self.x = x
 self.y = y

 def distance(self) -> float:
 return (self.x ** 2 + self.y ** 2) ** 0.5
```

In Java, C#, JavaScript, TypeScript, Kotlin, and Swift, `this`/`self` is implicit - available inside methods without declaration:

```java
class Point {
 int x, y;
 Point(int x, int y) { this.x = x; this.y = y; }
 double distance { return Math.sqrt(this.x * this.x + this.y * this.y); }
}
```

Dragon should do what Java/C#/TS do: **implicit `self` in `.dr` files**, explicit `self` in `.py` files for Python compatibility.

**Strict:** explicit `self` in a `.dr` method is a compile error, not a warning. Want Python-style `self`? Use a `.py` file. `.dr` files follow Dragon rules. One way.

---

## Design

### `.dr` files - Implicit `self` (strict)

```
class Point {
 def(x: int, y: int) -> None {
 self.x = x
 self.y = y
 }

 def distance -> float {
 return (self.x ** 2 + self.y ** 2) ** 0.5
 }

 def translate(dx: int, dy: int) -> None {
 self.x = self.x + dx
 self.y = self.y + dy
 }
}

p: Point = Point(3, 4)
print(p.distance) # 5.0
```

- `self` is NOT in the parameter list
- `self` is automatically available inside all instance methods
- `self.x` accesses instance fields
- Constructor parameters are just the "real" parameters (no `self` cluttering the signature)
- Method calls: `p.distance` - zero-argument, clean

### `.py` files - Explicit `self` (Python compatible)

```python
class Point:
 def __init__(self, x: int, y: int) -> None:
 self.x = x
 self.y = y

 def distance(self) -> float:
 return (self.x ** 2 + self.y ** 2) ** 0.5
```

- `self` IS the first parameter (standard Python)
- Everything works as Python developers expect
- Required for Python compatibility when importing `.py` files

### Both Compile to the Same C

```c
typedef struct {
 int64_t x;
 int64_t y;
} Point;

void Point___init__(Point* self, int64_t x, int64_t y) {
 self->x = x;
 self->y = y;
}

Point* Point_new(int64_t x, int64_t y) {
 Point* self = (Point*)malloc(sizeof(Point));
 Point___init__(self, x, y);
 return self;
}

double Point_distance(Point* self) {
 return pow((double)(self->x * self->x + self->y * self->y), 0.5);
}

void Point_translate(Point* self, int64_t dx, int64_t dy) {
 self->x = self->x + dx;
 self->y = self->y + dy;
}
```

The C output is identical regardless of whether the source was `.dr` (implicit) or `.py` (explicit). `self` always becomes the first C parameter.

---

## Current State

Today, `self` flows through the compiler as follows:

| Stage | Current Behavior |
|-------|-----------------|
| **Parser** | `self` is a regular parameter name, no special handling |
| **AST** | No `isMethod` or `isSelf` flag on `Parameter` or `FunctionDecl` |
| **Sema** | `self` is just another parameter in name resolution |
| **TypeChecker** | Defines `self` at class scope as `InstanceType(classType)`. Methods also receive it as explicit first parameter. |
| **CEmitter** | Skips parameter index 0 (assumed to be `self`), then explicitly adds `ClassName* self` in C output |

The CEmitter already stips `self` from the parameter list and re-adds it as a typed pointer. So this is mostly parser/AST work - backend's fine.

---

## Implementation Plan

### Current State (from code exploration)

Today `self` flows through the compiler with no special flags:

| Stage | File | Current Behavior |
|-------|------|-----------------|
| **AST** | `AST.h:481-490` | `FunctionDecl` has no `isMethod` or `hasImplicitSelf` flag |
| **Parser** | `Parser.cpp:925-938` | `classDeclaration` has no `inClassBody` tracking; methods parsed identically to functions |
| **Sema** | `Sema.cpp:477-512` | `self` is just another parameter - no special definition |
| **TypeChecker** | `TypeChecker.cpp:1270-1325` | `self` defined in class scope (line 1310) as `InstanceType(classType)`; BUT `self` is included in the `FunctionType` param list |
| **CEmitter** | `CEmitter.cpp:2160-2281` | Skips param index 0 (hardcoded `i = 1`) and re-adds `ClassName* self` as C parameter |

The CEmitter already reconstructs `self` - the transition is primarily a Parser + AST concern.

### Step 1: AST - Add flags to `FunctionDecl`

**File:** `include/dragon/AST.h` (line 488, after `bool isAsync = false;`)

```cpp
bool isMethod = false; // true if defined inside a class body
bool hasImplicitSelf = false; // true if self is implicit (.dr mode)
```

No changes needed to `Parameter` struct or `ClassDecl`.

### Step 2: Parser - Track class context, enforce implicit self

**File:** `src/Parser.cpp`

**2a.** Add `bool inClassBody = false;` to the parser's `Impl` struct.

**2b.** In `classDeclaration` (line 925), wrap `parseBlock`:

```cpp
bool savedInClass = impl_->inClassBody;
impl_->inClassBody = true;
decl->body = parseBlock;
impl_->inClassBody = savedInClass;
```

**2c.** In `functionDeclaration` (before `decl->body = parseBlock` at line 921):

```cpp
if (impl_->inClassBody) {
 decl->isMethod = true;
 if (impl_->options.isDragonFile) {
 decl->hasImplicitSelf = true;
 // Strict: explicit self in .dr is a compile error
 if (!decl->params.empty && decl->params[0].name == "self") {
 error("'self' is implicit in Dragon methods. Remove it.\n"
 " Write: def " + decl->name + "(...) -> ...");
 }
 }
}
```

### Step 3: Sema - Define implicit `self` in method scope

**File:** `src/Sema.cpp`, `visit(FunctionDecl&)` (line 490, after `pushScope`)

```cpp
if (node.isMethod && node.hasImplicitSelf) {
 Symbol selfSym;
 selfSym.name = "self";
 selfSym.kind = Symbol::Kind::Parameter;
 selfSym.declaration = node.location;
 selfSym.isInitialized = true;
 currentScope->define(selfSym);
}
```

Ensures `self.x` resolves during name resolution for implicit-self methods.

### Step 4: TypeChecker - Exclude `self` from FunctionType

**File:** `src/TypeChecker.cpp`

**4a.** `visit(FunctionDecl&)` (line 1270): When building `FunctionType`, skip `self` for BOTH modes. The `FunctionType` should never include `self` - callers write `p.distance`, not `p.distance(p)`.

```cpp
for (size_t i = 0; i < node.params.size; ++i) {
 // In .py mode, skip explicit self from FunctionType
 if (node.isMethod && !node.hasImplicitSelf && node.params[i].name == "self")
 continue;
 paramTypes.push_back(resolveType(node.params[i].type.get));
}
```

For implicit-self methods, `self` isn't in `params` at all, so no skipping needed.

**4b.** In the same function, after `pushScope`: define `self` for implicit-self methods by looking up the class scope's `self`:

```cpp
if (node.isMethod && node.hasImplicitSelf) {
 auto selfType = impl_->lookup("self"); // From ClassDecl scope (line 1310)
 if (selfType) impl_->define("self", selfType);
}
```

**4c.** When defining params in scope, also skip `self` for .py mode:

```cpp
for (size_t i = 0; i < node.params.size; ++i) {
 if (node.isMethod && !node.hasImplicitSelf && node.params[i].name == "self")
 continue;
 impl_->define(node.params[i].name, paramTypes[paramIdx++]);
}
```

**4d.** `visit(ClassDecl&)` (line 1299) - no changes. Already defines `self` at class scope.

### Step 5: CEmitter - Use `paramStart` instead of hardcoded `1`

**File:** `src/CEmitter.cpp`, `visit(ClassDecl&)` (lines 2160-2281)

Replace all `for (size_t i = 1; ...)` loops with:

```cpp
size_t paramStart = func->hasImplicitSelf ? 0 : 1;
for (size_t i = paramStart; i < func->params.size; ++i) { ... }
```

This affects 6 loops:
- `__init__` declaredVars (line 2171)
- `__init__` param list (line 2176)
- Constructor params (line 2202)
- Constructor `__init__` call args (line 2221)
- Regular method declaredVars (line 2238)
- Regular method param list (line 2247)

No changes to `visit(FunctionDecl&)` (top-level functions) or `visit(CallExpr&)` (method dispatch already works).

### Step 6: Tests

**Update existing tests** (all use explicit self in `.dr` mode - must remove `self` from params):
- `CEmitterTest.cpp`: `ClassDecl` (line 379), `ClassConstructorCall` (line 442), `ClassConstructorUsesArena` (line 1247)
- `TypeCheckerTest.cpp`: `ClassDecl` (line 671)
- `ParserTest.cpp`: `ClassWithMethods` (line 1263)

**New tests (~13):**

| Suite | Test | Verifies |
|-------|------|----------|
| ParserTest | `ImplicitSelfMethod` | Method in `.dr` class → `isMethod=true`, `hasImplicitSelf=true`, no self in params |
| ParserTest | `ExplicitSelfInDragonIsError` | `def foo(self)` in `.dr` class → parser error |
| ParserTest | `ExplicitSelfInPyModeOk` | `def foo(self)` in `.py` class → `hasImplicitSelf=false`, no error |
| ParserTest | `TopLevelFunctionNotMethod` | `def foo` outside class → `isMethod=false` |
| ParserTest | `SelfUsableOutsideClass` | `self: int = 42` outside class → no error |
| SemaTest | `ImplicitSelfResolvesInBody` | `self.x` inside implicit-self method resolves |
| SemaTest | `ImplicitSelfNotLeakedOutside` | `self` not visible outside method scope |
| TypeCheckerTest | `ImplicitSelfMethodType` | FunctionType param count excludes self |
| TypeCheckerTest | `ImplicitSelfFieldAccess` | `self.x` type checks correctly |
| TypeCheckerTest | `ExplicitSelfPyModeType` | `.py` mode FunctionType also excludes self |
| CEmitterTest | `ImplicitSelfClassEmission` | Struct + constructor + method emit correctly |
| CEmitterTest | `ImplicitSelfMethodParams` | C output has `ClassName* self` despite no self in source |
| CEmitterTest | `ImplicitSelfE2E` | Compile and run class with implicit self |

---

## Edge Cases

### Static Methods

Dragon uses `@staticmethod` - no `self` at all:

```
class MathUtils {
 @staticmethod
 def add(a: int, b: int) -> int {
 return a + b
 }
}

# Called as:
MathUtils.add(1, 2)
```

In `.dr` mode, the parser sees `@staticmethod` and does NOT inject implicit `self`. The function remains a regular function that happens to live inside a class namespace.

Implementation: when `@staticmethod` decorator is present, set `isMethod = false` even though the function is inside a class body.

### Class Methods

```
class Counter {
 count: int = 0

 @classmethod
 def increment(amount: int) -> None {
 cls.count = cls.count + amount # .dr mode: cls is implicit
 }
}
```

In `.py` mode: `def increment(cls, amount: int)` - explicit `cls`.
In `.dr` mode: `cls` is implicit, like `self`.

Implementation: `@classmethod` sets `hasImplicitCls = true` in `.dr` mode. The first parameter is the class type, not an instance.

### Calling Conventions Across File Boundaries

A `.dr` file importing a `.py` class should work seamlessly:

```python
# utils.py
class Helper:
 def __init__(self, name: str) -> None:
 self.name = name

 def greet(self) -> str:
 return "Hello from " + self.name
```

```
// main.dr
from utils import Helper

h: Helper = Helper("Dragon")
print(h.greet) // Works - caller never sees self
```

This works because:
1. ModuleResolver parses `utils.py` with explicit self
2. TypeChecker builds `FunctionType` for `greet` with **zero parameters** (self excluded from type)
3. CEmitter emits `Helper_greet(h)` - self is always the implicit first C argument
4. The calling convention is identical regardless of source mode

### `self` Used as a Regular Variable Name

What if someone writes `self = 42` outside a class?

```
self: int = 42 # Legal in .dr - self is only special inside class methods
```

`self` is only special when `isMethod` is true. Outside class bodies, it's a regular identifier. This matches Python's behavior (you can use `self` outside classes, it's just convention).

### Explicit `self` in `.dr` Files - Compile Error

Writing `self` explicitly in a `.dr` method is a **hard error**:

```
class Foo {
 def bar(self, x: int) -> int { # ERROR
 return self.x + x
 }
}
```

```
DRAGON SCALE ERROR at <file.dr:2:13>:
 | def bar(self, x: int) -> int {
 | ^^^^
 'self' is implicit in Dragon methods. Remove it from the parameter list.
 Write: def bar(x: int) -> int
 For explicit self, use a .py file instead.
```

**Rationale:** Dragon is v0.2.0 with no external users. There is no codebase to migrate. Being strict from day one establishes a clean convention before anyone has habits to break. Accepting both forms would lead to inconsistent codebases - some files with `self`, some without - which makes the language feel unfinished.

The escape hatch is `.py` files. If someone wants to paste Python code with explicit `self`, they save it as `.py` and the bilingual compiler handles it. Dragon files follow Dragon conventions.

### Properties

When `@property` is eventually implemented:

```
class Circle {
 def(radius: float) -> None {
 self.radius = radius
 }

 @property
 def area -> float {
 return 3.14159 * self.radius ** 2
 }
}

c: Circle = Circle(5.0)
print(c.area) # Access as field, not method call
```

Properties are zero-parameter methods with implicit `self` - they already fit the implicit model perfectly.

### Inheritance and `super`

When inheritance lands:

```
class Animal {
 def(name: str) -> None {
 self.name = name
 }

 def speak -> str {
 return self.name + " makes a sound"
 }
}

class Dog(Animal) {
 def(name: str, breed: str) -> None {
 super(name) # delegates to Animal's constructor; self is implicit
 self.breed = breed
 }

 def speak -> str {
 return self.name + " barks"
 }
}
```

In `.dr`, `super(args)` delegates to the parent's constructor (the nameless `def`) and `super.method` calls a parent method; both bind the current `self` implicitly, so it never appears in the argument list. (In `.py` mode the Python proxy forms `super.__init__(name)` / `super.method` are used instead.)

---

## What This Enables

Implicit `self` isn't just sugar - it's the foundation for a clean Dragon-native object model. Here's the progression:

### Stage 1: Implicit Self (This Decision)

```
class Point {
 def(x: int, y: int) -> None {
 self.x = x
 self.y = y
 }
 def distance -> float {
 return (self.x ** 2 + self.y ** 2) ** 0.5
 }
}
```

Changes: Parser, AST flags, Sema/TypeChecker/CEmitter adjustments.

### Stage 2: Field Declarations

Instead of extracting fields from `__init__` body (current fragile approach), declare fields explicitly:

```
class Point {
 x: int
 y: int

 def(x: int, y: int) -> None {
 self.x = x
 self.y = y
 }
}
```

This gives the compiler a complete field manifest at parse time. CEmitter can generate the struct without scanning `__init__` body. TypeChecker can validate field access statically.

### Stage 3: Dunder Protocols

With implicit self, dunders read cleanly:

```
class Vector {
 x: float
 y: float

 def(x: float, y: float) -> None {
 self.x = x
 self.y = y
 }

 def __str__ -> str {
 return f"Vector({self.x}, {self.y})"
 }

 def __len__ -> int {
 return 2
 }

 def __add__(other: Vector) -> Vector {
 return Vector(self.x + other.x, self.y + other.y)
 }

 def __eq__(other: Vector) -> bool {
 return self.x == other.x and self.y == other.y
 }
}

v1: Vector = Vector(1.0, 2.0)
v2: Vector = Vector(3.0, 4.0)
print(v1 + v2) # Vector(4.0, 6.0) - via __add__
print(len(v1)) # 2 - via __len__
print(v1 == v2) # False - via __eq__
```

Compare to Python where every dunder repeats `self`:
```python
def __add__(self, other: Vector) -> Vector:
 return Vector(self.x + other.x, self.y + other.y)
```

Dragon is cleaner - `other` is the only parameter because `self` is always there.

### Stage 4: Single Inheritance + super

```
class Shape {
 color: str

 def(color: str) -> None {
 self.color = color
 }

 def area -> float {
 return 0.0
 }
}

class Circle(Shape) {
 radius: float

 def(radius: float, color: str) -> None {
 super(color)
 self.radius = radius
 }

 def area -> float {
 return 3.14159 * self.radius ** 2
 }
}
```

CEmitter approach:
- `Circle` struct embeds `Shape` as first field (C struct inheritance)
- `super(color)` → `Shape___init__((Shape*)self, color)`
- Method dispatch: compiler knows the static type, emits direct calls
- Virtual dispatch (if needed later): vtable pointer in struct

### Stage 5: Decorators (@property, @staticmethod, @classmethod)

```
class Temperature {
 _celsius: float

 def(celsius: float) -> None {
 self._celsius = celsius
 }

 @property
 def fahrenheit -> float {
 return self._celsius * 9.0 / 5.0 + 32.0
 }

 @staticmethod
 def from_fahrenheit(f: float) -> Temperature {
 return Temperature((f - 32.0) * 5.0 / 9.0)
 }

 @classmethod
 def absolute_zero -> Temperature {
 return Temperature(273.15)
 }
}

t: Temperature = Temperature(100.0)
print(t.fahrenheit) # 212.0 (property access)
t2: Temperature = Temperature.from_fahrenheit(72.0) # static method
t3: Temperature = Temperature.absolute_zero # class method
```

### Stage 6: Iterator Protocol

```
class Range {
 current: int
 stop: int

 def(start: int, stop: int) -> None {
 self.current = start
 self.stop = stop
 }

 def __iter__ -> Range {
 return self
 }

 def __next__ -> int {
 if self.current >= self.stop {
 raise StopIteration
 }
 val: int = self.current
 self.current = self.current + 1
 return val
 }
}

for i in Range(0, 5) {
 print(i)
}
```

### Stage 7: Context Manager Protocol

```
class FileWriter {
 path: str
 handle: int

 def __enter__ -> FileWriter {
 self.handle = dragon_fopen(self.path, "w")
 return self
 }

 def __exit__ -> None {
 dragon_fclose(self.handle)
 }

 def write(data: str) -> None {
 dragon_fwrite(self.handle, data)
 }
}

with FileWriter("output.txt") as f {
 f.write("Hello, Dragon!")
}
```

---

## Cross-Mode Interop Summary

| Scenario | Works? | Mechanism |
|----------|--------|-----------|
| `.dr` defines class, `.dr` uses it | done | Same mode, implicit self throughout |
| `.py` defines class, `.py` uses it | done | Same mode, explicit self throughout |
| `.py` defines class, `.dr` imports and uses it | done | FunctionType excludes self in both modes; caller always writes `obj.method(args)` |
| `.dr` defines class, `.py` imports and uses it | done | Same - FunctionType is the interop contract |
| `.dr` subclasses `.py` class | done | Parser handles self mode per-file; C output is identical |
| `.py` subclasses `.dr` class | done | Same mechanism |

The interop works because:
1. **FunctionType** (the type system's view of a method) **never includes self** - in either mode
2. **C output** always has `ClassName* self` as the first parameter - in either mode
3. **Callers** always write `obj.method(args)` - in either mode

The self convention is purely a **source syntax** difference. It vanishes at the AST level (after the parser normalizes it) and is invisible to TypeChecker, CEmitter, and cross-file interop.

---

## Implementation Effort

| Step | Files | LOC | Risk |
|------|-------|-----|------|
| AST: add `isMethod`, `hasImplicitSelf` | `include/dragon/AST.h:488` | +2 | Low |
| Parser: `inClassBody` tracking + implicit self enforcement | `src/Parser.cpp:906,925` | +30 | Medium |
| Sema: define implicit self in method scope | `src/Sema.cpp:490` | +8 | Low |
| TypeChecker: exclude self from FunctionType + define in scope | `src/TypeChecker.cpp:1270` | +15 | Medium |
| CEmitter: `paramStart` instead of hardcoded `1` (6 loops) | `src/CEmitter.cpp:2171-2247` | ~12 changed | Low |
| Update 5 existing tests (remove `self` from params) | `test/*.cpp` | ~5 changed | Low |
| Add ~13 new tests | `test/*.cpp` | +130 | Low |
| **Total** | **9 files** | **~200** | |

Small, focued change with high impact on Dragon's identity.

---

## FAQ

**Q: Can I still write `self` explicitly in `.dr` files?**
A: No. It's a compile error. Dragon methods have implicit `self` - one way to do it, enforced by the compiler. If you want explicit `self`, use a `.py` file.

**Q: What about the name `self` - can Dragon use `this` instead?**
A: We keep `self` for Python familiarity. Dragon is "Python with curly braces," not Java. `self` is the right keyword. But `self` is not reserved - you can use it as a variable name outside classes.

**Q: Does this break any existing `.dr` code?**
A: Yes - existing `.dr` code with explicit `self` will get a clear error telling you to remove it. The fix is mechanical (delete `self` from parameter lists) and the error message shows exactly what to write. There are no external users at v0.2.0, so this is the right time to enforce it.

**Q: How does this interact with the REPL?**
A: The shell (see [005-dragon-shell.md](005-dragon-shell.md)) defaults to Dragon mode, so implicit self applies. Classes defined in the REPL work the same way.

**Q: What about `cls` for `@classmethod`?**
A: Same treatment. In `.dr` mode, `cls` is implicit. In `.py` mode, it's the explicit first parameter. The pattern is consistent.
