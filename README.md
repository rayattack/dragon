# Dragon

A statically typed, compiled language that borrows Python syntax but ships as one native binary. You write `.dr` files with braces, annotate your types, and you get an executable.

- No interpreter
- no virtualenv
- no runtime to install on the server.

Dragon is inspired by Python; **it is not** a superset of it. Where it compiles cleanly to fast static code it rejects the dynamic crutches (no `eval`, no monkey-patching, no untyped anything). Ambiguity is a compile error, not a runtime surprise.

It looks enough like Python that you can read it without a decoder ring. CPython extensions and the whole dynamic ecosystem do not apply here.

## 30 seconds to a native binary

```py
# hello.dr
def fib(n: int) -> int {
    if n < 2 { return n }
    return fib(n - 1) + fib(n - 2)
}
print(f"fib(32) = {fib(32)}")
```

```sh
# Linux x86-64 (.deb; .rpm and a portable .tar.gz are on the releases page)
curl -fsSL https://dragonlang.org/install.sh | sh

dragon run hello.dr          # compile and run:  fib(32) = 2178309
dragon build hello.dr -o fib # or keep the binary
./fib
```

Mac OS and Microsoft Windows are WIP. See [dragonlang.org/download](https://dragonlang.org/download).

## A web server is still one binary

The standard library is written in Dragon itself, including the HTTP server, TLS, templating, and database drivers. This is a complete web server; it compiles to a 1.8 MB executable:

```py
from html import HTML
from http.server import Router, Request, Response, Context

const app: Router = Router(8080, "127.0.0.1")

async def home(req: Request, res: Response, ctx: Context) -> None {
    const name: str = req.query["name"] if "name" in req.query else "world"
    res.html(template[HTML] { <h1>hello, !{name}</h1> })
}

app.GET('/', home)
app.listen()
```

`template[HTML]` is a typed content template: `!{name}` is escaped for its exact position in the markup, so string-concatenation injection bugs are not expressible in idiomatic Dragon. The same idea covers JSON and SQL.

Proof it holds up: [dragonlang.org](https://dragonlang.org) (site, docs, and package registry) is a single Dragon binary serving HTTPS. Even its port-80 ACME/redirect companion is ~50 lines of Dragon.

## Why Dragon

- **Types are honest.** Every value whose concrete type is knowable flows at that type. Generics monomorphize at compile time; nothing is boxed behind your back. `assertEqual(1, "x")` fails to compile, exactly like `name: int = "boy"`.
- **One binary out.** `dragon build` produces a self-contained executable. Copy it to a server and run it.
- **Batteries in the stdlib, written in Dragon.** HTTP client and server (embedded mbedTLS), SQLite/PostgreSQL/MySQL, JSON, crypto (KAT-verified), green threads over epoll/kqueue, subprocess, zip/tar/zstd.
- **Speed with receipts.** On compute-bound microbenchmarks (fib, mandelbrot) Dragon runs at parity with Rust, and trails C by about 12% on fib. We also publish the number we are not proud of yet: binary-trees runs 2.7x slower than Rust because it stresses the refcount runtime, and it is an optimization target. Rust is a yardstick, not the goal. Full table: [dragonlang.org/benchmarks](https://dragonlang.org/benchmarks).
- **Coming from Python?** A typed `.py` file compiles directly: `dragon build script.py`. Annotations are mandatory and the dynamic parts of Python do not carry over. It is an on-ramp, not a compatibility promise.

## Platform status

| Platform                        | Status                                                                                         |
| ------------------------------- | ---------------------------------------------------------------------------------------------- |
| Linux x86-64                    | Installers on [releases](https://github.com/rayattack/dragon/releases) (.deb / .rpm / .tar.gz) |
| macOS (Apple Silicon and Intel) | Building in CI now; .dmg lands on the same releases page                                       |
| Windows x86-64                  | Parked pending a MinGW toolchain rework                                                        |

The compiler needs a C compiler (gcc or clang) on PATH for the final link; the .deb declares that dependency and apt resolves it.

## Build from source

Requirements: a C++17 compiler, CMake 3.16+, and LLVM 21+ with dev headers.

```bash
git clone https://github.com/rayattack/dragon.git
cd dragon
mkdir build && cd build
cmake .. -DDRAGON_BUILD_TESTS=ON
cmake --build . -j4
ctest -j2 --output-on-failure
```

The compiler lands at `build/dragon`.

## Running things

```bash
dragon run main.dr       # compile and run
dragon build main.dr -o app  # leave an executable
dragon check main.dr     # type-check only
dragon build main.py     # compile a typed Python file directly
```

Common flags: `-O0`..`-O3`, `-g`, `-v`. Python files need `-f` if you want the Python surface parser.

## Language sketch

Declarations use `name: Type = value`. Reassignment is bare `name = value` once the name exists. Functions take the usual annotations; `.dr` allows `def f() -> int { ... }` and multi-line lambdas with blocks. Exceptions use `catch` in brace mode (Python mode keeps `except`).

The compiler pipeline is boring on purpose: lex, parse, name resolution, type check, LLVM IR, link against the Dragon runtime. Multi-file programs resolve imports at compile time into one module.

## Where stuff lives

- `include/dragon/` -- compiler headers
- `src/` -- compiler implementation (`src/codegen/` is the big LLVM chunk)
- `stdlib/` -- standard library in Dragon
- `lib/Runtime/` -- C++ runtime (plus vendored third-party under `lib/`)
- `test/` -- GoogleTest suites and `.dr` regression tests
- `dragonlang-org/` -- docs site and longer-form guides

For detail beyond this file, start with `dragonlang-org/content/docs/0002-introduction.md`.

## Learn more

- [Introduction and docs](https://dragonlang.org/docs)
- [Download](https://dragonlang.org/download)
- [Benchmarks](https://dragonlang.org/benchmarks)
- `zen.md` for how the language is meant to feel

Dragon is pre-release (v0.0.1). Expect sharp edges; file issues, they get fixed at the root.

## Contributing

Fork, branch, run the tests, open a PR. Keep types honest and avoid drive-by refactors.

## License

MIT ([LICENSE.txt](LICENSE.txt)).
