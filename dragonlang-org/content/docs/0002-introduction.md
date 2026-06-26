# Introduction

## Who Dragon is for

Dragon is a programming language for people who like writing Python and
want their programs to run as fast as C or Rust. It compiles ahead of
time to a native binary - no virtual machine, no interpreter, no JIT
warm-up. The output of `dragon build hello.dr` is a small executable you
can ship as-is.

Concretely, Dragon is a fit if you are:

- Building backend services in Python today and want to deploy them as a single, self-contained binary with sub-millisecond startup and no garbage-collection pauses.
- Writing performance-sensitive code in C++ or Rust and missing the ergonomics of dicts, list comprehensions, and exceptions.
- Teaching programming and want a language that reads like Python but enforces types, so beginners catch mistakes at compile time instead of in production.
- Embedding a scripting layer in a larger system and want one language that runs at native speed and can drop down to raw `extern "C"` calls when needed.

If you have never written Python, you can still learn Dragon from this
book - the syntax is small and the chapter on common programming
concepts covers the basics. But the book does assume you have written
some code in *some* language before.

## What Dragon offers

Two design choices shape the rest of the book.

**Speed first.** Dragon's compiler is built around the assumption that
you would have written this in C or Rust if Python were a touch faster.
Where two ways exist to do something, the faster one is the one that's
shipped - fast string operations, native-typed collections, no boxing
on hot paths.

**Python where it matters.** When CPython has a name and a signature
for something, Dragon's standard library uses the same name and the
same signature. `from os.path import join`, `dict.get(k, default)`,
`s.startswith("foo")`, and a thousand other small things just work the
way you expect.

What you get on top of Python:

- **Mandatory types.** Types are required on every binding and signature - in both `.dr` and `.py` source. The compiler catches passing a `list[int]` where a `list[str]` was expected, before your code runs.
- **Real concurrency.** Three tiers - green threads (`fire`), scoped OS threads (`thread { ... }`), and a manual `Thread` class - all sharing one M:N scheduler. `await` works in any function; there is no async/sync function-coloring split.
- **User-defined generics, monomorphized.** `def first[T](items: list[T]) -> T` and `class Box[T]` specialize to a separate native version per concrete type - the full speed of hand-written code, with none of Python's type erasure or boxing.
- **Compile-time member privacy.** A leading underscore (`_protected`, `__private`) is enforced by the type checker, not by convention.
- **Refcounting plus a cycle collector.** Memory is reclaimed deterministically. Pauses are bounded. There is no stop-the-world GC.
- **Foreign-function interop.** `extern "C"` lets you call any C library without writing glue code. Dragon's runtime is small and the FFI surface is designed to be cheap to cross.

What you keep from Python that Rust and C don't give you:

- Exceptions, with `try` / `except` / `finally`, custom exception types, and exception chains.
- Context managers (`with`).
- Comprehensions for lists, dicts, sets, and generators.
- F-strings, including format specifiers (`f"{x:.2f}"`, `f"{n:08b}"`).
- A standard library that already includes `os`, `os.path`, `io`, `re`, `json`, `http.server`, `unittest`, `threading`, and many of the Python-named pieces you reach for most often.

## What this book covers

The chapters are arranged in the order you'd hit each piece writing
real code:

- **Getting Started** is a fast tour of the language - a web server, concurrency, threads - then introduces the `dragon` command-line tool.
- **Common Programming Concepts** covers variables, types, functions, comments, and control flow - enough to write small but useful programs.
- **Strings and Unicode**, **Templates**, **Databases**, **Collections**, and **Classes** fill in the building blocks of larger programs.
- **The Type System** (annotations and generics), **Advanced Functions** (closures, decorators, iterators), **Modules and Packages**, **Packaging and Sharing Code**, and **Error Handling** show you how to organize and generalize code.
- **Concurrency**, **The Standard Library**, and **FFI: Calling C** open the door to systems programming.
- **Building Applications** puts it together end to end - a database-backed web service, a concurrent program, and command-line tools - alongside **A Real Project**, which walks through the docs site you are reading right now, in Dragon.
- **Testing** shows how to test what you write.

The appendices catalog keywords, operators, builtin functions, the
standard library, and offer migration notes for Python and Rust users.

When the book is done, you should be able to write any program in
Dragon that you would write in Python - only smaller, faster, and with
more guarantees about correctness.

Let's start.
