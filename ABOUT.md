# Dragon

Dragon is a statically typed, compiled language that looks enough like Python that you can read it without a decoder ring. You annotate types, write `.dr` files with braces (or typed `.py` with indentation), and the toolchain lowers through LLVM to a normal native binary.

It is not a Python runtime. CPython extensions and the whole dynamic ecosystem do not apply here.

## Building

You need a C++17 compiler, CMake 3.16 or newer, and LLVM 14+ with development headers installed.

```bash
git clone https://github.com/tersoo/dragon.git
cd dragon
mkdir build && cd build
cmake ..
make -j$(nproc)
```

Turn tests on with `-DDRAGON_BUILD_TESTS=ON`, then `ctest` from the build directory.

## Running things

```bash
dragon run main.dr # compile and run
dragon build main.dr -o app # leave an executable
dragon check main.dr # type-check only
dragon migrate script.py # spit out a typed .dr draft
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

## Contributing

Fork, branch, run the tests, open a PR. Keep types honest and avoid drive-by refactors.

## License

MIT.
