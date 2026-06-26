# 008 -- Dragon Runtime Library

> **Version:** 0.2.0
> **Last Updated:** 2026-06-22
> **Source:** `lib/Runtime/` (split across ~22 translation units)

---

## 1. Overview

The Dragon runtime is a **separately compiled C++ static library** (`libdragon_runtime.a`). All functions use `extern "C"` linkage for LLVM calling convention compatibility. Functions are NOT `static` -- they are externally visible symbols that the LLVM-generated code calls directly.

The runtime is **not a single file**. It is split into roughly two dozen translation units under `lib/Runtime/`, listed in the top-level `CMakeLists.txt` (`add_library(dragon_runtime STATIC ...)`). The principal sources are:

| File | Responsibility |
|------|----------------|
| `runtime_core.cpp` | Reference counting, cycle collector, object lifecycle, GC class tables |
| `runtime_string.cpp` / `runtime_string_methods.cpp` | `DragonString` (PEP 393-lite), the `str` API |
| `runtime_list.cpp` | `DragonList` and the monomorphized list family |
| `runtime_dict.cpp` | `DragonDict` (compact, open-addressed) |
| `runtime_collections.cpp` | tuple, set, bytes, deque |
| `runtime_box.cpp` | `Any`/`Union` `{tag, payload}` box ops |
| `runtime_exception.cpp` | setjmp/longjmp exception machinery, unwind cleanup |
| `runtime_concurrency.cpp` | green-thread scheduler, vthreads, raw epoll/kqueue I/O |
| `runtime_builtins.cpp` | `min`/`max`/`sum`/`enumerate`/`zip`, closures, reflection |
| `runtime_platform.cpp` | OS/file syscalls, hash-secret seeding |
| `runtime_fileio.cpp` | file open/read/write |
| `runtime_crypto.cpp` / `runtime_argon2id.cpp` / `runtime_ed25519.cpp` / `runtime_tls.cpp` | crypto, password hashing, signatures, TLS |
| `runtime_zlib.cpp` / `runtime_zstd.cpp` | compression |
| `runtime_subprocess.cpp` / `runtime_getpass.cpp` / `runtime_sqltemplate.cpp` | subprocess, getpass, SQL template helpers |

A shared internal header, `lib/Runtime/runtime_internal.h`, declares the struct layouts, the object header, enums, and the cross-TU `extern` functions. This document references the specific source file for each subsystem rather than a single monolithic `runtime.cpp` (which no longer exists).

At link time, `libdragon_runtime.a` is linked into every Dragon executable. The linker's dead code elimination removes any unused functions, so the final binary only includes the runtime functions actually called by the program.

The runtime provides:
- I/O functions for printing each Dragon type, plus `input()` support
- Type conversion helpers (int-to-string, float-to-string, string-to-int, string-to-float)
- String operations covering the Python `str` API (38+ methods)
- Math helpers with Python semantics (floor division, modulo, power, abs)
- Complete list, dict, tuple, and set data structure implementations, monomorphized by element type
- **Reference counting plus a cycle collector** for all heap objects (see Section 2A)
- Numeric builtins (chr, ord, hex, oct, bin, round, repr)
- Aggregate functions (min, max, sum, any, all, sorted, reversed, enumerate, zip)
- Hash and identity helpers
- Slicing support for lists and strings
- Exception handling via setjmp/longjmp (per-thread TLS plus per-vthread stacks)
- File I/O (open, close, read, write, readline, readlines)

---

## 2. Includes and Structure

### Standard Headers

The runtime uses C++ standard headers plus a few platform headers (pthreads, atomics, sockets):
```cpp
#include <cmath>
#include <csetjmp>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <pthread.h>
#include <stdatomic.h>
```

### Extern "C" Linkage

The entire runtime body is wrapped in an `extern "C"` block:
```cpp
extern "C" {
    // ... all runtime functions ...
} // extern "C"
```

This ensures that all function symbols have C linkage (no name mangling), which is required for LLVM-generated code to call them by name.

### The Object Header

Every heap object the runtime allocates begins with a 16-byte `DragonObjectHeader` at offset 0 (`runtime_internal.h`). This is the single fact the reference counter, the cycle collector, and the type-dispatch in `dragon_dealloc` all rely on: given any heap pointer, the header is always at the front.

```cpp
typedef struct {
    int64_t  refcount;       // 8 bytes (offset 0)
    uint8_t  type_tag;       // 1 byte  (offset 8) - DragonTypeTag
    uint8_t  gc_flags;       // 1 byte  (offset 9) - GC_FLAG_* bits
    uint16_t class_id;       // 2 bytes (offset 10)
    int32_t  gc_track_idx;   // 4 bytes (offset 12)
} DragonObjectHeader;        // Total: 16 bytes
```

`type_tag` is a `DragonTypeTag` (`DRAGON_TAG_STR=1, _LIST=2, _DICT=3, _TUPLE=4, _SET=5, _BYTES=6, _CLASS=7, _GENERATOR=8, ...`) and drives the destructor switch in `dragon_dealloc`. `gc_flags` carries the GC bits: `GC_FLAG_HEAP_OBJ`, `GC_FLAG_TRACKED`, `GC_FLAG_REACHABLE`, `GC_FLAG_SHARED` (escaped to another OS thread -> atomic refcount ops), and `GC_FLAG_IN_TO_FREE` (claimed by the cycle collector for dealloc).

### Struct Types

The runtime defines its principal data structures as C++ structs in `runtime_internal.h`. Each begins with a `DragonObjectHeader`; values are no longer funnelled through a single `int64_t` representation - containers are **monomorphized** by element type (spec-30), so the storage width and the per-element ops are picked from the static `list[T]` / `dict[K,V]` at allocation.

```cpp
// runtime_internal.h, runtime_list.cpp
struct DragonList {              // I64 variant: list[int], list[bool]
    DragonObjectHeader header;
    void*    data;        // bytes; stride = elem_size
    int64_t  size;        // # of elements
    int64_t  capacity;    // capacity in elements
    uint8_t  elem_tag;    // TAG_INT, TAG_STR, TAG_BOOL, ...
    uint8_t  elem_size;   // 1 (bool, packed - spec-28) or 8 (everything else)
};
```

`DragonList` is the I64 variant (int and bool elements, with `list[bool]` packed at 1 byte/element). Two sibling structs share its exact field layout but type `data` natively, so a polymorphic op can cast a `DragonList*` and still read the header / size / capacity at the same offsets:

```cpp
struct DragonListF64 { DragonObjectHeader header; double* data; ... };   // list[float]
struct DragonListPtr { DragonObjectHeader header; void**  data; ... };   // list[str], list[<container>], list[<class>], list[bytes]
```

A fourth variant, `DragonListBox` (`runtime_list.cpp`), stores `list[Any]` / untyped lists as contiguous 16-byte `{int64_t tag, int64_t payload}` elements (Dragon's `[]interface{}`), each carrying its own tag.

```cpp
// runtime_internal.h, runtime_dict.cpp - compact, open-addressed, insertion-ordered (CPython-style)
struct DictEntry {
    uint64_t    hash;
    const char* key;     // for int-keyed dicts the i64 key is stored in this slot
    int64_t     value;
    int8_t      tag;     // DragonValueTag of the value
    int8_t      dead;    // lazy-delete tombstone in the dense array
};

struct DragonDict {
    DragonObjectHeader header;
    DictEntry* entries;       // dense, insertion-ordered
    int64_t*   indices;       // sparse hash -> entry-index table (-1 empty, -2 tombstone)
    int64_t    size;          // dense high-water mark (includes dead slots)
    int64_t    used;          // live entry count (what len() returns)
    int64_t    capacity;      // capacity of the dense entries array
    int64_t    index_size;    // size of the sparse index table (power of 2)
    uint8_t    keys_are_ptr;  // 0 = int-keyed, 1 = str-keyed (dict owns one ref per str key)
};
```

The dict is a compact, open-addressed, insertion-ordered design: a **dense** `entries` array in insertion order plus a **sparse** `indices` table that maps `hash -> entry index` for O(1) lookup. Deletes mark the dense slot `dead` (a tombstone) rather than shifting the array; `dict_compact` reclaims dead slots lazily.

```cpp
struct DragonTuple {
    DragonObjectHeader header;
    int64_t* data;
    int64_t  length;       // fixed at creation
    uint8_t* elem_tags;    // per-element DragonValueTag
};

struct DragonSet {
    DragonObjectHeader header;
    int64_t* buckets;      // open-addressing hash table
    uint8_t* states;       // 0=empty, 1=occupied, 2=deleted (tombstone)
    int64_t  capacity;
    int64_t  count;
    uint8_t  elem_tag;
};
```

The set uses open-addressing with linear probing for collision resolution. Per-entry tags (`DragonValueTag`: `TAG_INT=0, TAG_STR=1, TAG_FLOAT=2, TAG_BOOL=3, TAG_NONE=4, TAG_LIST=5, TAG_DICT=6, TAG_BYTES=7`) let mixed-type containers and `Any` boxes track the concrete type of each stored value, which drives refcount, equality, and repr.

### Memory Management

The runtime allocates with `malloc`/`free`, but heap objects are **not** caller-freed and they do not simply leak. Every heap object is reference counted, and a cycle collector reclaims reference cycles. The next section describes that system.

---

## 2A. Reference Counting and the Cycle Collector

Dragon heap objects are managed by reference counting backed by a trial-deletion cycle collector (the design spec). This is implemented in `runtime_core.cpp`.

### Reference counting

The `refcount` field of the object header is the live reference count. Codegen emits `dragon_incref` / `dragon_decref` (and the string variants `dragon_incref_str` / `dragon_decref_str`) around ownership transfers; the last `dragon_decref` to drive the count to 0 calls `dragon_dealloc`, which dispatches on `type_tag` to the per-type destructor (`dragon_list_destroy`, `dragon_dict_destroy`, etc.) and frees the storage.

```cpp
void dragon_incref(void* obj);   // ++refcount (atomic if GC_FLAG_SHARED)
void dragon_decref(void* obj);   // --refcount; last ref -> dragon_dealloc
```

Two ownership refinements matter:

- **Immortal objects.** A refcount at or above `DRAGON_IMMORTAL_REFCOUNT` (`0x4000000000000000`) is a sentinel: incref/decref become no-ops. Interned string literals and other never-freed objects use this, so a borrowed C-string literal is never accidentally freed.
- **Shared (cross-thread) objects.** When an object escapes to another OS thread (via `fire` / `thread`, or by becoming reachable from a shared parent), it is marked `GC_FLAG_SHARED`. Incref/decref then route to the atomic variants (`dragon_incref_atomic` / `dragon_decref_atomic`, `__atomic_*` with acquire/release ordering). The flag lives in the same cache line as `refcount`, so the single-threaded fast path (plain non-atomic `++`/`--`) stays branch-predictable and lock-free.

### The cycle collector

Reference counting alone cannot reclaim cycles (a list that contains itself, two objects that reference each other). `dragon_gc_collect` (`runtime_core.cpp`) is a synchronous trial-deletion collector that reclaims them:

1. Only cycle-capable objects are enrolled (`dragon_gc_track` sets `GC_FLAG_TRACKED`); leaf-only containers (e.g. `dict[str, int]`) are never tracked, so they can never bloat the collector.
2. Trial deletion: for each tracked object, traverse its children and subtract each internal reference from a private refcount snapshot.
3. Objects whose snapshot refcount stays above 0 are externally reachable and are marked `GC_FLAG_REACHABLE`; reachability is then propagated transitively.
4. Every still-unreachable object is marked `GC_FLAG_IN_TO_FREE` and queued, then cleared (`dragon_clear_refs` breaks the cycle by dropping contained references) and finally deallocated in a controlled loop.

The `GC_FLAG_IN_TO_FREE` bit is the ownership handshake: once the collector queues an object for dealloc, a concurrent or recursive `dragon_decref` that drives that object's refcount to 0 observes the bit and skips its own dealloc, so the collector's loop is the only thing that frees it (no double-free, no use-after-free). Collection is triggered on an allocation counter crossing `gc_threshold`, and `dragon_gc_collect` is reentrancy-guarded so a user `__dealloc__` that allocates during teardown cannot re-enter it. A single-threaded process keeps a lock-free fast path; the global `gc_lock` is only engaged once a second heap-mutating OS thread is spawned (`dragon_gc_go_concurrent`).

---

## 3. I/O Functions

These functions provide `print()` and `input()` support matching Python behavior.

| Function | Signature | Description |
|----------|-----------|-------------|
| `dragon_print_int` | `void dragon_print_int(int64_t value)` | Prints integer with `"%ld\n"` format |
| `dragon_print_float` | `void dragon_print_float(double value)` | Prints float with `"%g\n"` format |
| `dragon_print_str` | `void dragon_print_str(const char* s)` | Prints string with `"%s\n"` format; prints `"None\n"` for NULL |
| `dragon_print_bool` | `void dragon_print_bool(int64_t value)` | Prints `"True\n"` or `"False\n"` |
| `dragon_print_none` | `void dragon_print_none()` | Prints `"None\n"` |
| `dragon_print_newline` | `void dragon_print_newline()` | Prints `"\n"` (empty `print()` call) |
| `dragon_input` | `const char* dragon_input(const char* prompt)` | Prints prompt, reads line from stdin, strips trailing newline |

The `%g` format for floats matches Python's default float formatting: it suppresses trailing zeros and switches to scientific notation for very large or very small values.

`dragon_input` uses a static 4096-byte buffer for reading. It returns a `malloc`-allocated copy of the input string.

---

## 4. String Operations

All string functions operate on `const char*` (null-terminated C strings). Functions that return new strings allocate them via `malloc`. All functions handle NULL input gracefully (returning `""` for string returns, 0 or -1 for numeric returns).

### Concatenation and Conversion

#### `dragon_str_concat`
```cpp
const char* dragon_str_concat(const char* a, const char* b)
```
Returns a new heap-allocated string that is the concatenation of `a` and `b`. Handles NULL inputs by treating them as `""`.

#### `dragon_str_len`
```cpp
int64_t dragon_str_len(const char* s)
```
Returns `strlen(s)`, or 0 if `s` is NULL.

#### `dragon_int_to_str`
```cpp
const char* dragon_int_to_str(int64_t value)
```
Converts an integer to a string. Allocates 32 bytes and uses `snprintf` with `"%ld"` format.

#### `dragon_float_to_str`
```cpp
const char* dragon_float_to_str(double value)
```
Converts a float to a string. Allocates 64 bytes and uses `snprintf` with `"%g"` format.

#### `dragon_str_eq`
```cpp
int64_t dragon_str_eq(const char* a, const char* b)
```
Returns 1 if strings are equal (using `strcmp`), 0 otherwise. Handles NULL: two NULLs are equal, NULL and non-NULL are not.

### Type Conversions

#### `dragon_str_to_int`
```cpp
int64_t dragon_str_to_int(const char* s)
```
Converts string to integer using `atol`. Returns 0 for NULL.

#### `dragon_str_to_float`
```cpp
double dragon_str_to_float(const char* s)
```
Converts string to float using `atof`. Returns 0.0 for NULL.

### String Indexing

#### `dragon_str_index`
```cpp
const char* dragon_str_index(const char* s, int64_t index)
```
Returns a new single-character string at the given index. Supports **negative indexing**: if `index < 0`, adds string length. Exits with `"IndexError: string index out of range"` if out of bounds.

### Bool to String

#### `dragon_bool_to_str`
```cpp
const char* dragon_bool_to_str(int64_t value)
```
Returns the string literal `"True"` or `"False"`. Does not allocate (returns constant string pointers). Used for f-string interpolation.

### Case Transformation

#### `dragon_str_upper`
```cpp
const char* dragon_str_upper(const char* s)
```
Returns a new string with all ASCII lowercase letters converted to uppercase. Non-letter characters pass through unchanged.

#### `dragon_str_lower`
```cpp
const char* dragon_str_lower(const char* s)
```
Returns a new string with all ASCII uppercase letters converted to lowercase.

#### `dragon_str_title`
```cpp
const char* dragon_str_title(const char* s)
```
Returns a new string in title case: the first letter after a whitespace character (space, tab, newline) is uppercased; all other letters are lowercased.

#### `dragon_str_capitalize`
```cpp
const char* dragon_str_capitalize(const char* s)
```
Returns a new string with the first character uppercased and all others lowercased.

#### `dragon_str_swapcase`
```cpp
const char* dragon_str_swapcase(const char* s)
```
Returns a new string with uppercase letters converted to lowercase and vice versa.

#### `dragon_str_casefold`
```cpp
const char* dragon_str_casefold(const char* s)
```
Alias for `dragon_str_lower()`. Full Unicode casefold is approximated with ASCII lowercasing.

### Whitespace Stripping

#### `dragon_str_strip`
```cpp
const char* dragon_str_strip(const char* s)
```
Returns a new string with leading and trailing whitespace removed (space, tab, `\n`, `\r`).

#### `dragon_str_lstrip`
```cpp
const char* dragon_str_lstrip(const char* s)
```
Returns a new string with leading whitespace removed.

#### `dragon_str_rstrip`
```cpp
const char* dragon_str_rstrip(const char* s)
```
Returns a new string with trailing whitespace removed.

### String Modification

#### `dragon_str_replace`
```cpp
const char* dragon_str_replace(const char* s, const char* old_s, const char* new_s)
```
Returns a new string with all occurrences of `old_s` replaced by `new_s`. Two-pass algorithm: first counts occurrences to compute result size, then performs the replacement. Returns `s` unchanged if `old_s` is empty.

#### `dragon_str_repeat`
```cpp
const char* dragon_str_repeat(const char* s, int64_t n)
```
Returns a new string consisting of `s` repeated `n` times. Returns `""` if `s` is NULL or `n <= 0`.

#### `dragon_str_removeprefix`
```cpp
const char* dragon_str_removeprefix(const char* s, const char* prefix)
```
If `s` starts with `prefix`, returns a copy with the prefix removed. Otherwise returns a copy of `s`.

#### `dragon_str_removesuffix`
```cpp
const char* dragon_str_removesuffix(const char* s, const char* suffix)
```
If `s` ends with `suffix`, returns a copy with the suffix removed. Otherwise returns a copy of `s`.

#### `dragon_str_expandtabs`
```cpp
const char* dragon_str_expandtabs(const char* s, int64_t tabsize)
```
Replaces tab characters with spaces. Computes the number of spaces based on the current column position and the `tabsize`. Newlines reset the column counter.

### Padding Methods

#### `dragon_str_center`
```cpp
const char* dragon_str_center(const char* s, int64_t w, char fill)
```
Centers the string in a field of width `w`, padding with `fill` character. If `len(s) >= w`, returns a copy of `s`. Left padding is `pad / 2`, right padding is `pad - left`.

#### `dragon_str_ljust`
```cpp
const char* dragon_str_ljust(const char* s, int64_t w, char fill)
```
Left-justifies the string in a field of width `w`, padding the right with `fill`.

#### `dragon_str_rjust`
```cpp
const char* dragon_str_rjust(const char* s, int64_t w, char fill)
```
Right-justifies the string in a field of width `w`, padding the left with `fill`.

#### `dragon_str_zfill`
```cpp
const char* dragon_str_zfill(const char* s, int64_t w)
```
Pads the string on the left with zeros to fill width `w`. If the string starts with `+` or `-`, the sign character stays at the front and zeros are inserted after it.

### Search Operations

#### `dragon_str_find`
```cpp
int64_t dragon_str_find(const char* s, const char* sub)
```
Returns the index of the first occurrence of `sub` in `s`, or -1 if not found. Uses `strstr` internally.

#### `dragon_str_rfind`
```cpp
int64_t dragon_str_rfind(const char* s, const char* sub)
```
Returns the index of the last occurrence of `sub` in `s`, or -1 if not found. Scans forward and tracks the last match.

#### `dragon_str_index_of`
```cpp
int64_t dragon_str_index_of(const char* s, const char* sub)
```
Like `find`, but exits with `"ValueError: substring not found"` if not found. Named `_index_of` to avoid collision with `dragon_str_index` (char-at-position).

#### `dragon_str_rindex`
```cpp
int64_t dragon_str_rindex(const char* s, const char* sub)
```
Like `rfind`, but exits with `"ValueError: substring not found"` if not found.

#### `dragon_str_count`
```cpp
int64_t dragon_str_count(const char* s, const char* sub)
```
Counts non-overlapping occurrences of `sub` in `s`. Returns 0 if either is NULL or `sub` is empty.

#### `dragon_str_startswith`
```cpp
int64_t dragon_str_startswith(const char* s, const char* prefix)
```
Returns 1 if `s` starts with `prefix`, 0 otherwise. Uses `strncmp`.

#### `dragon_str_endswith`
```cpp
int64_t dragon_str_endswith(const char* s, const char* suffix)
```
Returns 1 if `s` ends with `suffix`, 0 otherwise. Compares the tail of `s` with `strcmp`.

#### `dragon_str_contains`
```cpp
int64_t dragon_str_contains(const char* s, const char* sub)
```
Returns 1 if `sub` is found anywhere in `s`, 0 otherwise. Uses `strstr`. Used by the `in` operator.

### Boolean Test Methods (is-methods)

All return `int64_t` (0 or 1). All return 0 for NULL or empty strings (except `isascii` and `isprintable` which return 1 for NULL/empty).

#### `dragon_str_isdigit`
```cpp
int64_t dragon_str_isdigit(const char* s)
```
Returns 1 if all characters are ASCII digits (`'0'`-`'9'`), 0 otherwise.

#### `dragon_str_isalpha`
```cpp
int64_t dragon_str_isalpha(const char* s)
```
Returns 1 if all characters are ASCII letters (`a-z`, `A-Z`), 0 otherwise.

#### `dragon_str_isalnum`
```cpp
int64_t dragon_str_isalnum(const char* s)
```
Returns 1 if all characters are ASCII letters or digits, 0 otherwise.

#### `dragon_str_isspace`
```cpp
int64_t dragon_str_isspace(const char* s)
```
Returns 1 if all characters are whitespace (space, `\t`, `\n`, `\r`, `\f`, `\v`), 0 otherwise.

#### `dragon_str_isupper`
```cpp
int64_t dragon_str_isupper(const char* s)
```
Returns 1 if all cased characters are uppercase and there is at least one cased character. Non-cased characters (digits, punctuation) are ignored.

#### `dragon_str_islower`
```cpp
int64_t dragon_str_islower(const char* s)
```
Returns 1 if all cased characters are lowercase and there is at least one cased character.

#### `dragon_str_istitle`
```cpp
int64_t dragon_str_istitle(const char* s)
```
Returns 1 if the string is in title case: uppercase letters appear only after non-cased characters, lowercase letters appear only after cased characters. Requires at least one cased character.

#### `dragon_str_isascii`
```cpp
int64_t dragon_str_isascii(const char* s)
```
Returns 1 if all characters have ASCII code <= 127. Returns 1 for NULL input. Returns 1 for empty strings.

#### `dragon_str_isdecimal`
```cpp
int64_t dragon_str_isdecimal(const char* s)
```
Alias for `dragon_str_isdigit()`. In full Unicode, isdecimal is a subset of isdigit, but for ASCII they are identical.

#### `dragon_str_isnumeric`
```cpp
int64_t dragon_str_isnumeric(const char* s)
```
Alias for `dragon_str_isdigit()`. Same ASCII simplification.

#### `dragon_str_isprintable`
```cpp
int64_t dragon_str_isprintable(const char* s)
```
Returns 1 if all characters are printable (ASCII code >= 32 and != 127). Returns 1 for NULL or empty input.

#### `dragon_str_isidentifier`
```cpp
int64_t dragon_str_isidentifier(const char* s)
```
Returns 1 if the string is a valid Python/C identifier: starts with a letter or underscore, followed by letters, digits, or underscores. Returns 0 for empty or NULL.

### Splitting and Joining

#### `dragon_str_split`
```cpp
DragonList* dragon_str_split(const char* s, const char* sep)
```
Splits the string into a list of substrings.
- If `sep` is NULL or empty: splits on whitespace (spaces, tabs, newlines, carriage returns), consuming consecutive whitespace as a single delimiter and stripping leading/trailing whitespace
- Otherwise: splits on the exact separator string, including empty strings between consecutive separators

Returns a `DragonList*` where each element is an `int64_t`-cast `const char*` pointer.

#### `dragon_str_rsplit`
```cpp
DragonList* dragon_str_rsplit(const char* s, const char* sep, int64_t maxsplit)
```
Right-to-left split with a maximum number of splits.
- If `maxsplit < 0`: delegates to `dragon_str_split(s, sep)`
- If `maxsplit >= 0`: performs a full split, then if the result has more than `maxsplit + 1` elements, rejoins the leftmost elements into the first item

#### `dragon_str_splitlines`
```cpp
DragonList* dragon_str_splitlines(const char* s)
```
Splits the string at line boundaries (`\n`, `\r`, `\r\n`). Line terminators are not included in the result. Handles `\r\n` as a single line break.

#### `dragon_str_partition`
```cpp
DragonList* dragon_str_partition(const char* s, const char* sep)
```
Splits the string at the first occurrence of `sep`. Returns a 3-element list: `(before, sep, after)`. If `sep` is not found, returns `(s, "", "")`.

#### `dragon_str_rpartition`
```cpp
DragonList* dragon_str_rpartition(const char* s, const char* sep)
```
Splits the string at the last occurrence of `sep`. Returns a 3-element list: `(before, sep, after)`. If `sep` is not found, returns `("", "", s)`.

#### `dragon_str_join`
```cpp
const char* dragon_str_join(const char* sep, DragonList* l)
```
Joins all strings in list `l` with separator `sep`. Two-pass: first computes total length, then assembles the result. Returns `""` for empty or NULL list.

---

## 5. Math Helpers

These implement Python's math semantics, which differ from C's in important ways.

### `dragon_abs_int` / `dragon_abs_float`

```cpp
int64_t dragon_abs_int(int64_t x)
double dragon_abs_float(double x)
```

Absolute value for integers and floats respectively.

### `dragon_pow_int`

```cpp
int64_t dragon_pow_int(int64_t base, int64_t exp)
```

Integer exponentiation via a simple loop: multiplies `result` by `base` for `exp` iterations. Starting value is 1. This handles non-negative exponents only.

### `dragon_pow_float`

```cpp
double dragon_pow_float(double base, double exp)
```

Float exponentiation. Delegates to the C standard library `pow()`.

### `dragon_floordiv_int`

```cpp
int64_t dragon_floordiv_int(int64_t a, int64_t b)
```

Python-style floor division. Differs from C's integer division when operands have different signs:
- Division by zero: prints `"ZeroDivisionError: integer division by zero"` to stderr and calls `exit(1)`
- Computes `d = a / b` (C truncation)
- If operands have different signs (`(a ^ b) < 0`) and the division is not exact (`d * b != a`), decrements `d` by 1 to achieve floor behavior

Examples where this differs from C: `-7 // 2` yields `-4` in Python (floor) vs. `-3` in C (truncation).

### `dragon_mod_int`

```cpp
int64_t dragon_mod_int(int64_t a, int64_t b)
```

Python-style modulo. The result always has the same sign as the divisor:
- Division by zero: prints `"ZeroDivisionError: integer modulo by zero"` to stderr and calls `exit(1)`
- Computes `r = a % b` (C remainder)
- If the result is non-zero and has a different sign from `b` (`(r ^ b) < 0`), adds `b` to adjust

Example: `-7 % 2` yields `1` in Python vs. `-1` in C.

---

## 6. Exception Handling

Dragon implements exceptions using two mechanisms: a simple `dragon_raise` for immediate termination, and a full setjmp/longjmp machinery for try/except blocks.

### Simple Raise and Assert

#### `dragon_raise`
```cpp
void dragon_raise(const char* type, const char* message)
```
Prints `"TYPE: MESSAGE"` to stderr and calls `exit(1)`. Used for exceptions outside try/except blocks.

#### `dragon_assert`
```cpp
void dragon_assert(int64_t condition, const char* message)
```
If `condition` is false (0), prints `"AssertionError: MESSAGE"` to stderr and calls `exit(1)`. If `message` is NULL, prints just `"AssertionError"`.

#### `dragon_assert_no_msg`
```cpp
void dragon_assert_no_msg(int64_t condition)
```
Calls `dragon_assert(condition, nullptr)`.

### setjmp/longjmp Exception Machinery

#### Infrastructure

Exception state is **not** a single process-global stack. It is per-execution-context: each OS thread has its own thread-local (`__thread`) state, and each green thread (vthread) carries its own embedded exception stack. Both hold up to `DRAGON_EXC_STACK_SIZE` (32) levels of nested try/except.

```cpp
// runtime_internal.h - per-OS-thread state (TLS fallback for the main / non-vthread thread)
extern __thread jmp_buf     __dragon_exc_stack[DRAGON_EXC_STACK_SIZE];
extern __thread int         __dragon_exc_sp;     // -1 = empty
extern __thread int         __dragon_exc_type;
extern __thread const char* __dragon_exc_msg;
extern __thread void*       __dragon_exc_obj;    // typed exception instance, if any
extern __thread DragonVThread* __current_vthread;

// each green thread embeds its OWN exception stack (DragonVThread, runtime_internal.h)
typedef struct DragonVThread {
    jmp_buf     exc_stack[DRAGON_EXC_STACK_SIZE];
    int         exc_sp;
    int         exc_type;
    const char* exc_msg;
    void*       exc_obj;
    /* ... cleanup stack, scheduler fields ... */
} DragonVThread;
```

The machinery resolves the active context with `EXC_VT = __dragon_exc_vt ? __dragon_exc_vt : __current_vthread` (`runtime_exception.cpp`): a running generator installs its own isolated `__dragon_exc_vt`, otherwise the current green thread's struct is used, and when both are NULL the TLS globals are used (the bare OS thread). `dragon_exc_push_frame` increments the active context's `exc_sp` and returns its `jmp_buf` for `setjmp`; `dragon_exc_pop_frame` decrements it.

This is why a green thread that raises inside a `try` cannot clobber another vthread's setjmp frames on the same carrier thread: each context has a private stack. A separate per-context **unwind cleanup stack** (`DragonCleanupStack`) is the side channel that frees owned heap locals a `longjmp` skips over, since setjmp/longjmp unwinding bypasses the codegen-emitted scope cleanup.

#### `dragon_exc_push_frame`
```cpp
void* dragon_exc_push_frame()
```
Increments the exception stack pointer and returns a pointer to the `jmp_buf` at the new top. The LLVM-generated code passes this pointer to `setjmp`.

#### `dragon_exc_pop_frame`
```cpp
void dragon_exc_pop_frame()
```
Decrements the exception stack pointer if it is >= 0. Called when leaving a try/except block normally or after handling an exception.

#### `dragon_exc_get_type`
```cpp
int64_t dragon_exc_get_type()
```
Returns the current exception type code as an int64_t. Used by generated code to match `except` clauses.

#### `dragon_exc_get_msg`
```cpp
const char* dragon_exc_get_msg()
```
Returns the current exception message string. Used by `except ExcType as e` to bind the exception message.

#### `dragon_raise_exc`
```cpp
void dragon_raise_exc(int64_t type, const char* msg)
```

Raises an exception via the setjmp/longjmp machinery, operating on the active context (`EXC_VT` if set, else the TLS globals):
1. Stores `type` and `msg` (the message slot owns a protective heap copy of mortal strings; the `_cstr` / `_consume` variants exist for raw C strings and owned temporaries)
2. If there is an active try block in that context (`exc_sp >= 0`): calls `longjmp(exc_stack[sp], type)` to jump to the most recent `setjmp` point
3. If no try block is active: prints `"Unhandled exception: MSG"` to stderr and calls `exit(1)`

A companion `dragon_raise_exc_obj` carries a typed user-exception instance alongside the message, so `except X as e` can bind `e` to the instance with its typed fields intact (read back via `dragon_exc_get_obj`).

### Usage Pattern

The LLVM CodeGen generates this pattern for `try`/`except`:

```c
// try {
void* frame = dragon_exc_push_frame();
int exc_val = setjmp(*(jmp_buf*)frame);
if (exc_val == 0) {
    // ... try body ...
    dragon_exc_pop_frame();       // Normal exit: pop the handler
} else {
    dragon_exc_pop_frame();       // Exception caught: pop the handler
    int64_t exc_type = dragon_exc_get_type();
    const char* exc_msg = dragon_exc_get_msg();
    // ... match exc_type against except clauses ...
}
```

---

## 7. List Operations

The polymorphic list functions below operate on `DragonList*` (the I64 variant - int and bool elements). Monomorphized variants (`dragon_list_*_f64`, `dragon_list_*_ptr`, and the `list[Any]` box ops) handle `list[float]`, `list[<heap>]`, and `list[Any]` at their native element types; codegen picks the family from `list[T]`. The pointer ops are refcount-aware (set/append/destroy own the incref/decref accounting), so codegen does not emit RC around element stores.

### Creation and Core Operations

#### `dragon_list_new`
```cpp
DragonList* dragon_list_new(int64_t capacity)
```
Creates a new list. Both the `DragonList` header and the `data` array are heap-allocated via `malloc`. Default minimum capacity is 8.

#### `dragon_list_append`
```cpp
void dragon_list_append(DragonList* list, int64_t value)
```
Appends a value. If `size >= capacity`, doubles the capacity via `realloc`. Amortized O(1).

#### `dragon_list_get`
```cpp
int64_t dragon_list_get(DragonList* list, int64_t index)
```
Returns the element at index. Supports **negative indexing**: if `index < 0`, adds `list->size` to get the actual index. Exits with `"IndexError: list index out of range"` if out of bounds.

#### `dragon_list_set`
```cpp
void dragon_list_set(DragonList* list, int64_t index, int64_t value)
```
Sets the element at index. Supports negative indexing. Exits with `"IndexError: list assignment index out of range"` if out of bounds.

#### `dragon_list_len`
```cpp
int64_t dragon_list_len(DragonList* list)
```
Returns `list->size`, or 0 if `list` is NULL.

#### `dragon_list_pop`
```cpp
int64_t dragon_list_pop(DragonList* list, int64_t index)
```
Removes and returns the element at index. Supports negative indexing. Shifts subsequent elements left. Exits with `"IndexError: pop from empty list"` for empty lists, or `"IndexError: pop index out of range"` if index is out of bounds.

#### `dragon_list_clear`
```cpp
void dragon_list_clear(DragonList* list)
```
Sets `size` to 0. Does not free or reallocate the data array.

### Copy and Search Operations

#### `dragon_list_copy`
```cpp
DragonList* dragon_list_copy(DragonList* list)
```
Creates a shallow copy. Allocates a new list and appends all elements from the source.

#### `dragon_list_count`
```cpp
int64_t dragon_list_count(DragonList* list, int64_t value)
```
Returns the number of elements equal to `value`.

#### `dragon_list_extend`
```cpp
void dragon_list_extend(DragonList* list, DragonList* other)
```
Appends all elements from `other` to `list` by calling `dragon_list_append` for each.

#### `dragon_list_index`
```cpp
int64_t dragon_list_index(DragonList* list, int64_t value)
```
Returns the index of the first occurrence of `value`. Exits with `"ValueError: N is not in list"` if not found.

#### `dragon_list_insert`
```cpp
void dragon_list_insert(DragonList* list, int64_t index, int64_t value)
```
Inserts `value` at index. Supports negative indexing with clamping: negative indices are adjusted by `+size`, then clamped to `[0, size]`. Shifts subsequent elements right.

#### `dragon_list_remove`
```cpp
void dragon_list_remove(DragonList* list, int64_t value)
```
Removes the first occurrence of `value`. Exits with `"ValueError: list.remove(x): x not in list"` if not found.

#### `dragon_list_reverse`
```cpp
void dragon_list_reverse(DragonList* list)
```
Reverses the list in-place using a two-pointer swap approach.

#### `dragon_list_sort`
```cpp
void dragon_list_sort(DragonList* list)
```
Sorts the list in-place using **insertion sort**. O(n^2) worst case but simple and cache-friendly for small lists. Sorts `int64_t` values in ascending order.

### List Printing

#### `dragon_print_list_int`
```cpp
void dragon_print_list_int(DragonList* list)
```
Prints a list in Python format: `[1, 2, 3]\n`. Uses `%ld` format for each element.

---

## 8. Dict Operations

Dict functions operate on `DragonDict*`. The implementation is a **compact, open-addressed, insertion-ordered** hash table (CPython-style): a dense `entries` array in insertion order plus a sparse `indices` table that maps `hash -> entry index`. Lookup is **O(1)** via `dict_probe` on the index table (`runtime_dict.cpp`), **not** linear search. Both string keys and int keys are supported (`keys_are_ptr` selects); string lookups hash via keyed SipHash-1-3 (HashDoS defense). Values carry a per-entry `DragonValueTag`, so a dict can hold typed or `Any` values.

### Creation and Core Operations

#### `dragon_dict_new`
```cpp
DragonDict* dragon_dict_new(int64_t cap)
```
Creates a new dict. The `DragonDict` header, the dense `entries` array, and the sparse `indices` table are all heap-allocated; the index table is sized to ~2x capacity (load factor ~0.5) and every slot is initialized to `DICT_EMPTY` (-1). Minimum capacity is 4.

#### `dragon_dict_set_tagged`
```cpp
void dragon_dict_set_tagged(DragonDict* d, const char* key, int64_t value, int64_t tag)
```
The real string-keyed store. Hashes the key, probes the index table (`dict_probe`) for the matching or insertion slot, and:
- If the key exists, overwrites the value in place (adopting one ref of the new value, dropping the old value's ref by its tag)
- If not, appends to the dense `entries` array and points the index slot at it; grows (or first compacts away tombstones) when the dense array or index table fills

The dict owns one DragonString reference per string key (`keys_are_ptr` is set), and enrolls itself in cycle tracking the first time a traceable (list/dict/bytes) value is inserted. `dragon_dict_set(d, key, value)` is a thin wrapper that calls this with `tag = TAG_INT`.

#### `dragon_dict_get`
```cpp
int64_t dragon_dict_get(DragonDict* d, const char* key)
```
Hashes the key, probes the index table, and returns the value at the resolved entry. Raises a catchable `KeyError` (the message is heap-dup'd so it survives the longjmp) if not found.

#### `dragon_dict_len`
```cpp
int64_t dragon_dict_len(DragonDict* d)
```
Returns `d->size`, or 0 if `d` is NULL.

#### `dragon_dict_has_key`
```cpp
int64_t dragon_dict_has_key(DragonDict* d, const char* key)
```
Returns 1 if key exists, 0 otherwise. Used by the `in` operator on dicts.

### Retrieval and Mutation

#### `dragon_dict_get_default`
```cpp
int64_t dragon_dict_get_default(DragonDict* d, const char* key, int64_t def)
```
Returns the value for key, or `def` if not found. Implements Python's `dict.get(key, default)`.

#### `dragon_dict_keys`
```cpp
DragonList* dragon_dict_keys(DragonDict* d)
```
Returns a new list of the dict's keys, in insertion order.

#### `dragon_dict_values`
```cpp
DragonList* dragon_dict_values(DragonDict* d)
```
Returns a new list containing all values.

#### `dragon_dict_items`
```cpp
DragonList* dragon_dict_items(DragonDict* d)
```
Returns a new list of `DragonTuple*` (bitcast to `int64_t`). Each tuple contains `(key, value)` as a 2-element tuple.

#### `dragon_dict_pop`
```cpp
int64_t dragon_dict_pop(DragonDict* d, const char* key)
```
Removes and returns the value for key. Raises a catchable `KeyError` if not found. Deletion marks the dense slot `dead` (a tombstone) and tombstones the index slot rather than shifting the array; dead slots are reclaimed lazily by `dict_compact`. Insertion order is preserved.

#### `dragon_dict_pop_default`
```cpp
int64_t dragon_dict_pop_default(DragonDict* d, const char* key, int64_t def)
```
Removes and returns the value for key. If the key is not found, returns `def` instead of raising an error.

#### `dragon_dict_clear`
```cpp
void dragon_dict_clear(DragonDict* d)
```
Sets `size` to 0. Does not free arrays.

#### `dragon_dict_copy`
```cpp
DragonDict* dragon_dict_copy(DragonDict* d)
```
Creates a shallow copy by iterating live entries and re-inserting each (incref'ing refcounted values).

#### `dragon_dict_update`
```cpp
void dragon_dict_update(DragonDict* d, DragonDict* other)
```
Merges all live entries from `other` into `d`. Existing keys are overwritten.

#### `dragon_dict_setdefault`
```cpp
int64_t dragon_dict_setdefault(DragonDict* d, const char* key, int64_t def)
```
If key exists, returns its value. Otherwise, inserts `(key, def)` and returns `def`.

### Dict Printing

#### `dragon_print_dict`
```cpp
void dragon_print_dict(DragonDict* d)
```
Prints in Python format: `{'key1': 1, 'key2': 2}\n`. Uses `%s` for keys and `%ld` for values.

---

## 9. Tuple Operations

Tuples are fixed-size containers. Unlike lists, their length is set at creation and does not change. The `DragonTuple` struct stores a `length` field (not `capacity`/`size` like lists).

#### `dragon_tuple_new`
```cpp
DragonTuple* dragon_tuple_new(int64_t count)
```
Creates a new tuple with `count` elements. Both the header and data array are heap-allocated. Data is uninitialized -- elements must be set via `dragon_tuple_set`.

#### `dragon_tuple_get`
```cpp
int64_t dragon_tuple_get(DragonTuple* t, int64_t index)
```
Returns element at index. Supports negative indexing. Exits with `"IndexError: tuple index out of range"` if out of bounds.

#### `dragon_tuple_set`
```cpp
void dragon_tuple_set(DragonTuple* t, int64_t index, int64_t val)
```
Sets element at index. Only used during tuple construction -- silently ignores out-of-bounds indices.

#### `dragon_tuple_len`
```cpp
int64_t dragon_tuple_len(DragonTuple* t)
```
Returns `t->length`, or 0 if `t` is NULL.

#### `dragon_print_tuple`
```cpp
void dragon_print_tuple(DragonTuple* t)
```
Prints in Python format: `(1, 2, 3)\n`. Single-element tuples include a trailing comma: `(1,)\n`.

---

## 10. Set Operations

Sets use an open-addressing hash table with linear probing. Each bucket has a state: 0 (empty), 1 (occupied), or 2 (deleted / tombstone). The hash function uses Knuth's multiplicative hash (`val * 2654435761`).

### Creation and Core Operations

#### `dragon_set_new`
```cpp
DragonSet* dragon_set_new()
```
Creates a new empty set with initial capacity 16.

#### `dragon_set_add`
```cpp
void dragon_set_add(DragonSet* s, int64_t val)
```
Adds a value to the set. If the load factor exceeds 50% (`count * 2 >= capacity`), the table is doubled in size. Duplicates are silently ignored.

#### `dragon_set_contains`
```cpp
int64_t dragon_set_contains(DragonSet* s, int64_t val)
```
Returns 1 if `val` is in the set, 0 otherwise.

#### `dragon_set_remove`
```cpp
void dragon_set_remove(DragonSet* s, int64_t val)
```
Removes `val` from the set. Exits with `"KeyError: val"` if not found. Uses tombstone deletion (state set to 2).

#### `dragon_set_discard`
```cpp
void dragon_set_discard(DragonSet* s, int64_t val)
```
Like `remove`, but silently ignores values not in the set.

#### `dragon_set_pop`
```cpp
int64_t dragon_set_pop(DragonSet* s)
```
Removes and returns an arbitrary element. Exits with `"KeyError: 'pop from an empty set'"` if the set is empty.

#### `dragon_set_len`
```cpp
int64_t dragon_set_len(DragonSet* s)
```
Returns `s->count`, or 0 if `s` is NULL.

#### `dragon_set_clear`
```cpp
void dragon_set_clear(DragonSet* s)
```
Resets all bucket states to 0 (empty) and sets count to 0.

### Copy and Set Algebra

#### `dragon_set_copy`
```cpp
DragonSet* dragon_set_copy(DragonSet* s)
```
Creates a deep copy of the set, copying both the bucket and state arrays.

#### `dragon_set_union`
```cpp
DragonSet* dragon_set_union(DragonSet* a, DragonSet* b)
```
Returns a new set containing all elements from both `a` and `b`.

#### `dragon_set_intersection`
```cpp
DragonSet* dragon_set_intersection(DragonSet* a, DragonSet* b)
```
Returns a new set containing only elements present in both `a` and `b`.

#### `dragon_set_difference`
```cpp
DragonSet* dragon_set_difference(DragonSet* a, DragonSet* b)
```
Returns a new set containing elements in `a` but not in `b`.

#### `dragon_set_symmetric_difference`
```cpp
DragonSet* dragon_set_symmetric_difference(DragonSet* a, DragonSet* b)
```
Returns a new set containing elements in either `a` or `b` but not both.

#### `dragon_set_issubset`
```cpp
int64_t dragon_set_issubset(DragonSet* a, DragonSet* b)
```
Returns 1 if every element in `a` is also in `b`, 0 otherwise.

#### `dragon_set_issuperset`
```cpp
int64_t dragon_set_issuperset(DragonSet* a, DragonSet* b)
```
Returns 1 if every element in `b` is also in `a`. Delegates to `dragon_set_issubset(b, a)`.

#### `dragon_set_isdisjoint`
```cpp
int64_t dragon_set_isdisjoint(DragonSet* a, DragonSet* b)
```
Returns 1 if `a` and `b` have no elements in common, 0 otherwise.

#### `dragon_set_update`
```cpp
void dragon_set_update(DragonSet* a, DragonSet* b)
```
Adds all elements from `b` into `a` in-place.

### Set Printing

#### `dragon_print_set`
```cpp
void dragon_print_set(DragonSet* s)
```
Prints in Python format: `{1, 2, 3}\n`. Iterates only over occupied buckets.

---

## 11. Aggregate Functions and Builtins

These operate on `DragonList*` values and provide Python builtin functionality.

### Min / Max (scalar and list variants)

| Function | Signature | Description |
|----------|-----------|-------------|
| `dragon_min_int` | `int64_t dragon_min_int(int64_t a, int64_t b)` | Returns the smaller of two integers |
| `dragon_max_int` | `int64_t dragon_max_int(int64_t a, int64_t b)` | Returns the larger of two integers |
| `dragon_min_float` | `double dragon_min_float(double a, double b)` | Returns the smaller of two floats |
| `dragon_max_float` | `double dragon_max_float(double a, double b)` | Returns the larger of two floats |
| `dragon_min_list` | `int64_t dragon_min_list(DragonList* list)` | Returns the minimum value in a list (returns 0 for empty/NULL) |
| `dragon_max_list` | `int64_t dragon_max_list(DragonList* list)` | Returns the maximum value in a list (returns 0 for empty/NULL) |

### List Aggregates

| Function | Signature | Description |
|----------|-----------|-------------|
| `dragon_sum_list` | `int64_t dragon_sum_list(DragonList* list)` | Sum of all elements; returns 0 for NULL |
| `dragon_any_list` | `int64_t dragon_any_list(DragonList* list)` | 1 if any element is truthy (non-zero) |
| `dragon_all_list` | `int64_t dragon_all_list(DragonList* list)` | 1 if all elements are truthy; 1 for empty/NULL (Python convention) |

### Iteration Helpers

#### `dragon_enumerate`
```cpp
DragonList* dragon_enumerate(DragonList* list, int64_t start)
```
Returns a list of 2-element tuples. Each tuple contains `(start + i, list->data[i])`. The tuples are `DragonTuple*` stored as `int64_t`-cast pointers.

#### `dragon_zip`
```cpp
DragonList* dragon_zip(DragonList* a, DragonList* b)
```
Returns a list of 2-element tuples. Length is `min(a->size, b->size)`. Each tuple contains `(a->data[i], b->data[i])`.

#### `dragon_sorted`
```cpp
DragonList* dragon_sorted(DragonList* list)
```
Returns a new sorted copy. Copies the list then sorts in-place.

#### `dragon_reversed`
```cpp
DragonList* dragon_reversed(DragonList* list)
```
Returns a new reversed copy.

### Hash and Identity

| Function | Signature | Description |
|----------|-----------|-------------|
| `dragon_hash_int` | `int64_t dragon_hash_int(int64_t x)` | Returns `x` itself (Python convention for ints) |
| `dragon_hash_str` | `int64_t dragon_hash_str(const char* s)` | djb2 hash of string; returns 0 for NULL |
| `dragon_id` | `int64_t dragon_id(int64_t val)` | Returns the value itself (address for pointers) |

### Numeric Builtins

#### `dragon_ord`
```cpp
int64_t dragon_ord(const char* s)
```
Returns the integer value of the first character as an unsigned byte. Returns 0 for NULL or empty input.

#### `dragon_chr`
```cpp
const char* dragon_chr(int64_t code)
```
Converts an integer to a single-character string. Allocates a 2-byte buffer via `malloc`.

#### `dragon_round_int`
```cpp
int64_t dragon_round_int(double x)
```
Python-style banker's rounding (round-half-to-even). Uses `round()` from `<cmath>`, with special-case logic for halfway values to round to the nearest even integer.

#### `dragon_pow_float`
```cpp
double dragon_pow_float(double base, double exp)
```
Float exponentiation via `pow()`.

#### `dragon_divmod`
```cpp
DragonTuple* dragon_divmod(int64_t a, int64_t b)
```
Returns a 2-element tuple containing `(quotient, remainder)` using Python-style floor division and modulo semantics. Returns `(0, 0)` for division by zero.

#### `dragon_hex`
```cpp
const char* dragon_hex(int64_t x)
```
Returns the hexadecimal string representation with `0x` prefix. Negative values get a `-0x` prefix. Uses `snprintf` with `%llx` format into a 32-byte buffer.

#### `dragon_oct`
```cpp
const char* dragon_oct(int64_t x)
```
Returns the octal string representation with `0o` prefix. Negative values get `-0o` prefix. Uses `snprintf` with `%llo` format.

#### `dragon_bin`
```cpp
const char* dragon_bin(int64_t x)
```
Returns the binary string representation with `0b` prefix. Builds the binary digits in reverse order (LSB first), then reverses during output. Handles negative values with `-0b` prefix and zero as `0b0`.

### Repr Functions

| Function | Signature | Description |
|----------|-----------|-------------|
| `dragon_repr_int` | `const char* dragon_repr_int(int64_t x)` | Delegates to `dragon_int_to_str` |
| `dragon_repr_str` | `const char* dragon_repr_str(const char* s)` | Wraps string in single quotes; returns `"None"` for NULL |
| `dragon_repr_float` | `const char* dragon_repr_float(double x)` | Delegates to `dragon_float_to_str` |
| `dragon_repr_bool` | `const char* dragon_repr_bool(int64_t x)` | Returns `"True"` or `"False"` |

---

## 12. Slicing

### Sentinel Value

```cpp
#define DRAGON_SLICE_NONE (-9223372036854775807LL - 1)
```

This is `INT64_MIN`, used as a sentinel to indicate an omitted slice bound. The LLVM CodeGen emits this literal when a slice bound is not specified (e.g., `x[:]` or `x[::2]`).

### `dragon_slice_indices`

```cpp
static void dragon_slice_indices(int64_t len, int64_t* start, int64_t* stop, int64_t step)
```

Resolves slice bounds according to Python semantics. This is the core algorithm shared by list and string slicing. Note: this function is `static` (file-internal) since it is only called by the two slicing functions below.

- If `*start` is `DRAGON_SLICE_NONE`: set to `len - 1` (negative step) or `0` (positive step)
- If `*start` is negative: add `len`; if still negative, clamp to `-1` (negative step) or `0` (positive step)
- If `*start >= len`: clamp to `len - 1` (negative step) or `len` (positive step)
- If `*stop` is `DRAGON_SLICE_NONE`: set to `-1` (negative step) or `len` (positive step)
- If `*stop` is negative: add `len`; if still negative, clamp to `-1` (negative step) or `0` (positive step)
- If `*stop >= len`: clamp to `len` (negative step) or `len` (positive step)

### `dragon_str_slice`

```cpp
const char* dragon_str_slice(const char* s, int64_t start, int64_t stop, int64_t step)
```

Returns a new string containing characters from the given slice range.
- Step of 0 exits with `"ValueError: slice step cannot be zero"`
- Calls `dragon_slice_indices` to resolve bounds
- Positive step: iterates forward collecting characters
- Negative step: iterates backward collecting characters
- Returns `""` for NULL input

### `dragon_list_slice`

```cpp
DragonList* dragon_list_slice(DragonList* l, int64_t start, int64_t stop, int64_t step)
```

Returns a new list containing elements from `start` to `stop` (exclusive) with the given `step`.
- Step of 0 exits with `"ValueError: slice step cannot be zero"`
- Calls `dragon_slice_indices` to resolve bounds
- Positive step: iterates `i = start; i < stop; i += step`
- Negative step: iterates `i = start; i > stop; i += step`
- Returns an empty list for NULL input

---

## 13. File I/O

These functions provide Python-style file operations. File handles are opaque `void*` pointers wrapping `FILE*`.

#### `dragon_file_open`
```cpp
void* dragon_file_open(const char* filename, const char* mode)
```
Opens a file via `fopen` and returns the `FILE*` cast to `void*`. Returns NULL on failure.

#### `dragon_file_close`
```cpp
void dragon_file_close(void* handle)
```
Closes the file via `fclose`. No-op for NULL handles.

#### `dragon_file_read`
```cpp
const char* dragon_file_read(void* handle)
```
Reads the entire remaining contents of the file into a heap-allocated string. Computes remaining bytes via `ftell`/`fseek`, then uses `fread`. Returns `""` for NULL handles or empty files.

#### `dragon_file_readline`
```cpp
const char* dragon_file_readline(void* handle)
```
Reads a single line (up to 4096 bytes) via `fgets`. Returns a heap-allocated string. Returns `""` for NULL handles or EOF.

#### `dragon_file_write`
```cpp
void dragon_file_write(void* handle, const char* data)
```
Writes a string to the file via `fputs`. No-op if either argument is NULL.

#### `dragon_file_readlines`
```cpp
DragonList* dragon_file_readlines(void* handle)
```
Reads all remaining lines into a `DragonList*` of heap-allocated strings. Each line retains its newline character. Returns an empty list for NULL handles.

---

## 14. Core Function Index (partial)

For reference, the tables below list the core type-system runtime functions by category. This is **not exhaustive**: the runtime has grown well beyond this set and now also exports reference-counting primitives (`dragon_incref` / `dragon_decref` and their atomic and string variants, `dragon_gc_collect`, `dragon_gc_track`), `Any`/`Union` box ops (`runtime_box.cpp`), the green-thread / concurrency API (`dragon_vthread_*`, scheduler-aware socket I/O), monomorphized container variants (`dragon_list_*_f64` / `_ptr`, `dragon_dict_int_*` / `_str_*`), reflection (`dragon_dir`, `dragon_class_find_method`), crypto / TLS / compression, and more. Treat the following as a representative core, not the full symbol table; the authoritative list is the source under `lib/Runtime/` and the declarations in `runtime_internal.h`.

### I/O (7 functions)
| Function | Signature |
|----------|-----------|
| `dragon_print_int` | `void dragon_print_int(int64_t value)` |
| `dragon_print_float` | `void dragon_print_float(double value)` |
| `dragon_print_str` | `void dragon_print_str(const char* s)` |
| `dragon_print_bool` | `void dragon_print_bool(int64_t value)` |
| `dragon_print_none` | `void dragon_print_none()` |
| `dragon_print_newline` | `void dragon_print_newline()` |
| `dragon_input` | `const char* dragon_input(const char* prompt)` |

### String Operations (6 functions)
| Function | Signature |
|----------|-----------|
| `dragon_str_concat` | `const char* dragon_str_concat(const char* a, const char* b)` |
| `dragon_str_len` | `int64_t dragon_str_len(const char* s)` |
| `dragon_int_to_str` | `const char* dragon_int_to_str(int64_t value)` |
| `dragon_float_to_str` | `const char* dragon_float_to_str(double value)` |
| `dragon_str_eq` | `int64_t dragon_str_eq(const char* a, const char* b)` |
| `dragon_bool_to_str` | `const char* dragon_bool_to_str(int64_t value)` |

### Type Conversions (2 functions)
| Function | Signature |
|----------|-----------|
| `dragon_str_to_int` | `int64_t dragon_str_to_int(const char* s)` |
| `dragon_str_to_float` | `double dragon_str_to_float(const char* s)` |

### Math (6 functions)
| Function | Signature |
|----------|-----------|
| `dragon_abs_int` | `int64_t dragon_abs_int(int64_t x)` |
| `dragon_abs_float` | `double dragon_abs_float(double x)` |
| `dragon_pow_int` | `int64_t dragon_pow_int(int64_t base, int64_t exp)` |
| `dragon_floordiv_int` | `int64_t dragon_floordiv_int(int64_t a, int64_t b)` |
| `dragon_mod_int` | `int64_t dragon_mod_int(int64_t a, int64_t b)` |
| `dragon_pow_float` | `double dragon_pow_float(double base, double exp)` |

### Exception Handling (6 functions)
| Function | Signature |
|----------|-----------|
| `dragon_raise` | `void dragon_raise(const char* type, const char* message)` |
| `dragon_assert` | `void dragon_assert(int64_t condition, const char* message)` |
| `dragon_assert_no_msg` | `void dragon_assert_no_msg(int64_t condition)` |
| `dragon_exc_push_frame` | `void* dragon_exc_push_frame()` |
| `dragon_exc_pop_frame` | `void dragon_exc_pop_frame()` |
| `dragon_exc_get_type` | `int64_t dragon_exc_get_type()` |
| `dragon_exc_get_msg` | `const char* dragon_exc_get_msg()` |
| `dragon_raise_exc` | `void dragon_raise_exc(int64_t type, const char* msg)` |

### List (16 functions)
| Function | Signature |
|----------|-----------|
| `dragon_list_new` | `DragonList* dragon_list_new(int64_t capacity)` |
| `dragon_list_append` | `void dragon_list_append(DragonList* list, int64_t value)` |
| `dragon_list_get` | `int64_t dragon_list_get(DragonList* list, int64_t index)` |
| `dragon_list_set` | `void dragon_list_set(DragonList* list, int64_t index, int64_t value)` |
| `dragon_list_len` | `int64_t dragon_list_len(DragonList* list)` |
| `dragon_print_list_int` | `void dragon_print_list_int(DragonList* list)` |
| `dragon_list_insert` | `void dragon_list_insert(DragonList* list, int64_t index, int64_t value)` |
| `dragon_list_remove` | `void dragon_list_remove(DragonList* list, int64_t value)` |
| `dragon_list_pop` | `int64_t dragon_list_pop(DragonList* list, int64_t index)` |
| `dragon_list_clear` | `void dragon_list_clear(DragonList* list)` |
| `dragon_list_extend` | `void dragon_list_extend(DragonList* list, DragonList* other)` |
| `dragon_list_index` | `int64_t dragon_list_index(DragonList* list, int64_t value)` |
| `dragon_list_count` | `int64_t dragon_list_count(DragonList* list, int64_t value)` |
| `dragon_list_sort` | `void dragon_list_sort(DragonList* list)` |
| `dragon_list_reverse` | `void dragon_list_reverse(DragonList* list)` |
| `dragon_list_copy` | `DragonList* dragon_list_copy(DragonList* list)` |

### Dict (16 functions)
| Function | Signature |
|----------|-----------|
| `dragon_dict_new` | `DragonDict* dragon_dict_new(int64_t cap)` |
| `dragon_dict_set` | `void dragon_dict_set(DragonDict* d, const char* key, int64_t value)` (wraps `dragon_dict_set_tagged`) |
| `dragon_dict_set_tagged` | `void dragon_dict_set_tagged(DragonDict* d, const char* key, int64_t value, int64_t tag)` |
| `dragon_dict_get` | `int64_t dragon_dict_get(DragonDict* d, const char* key)` |
| `dragon_dict_len` | `int64_t dragon_dict_len(DragonDict* d)` |
| `dragon_dict_has_key` | `int64_t dragon_dict_has_key(DragonDict* d, const char* key)` |
| `dragon_dict_get_default` | `int64_t dragon_dict_get_default(DragonDict* d, const char* key, int64_t def)` |
| `dragon_dict_keys` | `DragonList* dragon_dict_keys(DragonDict* d)` |
| `dragon_print_dict` | `void dragon_print_dict(DragonDict* d)` |
| `dragon_dict_values` | `DragonList* dragon_dict_values(DragonDict* d)` |
| `dragon_dict_items` | `DragonList* dragon_dict_items(DragonDict* d)` |
| `dragon_dict_pop` | `int64_t dragon_dict_pop(DragonDict* d, const char* key)` |
| `dragon_dict_pop_default` | `int64_t dragon_dict_pop_default(DragonDict* d, const char* key, int64_t def)` |
| `dragon_dict_clear` | `void dragon_dict_clear(DragonDict* d)` |
| `dragon_dict_update` | `void dragon_dict_update(DragonDict* d, DragonDict* other)` |
| `dragon_dict_setdefault` | `int64_t dragon_dict_setdefault(DragonDict* d, const char* key, int64_t def)` |
| `dragon_dict_copy` | `DragonDict* dragon_dict_copy(DragonDict* d)` |

### String Indexing (1 function)
| Function | Signature |
|----------|-----------|
| `dragon_str_index` | `const char* dragon_str_index(const char* s, int64_t index)` |

### String Methods (38 functions)
| Function | Signature |
|----------|-----------|
| `dragon_str_upper` | `const char* dragon_str_upper(const char* s)` |
| `dragon_str_lower` | `const char* dragon_str_lower(const char* s)` |
| `dragon_str_strip` | `const char* dragon_str_strip(const char* s)` |
| `dragon_str_lstrip` | `const char* dragon_str_lstrip(const char* s)` |
| `dragon_str_rstrip` | `const char* dragon_str_rstrip(const char* s)` |
| `dragon_str_title` | `const char* dragon_str_title(const char* s)` |
| `dragon_str_capitalize` | `const char* dragon_str_capitalize(const char* s)` |
| `dragon_str_swapcase` | `const char* dragon_str_swapcase(const char* s)` |
| `dragon_str_casefold` | `const char* dragon_str_casefold(const char* s)` |
| `dragon_str_replace` | `const char* dragon_str_replace(const char* s, const char* old_s, const char* new_s)` |
| `dragon_str_repeat` | `const char* dragon_str_repeat(const char* s, int64_t n)` |
| `dragon_str_removeprefix` | `const char* dragon_str_removeprefix(const char* s, const char* prefix)` |
| `dragon_str_removesuffix` | `const char* dragon_str_removesuffix(const char* s, const char* suffix)` |
| `dragon_str_center` | `const char* dragon_str_center(const char* s, int64_t w, char fill)` |
| `dragon_str_ljust` | `const char* dragon_str_ljust(const char* s, int64_t w, char fill)` |
| `dragon_str_rjust` | `const char* dragon_str_rjust(const char* s, int64_t w, char fill)` |
| `dragon_str_zfill` | `const char* dragon_str_zfill(const char* s, int64_t w)` |
| `dragon_str_expandtabs` | `const char* dragon_str_expandtabs(const char* s, int64_t tabsize)` |
| `dragon_str_find` | `int64_t dragon_str_find(const char* s, const char* sub)` |
| `dragon_str_rfind` | `int64_t dragon_str_rfind(const char* s, const char* sub)` |
| `dragon_str_index_of` | `int64_t dragon_str_index_of(const char* s, const char* sub)` |
| `dragon_str_rindex` | `int64_t dragon_str_rindex(const char* s, const char* sub)` |
| `dragon_str_count` | `int64_t dragon_str_count(const char* s, const char* sub)` |
| `dragon_str_startswith` | `int64_t dragon_str_startswith(const char* s, const char* prefix)` |
| `dragon_str_endswith` | `int64_t dragon_str_endswith(const char* s, const char* suffix)` |
| `dragon_str_contains` | `int64_t dragon_str_contains(const char* s, const char* sub)` |
| `dragon_str_isdigit` | `int64_t dragon_str_isdigit(const char* s)` |
| `dragon_str_isalpha` | `int64_t dragon_str_isalpha(const char* s)` |
| `dragon_str_isalnum` | `int64_t dragon_str_isalnum(const char* s)` |
| `dragon_str_isspace` | `int64_t dragon_str_isspace(const char* s)` |
| `dragon_str_isupper` | `int64_t dragon_str_isupper(const char* s)` |
| `dragon_str_islower` | `int64_t dragon_str_islower(const char* s)` |
| `dragon_str_istitle` | `int64_t dragon_str_istitle(const char* s)` |
| `dragon_str_isascii` | `int64_t dragon_str_isascii(const char* s)` |
| `dragon_str_isdecimal` | `int64_t dragon_str_isdecimal(const char* s)` |
| `dragon_str_isnumeric` | `int64_t dragon_str_isnumeric(const char* s)` |
| `dragon_str_isprintable` | `int64_t dragon_str_isprintable(const char* s)` |
| `dragon_str_isidentifier` | `int64_t dragon_str_isidentifier(const char* s)` |

### Slicing (2 functions + 1 macro + 1 static helper)
| Item | Definition |
|------|------------|
| *(macro)* | `#define DRAGON_SLICE_NONE (-9223372036854775807LL - 1)` |
| *(static)* `dragon_slice_indices` | `static void dragon_slice_indices(int64_t len, int64_t* start, int64_t* stop, int64_t step)` |
| `dragon_str_slice` | `const char* dragon_str_slice(const char* s, int64_t start, int64_t stop, int64_t step)` |
| `dragon_list_slice` | `DragonList* dragon_list_slice(DragonList* l, int64_t start, int64_t stop, int64_t step)` |

### String Split/Join (6 functions)
| Function | Signature |
|----------|-----------|
| `dragon_str_split` | `DragonList* dragon_str_split(const char* s, const char* sep)` |
| `dragon_str_join` | `const char* dragon_str_join(const char* sep, DragonList* l)` |
| `dragon_str_splitlines` | `DragonList* dragon_str_splitlines(const char* s)` |
| `dragon_str_partition` | `DragonList* dragon_str_partition(const char* s, const char* sep)` |
| `dragon_str_rpartition` | `DragonList* dragon_str_rpartition(const char* s, const char* sep)` |
| `dragon_str_rsplit` | `DragonList* dragon_str_rsplit(const char* s, const char* sep, int64_t maxsplit)` |

### Tuple (5 functions)
| Function | Signature |
|----------|-----------|
| `dragon_tuple_new` | `DragonTuple* dragon_tuple_new(int64_t count)` |
| `dragon_tuple_get` | `int64_t dragon_tuple_get(DragonTuple* t, int64_t index)` |
| `dragon_tuple_set` | `void dragon_tuple_set(DragonTuple* t, int64_t index, int64_t val)` |
| `dragon_tuple_len` | `int64_t dragon_tuple_len(DragonTuple* t)` |
| `dragon_print_tuple` | `void dragon_print_tuple(DragonTuple* t)` |

### Set (18 functions + 2 static helpers)
| Function | Signature |
|----------|-----------|
| `dragon_set_new` | `DragonSet* dragon_set_new()` |
| `dragon_set_add` | `void dragon_set_add(DragonSet* s, int64_t val)` |
| `dragon_set_contains` | `int64_t dragon_set_contains(DragonSet* s, int64_t val)` |
| `dragon_set_remove` | `void dragon_set_remove(DragonSet* s, int64_t val)` |
| `dragon_set_discard` | `void dragon_set_discard(DragonSet* s, int64_t val)` |
| `dragon_set_len` | `int64_t dragon_set_len(DragonSet* s)` |
| `dragon_set_clear` | `void dragon_set_clear(DragonSet* s)` |
| `dragon_set_copy` | `DragonSet* dragon_set_copy(DragonSet* s)` |
| `dragon_set_union` | `DragonSet* dragon_set_union(DragonSet* a, DragonSet* b)` |
| `dragon_set_intersection` | `DragonSet* dragon_set_intersection(DragonSet* a, DragonSet* b)` |
| `dragon_set_difference` | `DragonSet* dragon_set_difference(DragonSet* a, DragonSet* b)` |
| `dragon_set_symmetric_difference` | `DragonSet* dragon_set_symmetric_difference(DragonSet* a, DragonSet* b)` |
| `dragon_set_issubset` | `int64_t dragon_set_issubset(DragonSet* a, DragonSet* b)` |
| `dragon_set_issuperset` | `int64_t dragon_set_issuperset(DragonSet* a, DragonSet* b)` |
| `dragon_set_isdisjoint` | `int64_t dragon_set_isdisjoint(DragonSet* a, DragonSet* b)` |
| `dragon_set_pop` | `int64_t dragon_set_pop(DragonSet* s)` |
| `dragon_set_update` | `void dragon_set_update(DragonSet* a, DragonSet* b)` |
| `dragon_print_set` | `void dragon_print_set(DragonSet* s)` |

### Aggregate / Builtin Functions (21 functions)
| Function | Signature |
|----------|-----------|
| `dragon_min_int` | `int64_t dragon_min_int(int64_t a, int64_t b)` |
| `dragon_max_int` | `int64_t dragon_max_int(int64_t a, int64_t b)` |
| `dragon_min_float` | `double dragon_min_float(double a, double b)` |
| `dragon_max_float` | `double dragon_max_float(double a, double b)` |
| `dragon_min_list` | `int64_t dragon_min_list(DragonList* list)` |
| `dragon_max_list` | `int64_t dragon_max_list(DragonList* list)` |
| `dragon_sum_list` | `int64_t dragon_sum_list(DragonList* list)` |
| `dragon_any_list` | `int64_t dragon_any_list(DragonList* list)` |
| `dragon_all_list` | `int64_t dragon_all_list(DragonList* list)` |
| `dragon_enumerate` | `DragonList* dragon_enumerate(DragonList* list, int64_t start)` |
| `dragon_zip` | `DragonList* dragon_zip(DragonList* a, DragonList* b)` |
| `dragon_sorted` | `DragonList* dragon_sorted(DragonList* list)` |
| `dragon_reversed` | `DragonList* dragon_reversed(DragonList* list)` |
| `dragon_hash_int` | `int64_t dragon_hash_int(int64_t x)` |
| `dragon_hash_str` | `int64_t dragon_hash_str(const char* s)` |
| `dragon_id` | `int64_t dragon_id(int64_t val)` |
| `dragon_ord` | `int64_t dragon_ord(const char* s)` |
| `dragon_chr` | `const char* dragon_chr(int64_t code)` |
| `dragon_round_int` | `int64_t dragon_round_int(double x)` |
| `dragon_divmod` | `DragonTuple* dragon_divmod(int64_t a, int64_t b)` |
| `dragon_hex` | `const char* dragon_hex(int64_t x)` |
| `dragon_oct` | `const char* dragon_oct(int64_t x)` |
| `dragon_bin` | `const char* dragon_bin(int64_t x)` |
| `dragon_repr_int` | `const char* dragon_repr_int(int64_t x)` |
| `dragon_repr_str` | `const char* dragon_repr_str(const char* s)` |
| `dragon_repr_float` | `const char* dragon_repr_float(double x)` |
| `dragon_repr_bool` | `const char* dragon_repr_bool(int64_t x)` |

### File I/O (6 functions)
| Function | Signature |
|----------|-----------|
| `dragon_file_open` | `void* dragon_file_open(const char* filename, const char* mode)` |
| `dragon_file_close` | `void dragon_file_close(void* handle)` |
| `dragon_file_read` | `const char* dragon_file_read(void* handle)` |
| `dragon_file_readline` | `const char* dragon_file_readline(void* handle)` |
| `dragon_file_write` | `void dragon_file_write(void* handle, const char* data)` |
| `dragon_file_readlines` | `DragonList* dragon_file_readlines(void* handle)` |

---

The tables above cover the core type-system surface (I/O, strings, math, exceptions, the four container families, slicing, aggregates, file I/O). They are a representative subset only - the full runtime additionally exports the reference-counting and cycle-collector primitives, the `Any`/`Union` box ops, the green-thread / concurrency API, the monomorphized container variants, reflection, and the crypto / TLS / compression subsystems. For an exact, current inventory, consult the sources under `lib/Runtime/` and the declarations in `runtime_internal.h`.

---

## Previous Document
[007 -- LLVM Code Generator (CodeGen)](007-codegen.md)

## Next Document
[009 -- Memory Management](009-memory.md)
