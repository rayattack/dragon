# The dragon CLI

The `dragon` (or `dr`, the shorter alias) command-line tool is the
single entry point to the toolchain. There are three subcommands you
will use day to day. We met `dragon run` and `dragon build` in the
previous chapter; the third is `dragon check`.

## `dragon run`

```
$ dragon run path/to/program.dr [-- arg1 arg2 ...]
```

Compiles the file and immediately runs the resulting binary. Anything
after `--` is forwarded as command-line arguments to your program. The
binary itself is written to a temporary location and removed when the
process exits.

Use `dragon run` while you are iterating - it's the fastest way to see
what your code does.

## `dragon build`

```
$ dragon build path/to/program.dr -o path/to/output
```

Compiles the file to a native executable at the path you give. The
output is a self-contained binary that you can ship: it does not need
the `dragon` toolchain on the target machine, only a libc compatible
with your build target.

The resulting binary statically links the Dragon runtime, so there is
nothing to install at deploy time. If your program imports modules from
the standard library - `from os.path import join`, `from re import
match`, and so on - those are bundled in too. Imports from your own
project are resolved relative to the file you passed to `dragon
build`.

Common flags:

| Flag | What it does |
|------|--------------|
| `-o PATH` | Output path. Without this, the binary is written next to the source with the `.dr` suffix stripped. |
| `-I DIR` | Add a directory to the module search path. Repeatable. |
| `--gc=rc` | Use refcounting + cycle collector (the default). |
| `--gc=none` | Disable refcount emission. Useful for very short programs and benchmarks; do not ship binaries built this way. |
| `--release` | Optimize aggressively. Slower compile, faster runtime. |
| `--dump-ast` | Print the parsed AST to stdout and exit. Useful for debugging the parser. |
| `--dump-tokens` | Print the token stream and exit. |

## `dragon check`

```
$ dragon check path/to/program.dr
```

Runs the lexer, parser, and type checker, but does not generate code
and does not produce a binary. It exits 0 if your program is
well-formed and non-zero with diagnostics if not.

`dragon check` is what you run from your editor's "save" hook or your
CI's pre-commit step. It is much faster than a full build because it
stops short of code generation and linking.

The same `--dump-ast` and `--dump-tokens` flags work with `check` if
you only want to see the parser's output.

## Project layout, briefly

For one-file programs, you can keep everything in a single `.dr` file.
For larger projects, the conventional layout is:

```
my_project/
├── main.dr            # the entry file you pass to dragon build
├── lib/
│   ├── server.dr      # imported as `from server import ...`
│   └── auth.dr
└── tests/
    └── test_server.dr
```

Imports are resolved relative to the file being compiled, then along
any extra search paths you pass with `-I <dir>`, then in the bundled
standard library (whose location you can override with the
`DRAGON_STDLIB_PATH` environment variable). We cover modules and
packages in detail in the Modules and Packages chapter.

## What's next

You now have the toolchain installed, you have written and run a
program, and you know which subcommand to reach for. The next chapter,
[How a Program Runs](/docs/0103-how-a-program-runs), explains Dragon's
entry-point model - why there is no `main`, what runs when, and how the
file you hand to `dragon` becomes the program.

From there, [Part 2](/docs/0201-variables) begins the language proper:
variables, types, operators, comments, and control flow. It's the most
Python-like part of the language, so if you're coming from Python most
of it will feel familiar - we'll point out the few places where Dragon
and Python differ.
