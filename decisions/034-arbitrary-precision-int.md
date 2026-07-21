# Decision 034: Arbitrary-Precision Integer Type (`intx`)

**Status:** Proposed

Someone ported RSA keygen from Python and hit `2 ** 63` overflowing at compile time. That's the whole problem in one line. I want `int` to stay native `i64` speed - making every integer arbitrary-precision is Python's model and it's ~50-100× slower. But I also can't tell crypto people "just use something else."

## Context / Motivation

Our `int` is a fixed-width 64-bit signed integer (`i64`). Python's `int` is arbitrary precision. The gap shows up at `2 ** 63`:

```python
# Python: works, returns 9223372036854775808
# Dragon: overflows
x = 2 ** 63
```

Real pain points if you're porting Python code (happens more than you'd think):
- Crypto (RSA moduli, big primes)
- Combinatorics (factorials, binomial coefficients)
- Hashing / number theory
- Financial math with huge integer cents
- Reference algorithms copied verbatim from Python

zen.md says speed first, parity second, no workarounds. Making every `int` arbitrary-precision (Python's model) breaks (1): every arithmetic op pays a tag check and maybe a heap alloc, roughly 50-100x slower than native `i64`. Leaving the gap breaks (2). The fix is a **separate type** for the escape hatch. Default `int` stays at C speed.

Java (`long` + `BigInteger`), Go (`int64` + `math/big.Int`), Rust (`i64` + `num-bigint`), C (`int64_t` + GMP) all do this two-tier thing. Python/Ruby/JS BigInt make arbitrary precision the default and pay on every op.

---

## Options Considered

### Option A - Leave it. Document the i64 ceiling.
- done Max speed, zero work.
- no Hard parity break for crypto / number theory / combinatorics.
- no Violates priority (2) and dogfooding: stdlib modules that silently truncate at 2^63 can't be fixed in pure Dragon.

### Option B - Add `intx` as a separate opt-in type.
- done Plain `int` code unchanged: still single-cycle `i64` arithmetic.
- done Parity gap closed via explicit opt-in: `from numbers import intx`.
- done Same pattern as Java/Go/Rust/C.
- no Two integer types. Users need to know when to reach for which.
- no Implementation cost: tagged representation + runtime arithmetic + codegen.

### Option C - Make `int` itself arbitrary-precision (Python model).
- done Full parity, no separate type.
- no Every arithmetic op pays tag-check + allocation. Small-int optimization helps but you're still ~3-6x slower than native i64. Without it, ~50-100x.
- no Directly violates priority (1). Dragon stops being "C speed, faster than Rust" on the most common op in any program.
- no SIMD/auto-vectorization for integer loops gets harder.

**Decision: Option B.** Add `intx` with tagged-pointer representation. Plain `int` stays `i64`.

---

## Naming

The type is **`intx`**, imported from `numbers`:

```dragon
from numbers import intx

x: intx = 2 ** 500
```

Why `intx`:
- **Autocomplete.** Type `int` and you see `int`, `intc`, `intx` together. `xint` wouldn't group that way.
- **Family consistency.** Dragon already has `intc` (C-int FFI bridge, i32). `int*` is the prefix: `intc` (narrow), `int` (default i64), `intx` (extended).
- **Mnemonic.** `x` = extended. Same ops as `int`, bigger range.

Rejected: `bigint` (boring), `Integer` (case confusion with `int`), `infinite`/`infinity` (float `inf` clash; the value isn't infinite, the precision is), `large` (vague), `Z` (math notation, too terse), `xint` (worse autocomplete).

---

## Performance Characteristics

### When `intx` is unused: zero overhead.

`int` stays `i64` at LLVM. Every `int + int`, `int * int`, `int < int` is one instruction. No tag checks, no allocation. Same as today. `intx` is purely additive.

### When `intx` is used: cost depends on magnitude.

Tagged pointer. Low bit of the payload:
- **Tag = 0**: inline small int. Remaining 63 bits hold signed `[-2^62, 2^62 - 1]`. No allocation.
- **Tag = 1**: pointer to heap bignum (header + GMP-style limb array).

| Value range | `intx` cost vs `int` (i64) | `intx` cost vs Python `int` |
|-----|-----|-----|
| Fits in i63 (the common case) | 3-6× slower | ~10-20× **faster** |
| Fits in 2-4 machine words (~10^38) | 10-50× slower | ~3-10× faster |
| Huge (1000+ decimal digits) | O(n) add, O(n log n) mul | comparable |

**Why faster than CPython even though both are bignum:**
- **Static dispatch.** Codegen knows the type is `intx` and calls `dragon_intx_add` directly. CPython goes through `tp_as_number->nb_add` every time.
- **No PyObject envelope.** CPython allocates a fresh `PyLongObject` per result (32+ byte header). Dragon's small-int path is a tagged i64, zero alloc.
- **No refcount on small path.** Small `intx` values are immediate; only heap path hits GC.
- **No type check per op.** CPython does `PyLong_Check(a)` on every operand; static types skip that.

### Speed summary
- **Code that doesn't use `intx`:** unchanged. Still C-speed.
- **Code that uses `intx` for small values:** ~5× slower than `int`, ~10× faster than Python int.
- **Code that uses `intx` for huge values:** algorithmic cost dominates. Comparable to GMP/PyLong.

Priority (1) holds for code that doesn't opt in. Code that does beats Python at every magnitude. It's not "Python slowness" - it's meaningfully faster than Python, with a known opt-in penalty vs native i64.

---

## Implementation Sketch

### LLVM representation
- `intx` is `i64` at LLVM (same width as `int`), low bit is the tag.
- Inline small: `(value << 1) | 0`. Range `[-2^62, 2^62 - 1]`.
- Heap bignum: `(ptr | 1)`. Mask low bit to dereference.

### Heap layout
```
struct DragonBigInt {
 DragonObjectHeader header; // refcount, type_tag = TAG_INTX, gc_flags
 int32_t sign; // -1, 0, +1
 uint32_t nlimbs; // number of u64 limbs
 uint64_t limbs[]; // flexible array, little-endian
};
```
New tag: `DragonValueTag::TAG_INTX = 13`.

### Runtime functions (lib/Runtime/runtime.cpp)
```c
extern "C" {
 // Construction
 intx_t dragon_intx_from_i64(int64_t v);
 intx_t dragon_intx_from_str(const char* s, int base);

 // Arithmetic - fast path inlined by codegen, slow path here
 intx_t dragon_intx_add_slow(intx_t a, intx_t b);
 intx_t dragon_intx_sub_slow(intx_t a, intx_t b);
 intx_t dragon_intx_mul_slow(intx_t a, intx_t b);
 intx_t dragon_intx_div_slow(intx_t a, intx_t b);
 intx_t dragon_intx_mod_slow(intx_t a, intx_t b);
 intx_t dragon_intx_pow_slow(intx_t a, intx_t b);

 // Comparison
 int dragon_intx_cmp(intx_t a, intx_t b);

 // Conversion
 int64_t dragon_intx_to_i64_checked(intx_t v); // raises OverflowError
 char* dragon_intx_to_str(intx_t v, int base);

 // GC integration
 void dragon_intx_dealloc(void* obj);
}
```

### Codegen fast path
For `a + b` where both are `intx`, codegen emits:
```llvm
; both tags zero (both small)?
%a_tag = and i64 %a, 1
%b_tag = and i64 %b, 1
%any_tag = or i64 %a_tag, %b_tag
%small = icmp eq i64 %any_tag, 0
br i1 %small, label %fast, label %slow

fast:
 ; shift off tag bits, add with overflow check
 %a_val = ashr i64 %a, 1
 %b_val = ashr i64 %b, 1
 %sum_pair = call {i64, i1} @llvm.sadd.with.overflow.i64(i64 %a_val, i64 %b_val)
 %sum = extractvalue {i64, i1} %sum_pair, 0
 %ovf = extractvalue {i64, i1} %sum_pair, 1
 br i1 %ovf, label %slow, label %fast_done
fast_done:
 %tagged = shl i64 %sum, 1
 br label %merge

slow:
 %big = call i64 @dragon_intx_add_slow(i64 %a, i64 %b)
 br label %merge

merge:
 %result = phi i64 [ %tagged, %fast_done ], [ %big, %slow ]
```
Two branches + overflow-checked add for the common case. Slow path handles tag dispatch and overflow promotion to bignum (basically the same as every other tagged-int design, nothing exotic here).

### Bignum backend
**Deferred to implementation.** Two options:
1. **Bundle GMP** (libgmp). Battle-tested, fastest. LGPL - workable for static linking but adds friction. ~1MB to binary.
2. **Roll our own.** Schoolbook + Karatsuba (~32 limbs crossover). No LGPL. ~1500 LOC in `lib/Runtime/bigint.cpp`.

Fast path doesn't care which we pick. Recommendation: roll our own first (avoids LGPL, simpler build), revisit GMP if large-bignum perf is a bottlneck.

### Type system
- New primitive `intx` in the type hierarchy.
- Subtyping: `bool <: int <: intx <: float`? **No.** No implicit widen/narrow between `int` and `intx`. Explicit conversion only. Implicit widening would sneak perf cost in; implicit narrowing can overflow. Force `int(x)` / `intx(y)` at boundaries.
- Literal default: integer literals are `int` (i64) unless context forces `intx`. `9999999999999999999999` is a type error; write `intx("9999999999999999999999")` or `intx(9) ** 30`.

### Frontend changes
- Lexer: no changes. `intx` is an identifier via `numbers` module.
- Parser: no changes.
- Sema/TypeChecker: register `intx` primitive when `from numbers import intx` is seen. `int` + `intx` arithmetic is an error (explicit conversion required).
- CodeGen: new `VarKind::IntX`, fast-path arithmetic, runtime plumbing.

### Stdlib (`stdlib/numbers.dr`)
Thin module exposing `intx` and conversion helpers. Type is a compiler primitive; module is the import surface.

---

## Phases

| Phase | Scope | Acceptance |
|---|---|---|
| 1 | Runtime: `DragonBigInt` struct, `dragon_intx_*` slow-path, GC hooks. Roll-our-own bignum (schoolbook + Karatsuba). | Runtime unit tests for add/sub/mul/div/pow across small/medium/large. |
| 2 | Frontend: register `intx`, type annotations, type-check arithmetic, reject implicit conversions. | TypeChecker tests for annotations and conversion errors. |
| 3 | CodeGen: `VarKind::IntX`, fast-path emission, slow-path lowering, `intx(int)` / `int(intx)`. | CodeGen E2E: `2 ** 100`, factorial(50), RSA-style mod-pow. |
| 4 | Stdlib: `numbers.dr`. | InteropTest for `from numbers import intx`. |
| 5 | Benchmarks: fast-path overhead vs `int` and vs CPython. Document in `decisions/034-benchmarks.md`. | Numbers in predicted ranges (3-6× small, faster-than-CPython at every size). |

---

## Upsides and downsides

**Positive:**
- Closes the big Python parity gap without touching default-path speed.
- Unblocks crypto / number-theory stdlib (currently impossible at i64 scale).
- Pattern for future opt-in numerics (`decimal`, `fraction`).
- `intx` is mostly expressible in pure Dragon (bignum kernel is runtime, which dogfooding policy already allows for data structures).

**Negative:**
- Two integer types. New users need to learn when to pick `intx` (not hard, just different).
- Explicit conversion at the boundary will briefly surprise Python migrants.
- ~1500 LOC of bignum runtime (or LGPL if we bundle GMP).
- Tagged repr means `intx` can't share LLVM type with `int` everywhere. Need `VarKind::IntX` (cheap; already did this for other kinds).

**Out of scope (deferred):**
- `decimal` (arbitrary-precision base-10 floats) - separate ADR.
- `fraction` (rationals) - separate ADR.
- SIMD / vectorized bignum - wait for benchmarks.
- Implicit `int → intx` on overflow (Python auto-promotion) - rejected; opt in by typing `intx`.
