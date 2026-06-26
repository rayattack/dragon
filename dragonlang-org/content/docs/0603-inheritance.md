# Inheritance and Virtual Dispatch

Inheritance lets one class build on another - reusing its fields and methods,
overriding what needs to change. Dragon's model is Python's: single inheritance,
`super` to reach the parent, and method overrides resolved at runtime through the
object's vtable. What Dragon adds is the cost profile of C++: a polymorphic call
is one pointer dereference, and a call the compiler can prove is monomorphic is a
direct call with no dispatch at all.

## Extending a class with `super`

A class extends another by naming it in parentheses. The subclass inherits the
parent's fields and methods, and reaches the parent through `super`:

```dragon
class Animal {
    def(name: str) {
        self.name = name
    }
    def speak() -> str {
        return self.name + " makes a sound"
    }
    def describe() -> str {
        return self.name + ": " + self.speak()
    }
}

class Dog(Animal) {
    def(name: str, breed: str) {
        super(name)     # run Animal's constructor first
        self.breed = breed
    }
    def speak() -> str {           # override
        return self.name + " barks"
    }
}

d: Dog = Dog("Rex", "Lab")
print(d.name)        # Rex     - inherited field
print(d.breed)       # Lab     - Dog's own field
print(d.speak())     # Rex barks
print(d.describe())  # Rex: Rex barks  - inherited method calling the override
```

`super(name)` runs the parent constructor - because the constructor is a nameless
`def()`, calling `super` with arguments delegates to it. Delegation is **opt-in**:
if a subclass constructor never calls `super(...)`, the parent constructor does
*not* run and inherited fields keep their defaults (Dragon does not chain base
constructors implicitly). To reach any parent *method*, use `super.method()`:

```dragon
class Sub(Base) {
    def greet() -> str {
        return super.greet() + " (a Sub)"
    }
}
```

> **`.py` files use Python's spelling.** In `.dr`, `super(args)` and
> `super.method()` are the canonical forms. In `.py` files, write Python's
> `super().__init__(args)` and `super().method()`. The two are mode-exclusive.

## Virtual dispatch

The line worth dwelling on above is `d.describe()`. `describe` is defined on
`Animal` and calls `self.speak()` - yet because `d` is a `Dog`, that call lands on
**`Dog.speak`**. The override wins even though the *calling* method lives in the
base class. That's virtual dispatch, and the question it answers is: when you hold
an object through a **base-typed** variable, which version runs? In Dragon, the
Python answer - the most derived override:

```dragon
class Animal {
    def(name: str) {
        self.name = name
    }
    def speak() -> str {
        return self.name + " makes a sound"
    }
}
class Dog(Animal) {
    def(name: str) { super(name) }
    def speak() -> str { return self.name + " barks" }
}
class Cat(Animal) {
    def(name: str) { super(name) }
    def speak() -> str { return self.name + " meows" }
}

a: Animal = Dog("Fido")       # an Animal-typed variable holding a Dog
print(a.speak())              # Fido barks  - Dog.speak runs

c: Animal = Cat("Whiskers")
print(c.speak())              # Whiskers meows
```

The standard place this pays off is a heterogeneous collection - a `list` of
base-typed objects, each dispatching to its own implementation:

```dragon
class Shape {
    def(name: str) { self.name = name }
    def area() -> float { return 0.0 }
}
class Square(Shape) {
    def(side: float) { super("square"); self.side = side }
    def area() -> float { return self.side * self.side }
}
class Circle(Shape) {
    def(r: float) { super("circle"); self.r = r }
    def area() -> float { return 3.14159 * self.r * self.r }
}

const shapes: list[Shape] = [Square(2.0), Circle(1.0)]
for s in shapes {
    print(f"{s.name}: {s.area()}")
}
# square: 4.0
# circle: 3.14159
```

Each `s` is typed `Shape`, but the loop calls the concrete `area()` of whichever
subclass it actually holds.

> **The cost is C++'s cost.** When the compiler can prove there's no override - a
> leaf class, or a call where the concrete type is known - it emits a *direct*
> call, zero overhead. Only genuinely polymorphic calls go through the vtable, and
> that's a single pointer dereference, identical to C++ and Rust. You never pay for
> dispatch you don't use.

## `isinstance` walks the chain

A subclass instance matches any of its ancestors: both `isinstance(d, Dog)` and
`isinstance(d, Animal)` are `True` for a `Dog`, exactly as in Python. It's there
when you need it - but when you want to vary *behavior* by subtype, prefer
overriding a method on the base (virtual dispatch) over a chain of `isinstance`
tests. It's both faster and the idiomatic Dragon answer to "do the right thing for
each subtype."

## At a glance

| You want to... | Write |
|----------------|-------|
| Inherit | `class Dog(Animal) { ... }` |
| Call the parent constructor | `super(args)` |
| Call a parent method | `super.method()` |
| Override | redefine the method in the subclass |
| Dispatch on the real type | hold it base-typed; the override runs via the vtable |
| Test ancestry | `isinstance(obj, Base)` (walks the chain) |

Next, the methods that make your types feel like built-ins - operators, `print`,
`len`, indexing: [Dunder Methods](/docs/0604-dunder-methods).
