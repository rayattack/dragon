# Type Contracts

A contract names a shape: "this value will have these methods." Go calls the
idea an interface; Dragon's version keeps the compile-time checking and drops
the duck typing - conformance is always something somebody wrote down, never a
coincidence the compiler noticed. Declare a contract with `type`, prove
conformance with a header promise (`->`) or a use-site cast (`as`), and the
whole feature costs nothing at runtime.

## Declaring a contract

A contract is a block of bodiless method signatures - the shape and nothing
else:

```dragon
type Amazing {
    def amazing_method() -> str
    def rating() -> int
}

type Speaker {
    def speak() -> str
}

type Performer(Amazing, Speaker) {}   # composition: unions the signature sets
```

No fields, no constructors, no method bodies, no parameter defaults, and no
empty contracts - each of those is a compile error. A contract that wants
shared behavior is a base class ([Inheritance](/docs/0603-inheritance));
contracts stay pure shape.

## Producer promise: `->` in the class header

A class that intends to satisfy a contract says so in its header, after the
optional base class:

```dragon
class Dog(Animal) -> Amazing, Speaker {
    def amazing_method() -> str { return "woof" }
    def rating() -> int { return 10 }
    def speak() -> str { return "bark" }
}
```

The promise is checked right there, against the flattened class - inherited
methods count - and a broken promise errors once, on the class, listing what
is missing. A promised class flows into contract-typed positions with no cast
anywhere:

```dragon
def show(x: Amazing) -> str {
    return x.amazing_method()
}

d: Dog = Dog()
print(show(d))            # woof - no cast needed, Dog promised
```

## Consumer cast: `as` at the use site

You cannot reach into a library and edit its class header. You do not have to:
assert conformance at your own call site, and the compiler checks it there.

```dragon
class Robot {                          # no promise anywhere
    def amazing_method() -> str { return "beep" }
    def rating() -> int { return 7 }
}

r: Robot = Robot()
a: Amazing = r as Amazing              # checked HERE, at compile time
print(show(r as Amazing))              # beep
```

`as` is upward only and compile-time only. It never fails at runtime because
it compiles to nothing - the same pointer goes in and comes out. Going back
down (contract to concrete class) is not a cast; that is narrowing, and it is
not part of contracts v1.

## The teaching error

Ordinary code, one missing assertion:

```dragon
class Duck {
    def amazing_method() -> str { return "quack" }
    def rating() -> int { return 9 }
}

d: Duck = Duck()
show(d)     # compile error
```

You did nothing wrong - Duck has exactly the right methods. But "has the right
method names" is a coincidence until somebody writes the conformance down, and
Dragon does not dispatch on coincidence. The error hands you both remedies:

```
argument 1 of type 'Duck' is not assignable to parameter type 'Amazing'.
Duck has a matching method set but no declared conformance - cast at the
call site ('d as Amazing') or promise it on the class ('class Duck -> Amazing')
```

## Plural sets: braces where a comma would mislead

One contract is written bare. Two or more group as a braced set (the value
must satisfy every member), in the three positions where a bare comma would
read as something else:

```dragon
m = robot as {Amazing, Speaker}              # cast
def duo(x: {Amazing, Speaker}) -> str {      # annotation
    return x.amazing_method() + x.speak()
}
def both[T: {Amazing, Speaker}](x: T) -> str {   # generic bound
    return x.speak()
}
```

The class-header promise is already delimited by `->` and `{`, so it stays a
plain comma list. A `{Amazing, Speaker}` value satisfies an `Amazing` position
(subset rule), and `pair as Amazing` re-views it upward.

## Contracts as generic bounds

A contract bound uses the same spelling as a class bound
([Generics](/docs/0705-generics)) and checks **structurally at stamp time** -
no cast, no promise:

```dragon
def show_mono[T: Amazing](x: T) -> str {
    return x.amazing_method()
}

print(show_mono(d))       # woof - Dog checked against Amazing at the stamp
print(show_mono(r))       # beep - Robot too, no cast needed
```

This is the workhorse form. Each instantiation monomorphizes: every call in
the stamped body is a direct call on the concrete type, zero dispatch.

## What it costs

Nothing that a virtual call does not already cost. A contract-typed value is
an ordinary instance pointer - not a fat pointer, not a box - and a call
through it is one vtable load plus an indexed call, exactly like a
polymorphic method call (docs 0603). Dragon compiles whole-program, so every
(class, contract) pair is known at compile time and each contract method gets
a reserved slot in every conforming class's vtable. Compare Go, which pays a
runtime lookup-and-cache at interface conversions because its conforming set
stays open; Dragon's is closed, so `as` is free, always.

## Quick reference

| Want | Write |
|---|---|
| Declare a contract      | `type Amazing { def m() -> str }`          |
| Compose contracts       | `type Performer(Amazing, Speaker) {}`      |
| Promise (producer)      | `class Dog(Animal) -> Amazing, Speaker { }`|
| Assert (consumer)       | `r as Amazing`, `r as {Amazing, Speaker}`  |
| Accept a contract value | `def f(x: Amazing)`, `x: {Amazing, Speaker}` |
| Bound a generic         | `def g[T: Amazing](x: T)`                  |
| Container of conformers | `pack: list[Amazing] = [dog, robot as Amazing]` |

