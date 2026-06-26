# Generics

Sooner or later you write a function - or a whole class - whose logic does not care
what type it operates on. A function that returns the first element of a list works
the same for a `list[int]` and a `list[str]`. A stack pushes and pops the same way
whether it holds numbers or names. In an untyped language you'd write it once and
let anything through. In Dragon, where every value has a fixed type, you reach for a
**type parameter**: a stand-in type, written `[T]`, that the compiler fills in at
each use.

The payoff is that you write the code once, keep it fully type-checked, and pay
nothing at runtime - each use compiles down to code as specific as if you'd written
it by hand. That last part is what separates Dragon's generics from Python's.

## Generic functions

Put a type parameter in brackets right after the function name (PEP 695 syntax).
Inside the body, `T` is a real type you can annotate parameters, locals, and the
return type with:

```dragon
def first[T](items: list[T]) -> T {
    return items[0]
}

print(first([10, 20, 30]))        # 10
print(first(["alpha", "beta"]))   # alpha
```

The type argument is **inferred from the call**: `first([10, 20, 30])` solves
`T = int`, `first(["alpha", "beta"])` solves `T = str`. When a call is ambiguous,
pin it explicitly: `first[int](nums)`.

## Generic classes

A class takes type parameters the same way - in brackets after the name, in scope
across every field and method:

```dragon
class Box[T] {
    item: T

    def(x: T) {
        self.item = x
    }

    def get() -> T {
        return self.item
    }
}

b: Box[int] = Box(99)
print(b.get())          # 99

s: Box[str] = Box("hello")
print(s.get())          # hello
```

The binding annotation pins the type argument: `b: Box[int]` makes this `Box`'s `T`
an `int`, so `b.item` is a real `i64` - not a boxed value that happens to hold an
integer. A class can take **more than one** parameter; name them in order, each
independent:

```dragon
class Pair[A, B] {
    first: A
    second: B

    def(a: A, b: B) {
        self.first = a
        self.second = b
    }

    def show() -> str {
        return f"({self.first}, {self.second})"
    }
}

p: Pair[int, str] = Pair(1, "one")
print(p.show())         # (1, one)
```

## A generic container, end to end

Generics earn their keep on container types. Here is a complete, type-safe stack
that works for any element type:

```dragon
class Stack[T] {
    items: list[T]

    def() {
        self.items = []
    }
    def push(x: T) -> None {
        self.items.append(x)
    }
    def pop() -> T {
        return self.items.pop()
    }
    def is_empty() -> bool {
        return len(self.items) == 0
    }
}

s: Stack[int] = Stack()
s.push(1)
s.push(2)
s.push(3)
print(s.pop())          # 3
print(s.pop())          # 2
print(s.is_empty())     # False

names: Stack[str] = Stack()
names.push("ada")
names.push("linus")
print(names.pop())      # linus
```

`Stack[int]`'s `items` field is a real `list[int]` - a packed `int64` array;
`Stack[str]`'s is a real `list[str]`. The same source text, two native layouts, and
the type checker still catches `s.push("oops")` on a `Stack[int]` before the program
runs.

## Generic methods

A method may declare its *own* type parameter, independent of the class's. On a
generic class this composes - the method's `U` and the class's `T` are stamped out
together:

```dragon
class Box[T] {
    item: T

    def(x: T) {
        self.item = x
    }
    def labeled[U](tag: U) -> str {
        return f"{tag}={self.item}"
    }
}

b: Box[int] = Box(99)
print(b.labeled("id"))   # id=99   - U = str
print(b.labeled(7))      # 7=99    - U = int
```

Each distinct `(T, U)` combination gets its own native specialization - no boxing,
no runtime type parameter.

## Bounded type parameters

By default a type parameter is **unbounded**: inside the body you can store, pass,
return, and compare a `T`, but you can't call methods on it - the checker has no proof
that a given method exists for *every* possible `T`. Add a **bound** to lift that
restriction. Write `T: Base`, and within the generic you may use whatever the bound
provides; each instantiation then requires its concrete argument to be the bound or a
subclass:

```dragon
class Animal {
    name: str
    def(name: str) { self.name = name }
    def speak() -> str { return self.name + " makes a sound" }
}
class Dog(Animal) {
    def(name: str) { self.name = name }
    def speak() -> str { return self.name + " says woof" }
}

def describe[T: Animal](x: T) -> str {
    return x.name + ": " + x.speak()   # legal: the bound Animal has .name and .speak()
}

print(describe(Dog("Rex")))   # Rex: Rex says woof  - T = Dog, the override runs
```

The bound is a **class**, not a Rust-style trait: a method call on `T` type-checks
against the bound and dispatches through the [vtable](/docs/0601-classes) to the
concrete argument's override - so passing a `Dog` runs `Dog.speak`, not `Animal.speak`.
Bounds work on generic functions, on generic classes (`class Shelter[T: Animal]`), and
on generic methods alike. Passing a type that doesn't satisfy the bound is a clean
compile error at the instantiation site, never a miscompile.

## Monomorphization, not erasure

Here is the part that matters for speed. Python's generics are *erased* - the type
parameter exists only for the type checker and vanishes at runtime, because every
Python value is already a boxed `PyObject*`. Dragon does the opposite. For each
distinct type argument it **monomorphizes**: it stamps out a separate, fully native
specialization. `Box[int]` and `Box[str]` are two different concrete types in the
emitted code - `Box[int]`'s `item` is an `i64`, `Box[str]`'s is a refcounted `ptr`.
A `Stack[int]` stores an `int64_t[]`, exactly as if you'd written the non-generic
version by hand.

This is the strategy C++ templates and Rust generics use, and the same trick the
built-in `list[T]` and `dict[K, V]` already rely on - now generalized to your own
types. Because each instantiation is just an ordinary monomorphic class, it reuses
every existing path: refcounting, vtables, field layout. Generics add no runtime
overhead of their own.

## What is deferred

The current generics cover both unbounded and bounded type parameters on free
functions, on classes, and on methods (including a generic method on a generic class,
shown above). A couple of things are not supported yet, and each is a **clean compile
error** - never a silent miscompile:

- **Subclassing a generic instantiation** (`class Dog(Animal[str])`) - a clean
  "not yet supported" error.
- **Cross-module generics** - a generic defined in one module and instantiated in
  another. v1 stamps each instantiation inside the module that requests it; a generic
  imported from another module is not yet instantiable across that boundary.

One inference limit worth knowing: a generic *function* whose parameter is itself a
generic instantiation (`def swap[A, B](p: Pair[A, B])`) can't always solve its type
arguments from the call - pin them explicitly with `swap[int, str](p)` when the
compiler asks.

## At a glance

| You want... | Write |
|-------------|-------|
| A type parameter on a function | `def first[T](items: list[T]) -> T { ... }` |
| Infer the type argument | `first(nums)` - solved from `nums: list[int]` |
| Pin it explicitly | `first[int](nums)` |
| A generic class | `class Box[T] { item: T  def(x: T) { ... } }` |
| Several type parameters | `class Pair[A, B] { ... }` |
| A generic method | `def labeled[U](tag: U) -> str { ... }` on a `Box[T]` |
| A bounded type parameter | `def describe[T: Animal](x: T) -> str { ... }` |
| Instantiate | `b: Box[int] = Box(99)` |

Generics close out the type system. Next, [Advanced Functions](/docs/0801-closures)
- closures that capture their environment, decorators built on the `@name` syntax,
and iterators and generators that produce their values lazily.
