# Functions

A function is the first tool any language gives you for naming a piece of
behavior and reusing it. The differences are in the contract. C makes you
declare a prototype and then define it, and forgets the types the moment you
cast. Go lets a function hand back several values but leaves them untyped at
the call boundary unless you spell it out. Rust demands `fn` signatures and
checks them ruthlessly. Python lets you write `def f(x):` with no types at all
and discovers the mismatch at runtime, if ever.

Dragon takes Rust's discipline and Python's shape. Every parameter and the
return value carry a type, the compiler checks them before your program runs,
and yet the code reads like the Python you already know - `def`, default
arguments, keyword calls, closures, lambdas. What you don't get is a magic
entry point: there is no `main`. The file you hand to `dragon run` *is* the
program, and its top-level statements execute top to bottom (see
[How a Program Runs](/docs/0103-how-a-program-runs)). A function runs only when
something calls it.

## Defining and calling

In `.dr` source a function is `def`, a typed parameter list, a `->` return
type, and a brace body:

```dragon
def greet(name: str) -> str {
    return f"Hello, {name}!"
}

print(greet("Dragon"))   # Hello, Dragon!
```

The types are mandatory - not a linter's suggestion but part of the language.
`greet` accepts a `str` and promises a `str`; calling it with an `int`, or
forgetting to return, is a compile error, not a surprise at runtime. (In `.py`
source the body is indented instead of braced, and you write
`def greet(name: str) -> str:` - the types are just as mandatory.)

A function that does its work through side effects rather than a value returns
`None`:

```dragon
def shout(message: str) -> None {
    print(message.upper())
}

shout("hello world")     # HELLO WORLD
```

## Returning, and returning early

`return` hands back a value of the declared type. Inside a `-> None` function
you can also use a bare `return` with no value to exit early - a guard clause:

```dragon
def describe(x: int) -> None {
    if x < 0 {
        print("negative, skipping")
        return
    }
    print(f"value: {x}")
}

describe(-1)             # negative, skipping
describe(5)              # value: 5
```

The bare `return` is only legal when the return type is `None`. In a function
that promises a real value, `return` with nothing is a compile error
(`return without value in function returning ...`) - the type system holds you
to the contract you wrote.

To hand back more than one value, return a `tuple` (covered with the rest of
parameter and return mechanics in [Parameters](/docs/0302-parameters)):

```dragon
def minmax(xs: list[int]) -> tuple[int, int] {
    lo: int = xs[0]
    hi: int = xs[0]
    for x in xs {
        if x < lo { lo = x }
        if x > hi { hi = x }
    }
    return (lo, hi)
}

const bounds: tuple[int, int] = minmax([4, 1, 7, 3])
print(bounds[0], bounds[1])   # 1 7
```

## Functions are values

A function in Dragon is a value like any other. Its type is written
`Callable[[ArgTypes], ReturnType]`, and you can pass one to another function,
store it in a binding, or return it. This is what makes higher-order code -
`map`, callbacks, strategy objects - possible:

```dragon
def apply(f: Callable[[int], int], x: int) -> int {
    return f(x)
}

def square(n: int) -> int {
    return n * n
}

print(apply(square, 5))   # 25
```

`apply` doesn't care *which* `int -> int` function it gets; the
`Callable[[int], int]` annotation is the whole contract. The full `Callable`
type and its relatives live in
[Callable, TypedDict, and intc](/docs/0704-callable-and-typeddict).

## Lambdas

When the function is small and only needed once, a lambda spares you a name. The
syntax mirrors a `def` body - a typed parameter list, a `->` return type, and a
brace expression:

```dragon
const double: Callable[[int], int] = lambda (n: int) -> int { return n * 2 }
print(double(7))          # 14
```

Note the binding: `const double: Callable[[int], int] = ...`. A lambda has no
name of its own, so the variable it is bound to must carry the type. This is the
one place beginners trip - an untyped `const double = lambda ...` is rejected,
because every binding in Dragon is typed. With the annotation in place, a lambda
is most at home passed directly into a higher-order function:

```dragon
def transform(nums: list[int], f: Callable[[int], int]) -> list[int] {
    result: list[int] = []
    for n in nums {
        result.append(f(n))
    }
    return result
}

const doubled: list[int] = transform([1, 2, 3, 4], lambda (x: int) -> int { return x * 2 })
print(doubled)            # [2, 4, 6, 8]
```

## Closures: functions that remember

A function defined inside another function can read - and, with `nonlocal`,
write - the enclosing function's variables, even after the outer call has
returned. The inner function *closes over* that state:

```dragon
def make_adder(amount: int) -> Callable[[int], int] {
    def add(n: int) -> int {
        return n + amount
    }
    return add
}

const add5: Callable[[int], int] = make_adder(5)
print(add5(10))           # 15
```

`add5` carries the `amount` of `5` with it. Closures are a small topic with a
deep tail - capturing by reference, mutating captured state with `nonlocal`,
and the way they interact with the green-thread scheduler - so the full
treatment lives in [Closures and Lambdas](/docs/0801-closures). For now it is
enough to know that a returned inner function is a real, self-contained value.

## Forward references

Order of definition does not constrain order of use. A function may call another
that appears *later* in the same file; the compiler resolves the whole module's
names before generating code:

```dragon
print(triple(7))         # 21

def triple(x: int) -> int {
    return x * 3
}
```

This is why you never need C-style forward declarations, and why a program reads
naturally top-down: the top-level statement `print(triple(7))` is the program,
and `triple` is simply defined somewhere in the file.

## At a glance

| You want to... | Write |
|----------------|-------|
| Define a function | `def f(x: int) -> int { return x + 1 }` |
| A function with no result | `def log(m: str) -> None { print(m) }` |
| Exit early (in a `-> None` fn) | `return` |
| Return several values | `return (lo, hi)` with `-> tuple[int, int]` |
| Take a function as a parameter | `def apply(f: Callable[[int], int], x: int) -> int` |
| A one-off function | `const f: Callable[[int], int] = lambda (n: int) -> int { return n * 2 }` |
| Capture surrounding state | a nested `def` returned from its parent |

Functions are the spine of every Dragon program. The next two chapters fill in
the call side - [Parameters](/docs/0302-parameters) covers default and keyword
arguments, and [Variadics](/docs/0303-variadics) covers `*args` and `**kwargs`
- and [Closures and Lambdas](/docs/0801-closures) returns to capturing functions
in full. When you want one function to work over many types at once, that is the
job of [Generics](/docs/0705-generics).
