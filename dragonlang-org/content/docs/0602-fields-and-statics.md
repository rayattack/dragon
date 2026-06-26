# Fields, Statics, and Constants

[Defining a class](/docs/0601-classes) showed per-instance fields. But some state
belongs to the *class*, not to any one object - a shared counter, a configuration
constant, a utility that needs no instance at all. This chapter covers the
distinction between instance and class-level state, the mutable-default trap that
distinction quietly defuses, and the `static` methods that hang off a class
directly.

## Instance fields vs. class fields

This is a decision Python gets wrong, and it's worth slowing down for. A field
declared in the class body with a default - `x: T = value` - is a **per-instance
default**: every object gets its own fresh copy. A field marked `static` is
**shared across all instances** - it lives once, at the class level, and you
reach it through the class name.

```dragon
class Counter {
    static total: int = 0      # shared by every Counter
    count: int = 0             # fresh per instance

    def(name: str) {
        self.name = name
    }

    def tick() -> None {
        self.count = self.count + 1
        Counter.total = Counter.total + 1
    }
}

a: Counter = Counter("a")
b: Counter = Counter("b")
a.tick()
a.tick()
b.tick()
print(a.count)        # 2  - a's own counter
print(b.count)        # 1  - b's own counter
print(Counter.total)  # 3  - shared across both
```

`count` is independent per object; `total` is one value reached through the class
name, `Counter.total`.

## The mutable-default trap, defused

Here's the payoff. In Python, a mutable default written in the class body (or as a
function default) is created **once** and silently shared by every instance - the
infamous footgun where two objects end up mutating the same list. Dragon evaluates
a non-`static` default **fresh for each instance**, so the trap simply doesn't
exist:

```dragon
class Bag {
    items: list[str] = []      # fresh empty list per Bag

    def(label: str) {
        self.label = label
    }

    def add(x: str) -> None {
        self.items.append(x)
    }
}

a: Bag = Bag("a")
b: Bag = Bag("b")
a.add("apple")
a.add("pear")
b.add("fig")
print(a.items)   # ['apple', 'pear']
print(b.items)   # ['fig']  - not shared with a
```

In Python this prints `['apple', 'pear', 'fig']` for *both* bags. In Dragon, each
`Bag` gets its own list. If you genuinely want shared state, that's what `static`
is for - and you have to ask for it by name.

## Constants on a class

Combine `static` with `const` for a class-level constant - declared once, and any
reassignment is a compile error:

```dragon
class Circle {
    static const PI: float = 3.14159

    def(r: float) {
        self.r = r
    }

    def area() -> float {
        return Circle.PI * self.r * self.r
    }
}

c: Circle = Circle(2.0)
print(c.area())      # 12.56636
print(Circle.PI)     # 3.14159
```

`const` is enforced at compile time and works at any scope - module, function, or
(with `static`) class. See
[Variables, Constants, and Statics](/docs/0201-variables) for the full story on
`const`.

## Static methods

A method that doesn't touch instance state takes no `self`, and isn't called on an
instance. Mark it with `@staticmethod` (the Python spelling) or `static def` (the
`.dr` spelling) - both compile to a plain function living in the class's namespace:

```dragon
class MathUtils {
    @staticmethod
    def add(a: int, b: int) -> int {
        return a + b
    }
}

print(MathUtils.add(2, 3))   # 5
```

The `static def` form shines for **factory methods** - alternate constructors that
return a fresh instance:

```dragon
class Temperature {
    def(celsius: float) {
        self.celsius = celsius
    }

    static def from_fahrenheit(f: float) -> Temperature {
        return Temperature((f - 32.0) * 5.0 / 9.0)
    }

    def show() -> str {
        return f"{self.celsius}C"
    }
}

t: Temperature = Temperature.from_fahrenheit(212.0)
print(t.show())   # 100.0C
```

`Temperature.from_fahrenheit(...)` is called on the class, not an instance, and
hands back a constructed `Temperature`. This is Dragon's preferred answer to the
"alternate constructor" patterns Python reaches for `@classmethod` to express.

## At a glance

| You want to... | Write |
|----------------|-------|
| A per-instance field | `count: int = 0` (fresh per object) |
| A shared class field | `static total: int = 0` → `Counter.total` |
| A class constant | `static const PI: float = 3.14159` |
| A function on the class | `@staticmethod def add(a: int, b: int) -> int` |
| A factory / alternate constructor | `static def from_x(...) -> T { return T(...) }` |

Shared and per-instance state in hand, the next chapter builds *hierarchies* of
classes - [Inheritance and Virtual Dispatch](/docs/0603-inheritance).
