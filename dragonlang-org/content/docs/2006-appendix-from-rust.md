# Differences from Rust


Rust and Dragon want the same destination - native code that runs at
the speed of C - and they take almost opposite roads to get there. Rust
hands you a borrow checker and asks you to prove, at compile time, that
your memory is sound; in exchange it owes you no garbage collector and
no runtime overhead. Dragon hands you the ergonomics of typed Python and
asks for almost nothing; in exchange it carries a small runtime - a
reference counter plus a cycle collector - and spends real engineering
effort driving that cost toward zero.

If you come from Rust, the single most important sentence in this
appendix is this: **there is no borrow checker, no lifetimes, and no
move semantics.** You will not annotate `'a`. You will not fight `&mut`.
You will not call `.clone()` to placate the compiler. That entire layer
of the Rust mental model simply does not exist here. What you give up for
it is Rust's *guarantee* of zero-overhead, data-race-free memory - Dragon
competes with Rust on speed, but reaches it through a managed runtime
rather than a compile-time proof, and it does not match Rust's
compiler-enforced safety (you reach for locks, not `Send`/`Sync`).

Everything below is the map between the two worlds.

## Memory: reference counting, not ownership

Rust's defining feature is the ownership system. Every value has exactly
one owner, borrows are tracked by lifetime, and the compiler refuses to
build code that could alias mutably or use-after-free. The payoff is no
GC and no runtime checks; the cost is that you encode aliasing facts into
the type system by hand.

Dragon takes the CPython road instead: every heap object
carries a reference count, and a supplementary tracing collector reclaims
reference cycles that pure counting would leak. You never write a
lifetime, never move a value to satisfy the compiler, never reach for
`Rc<RefCell<T>>` to share something mutably. Sharing is the default and
costs you nothing at the keyboard.

```rust
// Rust: ownership moves; the borrow checker tracks every reference
fn take(v: Vec<i32>) -> i32 { v.iter().sum() }
let xs = vec![1, 2, 3];
let total = take(xs);       // xs is moved; using it again is a compile error
```

```dragon
# Dragon: pass freely, alias freely; the runtime counts references
def take(v: list[int]) -> int {
    s: int = 0
    for x in v { s = s + x }
    return s
}
xs: list[int] = [1, 2, 3]
total: int = take(xs)       # xs is still usable afterward - no move
```

The trade-off is honest and worth stating plainly: reference counting and
cycle collection are not free. Increment/decrement traffic and the
occasional collector pass are real costs that Rust's model avoids
entirely. Dragon's first commandment is speed, so the runtime works hard
to drive that overhead toward zero (acyclic types skip cycle tracking, and
escape analysis is on the roadmap to stack-allocate non-escaping objects).
Dragon competes with Rust on raw speed head-to-head; the refcount and
collector traffic is a cost it engineers down, not a gap it concedes.

| Topic | Rust | Dragon |
|-------|------|--------|
| Reclamation | Ownership + drop at scope end | Refcount + cycle collector |
| Lifetimes | Explicit `'a` annotations | None |
| Move semantics | Values move by default | No moves - values are shared |
| Shared mutability | `Rc<RefCell<T>>`, `Arc<Mutex<T>>` | Just share the reference |
| Aliasing rules | Enforced by borrow checker | Unrestricted |
| Runtime cost | Zero (no GC) | RC traffic + occasional collection |

## Errors: exceptions, not `Result<T, E>`

Rust models fallibility in the type system. A function that can fail
returns `Result<T, E>`, the `?` operator threads errors up the stack, and
you exhaustively `match` on the error enum. Nothing is hidden; nothing is
thrown.

Dragon uses exceptions: `raise`, `try`/`except`, a real
exception hierarchy with `ValueError`, `KeyError`, `ZeroDivisionError`,
and friends, and user-defined exception classes. A function's signature
does **not** advertise what it can raise - there is no `throws` clause and
no error type parameter.

```rust
// Rust: errors are values; ? propagates them
fn parse(s: &str) -> Result<i32, std::num::ParseIntError> {
    let n = s.parse::<i32>()?;
    Ok(n * 2)
}
```

```dragon
# Dragon: errors are raised and caught
def parse(s: str) -> int {
    n: int = int(s)          # raises ValueError on bad input
    return n * 2
}

try {
    print(parse("21"))       # 42
    print(parse("nope"))     # raises
} except ValueError as e {
    print("bad number")
}
```

> **What you lose.** The compiler does not force you to handle a failure
> the way `Result` does - an uncaught exception is a runtime crash, not a
> build error. If you valued Rust's "you cannot ignore this error"
> guarantee, that guarantee is gone. What you gain is that the happy path
> reads top-to-bottom with no `?` noise and no error-type bookkeeping.

See [Error Handling](/docs/0901-exceptions) for the full hierarchy and
custom-exception syntax.

## Generics: yes, and monomorphized like Rust

Both languages make you write types, and both turn generics into specialized
native code rather than boxed values. Dragon's annotations are mandatory
everywhere - in both `.dr` and `.py` source - just as Rust demands them at
function boundaries. On **parametric polymorphism** the two languages agree
on the big decision: you write a generic once, and the compiler stamps
out one specialized copy per concrete type argument (monomorphization). No
boxing, no per-element tag, no runtime type parameter. The surface is PEP 695
brackets rather than Rust's angle brackets:

```rust
// Rust: write your own generic, monomorphized per T
fn first<T: Clone>(xs: &[T]) -> T { xs[0].clone() }
```

```dragon
# Dragon: same idea, PEP 695 brackets, monomorphized per T
def first[T](xs: list[T]) -> T {
    return xs[0]
}
print(first([10, 20, 30]))        # 10     - T = int
print(first(["alpha", "beta"]))   # alpha  - T = str
```

Generic classes and methods work the same way - `class Box[T]`,
`class Pair[A, B]`, a generic method `def labeled[U]()` on a `Box[T]`. A
`Box[int]`'s field is a real `i64` and a `Box[str]`'s is a refcounted pointer:
two distinct concrete types in the emitted code, exactly as Rust would emit.
The full surface is in [Generics](/docs/0705-generics).

Where the two languages genuinely diverge is the **kind of bound**, plus a
couple of features Dragon does not have:

- **Class bounds, not trait bounds.** Rust constrains a type parameter with a
  trait (`T: PartialOrd`). Dragon constrains it with a **base class**
  (`def describe[T: Animal](x: T)`), and a method call on `T` dispatches
  through the [vtable](/docs/0601-classes) to the concrete argument's
  override. An unbounded `T` may be stored, passed, returned, and compared,
  but you must add a bound before you can call methods on it.
- **No const generics and no lifetime parameters.** There is no
  `[N: usize]`-style value parameter, and no `'a` - lifetimes do not exist
  here at all (see the memory section above).
- **Subclassing a generic instantiation** (`class Dog(Animal[str])`) is not
  supported yet - a clean compile error, never a miscompile.

Where you genuinely need *dynamic* typing rather than parametric polymorphism,
`Any` and pipe-unions (`int | str`) fill that separate gap:

```dragon
# Dragon: a pipe-union is dynamic, not generic - narrow with isinstance
def label(x: int | str) -> str {
    if isinstance(x, int) {
        return "got int"
    }
    return "got str"
}
print(label(5))      # got int
print(label("hi"))   # got str
```

> **Union syntax.** Dragon writes sum-ish dynamic types as `int | str`
> (PEP 604 style), not `Union[int, str]` and not a Rust `enum`. Narrow
> with `isinstance`, exactly like the example above. A pipe-union is a
> 16-byte tagged box under the hood, not a checked algebraic data type -
> there is no exhaustiveness checking on the arms.

The container layout story - why a list of a million ints is a million
contiguous words - is in [Collections](/docs/0501-lists).

## Polymorphism: classes + vtables, not traits

Rust separates data (`struct`) from behavior (`impl`) and abstracts over
behavior with `trait`s, dispatched statically by default or dynamically
through `dyn Trait` fat pointers. There is no inheritance.

Dragon uses classes with single inheritance and **virtual dispatch
through vtables**. A subclass overrides a base method; a
base-typed reference holding a subclass instance dispatches to the
override through one pointer load - the same single-dereference cost as a
C++ virtual call or a Rust `dyn` call. When the compiler can prove no
subclass overrides a method (a whole-program check), it devirtualizes to
a direct call, so leaf and monomorphic calls pay nothing.

```rust
// Rust: trait + impl, dynamic dispatch via dyn
trait Animal { fn speak(&self) -> String; }
struct Dog;
impl Animal for Dog { fn speak(&self) -> String { "Woof".into() } }
fn describe(a: &dyn Animal) -> String { a.speak() }
```

```dragon
# Dragon: class + inheritance, virtual dispatch via vtable
class Animal {
    name: str
    def(name: str) { self.name = name }
    def speak() -> str { return "..." }
}
class Dog(Animal) {
    def(name: str) { self.name = name }
    def speak() -> str { return "Woof" }
}
def describe(a: Animal) -> str {
    return a.name + " says " + a.speak()
}
print(describe(Dog("Rex")))    # Rex says Woof
```

Two things a Rust reader should note. First, `self` is **implicit** in
`.dr` methods - you do not declare it in the parameter list (the
`def speak()` above receives `self` automatically). Second, the
constructor is the nameless `def(...)`, not `fn new()` and not
`__init__`. See [Classes and Objects](/docs/0601-classes).

| Topic | Rust | Dragon |
|-------|------|--------|
| Data + behavior | `struct` + separate `impl` | One `class` body |
| Abstraction | `trait` | Base class + override |
| Dynamic dispatch | `dyn Trait` fat pointer | Vtable pointer |
| Static dispatch | Default (monomorphized) | Devirtualized when no override exists |
| Inheritance | None | Single inheritance |
| Constructor | `fn new() -> Self` | Nameless `def(...)` |

## Concurrency: colorless green threads, not async + an executor

This is where the two languages feel most different. Rust's `async`/`await`
colors functions: an `async fn` returns a `Future` that does nothing
until you `.await` it on an executor you pull in as a crate (`tokio`,
`async-std`). The color is viral - an `async` leaf forces `async` all the
way up - and you sprinkle `Send`/`Sync` bounds to convince the type system
your futures are safe to move across threads.

Dragon ships concurrency in the language and the runtime,
not in a crate, and it is **colorless**. `fire` spawns an M:N-scheduled
green thread (about 64 KB each - spawn ten thousand); binding it yields a
`Task[T]`. `await` waits for a result and is legal in *any* function, not
just an `async` one - that is "the Dragon Rule," and it kills function
coloring outright. For real hardware threads there is `thread { ... }`, a
scoped OS thread that joins automatically at block exit. There are no
`Send`/`Sync` markers in user code.

```rust
// Rust: async fn + executor; await only inside async; Send bounds lurk
async fn compute(n: i32) -> i32 { n * n }
#[tokio::main]
async fn main() {
    let a = tokio::spawn(compute(3));
    let b = tokio::spawn(compute(4));
    let total = a.await.unwrap() + b.await.unwrap();   // 25
    println!("{total}");
}
```

```dragon
# Dragon: fire returns a Task[T]; await works anywhere; no executor crate
def compute(n: int) -> int { return n * n }

a: Task[int] = fire compute(3)
b: Task[int] = fire compute(4)
total: int = a.join() + b.join()    # 25
print(total)

thread {
    print("on a scoped OS thread")  # joins automatically at block end
}
```

> **No GIL, but also no compiler-enforced data-race freedom.** Dragon has
> no global lock, so green threads and OS threads genuinely run in
> parallel - but unlike Rust, the *compiler* does not prove your shared
> access is race-free. You reach for the locks in
> [the standard library](/docs/2004-appendix-stdlib) (`Lock`, `RWLock`,
> `Semaphore`) and use them correctly yourself. This is a real safety gap
> versus Rust's `Send`/`Sync` guarantees, and it is honest to call it one.

The full model - `Task[T]`, `async def`, `await`, the three tiers - is in
[Concurrency](/docs/1101-green-threads).

| Topic | Rust | Dragon |
|-------|------|--------|
| Lightweight tasks | `async fn` + executor crate | `fire` green threads |
| Function color | Viral `async` | Colorless `await` everywhere |
| Executor | External (`tokio`, etc.) | Built into the runtime |
| OS threads | `std::thread::spawn` + join | `thread { }` (auto-join) or `Thread` |
| Data-race safety | Compiler-enforced (`Send`/`Sync`) | Programmer-enforced (locks) |
| Awaiting a result | `.await` on a `Future` | `t.join()` or `await t` on a `Task[T]` |

## Metaprogramming: compile-time templates, not macros

Rust's metaprogramming is macros: `macro_rules!` for declarative pattern
expansion and procedural macros for token-stream rewriting via a separate
proc-macro crate. They are powerful and notoriously involved.

Dragon has no macro system. Its compile-time facility is the
`template { ... }` block - raw text with `!{expr}` interpolation that
compiles to plain string concatenation, plus typed variants like
`template[HTML]{ ... }` that auto-escape and return a distinct type. A
typo in an interpolation is a compile error, not a runtime blank. This
covers the "generate text/markup safely at compile time" use case that
sends Rust programmers reaching for a macro, without the token-stream
machinery.

```dragon
name: str = "World"
greeting: str = template {Hello !{name}}   # compiles to string concat
print(greeting)                             # Hello World
```

See [Templates](/docs/1201-templates) for the typed content-type variants
(`HTML`, `JSON`, `SQL`, and more).

## Build and deploy: one binary, no Cargo.toml for the stdlib

Both toolchains produce a native executable from a build command. Rust's
`cargo build` resolves a `Cargo.toml`, pulls dependencies from crates.io,
and links them. Dragon's `dragon build file.dr` compiles your program and
**statically links the batteries-included standard library** - TLS,
sockets, SQLite, regex, the crypto suite, the concurrency runtime - with
no manifest file and no registry step for those capabilities. There is no
`Cargo.toml`, no `cargo add`, no lockfile to commit for the things the
stdlib already covers.

```rust
// Rust: declare deps in Cargo.toml, then
cargo build --release      // → target/release/app
```

```dragon
# Dragon: no manifest for stdlib capabilities
dragon build app.dr        # → ./app, stdlib statically linked in
```

> **Honest caveat on "static binary."** Dragon's *own* stdlib and its
> vendored C libraries (SQLite, PCRE2, mbedTLS, llhttp, minicoro) are
> statically linked into the output, so you do not ship them alongside
> the binary. The executable itself is still dynamically linked against
> the system `libc`/`libm`, exactly as a default `cargo build` output is -
> a fully static musl-style build is a separate concern in both
> ecosystems. The win is "no dependency-resolution step for stdlib
> features," not "a single file with zero shared-library dependencies."

The full inventory of what ships in the box is in
[the standard library appendix](/docs/2004-appendix-stdlib).

## Syntax: Python shape, not `fn`/`struct`/`impl`

The surface is Python, not C-with-curly-braces-and-`fn`. You write `def`
and `class`, not `fn` and `struct` + `impl`. The `.dr` canonical mode uses
braces (and is indentation-friendly), and there is a `.py` adoption mode
for incrementally typed Python files. Declarations use `:` to introduce a
name (`x: int = 5`) and `=` only to reassign - a bare `x = 5` for an
undeclared name is an error, which is closer to Rust's "declare once" feel
than to dynamic Python.

| Concept | Rust | Dragon |
|---------|------|--------|
| Function | `fn f(x: i32) -> i32` | `def f(x: int) -> int` |
| Type + impl | `struct S {} impl S {}` | `class S { ... }` |
| Constructor | `fn new() -> Self` | nameless `def(...)` |
| Method receiver | explicit `&self` | implicit `self` |
| Immutable binding | `let x = 5;` | `const x: int = 5` |
| Mutable binding | `let mut x = 5;` | `x: int = 5` |
| Block comment | `/* ... */` | `# ...` (line) / `""" ... """` |

## At a glance

| Axis | Rust | Dragon |
|------|------|--------|
| Memory | Ownership, borrows, lifetimes, no GC | Refcount + cycle collector, no lifetimes |
| Move semantics | Values move by default | No moves - references are shared |
| Errors | `Result<T, E>` + `?`, no exceptions | Exceptions: `raise`/`try`/`except` |
| Generics | `fn f<T>()`, monomorphized | `def f[T]()` / `class Box[T]`, monomorphized |
| Dynamic types | `enum` / `dyn` | `int \| str` unions, `Any` |
| Polymorphism | Traits + `impl`, `dyn` | Classes + inheritance + vtables |
| Concurrency | `async`/`await` + executor crate | Colorless `fire`/`await` green threads |
| Thread safety | `Send`/`Sync`, compiler-proven | Locks, programmer-enforced |
| Metaprogramming | `macro_rules!` + proc macros | Compile-time `template { }` |
| Build | `cargo build` + Cargo.toml + crates.io | `dragon build` + stdlib statically linked ([stdlib](/docs/2004-appendix-stdlib)) |
| Syntax | `fn` / `struct` / `impl` | `def` / `class`, Python shape |
| Speed | Zero-overhead | Native + LLVM, competes head-to-head |

The honest summary: Dragon trades Rust's compile-time safety proofs for
Python-shaped ergonomics and a managed runtime, and it competes with Rust
on speed - native, LLVM-compiled, no universal box. If your project lives
or dies on guaranteed memory safety and data-race freedom with no runtime,
Rust is still the tool. If you want native speed with the feel of typed
Python - no borrow checker, no lifetimes, no async coloring, no Cargo step
for the standard library - that is the bargain Dragon offers.
