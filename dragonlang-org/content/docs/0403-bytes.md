# Bytes and Encoding

A `str` is text - a sequence of Unicode code points. But the world outside your
program speaks in **bytes**: a file on disk, a socket frame, a hash digest, a
binary protocol. Conflating the two is the source of a whole category of bugs,
which is why Dragon, like Python 3, keeps them as distinct types. `str` is for
text; `bytes` is for raw octets; and you cross between them explicitly, with
`encode` and `decode`, at the edges of your program.

## Bytes literals

A bytes literal is a string prefixed with `b`. Printable ASCII appears as-is;
anything else is written with `\xNN` hex escapes:

```dragon
const greeting: bytes = b"hello"
const frame: bytes = b"\xde\xad\xbe\xef"
const mixed: bytes = b"GET \x2f HTTP/1.1"
```

## Length, indexing, slicing

`len` is the **byte count**. Indexing a single position gives you an `int` in the
range 0-255 - the byte's value, not a one-character string. Slicing gives you
back `bytes`:

```dragon
const b: bytes = b"hello"
print(len(b))      # 5
print(b[0])        # 104   - an int (ord of 'h'), not "h"
print(b[1:3])      # b'el' - a slice is bytes
```

This `int`-on-index behavior is the key difference from `str`, and it's
deliberate: a byte *is* a number.

## Concatenation

`+` joins bytes objects, just like strings:

```dragon
const c: bytes = b"hello" + b" world"
print(c)           # b'hello world'
```

## Hex

`.hex()` renders bytes as a lowercase hex string; the `bytes.fromhex`
constructor goes the other way:

```dragon
const digest: bytes = b"\xde\xad\xbe\xef"
print(digest.hex())              # deadbeef
print(bytes.fromhex("deadbeef")) # b'\xde\xad\xbe\xef'
```

## Crossing the boundary: encode and decode

`str.encode` turns text into bytes; `bytes.decode` turns bytes back into text.
Both default to UTF-8 - the encoding you want almost always - and both accept an
explicit encoding name:

```dragon
const text: str = "café"
const raw: bytes = text.encode("utf-8")
print(len(text))            # 4   - code points
print(len(raw))             # 5   - UTF-8 bytes (é is two bytes)
print(raw.decode("utf-8"))  # café
```

`len(text)` is 4 and `len(raw)` is 5 - the same text/bytes distinction from
[Strings and Unicode](/docs/0401-strings), now made concrete: the `é` is one code
point but two UTF-8 bytes.

## Encoding errors are catchable

Strict decoding of invalid bytes raises `UnicodeDecodeError`; strict ASCII
encoding of non-ASCII text raises `UnicodeEncodeError`. Both are ordinary
exceptions you can catch (see [Exceptions](/docs/0901-exceptions)):

```dragon
try {
    const bad: bytes = b"\xff\xfe"
    const s: str = bad.decode("utf-8")
    print("decoded")
} except UnicodeDecodeError {
    print("UnicodeDecodeError caught")     # this fires
}
```

If you'd rather not fail, pass `"replace"` as the error handler: invalid input
becomes the replacement character `U+FFFD` on decode, or `?` on an ASCII encode,
instead of raising.

## Supported encodings

Dragon's codec set is deliberately small and fast: **`utf-8`** (aliases `utf8`,
`u8`) and **`ascii`** (alias `us-ascii`). UTF-8 is the universal default and
covers essentially every modern text need; ASCII is there for the protocols that
require it. Asking for any other codec name raises `LookupError` - Dragon doesn't
ship the long tail of legacy encodings in core.

## Where the boundary belongs

The discipline that keeps text code clean: **keep data as `str` while you work
with it, and drop to `bytes` only at the I/O edge** - reading or writing a binary
file, framing a socket message, computing a digest, building a wire protocol.
The crypto and hashing APIs in [Cryptography and Hashing](/docs/1407-stdlib-crypto)
operate on `bytes` for exactly this reason.

## At a glance

| You want to... | Write |
|----------------|-------|
| A bytes literal | `b"..."`, `b"\xde\xad"` |
| Byte count | `len(b)` |
| One byte (as int) | `b[i]` → `0`-`255` |
| A slice (as bytes) | `b[a:c]` |
| Concatenate | `b1 + b2` |
| To/from hex | `b.hex()`, `bytes.fromhex(s)` |
| Text → bytes | `s.encode()` (UTF-8) / `s.encode("ascii")` |
| Bytes → text | `b.decode()` / `b.decode("utf-8", "replace")` |
| Handle bad input | `except UnicodeDecodeError` or `errors="replace"` |

That completes Part 4. Next, the container types text so often flows into and out
of - starting with [Lists](/docs/0501-lists).
