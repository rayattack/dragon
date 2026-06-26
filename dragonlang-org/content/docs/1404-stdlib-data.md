# Data Formats

Most programs spend a surprising amount of their time turning bytes into
structure and structure back into bytes: parsing a config file, reading a
JSON request body, writing a CSV report, packing a binary wire frame,
encoding a token. This chapter covers the standard-library modules that do
that work:

- **`json`** - encode and decode JSON.
- **`csv`** - split and join comma-separated rows.
- **`tomllib`** - read TOML configuration (read-only).
- **`configparser`** - read and write INI files.
- **`base64`** - RFC 4648 Base64, standard and URL-safe.
- **`binascii`** - hex encoding and CRC32.
- **`struct`** - pack and unpack primitive values to and from `bytes`.
- **`uuid`** - generate and validate UUIDs.

Every one of these is written in Dragon (with a few thin `extern "C"`
bridges for number conversion and float bit-reinterpretation). Because they
are ordinary typed Dragon source, the surfaces are concrete and they do
**not** always match Python name-for-name. Each section documents what the
code *actually exposes* and ends with a "Differs from Python" note where the
shape is not the one you would reach for out of habit. Read it before you
port code over from CPython.

A module is used by importing it and qualifying the call (`import json` then
`json.dumps(...)`), or by pulling the names in directly
(`from csv import parse_row`).

## json

JSON is the module with the widest surface, because Dragon's type system
gives it two distinct decoding strategies and you should pick deliberately
between them.

**Decoding a whole document.** `loads(s: str) -> Any` parses a JSON string
into a boxed `Any` tree: objects become `dict[str, Any]`, arrays become
`list[Any]`, and scalars become tagged `Any` values. This is the
Python-parity convenience tier - every node is one box, so it is fine off
hot paths.

The catch is that you cannot index *straight* into the boxed `Any` and read
a value out of it; the box does not auto-unbox under a subscript. The
working pattern is to decode the top-level object with `loads_obj`, which is
typed as `dict[str, Any]`, and let each value unbox as you assign it to a
typed local:

```dragon
import json

const obj: dict[str, Any] = json.loads_obj("{\"name\": \"ada\", \"age\": 36}")
const name: str = obj["name"]    # the Any value unboxes into a str here
const age: int = obj["age"]      # ...and into an int here
print(name)                      # ada
print(age)                       # 36
```

`loads_obj(s: str) -> dict[str, Any]` raises `ValueError` if the top-level
value is not an object. When a value's type is not known statically, narrow
it with `isinstance` before binding:

```dragon
import json

const obj: dict[str, Any] = json.loads_obj("{\"port\": 8080}")
const v: Any = obj["port"]
if isinstance(v, int) {
    const n: int = v
    print(n)               # 8080
}
print("port" in obj)       # True
```

**Decoding a value of known type.** When you already know the shape, the
typed decoders skip the boxing entirely and hand you a native value. They
each parse a single JSON value of the stated type:

```dragon
import json

print(json.loads_int("42"))             # 42
print(json.loads_float("3.14"))         # 3.14
print(json.loads_str("\"hello\""))      # hello
print(json.loads_list_int("[1, 2, 3]")) # [1, 2, 3]
```

The full set is `loads_str`, `loads_int`, `loads_float`, `loads_bool`,
`loads_is_null`, and the homogeneous-array forms `loads_list_str`,
`loads_list_int`, `loads_list_float`, `loads_list_bool`.

**Encoding.** `dumps(obj: Any) -> str` serializes any scalar or
arbitrarily-nested homogeneous list/dict by dispatching on the value's
runtime tag:

```dragon
import json

const scores: dict[str, int] = {"ada": 90, "bob": 75}
print(json.dumps(scores))    # {"ada": 90, "bob": 75}
print(json.dumps([1, 2, 3])) # [1, 2, 3]
print(json.dumps(true))      # true
```

There are also typed encoders - `dumps_str`, `dumps_int`, `dumps_float`,
`dumps_bool`, and the `dumps_list_*` family - for when you want to avoid the
tag dispatch on a hot path.

> **Differs from Python.** `json.loads` returns an `Any` tree, not a
> `dict`/`list`, and a boxed `Any` does not unbox under a direct subscript -
> reach for `loads_obj` (typed `dict[str, Any]`) or the `loads_*` scalar
> decoders, which is where the typed, zero-box path lives. There is no
> `object_hook`, `indent=`, or `sort_keys=` keyword; `dumps` emits compact
> output.

## csv

The `csv` module works on whole lines and whole rows, not the file-object
reader/writer iterators Python uses. Every function takes the **delimiter**
as a required argument - there is no dialect object holding it for you.

`parse_row(line, delimiter) -> list[str]` splits one line into fields,
honoring quoted fields (which may contain the delimiter) and doubled-quote
escapes:

```dragon
import csv

const row: list[str] = csv.parse_row("ada,36,\"Lovelace, A.\"", ",")
print(row)     # ['ada', '36', 'Lovelace, A.']
```

`parse_rows(text, delimiter) -> list[list[str]]` does the same for a whole
multi-line document, and `format_row` / `format_rows` go the other way,
quoting a field only when it contains the delimiter, a quote, or a newline:

```dragon
import csv

const rows: list[list[str]] = csv.parse_rows("a,b\n1,2\n3,4", ",")
print(len(rows))      # 3
print(rows[1])        # ['1', '2']

const fields: list[str] = ["plain", "has,comma", "with\"quote"]
print(csv.format_row(fields, ","))   # plain,"has,comma","with""quote"
```

The delimiter is just a string, so tab-separated data is `parse_row(line,
"\t")`. For data you trust to have no quoting, `parse_row_simple` is a bare
`split`. The convenience helpers `field_count(line, delimiter)` and
`has_header(text, delimiter)` round out the module.

> **Differs from Python.** No `csv.reader` / `csv.writer` / `DictReader`,
> no dialect or `Sniffer` objects. You call `parse_row` / `format_row`
> directly and pass the delimiter every time.

## tomllib

`tomllib` is a read-only TOML parser, like Python 3.11's. The one call you
need is `loads(text: str) -> TomlDoc`. It returns a **`TomlDoc`**, not a
dict - you read values out of it with typed, dotted-key accessors, where a
key like `server.port` addresses `port` inside a `[server]` table:

```dragon
import tomllib

const src: str = "title = \"Dragon\"\n[server]\nport = 8080\ndebug = true\nratio = 0.75"
const doc: tomllib.TomlDoc = tomllib.loads(src)

print(doc.get("title"))              # Dragon
print(doc.get_int("server.port"))    # 8080
print(doc.get_bool("server.debug"))  # True
print(doc.get_float("server.ratio")) # 0.75
print(doc.has("server.missing"))     # False
print(doc.all_keys())                # ['title', 'server.port', 'server.debug', 'server.ratio']
```

The accessor methods are `get` (raw string), `get_int`, `get_float`,
`get_bool`, plus `has(key) -> bool` and `all_keys() -> list[str]`. A missing
key raises `ValueError`. The parser handles `[section]` headers, dotted
keys, strings, integers (with `_` separators), floats, booleans, and `#`
comments; it does **not** handle arrays of tables, inline tables,
multi-line strings, or datetime values.

> **Differs from Python.** `loads` returns a `TomlDoc` with typed
> `get_int` / `get_float` / `get_bool` accessors, not a plain `dict` of
> already-typed values, and there is no `load(fp)` file overload - read the
> text yourself and pass it in.

## configparser

`configparser` reads and writes the INI format through a `ConfigParser`
object. Construct one, feed it text with `read_string`, then query by
section and key:

```dragon
from configparser import ConfigParser

const cfg: ConfigParser = ConfigParser()
cfg.read_string("[db]\nhost = localhost\nport = 5432\npool = on")

print(cfg.sections())                # ['db']
print(cfg.get("db", "host"))         # localhost
print(cfg.get_int("db", "port"))     # 5432
print(cfg.get_bool("db", "pool"))    # True
print(cfg.has_option("db", "host"))  # True
```

The getters are `get`, `get_int`, `get_float`, and `get_bool` (which treats
`true`, `yes`, `on`, and `1` as true). Both `=` and `:` are accepted as
key/value separators, and `#` or `;` start a comment line. The class is also
mutable: `set(section, key, value)` and `add_section(name)` build a config
up, and `to_string()` serializes it back to INI text:

```dragon
from configparser import ConfigParser

const out: ConfigParser = ConfigParser()
out.set("app", "name", "dragon")
print(out.to_string())
# [app]
# name = dragon
```

`items(section) -> list[list[str]]` returns the `[key, value]` pairs of a
section for iteration.

> **Differs from Python.** Values are read with explicit typed getters
> (`get_int`, `get_bool`, …) rather than `getint` / `getboolean` methods,
> and there is no `[section]` subscript view, interpolation, or `DEFAULT`
> section. Input arrives via `read_string`, not `read(filename)`.

## base64

`base64` is RFC 4648, working in **`bytes`** at both ends to match Python.
`b64encode(data: bytes) -> bytes` encodes; `b64decode(data: bytes) -> bytes`
decodes. Since most text you want to encode starts life as a `str`, encode
it to UTF-8 first and decode the result back when you need to read it:

```dragon
import base64

const raw: bytes = "hello dragon".encode("utf-8")
const enc: bytes = base64.b64encode(raw)
print(enc.decode("ascii"))     # aGVsbG8gZHJhZ29u

const dec: bytes = base64.b64decode(enc)
print(dec.decode("utf-8"))     # hello dragon
```

The URL-safe variant swaps `+`/`/` for `-`/`_`, which is what JWT and other
URL-embedded tokens use:

```dragon
import base64

const u: bytes = base64.urlsafe_b64encode(bytes([251, 255, 191]))
print(u.decode("ascii"))       # -_-_
```

`standard_b64encode` / `standard_b64decode` are aliases for the default
pair, and `urlsafe_b64decode` reverses the URL-safe form. Decoding tolerates
missing padding (handy for JWS, which elides it).

> **Differs from Python.** The surface matches `base64`, but encode/decode
> are strictly `bytes`-in, `bytes`-out - there is no implicit `str`
> acceptance, so call `.encode()` / `.decode()` at the boundaries.

## binascii

`binascii` converts between binary and ASCII hex, and computes CRC32.
`hexlify(data: bytes) -> bytes` produces lowercase hex; `unhexlify` reverses
it and conveniently accepts either `bytes` **or** `str`:

```dragon
import binascii

const h: bytes = binascii.hexlify("Dragon".encode("utf-8"))
print(h.decode("ascii"))         # 447261676f6e

const back: bytes = binascii.unhexlify("447261676f6e")   # str input is fine
print(back.decode("utf-8"))      # Dragon
```

`b2a_hex` / `a2b_hex` are aliases for `hexlify` / `unhexlify`. `crc32(data:
bytes, value: int = 0) -> int` is the standard IEEE 802.3 CRC32 used by gzip
and PNG:

```dragon
import binascii

print(binascii.crc32("hello".encode("utf-8")))   # 907060870
```

> **Differs from Python.** The functions match (`hexlify`, `unhexlify`,
> `crc32`, `b2a_hex`, `a2b_hex`). `unhexlify` accepting a `str` directly is a
> small ergonomic addition over CPython, which wants bytes.

## struct

`struct` packs and unpacks primitive values to and from `bytes` using
CPython's format mini-language. The public surface is `pack(fmt, *args) ->
bytes`, `unpack(fmt, data) -> list`, and `calcsize(fmt) -> int`.

The format string opens with an optional byte-order char - `<` little-endian,
`>` or `!` big-endian (network), `=`/`@`/none native - followed by format
codes: `b`/`B` for int8, `h`/`H` for int16, `i`/`I`/`l`/`L` for int32,
`q`/`Q` for int64, `f` for float32, `d` for float64, `?` for bool, `s` for a
fixed-width string field, and `x` for a pad byte. Lowercase is signed,
uppercase unsigned.

```dragon
import struct

# Big-endian uint16, int32, float64.
const packed: bytes = struct.pack(">Hid", 8080, -1, 3.5)
print(len(packed))                # 14
print(struct.calcsize(">Hid"))    # 14

const vals: list[Any] = struct.unpack(">Hid", packed)
const port: int = vals[0]
const code: int = vals[1]
const ratio: float = vals[2]
print(port)                       # 8080
print(code)                       # -1
print(ratio)                      # 3.5
```

The float formats are the reason this module needs compiler support: a
value's raw IEEE-754 bytes are recovered with a register-level bitcast, no
numeric conversion. That makes the whole binary-protocol class - msgpack,
BSON, CBOR, the MySQL wire format, `.npy` - expressible in pure Dragon. A
malformed format or an argument that does not fit raises `struct.error`; note
that float64 is lowercase `d` (an uppercase `D` is not a valid code).

> **Differs from Python.** `unpack` returns a **`list[Any]`**, not a tuple -
> Dragon has no variadic-arity runtime tuple, so a list is the expressible
> analog; index access and iteration are identical. Bind each element to a
> typed local (`const port: int = vals[0]`) to use it.

## uuid

`uuid` generates version-4 (random) UUIDs and validates UUID strings.
`uuid4() -> str` returns the canonical 36-character form:

```dragon
import uuid

const u: str = uuid.uuid4()
print(len(u))                  # 36
print(uuid.uuid_is_valid(u))   # True
print(uuid.uuid_version(u))    # 4
print(len(uuid.uuid_hex(u)))   # 32  (dashes stripped)
```

The helpers are `uuid_hex` (strip dashes to 32 hex chars), `uuid_version`
(the version digit, 1-5, or -1), `uuid_is_valid` (full 8-4-4-4-12 format
check), and `uuid_compare(a, b) -> int` for canonical lexicographic ordering.

> **Differs from Python.** `uuid4()` returns a plain `str`, not a `UUID`
> object - there are no `.hex` / `.int` / `.version` attributes; the
> `uuid_hex` / `uuid_version` module functions take a string instead. The
> randomness comes from libc `rand()`, so it is **not** cryptographically
> secure; for tokens, reach for the crypto modules. Only `uuid4` is
> implemented (no `uuid1`/`uuid3`/`uuid5`).

## At a glance

| Task | Call |
|---|---|
| Decode a JSON object | `obj: dict[str, Any] = json.loads_obj(s)` |
| Decode a typed JSON scalar/array | `json.loads_int(s)`, `loads_str`, `loads_list_int`, … |
| Encode any value to JSON | `json.dumps(value) -> str` |
| Split a CSV line | `csv.parse_row(line, ",") -> list[str]` |
| Join CSV fields | `csv.format_row(fields, ",") -> str` |
| Read TOML | `doc = tomllib.loads(text)`; `doc.get_int("a.b")` |
| Read an INI file | `cfg.read_string(text)`; `cfg.get_int(sec, key)` |
| Build / write an INI | `cfg.set(sec, key, val)`; `cfg.to_string()` |
| Base64 encode / decode | `base64.b64encode(b)` / `b64decode(b)` (bytes ↔ bytes) |
| URL-safe Base64 | `base64.urlsafe_b64encode(b)` / `urlsafe_b64decode(b)` |
| Hex encode / decode | `binascii.hexlify(b)` / `unhexlify(s_or_b)` |
| CRC32 checksum | `binascii.crc32(b) -> int` |
| Pack / unpack binary | `struct.pack(fmt, *args)` / `unpack(fmt, data) -> list` |
| Binary field size | `struct.calcsize(fmt) -> int` |
| Generate a UUID | `uuid.uuid4() -> str` |
| Validate a UUID | `uuid.uuid_is_valid(s) -> bool` |

These modules sit at the seam between raw bytes and structured data, and
they all stay typed and compiled the whole way through - a JSON decode or a
`struct.pack` costs what the equivalent hand-written loop costs, with no
dynamic interpreter underneath. For the clocks and numbers your structured
data so often carries - timestamps, durations, sums, and averages - turn to
[Dates, Times, and Math](/docs/1405-stdlib-datetime-math).
