# How Dragon Compiles

When you run `dragon build hello.dr`, what comes out the other end is a
native ELF (or Mach-O, or PE) executable - the same kind of file a C
compiler produces. There is no interpreter shipped alongside it, no
bytecode VM, no JIT warming up at startup. Your `.dr` file became machine
code ahead of time, and that is the root of the speed claim: there is no
runtime layer standing between your loop and the CPU.

This chapter follows a program through the compiler so the rest of the
book's performance claims have a concrete picture behind them. You will
not need any of this to *write* Dragon - but knowing how `2 + 3` becomes
an `add` instruction with no boxing in between is what makes the
language's promises legible.

## The pipeline

A Dragon program moves through a fixed sequence of stages:

```
Source → Lexer → Parser → [TypeHintEnforcer] → Sema → TypeChecker → CodeGen → cc link
```

Each stage has one job:

- **Lexer** turns source text into tokens. It runs in one of two modes -
  brace blocks for `.dr`, indentation for `.py` - but emits the same
  token stream either way, so everything downstream is identical across
  the two surfaces.
- **Parser** builds an abstract syntax tree by recursive descent.
- **TypeHintEnforcer** runs *only for `.py` files*, and only to enforce
  that every binding is annotated - typing is mandatory in both modes,
  but `.py` syntax would otherwise permit the omission, so a dedicated
  pass rejects it. (`.dr` cannot express an un-annotated binding in the
  first place.)
- **Sema** resolves names and builds the scope tree - which `x` a given
  use refers to, which names are module globals, which need `global` or
  `nonlocal` to write.
- **TypeChecker** infers and checks types, turning every value's type
  into a known, concrete fact (or a compile error telling you to
  annotate the ambiguity away).
- **CodeGen** emits LLVM IR and hands it to LLVM, which optimizes and
  lowers it to a native object file.
- **cc link** invokes the system C compiler as the linker, producing the
  final executable with the statically-linked Dragon runtime baked in.

You can watch the first two stages directly. `--dump-tokens` prints what
the lexer saw:

```dragon
def add(a: int, b: int) -> int {
    return a + b
}
print(add(2, 3))
```

```
$ dragon check --dump-tokens add.dr
=== Tokens ===
DEF(def)
IDENTIFIER(add)
LEFT_PAREN(()
IDENTIFIER(a)
COLON(:)
IDENTIFIER(int)
...
```

and `--dump-ast` prints the tree the parser built:

```
$ dragon check --dump-ast add.dr
(module add.dr
  (def add
    (params a b)
    (returns: int)
    (body
      (return
        (binary +)
          (name a)
          (name b)))))
```

These two flags, plus `dragon check` (which runs the pipeline up to and
including the type checker but emits no code), are the windows into the
front end.

## Values flow at their native types

Here is the decision that makes Dragon fast, and the one most worth
understanding. A dynamic language like Python represents *every* value -
even a small integer - as a heap-allocated object with a type tag, a
reference count, and a payload. Adding two of them means unboxing both,
adding, and boxing the result. That indirection is most of why
interpreters are slow.

Dragon does the opposite. Because the type checker has already proven the
concrete type of every value, CodeGen emits each one at its **native
machine type**, with no box and no tag:

| Dragon type | LLVM type | Machine reality |
|---|---|---|
| `int` | `i64` | a 64-bit register |
| `float` | `f64` | a floating-point register |
| `bool` | `i1` | a single bit |
| `str`, `list[T]`, `dict[K,V]`, instances | `ptr` | a raw pointer |
| `Union[...]`, `Any` | `{ i64, i64 }` | a 16-byte tagged box - *only* here |

So `a + b` on two `int`s is a single LLVM `add` on two registers. No
allocation, no unboxing, no tag check. The only time a value gets the
heavyweight tagged-box representation is when its type is genuinely
dynamic - a `Union` or `Any` - and even then, narrowing it back with
`isinstance` extracts the payload at its native type. This is the third
commandment in mechanical form: a value whose type is known flows *at*
that type, never boxed.

## Monomorphization: one generic, many specializations

Generics raise an obvious question: if `first[T]` works for any `T`, how
can it compile to native code that needs to know the type? The answer is
**monomorphization** - the compiler stamps out a separate, fully-typed
copy of the function for each type it is actually called with.

```dragon
def first[T](xs: list[T]) -> T {
    return xs[0]
}
print(first([10, 20, 30]))          # T = int
print(first(["a", "b", "c"]))       # T = str
```

This single source function becomes two distinct native functions in the
binary, which you can see with `nm`:

```
$ nm first | grep first
00000000000038a0 T first[int]
00000000000038c0 T first[str]
```

`first[int]` indexes an `i64` array and returns an `i64`; `first[str]`
indexes a pointer array and returns a `ptr`. Neither pays for a generic
dispatch at runtime - each is as specialized as if you had written the
two functions by hand. The same machinery specializes the container types
themselves: a `list[int]` is a packed 64-bit array, a `list[float]` a
native `double[]`, a `list[bool]` one byte per element. There is no single
"list of boxed objects" type the way a dynamic runtime would have. (See
[Generics](/docs/0705-generics) for the language-level view.)

## One module, one binary

A multi-file Dragon program does not compile to separate object files
that link later. The module resolver gathers the entry file and every
module it imports, and CodeGen emits them all into a **single LLVM
module**. LLVM then optimizes across the whole program at once - it can
inline a stdlib function into your hot loop, because from its point of
view there is no library boundary, just one big module.

The result is a self-contained native executable. The Dragon runtime -
the refcounting helpers, the green-thread scheduler, the container
implementations - is statically linked in, so the binary depends only on
the system C library, not on any Dragon-specific shared object:

```
$ file hello
hello: ELF 64-bit LSB pie executable, x86-64, dynamically linked, ...

$ ldd hello
    linux-vdso.so.1
    libm.so.6 => /lib/x86_64-linux-gnu/libm.so.6
    libc.so.6 => /lib/x86_64-linux-gnu/libc.so.6
    /lib64/ld-linux-x86-64.so.2
```

`libc` and `libm` are there; a Dragon interpreter is not, because there
isn't one. You can copy that binary to another machine of the same
platform and run it with nothing installed.

## Why this shape

Every choice here serves the first commandment. Ahead-of-time native
compilation removes the interpreter. Native-typed values remove boxing.
Monomorphization removes generic dispatch. Whole-program LLVM removes the
library boundary that would block inlining. None of it is exotic - it is
the same architecture a C or Rust compiler uses - and that is the point:
Dragon reads like Python but compiles like a systems language. The
[Performance](/docs/1603-performance) chapter puts numbers to what that
buys.
