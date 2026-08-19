# tsparse: the TypeScript scanner and parser, ported to Dragon

A proof-of-concept port of tsc's scanner and parser to Dragon, in the spirit
of Microsoft's typescript-go: same raw token stream, same rescan hooks
(greater-than combining, slash-as-regex, template continuation), same
arrow-function disambiguation strategy (cheap lookahead first, snapshot and
backtrack for the ambiguous cases), and tsc-aligned AST node kinds.

Everything here is Dragon (`.dr`). The source is walked as `bytes` so the
scanner's hot loop indexes plain ints and never allocates per character.

## Files

- `scanner.dr` - token kinds, keyword table, and the `Scanner` class
- `parser.dr` - AST `Node`, the `Parser` class, and the validation walk
- `tslex.dr` - CLI: `dump` (token stream) and `bench` (timed raw scans)
- `tsparse.dr` - CLI: `count` (AST kind counts) and `bench` (timed parses)

```bash
dragon run tsparse.dr count file.ts
dragon build --release tsparse.dr -o tsparse_bin
./tsparse_bin bench file.ts 20
```

## Fidelity (validated against real tsc 5.9.3)

Corpus: `checker.ts` (54,434 lines / 3.15 MB), `parser.ts` (10,823 lines),
`scanner.ts` (4,101 lines) from microsoft/TypeScript main.

- **Token streams are byte-identical.** Raw-scan dumps (kind, start, end per
  token) match `ts.createScanner` exactly on all three files: 96,113 tokens.
- **Parse fingerprints are identical.** All three files parse to completion,
  and top-level statement counts plus node counts for 42 validated SyntaxKinds
  (ArrowFunction, CallExpression, TypeReference, UnionType, MappedType,
  RegularExpressionLiteral, ...) match `ts.createSourceFile` exactly.
  That includes every arrow disambiguation, regex-vs-division decision, and
  template rescan in checker.ts agreeing with the reference compiler.

## Benchmarks (best of N, single thread)

Machine: i5-8350U, Linux. Dragon 0.0.3 `--release` (LLVM -O2). Go 1.26.5
running typescript-go @ 8d29e62f via its real `internal/parser` (the same
setup as its own BenchmarkParse). tsc 5.9.3 on Node 23.11.1.

Full parse (scan + AST) of `checker.ts`, 3.15 MB:

| Engine | Best | Throughput | vs Go |
|---|---|---|---|
| typescript-go (Go, native) | 56.6 ms | 53.1 MB/s | 1.00x |
| Dragon `--release --gc=none` | 124.3 ms | 24.2 MB/s | 2.20x slower |
| Dragon `--release` (refcount + cycle collector) | 145.2 ms | 20.7 MB/s | 2.57x slower |
| tsc 5.9 (JS on Node/V8) | 189.9 ms | 16.6 MB/s | 3.35x slower |

Same ordering holds on parser.ts (Go 8.6 ms, Dragon 24.4 ms, ~2.8x) and
scanner.ts (Go 4.1 ms, Dragon 11.3 ms, ~2.8x). Peak RSS for one checker.ts
parse: Go 37 MB, Dragon 68 MB.

The headline: a one-day Dragon port outruns the production JavaScript tsc
parser on tsc's own hardest file, and lands within ~2.2-2.6x of the
typescript-go parser that a team spent months on.

Where the gap to Go lives (measured, not guessed): the scan side is
allocation-bound, not compute-bound. Go substrings are zero-copy views, so
tsgo materializes identifier text for free; Dragon `bytes` slice + `decode()`
copies on every identifier, and the keyword lookup is a str-keyed dict probe.
That is the same string-hash / allocation-churn hot spot already at the top
of the runtime roadmap (see docs 1603); the parse side adds one refcounted
`Node` per AST node (275,586 for checker.ts), which is the binary-trees row
of the same table. Both are runtime work, not language-surface work: the port
itself needed no workarounds.

## tsparse2: the flat-AST rewrite

`scanner2.dr` / `parser2.dr` / `tsparse2.dr` are the same validated grammar
over a compiler-shaped representation (the Zig/Carbon layout):

- Nodes live in one `list[int]` arena, stride 11; a node is its index and
  -1 is "none". Zero per-node heap objects, zero refcount traffic in the tree.
- Variadic children accumulate on an int scratch stack and are copied into a
  flat edges array when a production closes.
- Keywords are recognized by an int-array trie walked inline during
  identifier scanning: no dict probe, no per-identifier string.
- Tokens and nodes carry only (kind, pos, end); text is sliced lazily by
  whoever needs it (tsgo gets this for free from Go's zero-copy substrings,
  so this evens the representational playing field).
- Scanner snapshots for speculation are five ints on a save stack.

Fidelity: byte-count-identical to tsc on all three corpus files, same as v1.

Result on checker.ts (interleaved rounds, same conditions): typescript-go
48.8-57.5 ms, tsparse2 55.1-58.9 ms. The 2.57x gap closed to roughly
1.1x, inside this laptop's thermal noise band; tsgo keeps a single-digit
to ~13% edge. Peak RSS 45 MB vs Go's 37 MB. The remaining daylight is
runtime-level: bounds checks on every list index (Go's compiler eliminates
many; Dragon/LLVM cannot prove them away yet), the per-parse arena
zero-fill, and byte-at-a-time scanning that SIMD runtime helpers would
collapse. Those are exactly the roadmap items - escape analysis, bounds
check elision, SIMD scan helpers - and each is generic runtime work, not
tsparse-specific.

## Deliberate PoC limits

- No error recovery: the first grammar error raises (the corpus is valid).
- No JSDoc, no JSX, no incremental reparse.
- Bytes >= 0x80 are treated as identifier parts (no Unicode ID tables);
  positions are byte offsets (tsc reports UTF-16 code units), so validation
  ran on ASCII-identical copies of the two files containing one non-ASCII
  char each (both inside string/comment content).
- Numeric literals keep raw text; template cooked values skip \r\n
  normalization.
- `import defer` (TS 5.9) is scanned and parsed; `import.defer(...)` call
  syntax is not special-cased (absent from the corpus outside strings).
