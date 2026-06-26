# Marshalling and intc

[Calling C and C++](/docs/1501-ffi) showed the easy half of the foreign
interface: any function built from the *trivial core* - `int`, `float`,
`bool`, `intc`, `ptr` - binds with a bare declaration and calls at full
native speed, because Dragon values already flow at their machine types.
There is no marshalling layer to pay for.

This chapter is the other half: the cases that *do* need care. Numbers
that are C-shaped rather than Dragon-shaped (`intc`), text that has to be
encoded and NUL-terminated before C will accept it (`str`/`bytes`), and
results that C writes back through a pointer instead of returning. None
of it is hard, but each has one rule you must not get wrong, and that
rule is usually about *lifetime*.

## `intc`: the C-int bridge

Dragon's `int` is always 64-bit. C's `int` is whatever the platform says
- 32-bit on every mainstream desktop and server. When a C signature says
`int`, `unsigned`, or `size_t`-that-fits, you bind the parameter as
`intc`, not `int`, so Dragon passes a value of the right width:

```dragon
extern "C" def abs(x: intc) -> intc

print(abs(-7))          # 7
```

`intc` is a real type, not an alias: declaring `abs(x: int)` would push a
64-bit value where C reads 32 bits and you would get garbage in the high
half. The rule of thumb: **if the C header says `int`, write `intc`; if
it says `int64_t`/`long long`, write `int`.** Dragon converts between
`int` and `intc` automatically at the call boundary, so arithmetic stays
in `int` on the Dragon side and only narrows when it crosses over.

## Passing strings and bytes

This is the one place to slow down. A Dragon `str` is stored in one of
two ways:

- **ASCII** (every code point below `0x80`): a NUL-terminated buffer that
  is already valid UTF-8 and a valid C string.
- **Non-ASCII**: a UCS-4 (UTF-32) buffer. Those bytes are *not* UTF-8 and
  *not* a C string.

So the rule is simple.

**ASCII text passes straight through** - declare the parameter as `str`
and it's zero-copy:

```dragon
extern "C" def puts(s: str) -> intc

puts("hello")           # fine - ASCII, printed by libc
```

**Arbitrary text goes through UTF-8 `bytes` first.** The runtime gives
you a blessed converter that returns a NUL-terminated UTF-8 buffer
wrapped in a `bytes`, and two accessors to reach its pointer and length:

```dragon
extern "C" def dragon_str_to_utf8_bytes(s: str) -> bytes
extern "C" def dragon_bytes_data(b: bytes) -> ptr
extern "C" def dragon_bytes_len(b: bytes) -> int

name: str = "café"                            # non-ASCII
buf: bytes = dragon_str_to_utf8_bytes(name)   # 5 UTF-8 bytes, NUL-terminated
print(dragon_bytes_len(buf))                  # 5
```

The byte length is `5`, not `4`: `é` is two UTF-8 bytes. Hand
`dragon_bytes_data(buf)` to any C function that wants a `const char*` and
it sees exactly the bytes a C program would.

> **Lifetime rule.** The pointer from `dragon_bytes_data(buf)` is valid
> only while `buf` is alive. Bind it to a variable that stays in scope
> across the call, as above. Never inline the conversion and stash the
> pointer - the temporary `bytes` would be freed at the end of the
> statement, leaving you with a dangling pointer.

To go the other way - a C string back into a Dragon `str` - wrap it with
`dragon_string_alloc(ptr, len)`. It copies the bytes into a Dragon-owned
string that the collector then manages normally:

```dragon
extern "C" def dragon_string_alloc(src: ptr, length: int) -> str
extern "C" def getenv(name: str) -> ptr
extern "C" def strlen(s: ptr) -> int

p: ptr = getenv("HOME")                 # const char* from libc
home: str = dragon_string_alloc(p, strlen(p))
print(len(home) > 0)                    # True - a real Dragon str now
```

Binary buffers round-trip the same way: `dragon_bytes_new(ptr, len)` in,
`dragon_bytes_data` / `dragon_bytes_len` out.

## Out-parameters and scratch buffers

Plenty of C APIs don't return their result - they write it through a
pointer you pass in: `int sqlite3_open(const char*, sqlite3**)`,
`pthread_mutex_init(pthread_mutex_t*, …)`, `int frexp(double, int* exp)`.
Dragon has **no address-of operator** - there is no `&x` on a local, by
design. A Dragon value lives in a register under the garbage collector's
tracking; taking its address would force it into memory (slower code) and
hand C an alias the collector can't see. Both cut against the language's
first rule, speed, so out-parameters cross the boundary one of two ways
instead - pick by what's on the other side.

**Opaque handles - allocate scratch, keep it opaque.** When C wants to
fill in a struct or a pointer it owns (a mutex, a `sqlite3*`), hand it raw
memory from `malloc`, pass the `ptr`, and never look inside: you only ever
give the same `ptr` back to the library and free it at the end. This is
exactly how `stdlib/threading.dr` drives the pthread primitives:

```dragon
extern "C" def malloc(size: int) -> ptr
extern "C" def memset(s: ptr, c: intc, n: int) -> ptr
extern "C" def free(p: ptr)
extern "C" from "pthread" {
    def pthread_mutex_init(mtx: ptr, attr: ptr) -> intc
    def pthread_mutex_lock(mtx: ptr) -> intc
    def pthread_mutex_unlock(mtx: ptr) -> intc
    def pthread_mutex_destroy(mtx: ptr) -> intc
}

mtx: ptr = malloc(64)            # scratch large enough for pthread_mutex_t
memset(mtx, 0, 64)
pthread_mutex_init(mtx, none)    # C fills it in; `none` is the NULL attr
pthread_mutex_lock(mtx)
# ... critical section ...
pthread_mutex_unlock(mtx)
pthread_mutex_destroy(mtx)
free(mtx)
```

The buffer is just memory you own - the GC never touches a bare `ptr`
(see *Who owns what* below).

**Scalar out-parameters - flatten them in a one-line shim.** When the
value written back is a plain `int`/`double` you actually need to *read*,
don't reach for pointer arithmetic - flatten the out-param to a return
value in a thin `extern "C"` shim. The address-of happens in C, where it
is free and safe:

```c
// parseshim.c - the only C you author
#include <stdlib.h>

// strtol writes its endptr through an out-param; the shim drops it and
// just returns the parsed value.
long parseshim_strtol(const char* s, int base) {
    return strtol(s, NULL, base);
}
```

```dragon
extern "C" def parseshim_strtol(s: str, base: intc) -> int

print(parseshim_strtol("2a", 16))   # 42
```

Build it with `--cc-source parseshim.c` (covered in
[Linking Native Libraries](/docs/1503-ffi-linking)). If a function hands
back *several*
values, return one and treat the rest as an opaque handle (the scratch
pattern above), or pack them into a C struct the shim allocates and
exposes through accessors - keep the Dragon side return-value-only. One
tiny C function per awkward signature keeps raw pointer reads and `&` out
of `.dr` entirely, where they would cost speed and confuse the collector.

## Who owns what

Crossing into C means stepping outside the reach of Dragon's garbage
collector, so ownership follows a small, CPython-like discipline:

- **Scalars and `ptr`** carry no ownership - pass them freely.
- **A `str` or `bytes` passed *into* C for the duration of one call** is
  borrowed. The C side must copy if it wants to keep it past the call.
- **A `str` or `bytes` returned from a Dragon constructor**
  (`dragon_string_alloc`, `dragon_bytes_new`, `dragon_str_to_utf8_bytes`)
  is owned by Dragon and freed by normal scope cleanup.
- **A C library's own allocations** - a `CURL*`, a `cv::Mat*` - are owned
  by C. Dragon holds them as an opaque `ptr` and you must call the
  library's destructor yourself. The GC never touches a bare `ptr`.

## The runtime ABI you can lean on

Dragon's runtime is statically linked into every binary, so you can
`extern "C"`-declare and call these helpers directly. This set is a
*stability contract*; everything else inside the runtime is internal and
may change without notice.

```dragon
# Strings
extern "C" def dragon_string_alloc(src: ptr, length: int) -> str
extern "C" def dragon_str_to_utf8_bytes(s: str) -> bytes   # NUL-terminated UTF-8
extern "C" def dragon_str_encode(s: str) -> bytes          # str.encode(): UTF-8 bytes

# Bytes
extern "C" def dragon_bytes_new(data: ptr, len: int) -> bytes
extern "C" def dragon_bytes_data(b: bytes) -> ptr
extern "C" def dragon_bytes_len(b: bytes) -> int

# Surface a C-side failure as a Dragon exception
extern "C" def dragon_raise_exc(code: int, msg: str)
```

With the data rules in hand, the last step is pointing the build at the
native code: [Linking Native Libraries](/docs/1503-ffi-linking).
