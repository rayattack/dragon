# Microsoft rewrote TypeScript in Go. I raced them with Python's younger brother.

*July 26, 2026*

In March 2025, Anders Hejlsberg announced that the TypeScript compiler was
going native. The team had ported `tsc` from TypeScript to Go, the port was
roughly 10x faster, and it would ship as TypeScript 7. It has: run
`npx tsgo` today and a native binary type-checks your project while the old
JavaScript compiler is still warming up.

The interesting part, for language nerds, was the shortlist. Why Go? The
team's answer boiled down to four requirements:

1. **Native binaries.** A compiler is a CLI tool; startup time and raw
   throughput are the product.
2. **Garbage collection.** The compiler's ASTs and type graphs are cyclic
   webs. Rust was considered and rejected: manual memory management would
   have forced a redesign, and they wanted a port, not a rewrite.
3. **Real concurrency.** About half of the 10x came from parsing and
   checking files in parallel.
4. **Structural similarity.** The existing codebase is imperative,
   function-heavy TypeScript. Go code could be translated almost
   mechanically, preserving twenty years of battle-tested parser behavior.

## The candidate nobody even bothered to reject

Notice the language that never came up: Python. Nobody at Microsoft had to
write the paragraph explaining why, because every reader could write it
themselves:

- **There is no binary.** CPython compiles to bytecode and interprets it.
  Shipping a compiler means shipping an interpreter with your program
  hidden inside it. That fails requirement one before lunch.
- **The hot loop is interpreted.** A parser is millions of iterations of
  "load a byte, compare, branch". CPython pays interpreter dispatch on
  every one of them. This is the exact workload where Python is at its
  slowest relative to native code, often 50x or worse.
- **The GIL.** Half of typescript-go's win is parallelism. Python's
  free-threaded build is still experimental; the deployed reality is one
  thread of progress at a time.
- **Types are decoration.** Python has annotations, and `checker.ts` would
  read beautifully with them. But the runtime boxes every integer, and an
  attribute access is a hash lookup. Annotations don't make code fast; they
  make it documented.
- **Every AST node is a PyObject.** A 3MB source file becomes hundreds of
  thousands of heap objects with headers, refcounts, and pointer soup. The
  memory story disqualifies itself.

So Python was never in the race. Fair. The mother tongue of half the
world's programmers sat this one out.

But Python has a younger brother.

## Same mother, different father

Dragon has Python's syntax for a mother: the `def`s, the `for x in`, the
list comprehensions, the f-strings, the entire muscle memory. The father
is not Guido van Rossum. And yet, by what I can only assume is a
statistical property of the soil, it was also built in the Netherlands.

This is apparently just what the Netherlands does. The Mathematisch
Centrum in Amsterdam gave the world ALGOL 68 (Adriaan van Wijngaarden).
Its successor, CWI, incubated ABC (Lambert Meertens and Leo Geurts),
and then a CWI employee named Guido used ABC as the blueprint for Python.
Dijkstra, meanwhile, spent a career in Eindhoven telling everyone their
control flow was considered harmful. Programming languages are minted
below sea level the same way the land is: methodically, and against
everyone's expectations.

Dragon's pitch is one sentence: reads like Python, compiles like C. It is
statically typed, monomorphized, and lowered through LLVM to a native
binary. Memory is reference counting with a cycle collector, so cyclic
ASTs are fine (requirement two). Threads are OS threads with no GIL
(requirement three). And the surface is close enough to TypeScript's
imperative style that a mechanical port is natural (requirement four).

```dragon
def fib(n: int) -> int {
    if n < 2 {
        return n
    }
    return fib(n - 1) + fib(n - 2)
}
```

That is ordinary Dragon, and at `-O2` it trades blows with Rust. Which
raised an obvious question: could the younger brother do what the older
one was never invited to attempt?

## The experiment

I ported tsc's scanner and parser to Dragon: the full token grammar, the
full statement and expression grammar, the type grammar (conditional
types, mapped types, template literal types, the works), and the three
famous scanner rescans that make TypeScript parseable at all
(greater-than combining for nested generics, slash-as-regex, template
continuation). Arrow-function disambiguation uses tsc's own strategy:
cheap token lookahead for the common cases, snapshot-and-backtrack
speculation for the ambiguous ones.

Benchmarks are easy to fake with a parser that "mostly works", so the
fidelity bar was set first, against the real compiler:

- **Token streams are byte-identical.** Kind, start, and end of all
  96,113 raw tokens match `ts.createScanner` (TypeScript 5.9.3) exactly,
  across `checker.ts`, `parser.ts`, and `scanner.ts` from the TypeScript
  repo.
- **Parses are fingerprint-identical.** All three files parse to
  completion, and the node counts for 42 SyntaxKinds (ArrowFunction,
  CallExpression, TypeReference, UnionType, MappedType,
  RegularExpressionLiteral, ...) match `ts.createSourceFile` exactly.

The star witness is `checker.ts`: 54,434 lines, 3.15MB, 275,586 AST
nodes, the largest and least merciful file in the TypeScript compiler.
Every arrow, every regex-versus-division call, every generic
instantiation in it agrees with the reference compiler.

The competition was not a stand-in. I built the benchmark against
typescript-go's actual `internal/parser`, from source, using the same
setup as its own `BenchmarkParse`.

## The numbers

Parse `checker.ts` from bytes to full AST, single thread, best-of-N,
interleaved runs on the same laptop (i5-8350U):

| Engine | checker.ts (3.15 MB) |
|---|---|
| tsc 5.9, the shipping JavaScript compiler (Node 23) | ~190 ms |
| Dragon, day-one port (one object per AST node) | 145 ms |
| **Dragon, flat-AST rewrite** | **55 - 59 ms** |
| typescript-go, Microsoft's TS 7 parser (Go 1.26) | 49 - 57 ms |

Two claims, stated carefully:

- **Dragon parses the TypeScript compiler's hardest file about 3.4x
  faster than the shipping JavaScript tsc.**
- **Dragon sits in typescript-go's performance band.** Go keeps an edge
  of roughly 5 to 13 percent, which on this laptop is inside thermal
  noise territory, so I call it what it is: a statistical tie that Go
  is winning. I do not claim "faster than Go". Yet.

## What the rewrite taught me

The day-one port was honest, idiomatic code: one Dragon object per AST
node, a dict for keyword lookup, a string for every identifier. It beat
the JavaScript compiler on day one, and sat 2.6x behind Go.

The rewrite changed no grammar logic at all (the fidelity fingerprints
held byte-for-byte before and after). It changed only the representation,
to the shape real compilers use:

- The AST became **one flat int array**. A node is an index, stride 11;
  "none" is -1. Zero heap objects per node, zero refcount traffic in the
  tree, cache lines instead of pointer soup.
- Keywords are recognized by an **int-array trie walked inline** during
  identifier scanning. No dict probe, no string allocated per identifier.
- Tokens and nodes carry **(kind, start, end)** and nothing else. Text is
  sliced lazily by whoever needs it, which is the honest equivalent of
  Go's zero-copy substrings.

Same language, same compiler, 145ms to 55ms. The representation was worth
2.6x; the memory model was never the bottleneck. There is a lesson in
there about porting: typescript-go deliberately mirrored tsc's
pointer-heavy AST one-to-one to keep their translation mechanical, and
they pay for that fidelity on every parse. A native design does not have
to.

One result surprised me. Dragon has a `--gc=none` mode (never free, let
the OS clean up), which is the classic batch-compiler trick, and on the
day-one port it was the fastest configuration. On the flat-AST version,
**ordinary reference counting beat `--gc=none`**: freed arenas from one
parse get reused hot by the next, while never-freeing keeps faulting in
cold pages. When the representation stops allocating, the collector
stops mattering, and locality decides.

As for the remaining sliver of daylight to Go, I can name every meter of
it: bounds checks on array indexing (Go's compiler eliminates many;
Dragon's does not yet), the arena zero-fill on parse startup, and
byte-at-a-time scanning that SIMD helpers would collapse. All three are
generic runtime work already on Dragon's roadmap. None of them are
tsparse-specific tricks. When one lands, I will rerun the table and
write part two.

## The caveats, in plain sight

This is a parser race, not a compiler race: no type checking, no
emit, and no comparison against typescript-go's program-level 10x, which
also leans on multi-file concurrency (Dragon has GIL-free OS threads, so
that fight is available, but I have not picked it yet). The port skips
error recovery, JSDoc, and JSX. Numbers are one machine, best-of-N. The
code, the validation harness, and the methodology are all in
[`packages/tsparse`](https://github.com/rayattack/dragon/tree/main/packages/tsparse)
in the Dragon repo, and the receipts are reproducible below.

## Reproduce it

You need CMake, LLVM 21+, Node 20+, and Go 1.26+ (only for the
competition). Everything else is in the repo.

```bash
git clone https://github.com/rayattack/dragon && cd dragon
mkdir build && cd build && cmake .. && cmake --build . -j4
cd ../packages/tsparse

# the star witness
curl -LO https://raw.githubusercontent.com/microsoft/TypeScript/main/src/compiler/checker.ts

# Dragon: AST fingerprint + timing
../../build/dragon build --release tsparse2.dr -o tsparse2_bin
./tsparse2_bin count checker.ts > dragon.txt
./tsparse2_bin bench checker.ts 20

# the referee: real tsc must produce the identical fingerprint
npm install typescript@5.9
node harness/parsecount.js checker.ts > tsc.txt
diff tsc.txt dragon.txt          # the fidelity claim IS this empty diff

# the competition: typescript-go's actual parser, their own bench setup
git clone --depth 1 https://github.com/microsoft/typescript-go
mkdir -p typescript-go/cmd/parsebench
cp harness/parsebench.go typescript-go/cmd/parsebench/main.go
cd typescript-go && go build -o ../parsebench ./cmd/parsebench && cd ..
./parsebench checker.ts 20
```

`harness/lexdump.js` is also there if you want the stricter check: the
raw token stream, kind by kind and offset by offset, diffed against
`ts.createScanner`.

## The appetizer

A two-day-old parser written in a v0.0.3 language just held its own
against the parser Microsoft's team spent months on, in the language they
chose precisely because Python could not do this. It turns out the family
resemblance was never the problem. Python's syntax was always fast enough
to read; it just needed a sibling that compiles.

The parser is the appetizer. `checker.ts` is not just my benchmark, it
is my to-do list: 54,434 lines that end in a type checker, and the type
checker is the boss fight.

The Netherlands, presumably, stands ready to mint whatever language wins.
