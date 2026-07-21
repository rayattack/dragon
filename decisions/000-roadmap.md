# Dragon: The Snake That Became a Dragon [Partially Implemented]

I keep this doc around as a reality check, not the roadmap I wish we had. Dragon is a typed compiled language *inspired by* Python, not a superset, not CPython. You write `.dr` with braces or `.py` with indentation, both go through our own frontend/runtime/semantics and come out as native binaries. Scoping, types, dispatch: Dragon rules, not Python's.

When I first wrote this I had 647 tests across 11 suites (v0.0.0-ish). Now it's 1089 tests across 10 suites. Everything below is basically a historical snapshot from early on - useful for seeing how optimistic I was, not for current status. Check the individual decision docs for what's actually true today.

## Status Legend


| Symbol  | Meaning                                                            |
| ------- | ------------------------------------------------------------------ |
| done    | Works end-to-end (compiles + runs)                                 |
| partial | Partially implemented (works for common cases, edge cases missing) |
| parsed  | Parsed only (AST exists, no code generation / no-op at runtime)    |
| no      | Not supported at all (syntax error or silently ignored)            |
| blocked | Intentionally unsupported (incompatible with AOT compilation)      |


---

## The Honest Numbers

I deliberately made these numbers pessimistic because our old "~42% done" claim was bullshit. It counted stuff that parses but never emits, and stdlib modules we'd only written down on paper. Parsed-but-not-emitted doesn't count. Planned modules that never shipped don't count either.


| Category                     | Done | Total | Real %      | Notes                                                                             |
| ---------------------------- | ---- | ----- | ----------- | --------------------------------------------------------------------------------- |
| Syntax (grammar productions) | ~70% | 100%  | **70%**     | Missing: match/case, generators, tuple unpack, param separators                   |
| Keywords                     | 23   | 35+4  | **59%**     | 7 parsed-only, 4 missing entirely                                                 |
| Built-in functions           | 31   | 71    | **44%**     | Many are stubs (limited type dispatch)                                            |
| Operators                    | 33   | 34    | **97%**     | Missing: `@` matrix multiply only                                                 |
| String methods               | 38   | 47    | **81%**     | Missing: encode, format, format_map, maketrans, translate                         |
| List methods                 | 11   | 11    | **100%**    | Complete                                                                          |
| Dict methods                 | 8    | 11    | **73%**     | Missing: fromkeys, popitem; items is broken                                       |
| Set methods                  | 0    | 17    | **0%**      | Set is a list internally - no real set                                            |
| Exceptions                   | 7    | 68    | **10%**     | Only 7 exception types mapped                                                     |
| Dunder/magic methods         | 1    | ~105  | **~1%**     | Only `__init__`. No operator overloading, no protocols                            |
| Object model features        | 2    | ~25   | **8%**      | `__init__` + basic method dispatch. No inheritance emit, no super, no descriptors |
| Stdlib modules               | 5    | ~300  | **~2%**     | Only math/os/sys/time/string actually work. 8 more planned but not shipped        |
| Memory management            | 0%   | 100%  | **0%**      | 53 malloc sites, 0 free calls. 100% leak rate                                     |
| **Realistic weghted total**  |      |       | **~15-18%** | Weighted by real-world usage frequency                                            |


We used to claim ~42% weighted overall. That was bullshit - it counted stuff that parses but never emits, and stdlib modules we'd only written down on paper. Real number for things that compile and run: **15-18%** of Python 3.12.

What does work is solid though: arithmetic, strings, lists/dicts, typed functions, basic classes, multi-file builds, exceptions. Good base to build on.

---

## 1. Syntax Coverage

### 1.1 Keywords (35 hard + 4 soft)


| Keyword    | Status  | Notes                                                           |
| ---------- | ------- | --------------------------------------------------------------- |
| `False`    | done    | Literal → `0` / `i1 0`                                          |
| `True`     | done    | Literal → `1` / `i1 1`                                          |
| `None`     | done    | Literal → `NULL` / `null ptr`                                   |
| `and`      | done    | → `&&` / LLVM short-circuit                                     |
| `or`       | done    | → `                                                             |
| `not`      | done    | → `!` / `xor i1`                                                |
| `if`       | done    | Full if/elif/else                                               |
| `elif`     | done    | Chained else-if                                                 |
| `else`     | done    | if-else, try-else                                               |
| `for`      | done    | range loops; collection iteration (CEmitter)                    |
| `while`    | done    | Full while loops                                                |
| `break`    | done    | Both backends                                                   |
| `continue` | done    | Both backends                                                   |
| `pass`     | done    | `/* pass */` / nop                                              |
| `def`      | done    | Functions with type annotations                                 |
| `return`   | done    | With/without value                                              |
| `class`    | done    | Struct + constructor + methods (CEmitter)                       |
| `lambda`   | done    | Hoisted to top-level function (CEmitter)                        |
| `import`   | done    | stdlib registry + multi-file module resolution                  |
| `from`     | done    | from...import with aliases + cross-file imports                 |
| `as`       | partial | Import aliases work; except-as partial                          |
| `assert`   | done    | Runtime check with abort                                        |
| `raise`    | done    | dragon_raise with typed exceptions                              |
| `try`      | done    | setjmp/longjmp (CEmitter)                                       |
| `except`   | done    | Typed exception matching                                        |
| `finally`  | done    | Always-execute block                                            |
| `with`     | parsed  | Parsed, emits block body only (no context manager protocol)     |
| `yield`    | parsed  | Parsed, not emitted (no generator machinery)                    |
| `global`   | parsed  | Parsed, no-op (all module-level vars are already C globals)     |
| `nonlocal` | parsed  | Parsed, no-op (no closure capture)                              |
| `del`      | parsed  | Parsed, basic support (no `__del__` or ref management)          |
| `in`       | done    | for-in loops; `x in list/str/dict` membership                   |
| `is`       | partial | → `==` pointer comparison (correct for None, wrong for objects) |
| `async`    | parsed  | Parsed, not emitted                                             |
| `await`    | parsed  | Parsed, not emitted                                             |
| `match`    | no      | Soft keyword, not parsed (see decisions/002-syntax-parity.md)   |
| `case`     | no      | Soft keyword, not parsed                                        |
| `type`     | no      | Soft keyword (3.12 type aliases), not parsed                    |
| `_`        | no      | Soft keyword (match wildcard), not parsed                       |


**Keywords: 23/39 done (59%) | 2 partial | 7 parsed | 4 no**

### 1.2 Missing Syntax Constructs


| Feature                                     | Impact                       | Status                                                |
| ------------------------------------------- | ---------------------------- | ----------------------------------------------------- |
| `match`/`case` (PEP 634)                    | High - growing adoption      | no Not parsed                                         |
| Generator expressions `(x for x in y)`      | High - used in sum, any, all | no No AST node                                        |
| Tuple unpacking `a, b = 1, 2`               | High - used everywhere       | no Not supported                                      |
| `*args` / `**kwargs` in calls               | High - core Python idiom     | partial Partial (starred parsed, double-star missing) |
| Keyword-only params `def f(*, key=val)`     | Medium                       | no Not parsed                                         |
| Positional-only params `def f(x)`           | Medium                       | no Not parsed                                         |
| F-string format specs `f"{x:.2f}"`          | Medium                       | no Not parsed                                         |
| Chained comparisons `a < b < c`             | Medium                       | partial Parses but wrong semantics (left-to-right)    |
| `type` alias statement (PEP 695)            | Low                          | no Not parsed                                         |
| `except`* exception groups (PEP 654)        | Low                          | no Not parsed                                         |
| Nested f-strings                            | Low                          | no                                                    |
| Class keyword args `class Foo(metaclass=M)` | Low                          | no                                                    |


See [decisions/002-syntax-parity.md](decisions/002-syntax-parity.md) for the complete roadmap.

---

## 2. Built-in Functions (71 total)


| Function       | Status  | Notes                                                       |
| -------------- | ------- | ----------------------------------------------------------- |
| `abs`          | done    | `llabs` / `fabs` dispatch                                   |
| `aiter`        | no      | Requires async iteration                                    |
| `all`          | done    | `dragon_list_all`                                           |
| `anext`        | no      | Requires async iteration                                    |
| `any`          | done    | `dragon_list_any`                                           |
| `ascii`        | no      |                                                             |
| `bin`          | done    | `dragon_bin`                                                |
| `bool`         | done    | Type cast                                                   |
| `breakpoint`   | no      |                                                             |
| `bytearray`    | no      | No bytes type                                               |
| `bytes`        | no      | No bytes type                                               |
| `callable`     | no      | No runtime type introspection                               |
| `chr`          | done    | `dragon_chr`                                                |
| `classmethod`  | parsed  | Parsed as decorator, not emitted                            |
| `compile`      | blocked | Inherently dynamic - incompatible with AOT                  |
| `complex`      | no      | No complex type                                             |
| `delattr`      | no      | No dynamic attribute access                                 |
| `dict`         | partial | Literal `{}` works, `dict` constructor doesn't              |
| `dir`          | blocked | Requires runtime introspection                              |
| `divmod`       | done    | `dragon_divmod` returns list                                |
| `enumerate`    | done    | `dragon_enumerate`                                          |
| `eval`         | blocked | Inherently dynamic - incompatible with AOT                  |
| `exec`         | blocked | Inherently dynamic - incompatible with AOT                  |
| `filter`       | done    | `dragon_filter` with fn ptr                                 |
| `float`        | done    | Type cast                                                   |
| `format`       | no      |                                                             |
| `frozenset`    | no      |                                                             |
| `getattr`      | no      | Would need runtime field lookup table                       |
| `globals`      | blocked | Requires runtime symbol table                               |
| `hasattr`      | no      | Would need runtime field lookup table                       |
| `hash`         | no      | No `__hash__` protocol                                      |
| `help`         | blocked | REPL feature                                                |
| `hex`          | done    | `dragon_hex`                                                |
| `id`           | partial | Could return pointer address, not implemented               |
| `input`        | done    | `dragon_input` runtime                                      |
| `int`          | done    | Type cast                                                   |
| `isinstance`   | partial | Always returns 1 (no runtime type tags)                     |
| `issubclass`   | no      | No class hierarchy at runtime                               |
| `iter`         | no      | No iterator protocol                                        |
| `len`          | done    | str + list dispatch (not custom `__len__`)                  |
| `list`         | partial | Literal `[]` works, `list` constructor doesn't              |
| `locals`       | blocked | Requires runtime symbol table                               |
| `map`          | done    | `dragon_map` with fn ptr                                    |
| `max`          | done    | Two-arg ternary or `dragon_list_max`                        |
| `memoryview`   | no      |                                                             |
| `min`          | done    | Two-arg ternary or `dragon_list_min`                        |
| `next`         | no      | No iterator protocol                                        |
| `object`       | no      | No base object type                                         |
| `oct`          | done    | `dragon_oct`                                                |
| `open`         | no      | No file I/O runtime                                         |
| `ord`          | done    | `dragon_ord`                                                |
| `pow`          | done    | `dragon_pow_int` (also `**` operator)                       |
| `print`        | done    | Type-aware dispatch (int/float/str/bool/None/list)          |
| `property`     | parsed  | Parsed as decorator, not emitted                            |
| `range`        | done    | Special-cased in for loops (1/2/3 arg forms)                |
| `repr`         | no      | No `__repr__` protocol                                      |
| `reversed`     | done    | `dragon_list_reversed`                                      |
| `round`        | done    | `dragon_round`                                              |
| `set`          | partial | Literal works but stored as list (no dedup, no O(1) lookup) |
| `setattr`      | no      | No dynamic attribute access                                 |
| `slice`        | done    | Slice syntax `[a:b:c]` fully emitted                        |
| `sorted`       | done    | `dragon_list_sorted`                                        |
| `staticmethod` | parsed  | Parsed as decorator, not emitted                            |
| `str`          | done    | Type cast                                                   |
| `sum`          | done    | `dragon_list_sum`                                           |
| `super`        | no      | No inheritance dispatch                                     |
| `tuple`        | partial | Literal parsed, limited emission                            |
| `type`         | partial | Returns type name string (limited, no runtime type)         |
| `vars`         | blocked | Requires runtime symbol table                               |
| `zip`          | done    | `dragon_zip` (2-arg)                                        |
| `__import__`   | blocked | Compile-time import only                                    |


**Builtins: 31/71 done (44%) | 7 partial | 3 parsed | 23 no | 7 blocked**

**Note:** 7 functions marked blocked are fundamentally incompatible with ahead-of-time compilation. These will never be supported. The real denominator is 64, making the effective rate **31/64 = 48%**.

---

## 3. Operators

### Arithmetic


| Operator  | Status | Notes                                             |
| --------- | ------ | ------------------------------------------------- |
| `+`       | done   | int, float; string concat via `dragon_str_concat` |
| `-`       | done   | int, float                                        |
| `*`       | done   | int, float; string repeat via `dragon_str_repeat` |
| `/`       | done   | true division                                     |
| `//`      | done   | `dragon_floordiv_int` Python semantics            |
| `%`       | done   | `dragon_mod_int` Python semantics                 |
| `*`*      | done   | `dragon_pow_int`                                  |
| `@`       | no     | Matrix multiply (no numpy support)                |
| Unary `-` | done   |                                                   |
| Unary `+` | done   |                                                   |
| `~`       | done   | Bitwise NOT                                       |


### Comparison


| Operator          | Status | Notes                             |
| ----------------- | ------ | --------------------------------- |
| `==`              | done   | Primitive + string (via `strcmp`) |
| `!=`              | done   | Primitive + string                |
| `<` `<=` `>` `>=` | done   | Primitive + string comparison     |


**Note:** Comparison operators work on primitives and strings. They do NOT dispatch through `__eq__`/`__lt__` etc. on custom classes.

### Logical


| Operator | Status | Notes                                                      |
| -------- | ------ | ---------------------------------------------------------- |
| `and`    | done   | Short-circuit `&&` (NOT Python truthiness - no `__bool__`) |
| `or`     | done   | Short-circuit `                                            |
| `not`    | done   | `!`                                                        |


### Bitwise

All 5 bitwise operators (`&` `\|` `^` `<<` `>>`) - done

### Augmented Assignment (13)


| Operator         | Status              | Notes                                 |
| ---------------- | ------------------- | ------------------------------------- |
| `+=`             | done                | Int/float + string concat via realloc |
| `-=` `*=` `/=`   | done                |                                       |
| `//=` `%=` `**=` | done                | Runtime function calls                |
| `&=` `           | = ``^= ``<<= ``>>=` | done                                  |
| `@=`             | no                  | Matrix multiply                       |


### Chained Comparisons


| Feature     | Status  | Notes                                                                                                                   |
| ----------- | ------- | ----------------------------------------------------------------------------------------------------------------------- |
| `a < b < c` | partial | **Parses but WRONG semantics** - evaluates as `(a < b) < c` instead of `(a < b) and (b < c)`. See 002-syntax-parity.md. |


**Operators: 33/34 done (97%) - but chained comparisons are semantically broken**

---

## 4. Data Types

### Primitive Types


| Type        | Status | C Repr          | LLVM Repr | Semantic Gaps                                               |
| ----------- | ------ | --------------- | --------- | ----------------------------------------------------------- |
| `int`       | done   | `int64_t`       | `i64`     | Fixed 64-bit (Python has arbitrary precision)               |
| `float`     | done   | `double`        | `double`  | Same as CPython                                             |
| `bool`      | done   | `int`           | `i1`      | No truthiness protocol (`__bool__`)                         |
| `str`       | done   | `const char`*   | `i8`*     | Immutable C strings. No Unicode beyond ASCII. No `__str__`. |
| `None`      | done   | `NULL` / `void` | `null`    |                                                             |
| `complex`   | no     | -               | -         |                                                             |
| `bytes`     | no     | -               | -         |                                                             |
| `bytearray` | no     | -               | -         |                                                             |


**Critical gap:** `int` is 64-bit fixed-width, not arbitrary precision. `2 ** 100` will overflow silently.

### Collection Types


| Type         | Status  | Implementation                                 | Semantic Gaps                                                                |
| ------------ | ------- | ---------------------------------------------- | ---------------------------------------------------------------------------- |
| `list`       | done    | `dragon_list_t`* (void* array, int64 size/cap) | No type homogeneity enforcement at runtime. 100% memory leak.                |
| `dict`       | done    | `dragon_dict_t`* (string keys → void* values)  | Keys are `const char`* only (not arbitrary hashable). Leaks memory.          |
| `set`        | partial | Stored as `dragon_list_t`* internally          | No deduplication, no O(1) membership, no set operations. Essentially broken. |
| `tuple`      | parsed  | Parsed, limited emission                       | No tuple type at runtime. Cannot unpack.                                     |
| `frozenset`  | no      | -                                              |                                                                              |
| `range`      | done    | Special-cased in for loops                     | Not a real object - can't be stored in a variable or passed                  |
| `slice`      | done    | `dragon_list_slice` / `dragon_str_slice`       | Works for list and str slicing                                               |
| `memoryview` | no      | -                                              |                                                                              |


### Type Annotations


| Feature                           | Status | Notes                               |
| --------------------------------- | ------ | ----------------------------------- |
| Named types (`int`, `str`)        | done   | Resolved by TypeChecker             |
| Generic types (`list[int]`)       | done   | Parsed and checked                  |
| Optional (`int                    | None`) | parsed                              |
| Union (`int                       | str`)  | parsed                              |
| Callable (`Callable[[int], str]`) | parsed | Parsed, no function type at runtime |
| Tuple type (`tuple[int, str]`)    | parsed | Parsed, no tuple type at runtime    |


---

## 5. String Methods (47 total)


| Method         | Status | Runtime Function          |
| -------------- | ------ | ------------------------- |
| `upper`        | done   | `dragon_str_upper`        |
| `lower`        | done   | `dragon_str_lower`        |
| `strip`        | done   | `dragon_str_strip`        |
| `find`         | done   | `dragon_str_find`         |
| `replace`      | done   | `dragon_str_replace`      |
| `startswith`   | done   | `dragon_str_startswith`   |
| `endswith`     | done   | `dragon_str_endswith`     |
| `capitalize`   | done   | `dragon_str_capitalize`   |
| `casefold`     | done   | `dragon_str_casefold`     |
| `center`       | done   | `dragon_str_center`       |
| `count`        | done   | `dragon_str_count`        |
| `encode`       | no     | No bytes type             |
| `expandtabs`   | done   | `dragon_str_expandtabs`   |
| `format`       | no     | Use f-strings instead     |
| `format_map`   | no     |                           |
| `index`        | done   | `dragon_str_index`        |
| `isalnum`      | done   | `dragon_str_isalnum`      |
| `isalpha`      | done   | `dragon_str_isalpha`      |
| `isascii`      | done   | `dragon_str_isascii`      |
| `isdecimal`    | done   | `dragon_str_isdecimal`    |
| `isdigit`      | done   | `dragon_str_isdigit`      |
| `isidentifier` | done   | `dragon_str_isidentifier` |
| `islower`      | done   | `dragon_str_islower`      |
| `isnumeric`    | done   | `dragon_str_isnumeric`    |
| `isprintable`  | done   | `dragon_str_isprintable`  |
| `isspace`      | done   | `dragon_str_isspace`      |
| `istitle`      | done   | `dragon_str_istitle`      |
| `isupper`      | done   | `dragon_str_isupper`      |
| `join`         | done   | `dragon_str_join`         |
| `ljust`        | done   | `dragon_str_ljust`        |
| `lstrip`       | done   | `dragon_str_lstrip`       |
| `maketrans`    | no     |                           |
| `partition`    | done   | `dragon_str_partition`    |
| `removeprefix` | done   | `dragon_str_removeprefix` |
| `removesuffix` | done   | `dragon_str_removesuffix` |
| `rfind`        | done   | `dragon_str_rfind`        |
| `rindex`       | done   | `dragon_str_rindex`       |
| `rjust`        | done   | `dragon_str_rjust`        |
| `rpartition`   | done   | `dragon_str_rpartition`   |
| `rsplit`       | done   | `dragon_str_rsplit`       |
| `rstrip`       | done   | `dragon_str_rstrip`       |
| `split`        | done   | `dragon_str_split`        |
| `splitlines`   | done   | `dragon_str_splitlines`   |
| `swapcase`     | done   | `dragon_str_swapcase`     |
| `title`        | done   | `dragon_str_title`        |
| `translate`    | no     |                           |
| `zfill`        | done   | `dragon_str_zfill`        |


**String methods: 38/47 done (81%)** - This is genuinely strong. Every string method that works allocates new memory and never frees it.

---

## 6. List Methods (11 total)

All 11 list methods are implemented: `append`, `clear`, `copy`, `count`, `extend`, `index`, `insert`, `pop`, `remove`, `reverse`, `sort`.

**List methods: 11/11 done (100%)**

**Caveat:** `clear` resets the size counter but does NOT free the backing array or contained elements. Every list operation leaks.

---

## 7. Dict Methods (11 total)


| Method       | Status  | Notes                                                      |
| ------------ | ------- | ---------------------------------------------------------- |
| `clear`      | partial | Resets size counter, does NOT free keys/values             |
| `copy`       | done    | Shallow copy                                               |
| `fromkeys`   | no      |                                                            |
| `get`        | done    | `dragon_dict_get_default`                                  |
| `items`      | partial | Returns keys only (broken - should return key-value pairs) |
| `keys`       | done    | `dragon_dict_keys`                                         |
| `pop`        | done    | `dragon_dict_pop`                                          |
| `popitem`    | no      |                                                            |
| `setdefault` | done    | `dragon_dict_setdefault`                                   |
| `update`     | done    | `dragon_dict_update`                                       |
| `values`     | done    | `dragon_dict_values`                                       |


**Dict methods: 7/11 done (64%)** - `items` is broken, `clear` leaks.

---

## 8. Set Methods (17 total)

**Set methods: 0/17 done (0%)**

Set is stored as a `dragon_list_t`* internally. So:

- No deduplication (`{1, 1, 2}` stores three elements)
- No O(1) membership test
- None of the 17 set methods work: `add`, `remove`, `discard`, `pop`, `clear`, `union`, `intersection`, `difference`, `symmetric_difference`, `issubset`, `issuperset`, `isdisjoint`, `update`, `intersection_update`, `difference_update`, `symmetric_difference_update`, `copy`

Set needs a proper hash table implementation to be functional.

---

## 9. Built-in Exceptions (68 total)


| Exception           | Status | C Runtime Constant     |
| ------------------- | ------ | ---------------------- |
| `Exception`         | done   | `DRAGON_EXC_EXCEPTION` |
| `ValueError`        | done   | `DRAGON_EXC_VALUE`     |
| `TypeError`         | done   | `DRAGON_EXC_TYPE`      |
| `RuntimeError`      | done   | `DRAGON_EXC_RUNTIME`   |
| `IndexError`        | done   | `DRAGON_EXC_INDEX`     |
| `KeyError`          | done   | `DRAGON_EXC_KEY`       |
| `ZeroDivisionError` | done   | `DRAGON_EXC_ZERO_DIV`  |


Missing exceptions that matter for real code:

- `AttributeError` - needed for any attribute access failure
- `FileNotFoundError` / `IOError` - needed for file operations
- `ImportError` / `ModuleNotFoundError` - needed for import failures
- `StopIteration` - needed for iterator protocol
- `NameError` - needed for undefined variable errors
- `OverflowError` - needed for int overflow (we silently wrap instead)
- `NotImplementedError` - common in abstract base patterns
- `OSError` and subclasses - needed for system calls

**Exceptions: 7/68 done (10%)**

---

## 10. Object Model

This is the biggest gap between Dragon and Python. Python's power comes from its object model - everything is an object, every operation goes through protocols (`__dunder__` methods). Dragon has none of this.

### What Works


| Feature                    | Status | Notes                                          |
| -------------------------- | ------ | ---------------------------------------------- |
| `class` declaration        | done   | → C struct                                     |
| `__init__` constructor     | done   | Called by `ClassName_new`                      |
| Instance fields (`self.x`) | done   | Extracted from `__init__` body                 |
| Instance methods           | done   | Static dispatch: `ClassName_method(self, ...)` |
| Constructor calls          | done   | `MyClass` → `MyClass_new`                      |
| Attribute access           | done   | `obj.field` → `obj->field`                     |
| Method calls               | done   | `obj.method` → `ClassName_method(obj, ...)`    |


### What Doesn't Work


| Feature                       | Status | Impact                                         |
| ----------------------------- | ------ | ---------------------------------------------- |
| Single inheritance            | parsed | Parsed, not emitted in C                       |
| Multiple inheritance          | no     | No MRO                                         |
| `super`                       | no     | No parent dispatch                             |
| `@staticmethod`               | parsed | Parsed, not emitted                            |
| `@classmethod`                | parsed | Parsed, not emitted                            |
| `@property`                   | parsed | Parsed, not emitted                            |
| Decorators (general)          | parsed | Parsed, not emitted                            |
| Class variables               | no     | No static fields                               |
| `__slots__`                   | no     |                                                |
| `__str__` / `__repr__`        | no     | print uses type dispatch, not dunder           |
| `__len__`                     | no     | len uses type dispatch, not dunder             |
| `__getitem__` / `__setitem__` | no     | Subscript works for list/dict only, not custom |
| `__iter__` / `__next__`       | no     | No iterator protocol                           |
| `__enter__` / `__exit__`      | no     | No context manager protocol                    |
| `__call__`                    | no     | No callable objects                            |
| `__eq__` / `__lt__` etc.      | no     | Operators work on primitives only              |
| `__add__` / `__mul__` etc.    | no     | No operator overloading                        |
| `__hash__`                    | no     | No hashable protocol                           |
| `__bool__`                    | no     | No truthiness protocol                         |
| Descriptors                   | no     | No descriptor protocol                         |
| Metaclasses                   | no     |                                                |
| `__init_subclass__`           | no     |                                                |
| Abstract methods              | no     |                                                |


**Object model: ~8% of Python's object protocol is supported.** This is the single biggest barrier to Python compatibility. Until Dragon has at least `__str__`, `__len__`, `__getitem__`, `__iter__`, and `__eq__`, most Python libraries cannot run.

---

## 11. Standard Library

### Actually Working (5 modules)

These modules have C-shim implementations that produce correct output:


| Module   | Functions                                                               | Status |
| -------- | ----------------------------------------------------------------------- | ------ |
| `math`   | `sqrt sin cos tan log log10 log2 exp pow fabs abs ceil floor pi e` (15) | done   |
| `os`     | `getcwd getenv system getpid` (4)                                       | done   |
| `sys`    | `exit` (1)                                                              | done   |
| `time`   | `time clock sleep` (3)                                                  | done   |
| `string` | `ascii_lowercase ascii_uppercase digits` (3)                            | done   |


### Planned but Not Shipped (8 modules)

These appear in the plan but have no runtime implementation yet:


| Module        | Planned Functions                 | Status                          |
| ------------- | --------------------------------- | ------------------------------- |
| `random`      | `random randint seed choice`      | no Not implemented              |
| `json`        | `dumps loads`                     | no Not implemented              |
| `re`          | `match search sub findall`        | no Not implemented              |
| `collections` | `Counter defaultdict OrderedDict` | no Not implemented              |
| `hashlib`     | `md5 sha256`                      | no Not implemented              |
| `datetime`    | `now timestamp`                   | no Not implemented              |
| `functools`   | `reduce`                          | no Not implemented              |
| `itertools`   | `range enumerate zip`             | no Already built-in, not module |


### Complete Python 3.12 Stdlib (311 modules)

For reference, here is the full Python 3.12 standard library categorized by implementation feasibility for an AOT compiler:

#### Feasible to Implement (C-shim approach) - ~80 modules


| Category            | Modules                                                                                                           | Estimated Effort          |
| ------------------- | ----------------------------------------------------------------------------------------------------------------- | ------------------------- |
| **Math/Science**    | `math` done, `cmath`, `decimal`, `fractions`, `random`, `statistics`                                              | Low-Medium                |
| **String/Text**     | `string` done, `re`, `difflib`, `textwrap`, `unicodedata`, `stringprep`                                           | Medium                    |
| **Data Structures** | `collections`, `collections.abc`, `heapq`, `bisect`, `array`, `copy`, `pprint`, `reprlib`, `enum`                 | Medium                    |
| **File/IO**         | `os` partial, `os.path`, `pathlib`, `glob`, `fnmatch`, `shutil`, `tempfile`, `io`, `fileinput`, `stat`, `filecmp` | Medium-High               |
| **Date/Time**       | `datetime`, `time` done, `calendar`, `zoneinfo`                                                                   | Medium                    |
| **Encoding**        | `base64`, `binascii`, `codecs`, `hashlib`, `hmac`, `secrets`                                                      | Medium                    |
| **Serialization**   | `json`, `csv`, `tomllib`, `configparser`, `struct`                                                                | Medium                    |
| **Functional**      | `functools`, `itertools`, `operator`                                                                              | Medium                    |
| **Compression**     | `gzip`, `bz2`, `lzma`, `zipfile`, `tarfile`, `zlib`                                                               | Medium (link to C libs)   |
| **Networking**      | `socket`, `http.client`, `urllib`, `email`                                                                        | High                      |
| **Process**         | `subprocess`, `signal`, `sys` partial, `argparse`, `logging`                                                      | Medium-High               |
| **Database**        | `sqlite3`, `dbm`                                                                                                  | High (link to libsqlite3) |
| **Testing**         | `unittest`, `doctest`                                                                                             | High                      |


#### Difficult / Requires Runtime Support - ~40 modules


| Category          | Modules                                                        | Blockers                                                              |
| ----------------- | -------------------------------------------------------------- | --------------------------------------------------------------------- |
| **Async**         | `asyncio`, `aiohttp`                                           | Needs event loop, coroutine runtime                                   |
| **Threading**     | `threading`, `multiprocessing`, `concurrent.futures`, `queue`  | Needs thread-safe runtime                                             |
| **Introspection** | `inspect`, `dis`, `gc`, `traceback`, `warnings`, `tracemalloc` | Needs runtime metadata                                                |
| **Import system** | `importlib`, `pkgutil`, `zipimport`                            | Dynamic import incompatible with AOT                                  |
| **Type system**   | `typing`, `types`, `abc`                                       | Partially feasible (compile-time only)                                |
| **Dynamic**       | `ctypes`, `dataclasses`                                        | ctypes is inherently dynamic; dataclasses need metaclass-like codegen |


#### Never Implementable (Require CPython Internals) - ~20 modules


| Modules                        | Reason                      |
| ------------------------------ | --------------------------- |
| `eval`, `exec`, `compile`      | Dynamic code execution      |
| `globals`, `locals`, `vars`    | Runtime symbol tables       |
| `__import__`                   | Dynamic import              |
| `code`, `codeop`, `compileall` | CPython compiler internals  |
| `pdb`, `bdb`                   | Interactive debugger        |
| `tkinter`                      | GUI toolkit binding         |
| `IDLE`, `turtle`               | Applications, not libraries |


**Stdlib: 5/311 modules working (~1.6%). ~80 modules feasible, ~40 difficult, ~20 impossible.**

---

## 12. Memory Management - CRITICAL

### Current State: 100% Leak Rate

The Dragon runtime has **53 `malloc`/`calloc` call sites** in `CEmitterRuntime.cpp` and **0 `free` calls** anywhere. Every allocation leaks.


| Operation                                               | Allocations | Frees |
| ------------------------------------------------------- | ----------- | ----- |
| String operations (concat, upper, lower, replace, etc.) | ~35         | 0     |
| List operations (new, copy, slice, sorted, etc.)        | ~10         | 0     |
| Dict operations (new, copy, keys, values, etc.)         | ~5          | 0     |
| Utility (bin, hex, oct, chr, f-string helpers)          | ~3          | 0     |


### What This Means

```python
# This program leaks ~80 bytes per iteration
for i in range(1000000):
 s: str = "hello" + " world" # malloc(12), never freed
```

Every `dragon_str_concat`, `dragon_str_upper`, `dragon_list_new`, `dragon_list_copy`, `dragon_list_sorted`, `dragon_dict_new`, `dragon_str_split`, and every other function that returns a newly allocated value - leaks.

`dragon_list_clear` and `dragon_dict_clear` reset the size counter to 0 but do NOT free the backing array or contained elements.

### Impact

- Short-lived programs (scripts, one-shot tools): Works fine - OS reclaims on exit
- Long-running programs (servers, daemons): Will OOM
- Any program with loops that create strings/lists: Leaks proportional to iteration count

### Planned Fix

See [decisions/003-memory-management.md](decisions/003-memory-management.md) for the full plan.

TL;DR: Arena allocator for short-lived programs, reference counting for long-lived programs, with a compile-time flag to select strategy.

---

## 13. Bilingual Compiler Architecture (v0.2.0)

### File Types


| Extension | Mode   | Description                                          |
| --------- | ------ | ---------------------------------------------------- |
| `.dr`     | Dragon | Brace-delimited blocks, mandatory type annotations   |
| `.py`     | Python | Indentation-based, PEP-484 type annotations enforced |


### Compilation Pipeline

```
Source (.dr/.py)
 → Lexer (brace-blocks or indentation)
 → Parser (shared grammar, mode-specific block handling)
 → TypeHintEnforcer (.py files only - verify PEP-484 annotations present)
 → Sema (name resolution, scope analysis)
 → TypeChecker (type correctness, cross-file type flow)
 → ModuleResolver (import graph, topological sort, cycle detection)
 → CEmitter (AST → C source, multi-module coordination)
 → cc (system C compiler → native binary)
```

### Two Backends


|                      | CEmitter (Primary)                           | LLVM CodeGen (Secondary)          |
| -------------------- | -------------------------------------------- | --------------------------------- |
| **LOC**              | 2,155 + 673 runtime + 140 stdlib = **2,968** | **1,608**                         |
| **Feature coverage** | ~85% of Dragon language                      | ~35-40%                           |
| **Classes**          | done structs + methods + constructor         | no                                |
| **Exceptions**       | done setjmp/longjmp                          | no                                |
| **List/Dict**        | done full runtime                            | partial list only, basic          |
| **Comprehensions**   | done list + dict                             | no                                |
| **Stdlib**           | done 5 module shims                          | no                                |
| **Multi-file**       | done coordinated C emission                  | no                                |
| **Optimization**     | Via `cc -O2` (system compiler does it)       | No optimization passes configured |
| **Debug info**       | Via `cc -g`                                  | Not implemented                   |


See [decisions/003-backend-strategy.md](decisions/003-backend-strategy.md) for the C vs LLVM analysis.

### Cross-Language Interop

- `.dr` files can import `.dr` and typed `.py` files
- `.py` files can import `.dr` and typed `.py` files
- Untyped `.py` imports produce "Borders must be secured" error
- Diamond imports handled (topological ordering, deduplication)
- Circular imports detected with clear error messages
- Cross-file type checking: TypeChecker resolves imported symbol types
- `-I <dir>` flag for additional module search paths

---

## 14. Control Flow


| Feature                   | Status  | Backend  | Notes                              |
| ------------------------- | ------- | -------- | ---------------------------------- |
| `if/elif/else`            | done    | Both     |                                    |
| `while`                   | done    | Both     |                                    |
| `for x in range(n)`       | done    | Both     | All 3 arg forms                    |
| `for x in list`           | done    | CEmitter |                                    |
| `for x in dict`           | partial | CEmitter | Iterates keys only                 |
| `for x in string`         | partial | CEmitter | Character iteration                |
| `for k, v in items`       | no      | -        | Requires tuple unpacking           |
| `while...else`            | parsed  | Parsed   |                                    |
| `for...else`              | parsed  | Parsed   |                                    |
| `break` / `continue`      | done    | Both     |                                    |
| `try/except/finally/else` | done    | CEmitter | setjmp/longjmp                     |
| `raise`                   | done    | CEmitter |                                    |
| `with`                    | parsed  | Parsed   | No `__enter__`/`__exit__` protocol |
| `match/case`              | no      | -        | Not parsed                         |
| List comprehension        | done    | CEmitter |                                    |
| Dict comprehension        | done    | CEmitter |                                    |
| Set comprehension         | no      | -        |                                    |
| Generator expression      | no      | -        | No AST node                        |
| `yield` / generators      | parsed  | Parsed   | No coroutine/state machine         |
| `async/await`             | parsed  | Parsed   | No event loop                      |


---

## 15. What Dragon CAN Do Today

Despite the gaps, Dragon compiles real programs. Here's what works reliably:

```python
# Functions with types, arithmetic, string ops
def fibonacci(n: int) -> int {
 if n <= 1 { return n }
 return fibonacci(n - 1) + fibonacci(n - 2)
}

# Classes with constructors and methods
class Point {
 def __init__(self, x: int, y: int) -> None {
 self.x = x
 self.y = y
 }
 def distance(self) -> float {
 return (self.x ** 2 + self.y ** 2) ** 0.5
 }
}

# Lists, dicts, comprehensions
names: list[str] = ["Alice", "Bob", "Charlie"]
lengths: list[int] = [len(n) for n in names]
scores: dict[str, int] = {"Alice": 95, "Bob": 87}

# Exception handling
try {
 result: int = 10 // 0
} except ZeroDivisionError {
 print("Cannot divide by zero")
} finally {
 print("Done")
}

# Multi-file imports (typed .py or .dr)
from math_utils import Vector # Cross-file type checking
from utils import helper # Topological compilation
```

---

## 16. Semantic Differences from CPython

Even where syntax is identical, Dragon's runtime behavior may differ:


| Behavior        | CPython                       | Dragon                              | Impact                                            |
| --------------- | ----------------------------- | ----------------------------------- | ------------------------------------------------- |
| Integer size    | Arbitrary precision           | 64-bit fixed                        | Overflow wraps silently                           |
| String encoding | UTF-8 Unicode                 | ASCII `const char*`                 | No emoji, no CJK                                  |
| Method dispatch | Dynamic (vtable/dict lookup)  | Static (compile-time)               | No monkey-patching                                |
| `is` operator   | Identity (object address)     | `==` (value comparison)             | `a is b` same as `a == b`                         |
| Truthiness      | `__bool__` → `__len__` → True | Non-zero / non-null                 | `bool([])` is True in Dragon                      |
| Closures        | Full closure capture          | No closure capture                  | `nonlocal` is no-op                               |
| Scoping         | LEGB rule                     | C scoping rules                     | Subtle differences in nested functions            |
| GIL             | Yes                           | No                                  | Dragon is truly concurrent (when threading lands) |
| Memory          | Reference counting + GC       | Manual (currently: leak everything) | Dragon programs leak                              |
| Exceptions      | Class hierarchy               | Integer enum + setjmp               | No exception inheritance                          |
| `for...else`    | Runs if no break              | Parsed, not emitted                 | Silent behavior difference                        |


---

## 17. Test Coverage


| Suite            | Tests    | Status          |
| ---------------- | -------- | --------------- |
| LexerTests       | ~60      | done            |
| ParserTests      | ~124     | done            |
| SemaTests        | ~39      | done            |
| TypeCheckerTests | ~103     | done            |
| ASTTests         | ~36      | done            |
| CEmitterTests    | ~164     | done            |
| EnforcerTests    | 21       | done            |
| ResolverTests    | 12       | done            |
| DiagnosticTests  | 19       | done            |
| InteropTests     | 8        | done            |
| CodeGenTests     | ~63      | done            |
| **Total**        | **~647** | **All passing** |


---

## 18. Known Issues (v0.2.0)


| Issue                          | Severity     | Description                                                            |
| ------------------------------ | ------------ | ---------------------------------------------------------------------- |
| Memory leaks (all allocations) | **Critical** | 53 malloc sites, 0 free calls. Every string/list/dict operation leaks. |
| `int` overflow                 | High         | 64-bit fixed width, not arbitrary precision. Silent wraparound.        |
| No Unicode                     | High         | Strings are ASCII `const char`*. No UTF-8 support.                     |
| Set is broken                  | Medium       | Stored as list - no dedup, no O(1) lookup.                             |
| `dict.items` broken            | Medium       | Returns keys only, should return key-value pairs.                      |
| Chained comparisons wrong      | Medium       | `a < b < c` evaluates as `(a < b) < c`.                                |
| `is` is `==`                   | Low          | Identity comparison is value comparison in Dragon.                     |
| No closures                    | Low          | `nonlocal` is no-op. Nested function can't capture outer variables.    |
| `dragon_list_clear` leaks      | Low          | Resets size but doesn't free backing storage.                          |


---

## 19. Roadmap


| Priority | Milestone                                                                       | Reference                                                                |
| -------- | ------------------------------------------------------------------------------- | ------------------------------------------------------------------------ |
| **1**    | 100% Python 3.12 syntax parity                                                  | [decisions/002-syntax-parity.md](decisions/002-syntax-parity.md)         |
| **2**    | Memory management (arena + refcount)                                            | [decisions/003-memory-management.md](decisions/003-memory-management.md) |
| **3**    | Object model basics (`__str__`, `__len__`, `__getitem__`, `__iter__`, `__eq__`) | TBD                                                                      |
| **4**    | Proper set implementation (hash table)                                          | TBD                                                                      |
| **5**    | Stdlib expansion (Tier 1: json, re, pathlib, io, collections)                   | TBD                                                                      |
| **6**    | Single inheritance emission                                                     | TBD                                                                      |
| **7**    | LLVM backend feature parity with CEmitter                                       | [decisions/003-backend-strategy.md](decisions/003-backend-strategy.md)   |
| **8**    | Generator/coroutine runtime                                                     | TBD                                                                      |
| **9**    | Unicode string support                                                          | TBD                                                                      |
| **10**   | Arbitrary-precision integers                                                    | TBD                                                                      |

