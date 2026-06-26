# Performance and the Three Commandments

Dragon's entire design answers to three rules, in strict priority order.
They are not marketing - they are the tie-breakers the language uses
whenever two designs compete, and they explain every trade-off in the
book.

1. **Speed is king.** Dragon aims to run at C-class speed and to beat Rust
   on compute. When two approaches both work, the one that emits faster
   code wins - no exceptions.
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
at `-O2`, best-of-three on the same machine. The live, continuously
re-run table is at [the benchmarks page](/benchmarks); the snapshot below
is current as of this writing.

| Benchmark | Dragon | Rust | Verdict |
|---|---|---|---|
| Fibonacci (recursive, n=42) | 0.848s | 0.986s | **Dragon 1.16× faster** |
| Mandelbrot (float, 1600², 100 iter) | 0.421s | 0.452s | **Dragon 1.07× faster** |
| Sieve of Eratosthenes (1M) | 0.007s | 0.006s | tie (noise) |
| String concat (10k) | 0.003s | 0.003s | tie (noise) |
| Object creation (1M) | 0.002s | 0.003s | tie (noise) |
| Parallel sum (8×30M fork-join) | 0.174s | 0.149s | Dragon 1.17× slower |
| Dictionary / hashmap (3M str-key ops) | 1.144s | 0.666s | Dragon 1.72× slower |
| Binary trees (alloc churn, depth 14) | 0.255s | 0.095s | Dragon 2.68× slower |

The headline result is real: on **compute-bound** work Dragon is at or
ahead of `rustc`. Recursive integer math and tight floating-point loops -
where the program is dominated by arithmetic, not allocation - lean
entirely on the LLVM backend and the native-typed value model, and there
Dragon already wins. (On fib it also beats Go and Java, and trails C and
C++ by only about 12%.)

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

- **Binary trees (2.68× slower)** is the worst, and it is pure
  allocate-and-free churn. It barely computes anything; it measures the
  **reference-counting runtime**, not the compiler. Every tree node is a
  pair of refcount operations, and at this allocation rate that
  bookkeeping dominates. It is the canary benchmark: it will move the day
  the runtime stops counting acyclic objects it can prove never form
  cycles.
- **Dicts (1.72× slower)** is a string-keyed hashmap. The suspect is the
  hash-and-compare path for string keys in the dictionary implementation,
  not the loop around it.
- **Parallel sum (1.17× slower)** is the closest gap - green-thread
  fork-join carries slightly more overhead than Rust's OS threads, though
  Dragon still beats Java and sits near Go here.

The sub-10-millisecond rows - sieve, string concat, object creation - are
**noise, not signal**. They finish in two to seven milliseconds, where
process startup, not the workload, dominates the clock. (That object
creation lands at two milliseconds is itself worth noting: the
refcount-tracking overhead that used to show up at this scale no longer
does.)

## The work in front of the commandments

Because *speed is king*, the allocation and hashmap paths are the
highest-leverage targets, and they are exactly what the runtime work
focuses on next - not new features stacked on top, but the slow rows
driven down. The plan is the kind of root-cause fix the second
commandment demands: teach the refcounter to skip objects it can prove
are acyclic (so binary-trees stops paying for cycle-safety it never
needs), and profile the string-key hash path until the dict gap closes.

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
