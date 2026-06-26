# Linking Native Libraries

You have declared your foreign functions
([Calling C and C++](/docs/1501-ffi)) and you know how data crosses the boundary
([Marshalling and intc](/docs/1502-ffi-marshalling)). The last step is
the build: telling the linker where the native code lives. Dragon reuses
the flags every C programmer already knows - `-l`, `-L`, `-I` - and adds
one of its own, `--cc-source`, for shims it should compile for you.

## Linking a system library

Declare what you need, then point the build at the library with the
familiar flags. Here is zlib's CRC-32, end to end:

```dragon
# crc.dr
extern "C" from "z" {
    def crc32(crc: int, buf: ptr, len: intc) -> int
}
extern "C" def dragon_bytes_data(b: bytes) -> ptr
extern "C" def dragon_bytes_len(b: bytes) -> int

def crc_of(data: bytes) -> int {
    return crc32(0, dragon_bytes_data(data), dragon_bytes_len(data))
}

print(crc_of("hello world".encode()))   # 222957957
```

```bash
dragon build crc.dr -lz
```

| Flag | Meaning |
|---|---|
| `-l<name>` | Link `lib<name>` - `-lz`, `-lcurl`, `-lsqlite3`. |
| `-L<dir>`  | Add a library search directory (for libs outside the default paths). |
| `-I<dir>`  | Add an include directory (also forwarded to C++/C shim compiles). |

There are two ways to get the `-l`. You can pass it on the command line,
as above, or you can fold it into the declaration with `extern "C" from
"z" { … }` - the `from "z"` clause makes the build add `-lz`
automatically, so the bare `dragon build crc.dr` would link. The
command-line form is handy when one `-l` covers symbols declared in
several places; the `from` form keeps a self-contained binding's link
requirement next to the binding itself.

## `--cc-source`: compiling shims

When you need a C or C++ shim - to call C++ at all, or to flatten a
scalar out-parameter (both shown in the previous chapters) - hand the
source file to the build with `--cc-source`. You can pass it more than
once.

```bash
# A C shim (compiled with cc):
dragon build app.dr --cc-source parseshim.c

# A C++ shim plus the libraries it needs (compiled with c++):
dragon build imgsize.dr --cc-source cvshim.cpp \
    -I/usr/include/opencv4 -lopencv_core -lopencv_imgcodecs
```

`--cc-source` does three things:

1. **Compiles the shim to a temporary object** - with `c++` when the file
   is `.cpp`, with `cc` when it is `.c`. Your `-I` paths and `-O` level
   are forwarded to that compile.
2. **Links that object into your program**, alongside the Dragon-generated
   code and any `-l` libraries.
3. **Switches the final link driver to `c++` when any C++ translation
   unit is present**, so libstdc++, static initializers, and
   exception/RTTI tables are wired up correctly. (A bare `cc … -lstdc++`
   is fragile for those, so Dragon switches the driver rather than
   patching flags.)

That third point is the subtle one. A pure-C program links with `cc`; the
moment a `.cpp` shim enters the build, Dragon links the whole binary with
`c++`. You never select the driver by hand - the presence of a C++ source
decides it.

## When things don't link

| Symptom | Cause and fix |
|---|---|
| `undefined reference to '<fn>'` | The library isn't linked - add `-l<name>` (or an `extern "C" from "<name>"` block), or you forgot to pass a C++ shim with `--cc-source`. |
| `undefined reference to 'std::…'` | A C++ shim wasn't passed via `--cc-source`, so the C++ driver wasn't selected; or a needed `-l<cpplib>` is missing. |
| `fatal error: <header.h> not found` | Add the header's directory with `-I`. |
| `cannot find -l<name>` | The library is installed somewhere off the default search path - add `-L<dir>`, or install the `-dev`/`-devel` package that ships the `.so`/`.a`. |
| Garbled or truncated text in C | You passed a non-ASCII `str` directly. Convert with `dragon_str_to_utf8_bytes` ([Marshalling](/docs/1502-ffi-marshalling)). |
| Crash reading a buffer after a call | The `str`/`bytes` backing the pointer went out of scope. Keep it bound across the call. |

## Where this fits

Linking is the least Dragon-specific part of the FFI - it is the same
toolchain contract C and C++ have always had, surfaced through the
`dragon build` command. The interesting design is upstream of it: native
values that need no marshalling, a string model that is honest about
encoding, and out-parameters handled without an address-of operator. Once
the symbols resolve, a foreign call is indistinguishable from a Dragon
one - which is the whole point. Build the thin native seam, then write the
real library on top of it in `.dr`.
