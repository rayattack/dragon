# Classes and Objects

A class bundles data and the behavior that operates on it into one named type.
C keeps the two apart - a `struct` for the fields, free functions that take a
pointer to it. Python unifies them but leaves the shape open: any method can
invent a new attribute at runtime, and you find out about a typo only when it
blows up. Dragon takes the unified, object-oriented shape and nails it down with
types: a class has a **declared set of typed fields**, a constructor, and
methods - and the field set is fixed at compile time. 

> **Dragon Safety** 
> Accessing a member the class doesn't declare is a
> **compile error**, not a runtime `AttributeError`.

## Your first class

A class is the `class` keyword, a brace body, typed fields, a constructor, and
methods:

```dragon
class Counter {
    total: int = 0
    def(start: int) {
        self.total = start
    }
    def bump() -> None {
        self.total = self.total + 1
    }
}

c: Counter = Counter(10)
c.bump()
c.bump()
print(c.total)   # 12
```

`Counter(10)` calls the constructor; `c.bump()` calls a method; `c.total` reads a
field. Each piece is typed, and the compiler checks every use against the
declaration.

## The constructor is `def()`, not `__init__`

In `.dr` source the constructor is a **nameless `def()`** - the parameter list
the call site supplies. There is no `__init__` and no explicit `self` in the
signature:

```dragon
class Point {
    x: float = 0.0
    y: float = 0.0
    def(x: float, y: float) {
        self.x = x
        self.y = y
    }
}

const p: Point = Point(3.0, 4.0)
print(f"({p.x}, {p.y})")   # (3.0, 4.0)
```

If you're writing `.py`-mode source, the constructor is spelled the Python way -
`def __init__(self, x: float, y: float)` - with an explicit `self`. Both compile
to the same thing; it's purely the surface convention of the two file modes (see
[How a Program Runs](/docs/0103-how-a-program-runs)).

## `self` is implicit in methods

This is the one thing to internalize coming from Python. In a `.dr` method,
**you do not list `self` as a parameter** - it's implicit. You still *use* `self`
inside the body to reach fields and other methods; you just don't declare it:

```dragon
class Rect {
    w: int = 0
    h: int = 0
    def(w: int, h: int) {
        self.w = w
        self.h = h
    }
    def area() -> int {
        return self.w * self.h      # self is available, not declared
    }
    def describe() -> str {
        return f"{self.w}x{self.h} = {self.area()}"   # call a sibling method
    }
}

const r: Rect = Rect(3, 4)
print(r.area())       # 12
print(r.describe())   # 3x4 = 12
```

Writing `def area(self) -> int` in a `.dr` file is a compile error that tells you
to drop the `self`. (In `.py` mode you keep it, just as Python requires.)

## Fields are declared and typed

Every field is named with its type in the class body, optionally with a default.
The declared set is the *whole* set: there is no adding fields at runtime, and
reading or writing a name the class doesn't declare is a compile error.

```dragon
class User {
    name: str = ""
    age: int = 0
    active: bool = true
    def(name: str, age: int) {
        self.name = name
        self.age = age
    }
}

const u: User = User("Ada", 36)
print(u.name)        # Ada
print(u.active)      # True
# u.email          # <- compile error: User has no attribute 'email'
```

This is the static type system doing its job - the same reason there is no
`getattr`/`setattr` or runtime attribute discovery in Dragon. The shape of an
object is known at compile time, which is what makes field access a direct memory
load rather than a dictionary lookup.

## Classes are not values

A class is a compile-time entity, not a runtime value: you cannot assign a
class to a binding and construct through it (`factory = Widget; factory(7)`),
and there is no `type(x)` that hands back a class object to call or compare.
The compiler resolves every construction and method call at the definition
site, which is what keeps them direct calls rather than runtime dispatch. For
"factory" needs, use a `static def` alternate constructor on the class itself.

## At a glance

| You want to... | Write |
|----------------|-------|
| Define a class | `class C { ... }` |
| A field (with default) | `count: int = 0` |
| The constructor (`.dr`) | `def(a: int) { self.x = a }` |
| The constructor (`.py`) | `def __init__(self, a: int) -> None:` |
| A method (`.dr`) | `def m() -> int { return self.x }` - no `self` param |
| Instantiate | `c: C = C(10)` |
| Read a field / call a method | `c.x`, `c.m()` |

That's the anatomy of a class. The next chapters go deeper:
[Fields, Statics, and Constants](/docs/0602-fields-and-statics) on instance vs
class-level state, [Inheritance and Virtual Dispatch](/docs/0603-inheritance) on
building type hierarchies, [Dunder Methods](/docs/0604-dunder-methods) on making
your types feel built-in, and [Member Privacy](/docs/0605-privacy) on `_protected`
and `__private`.
