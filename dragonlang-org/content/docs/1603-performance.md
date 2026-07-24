# Performance and the Three Commandments

Dragon's entire design answers to three rules, in strict priority order.
They are not marketing - they are the tie-breakers the language uses
whenever two designs compete, and they explain every trade-off in the
book.

1. **Speed is king.** Dragon aims to run at C-class speed. When two
   approaches both work, the one that emits faster code wins - no
   exceptions.
2. **Efficiency trumps quick wins.** Never take the easy road when a
   better one exists. No workarounds, no convenience shims that leak
   performance. When something is slow at the root, the root gets fixed.
3. **Types must be honest.** Every value whose type is known flows *at*
   that type - monomorphized, never boxed - so the hot path stays
   allocation-free. The dynamic escape hatches (`Any`, boxing) exist only
   for genuinely dynamic data, never as a convenience that quietly costs
   speed.

The previous two chapters showed the machinery:
[native-typed values](/docs/1602-how-dragon-compiles) and the
[reference-counted memory model](/docs/1601-memory-model). This chapter is about the
results - measured, and honest about where Dragon still loses.

## The numbers

These are from the project's `versus/` suite: Dragon against Rust, both
at `-O2`, best-of-three on the same machine. Rust is the yardstick rather
than the target - it is the fastest language with a comparable safety
story, so it is the honest thing to measure a young runtime against. The
live, continuously re-run table is at [the benchmarks page](/benchmarks);
the snapshot below is current as of this writing.

| Benchmark | Dragon | Rust | Verdict |
|---|---|---|---|
| Fibonacci (recursive, n=42) | 0.857s | 0.878s | **Dragon 1.02× faster** |
| Mandelbrot (float, 1600², 100 iter) | 0.457s | 0.457s | tie |
| Sieve of Eratosthenes (1M) | 0.007s | 0.006s | tie (noise) |
| String concat (10k) | 0.003s | 0.003s | tie (noise) |
| Object creation (1M) | 0.006s | 0.003s | tie (noise) |
| Parallel sum (8×30M fork-join) | 0.214s | 0.149s | Dragon 1.44× slower |
| Dictionary / hashmap (3M str-key ops) | 1.657s | 0.704s | Dragon 2.35× slower |
| Binary trees (alloc churn, depth 14) | 0.316s | 0.089s | Dragon 3.55× slower |

The headline result is real: on **compute-bound** work Dragon runs at
parity with `rustc` - a tie on mandelbrot, a hair ahead on recursive
`fib`. Recursive integer math and tight floating-point loops - where the
program is dominated by arithmetic, not allocation - lean entirely on the
LLVM backend and the native-typed value model, and there Dragon matches a
mature systems compiler. (On fib it also beats Go and Java outright and
trails C and C++ by under 30%.)

The fib benchmark is just ordinary Dragon, compiled with `--release`:

```dragon
def fib(n: int) -> int {
    if n < 2 {
        return n
    }
    return fib(n - 1) + fib(n - 2)
}
print(fib(30))      # 832040
```

```bash
dragon build --release fib.dr -o fib
```

Nothing here is special-cased. Each `int` is an `i64` in a register, each
call is a direct native call, and LLVM at `-O2` does the rest. That is
what "reads like Python, runs like C" means in practice.

## Where Dragon loses, and why

The losses are kept visible on purpose - hiding them would violate the
second commandment more than having them does. All three slow rows share
a cause, and it is *not* code generation:

- **Binary trees (3.55× slower)** is the worst, and it is pure
  allocate-and-free churn. It barely computes anything; it measures the
  **memory-management runtime**, not the compiler. The `Node` type holds
  `Node` fields, so it cannot be *statically* proven acyclic - the cyclic
  collector must consider it. But the trees this benchmark builds never
  actually form cycles; reference counting frees every node the instant it
  goes out of scope, and each cyclic-collection pass scans the live set and
  reclaims nothing. The runtime now backs off that trigger adaptively -
  when a collection frees no cycles it grows the interval before the next
  one (bounded proportional to the live set, so genuine cyclic garbage is
  still caught) - which cut this row from over 1.7 seconds to its current
  time. Closing the rest of the gap is a static-escape or generational
  question, not a code-generation one.
- **Dicts (2.35× slower)** is a string-keyed hashmap. The suspect is the
  hash-and-compare path for string keys in the dictionary implementation,
  not the loop around it.
- **Parallel sum (1.44× slower)** is fork-join over eight workers -
  green-thread scheduling carries more overhead than Rust's raw OS threads,
  though Dragon still beats Java here.

The sub-10-millisecond rows - sieve, string concat, object creation - are
**noise, not signal**. They finish in three to seven milliseconds, where
process startup, not the workload, dominates the clock and run-to-run
jitter swamps any real difference.

## The work in front of the commandments

Because *speed is king*, the allocation and hashmap paths are the
highest-leverage targets, and they are exactly what the runtime work
focuses on next - not new features stacked on top, but the slow rows
driven down. The adaptive cycle-collection trigger described above is one
such root-cause fix already landed; the next steps are the same shape:
static-escape analysis so a provably-acyclic allocation is never tracked
at all (driving binary-trees the rest of the way down), and profiling the
string-key hash path until the dict gap closes.

This is the honest shape of a young compiled language: the codegen story
is already won, and the remaining gaps are in the runtime data structures,
where they are tractable and measurable. The benchmarks are re-run as the
runtime improves, so [the live table](/benchmarks) is the source of truth
- this page describes the shape of the result, not a frozen scoreboard.

## What you can do today

The language already gives you the levers that matter:

- **Build with `--release`** (LLVM `-O2`) for anything you measure or
  ship. The default `-O0` build is for fast iteration, not for speed
  claims.
- **For short-lived programs, `--gc=none`** skips reference counting
  entirely and lets the OS reclaim memory at exit - ideal for a one-shot
  CLI filter or a micro-benchmark, never for a long-running service (see
  [The Memory Model](/docs/1601-memory-model)).
- **Keep types concrete.** A value left as `Any` is boxed; a value with a
  known type is not. Honest, specific types are not just safer - per the
  third commandment, they are the difference between a register and a heap
  allocation.

Write ordinary, well-typed Dragon, build it with `--release`, and the
compute path is already competitive with the fastest languages in the
world. The rest is the runtime catching up to the codegen, in priority
order, exactly as the commandments dictate.
