# The Standard Library: Overview

A language is only as useful as the day-one things you can do without
writing them yourself: read a file, parse some JSON, hash a password,
open a socket, sort a list, format a date. Dragon's standard library is
where those things live. This chapter is the map - what the library is,
how it's shaped, and which chapter to turn to for each module. The
chapters that follow are the territory.

## What "standard library" means in Dragon

Three properties distinguish Dragon's stdlib from the libraries you may
be used to.

**It ships with the compiler, statically linked.** There is no
`pip install`, no `npm`, no lockfile, no runtime dependency to ship next
to your binary. You write `import json` and the code is already on disk,
compiled into your executable alongside your own. The bundled native
libraries a few modules lean on - PCRE2 for `re`, SQLite for the
database layer, mbedTLS for TLS and hashing, zlib and zstd for
compression - are linked the same way. Nothing is fetched at build time.

**It's written mostly in Dragon itself.** The library is *dogfooded*:
the rule across the project is that anything expressible in Dragon is
written in Dragon, and C++ is reserved for what genuinely can't be -
raw syscalls, codegen primitives, refcount intrinsics, runtime data
structures. So the great majority of stdlib modules (`textwrap`,
`csv`, `json`, `datetime`, `urllib.parse`, the entire `http` server, the
Postgres and MySQL drivers, and many more) are ordinary `.dr` source you
can read, and they compile to native code at exactly the speed your own
code does.

**It matches Python's names where it can - but it is not Python.**
Dragon is *inspired by* Python, not a superset of it: the CPython C API
and the dynamic ecosystem (numpy, pandas, Flask) do not work and never
will. Within the subset Dragon does implement, the goal is that `import`
names, function signatures, and semantics read identically to CPython -
so your muscle memory transfers - *except* where matching Python would
cost speed or force a workaround. Speed wins those ties. The result is a
deliberate **subset**: not the whole CPython stdlib, and within each
module not every function or flag.

Because Dragon is statically typed and compiled ahead of time, a handful
of modules wear that subset on their sleeve. The compiler can't dispatch
on a value's runtime type the way CPython does, so APIs that are one
generic function in Python sometimes appear as a small family of
typed functions in Dragon. You'll meet these as you go - the most
prominent are:

- **`json`** has no single generic `loads`/`dumps`. `loads` returns
  `Any` (a boxed value you narrow with `isinstance`), and the typed
  encoders are monomorphized (`dumps_int`, `dumps_list_str`, …).
- **`re.match`/`re.search`** return an `int` index (or `-1`), not a
  `Match` object; captures come from a `Pattern` API.
- **`csv`** exposes `parse_row`/`format_row` with an explicit delimiter,
  not Python's `reader`/`writer` objects.
- **`tomllib.loads`** returns a typed `TomlDoc` with dotted-key
  accessors, not a plain `dict`.

These divergences are documented module-by-module in the chapters that
follow, and each chapter leads with what the code *actually* exposes -
never with what Python would do.

## Importing a module

Imports work the way they do in Python. Bring in the whole module and
qualify each call:

```dragon
import math

print(math.pi)   # 3.141592653589793
```

Or pull specific names into your namespace with `from`:

```dragon
from textwrap import shorten

s: str = shorten("Dragon ships its standard library with the compiler", width=24)
print(s)   # Dragon ships its [...]
```

Packages use dotted paths. A submodule is imported by its full dotted
name, and `from … import …` reaches into it:

```dragon
from os.path import join
from urllib.parse import urlencode

print(join("/etc", "hosts"))   # /etc/hosts
```

That's the whole import story - `import X`, `from X import f`, and
dotted names for submodules. There is no `import *`, and no install step
stands between the line you write and the code it names.

## The module index

The rest of Part 14 walks the library by category. Each row below links
to the chapter that covers the module in depth. (For a single
alphabetical list of everything, see the
[Standard Library Index appendix](/docs/2004-appendix-stdlib).)

### Files and the filesystem

How a program reads, writes, and reasons about files, paths, and the
process around it.

| Module | Purpose | Chapter |
|--------|---------|---------|
| `io` | Read and write files - a `File` class with `with`-block support | [Files](/docs/1402-stdlib-io) |
| `os` | The OS interface: directory listings, environment, processes | [Files](/docs/1402-stdlib-io) |
| `os.path` | Pure path-string surgery: `join`, `basename`, `splitext`, existence checks | [Files](/docs/1402-stdlib-io) |
| `pathlib` | Object-oriented paths - a `Path` class with `/` joining | [Files](/docs/1402-stdlib-io) |
| `glob` | Unix-style pathname expansion to a list of matches | [Files](/docs/1402-stdlib-io) |
| `shutil` | High-level file ops: `copyfile`, `copytree`, … | [Files](/docs/1402-stdlib-io) |
| `tempfile` | Temporary files and directories | [Files](/docs/1402-stdlib-io) |
| `stat` | Interpret `os.stat` mode bits (`S_ISDIR`, …) | [Files](/docs/1402-stdlib-io) |
| `sys` | Process exit, the command line via `argv()`, system limits | [Files](/docs/1402-stdlib-io) |

### Text

Tools for building, wrapping, matching, and pulling apart strings.

| Module | Purpose | Chapter |
|--------|---------|---------|
| `string` | String constants (`ascii_lowercase`, `digits`, …) and helpers | [Text](/docs/1403-stdlib-text) |
| `textwrap` | Wrap, fill, dedent, indent, and `shorten` text | [Text](/docs/1403-stdlib-text) |
| `re` | Regular expressions, backed by bundled PCRE2 | [Text](/docs/1403-stdlib-text) |
| `fnmatch` | Glob-style filename matching (`*`, `?`, `[abc]`) | [Text](/docs/1403-stdlib-text) |
| `shlex` | Shell-like lexical splitting with POSIX quoting | [Text](/docs/1403-stdlib-text) |
| `difflib` | Sequence similarity - common blocks and ratios | [Text](/docs/1403-stdlib-text) |
| `html` | HTML escape/unescape, tag stripping, entity codecs | [Text](/docs/1403-stdlib-text) |

### Data formats

Reading and writing structured data: JSON, CSV, INI, TOML, and binary.

| Module | Purpose | Chapter |
|--------|---------|---------|
| `json` | JSON encode/decode (`loads` → `Any`; typed `dumps_*` encoders) | [Data Formats](/docs/1404-stdlib-data) |
| `csv` | CSV `parse_row`/`format_row` with explicit delimiter | [Data Formats](/docs/1404-stdlib-data) |
| `configparser` | INI files - sections, key-values, comments | [Data Formats](/docs/1404-stdlib-data) |
| `tomllib` | Read-only TOML, returning a typed `TomlDoc` | [Data Formats](/docs/1404-stdlib-data) |
| `base64` | RFC 4648 Base64, plus URL-safe variants | [Data Formats](/docs/1404-stdlib-data) |
| `binascii` | `hexlify`/`unhexlify` and CRC32 over bytes | [Data Formats](/docs/1404-stdlib-data) |
| `struct` | Pack/unpack primitives to/from bytes | [Data Formats](/docs/1404-stdlib-data) |
| `gzip` | gzip compress/decompress | [Data Formats](/docs/1404-stdlib-data) |
| `zipfile` | PKWARE ZIP archive read/write | [Data Formats](/docs/1404-stdlib-data) |
| `tarfile` | POSIX USTAR tar, composing with gzip/zstd | [Data Formats](/docs/1404-stdlib-data) |
| `zstandard` | Zstandard compress/decompress (beyond Python) | [Data Formats](/docs/1404-stdlib-data) |

### Dates, times, and math

Clocks, calendars, numeric functions, and number theory.

| Module | Purpose | Chapter |
|--------|---------|---------|
| `time` | Current time, monotonic clocks, `sleep` | [Dates & Math](/docs/1405-stdlib-datetime-math) |
| `datetime` | `timedelta`/`date`/`time`/`datetime` (UTC-only) | [Dates & Math](/docs/1405-stdlib-datetime-math) |
| `calendar` | Pure date math and formatting | [Dates & Math](/docs/1405-stdlib-datetime-math) |
| `math` | libm functions and constants (`pi`, `e`, `tau`) | [Dates & Math](/docs/1405-stdlib-datetime-math) |
| `statistics` | Mean, median, variance, … over lists | [Dates & Math](/docs/1405-stdlib-datetime-math) |
| `fractions` | Exact rational arithmetic via `Fraction` | [Dates & Math](/docs/1405-stdlib-datetime-math) |
| `random` | Pseudo-random floats, ints, sequence ops (not secure) | [Dates & Math](/docs/1405-stdlib-datetime-math) |

### Functional tools and collections

Iterator building blocks, higher-order helpers, and ordered/priority structures.

| Module | Purpose | Chapter |
|--------|---------|---------|
| `itertools` | `accumulate`, `chain`, `islice`, `pairwise`, … | [Functional Tools](/docs/1406-stdlib-functional) |
| `functools` | `reduce` and friends (monomorphic subset) | [Functional Tools](/docs/1406-stdlib-functional) |
| `operator` | Python operators as plain functions | [Functional Tools](/docs/1406-stdlib-functional) |
| `bisect` | Binary search over a sorted `list[int]` | [Functional Tools](/docs/1406-stdlib-functional) |
| `heapq` | Min-heap / priority-queue ops on `list[int]` | [Functional Tools](/docs/1406-stdlib-functional) |
| `enum` | `Enum`/`IntEnum`/`StrEnum` base classes | [Functional Tools](/docs/1406-stdlib-functional) |

### Crypto and hashing

Digests, MACs, secure randomness, password hashing, and TLS.

| Module | Purpose | Chapter |
|--------|---------|---------|
| `hashlib` | SHA-256/SHA-1/MD5 with `update`/`copy`, plus `pbkdf2_hmac` | [Crypto](/docs/1407-stdlib-crypto) |
| `hmac` | RFC 2104 keyed-hash MAC (mbedTLS) | [Crypto](/docs/1407-stdlib-crypto) |
| `crypto` | Superset surface: asymmetric sign/verify and AEAD ciphers | [Crypto](/docs/1407-stdlib-crypto) |
| `secrets` | OS-CSPRNG randomness - `token_urlsafe`, `compare_digest` | [Crypto](/docs/1407-stdlib-crypto) |
| `argon2id` | Argon2id password hashing (beyond Python's stdlib) | [Crypto](/docs/1407-stdlib-crypto) |
| `totp` | RFC 6238/4226 TOTP/HOTP (beyond Python's stdlib) | [Crypto](/docs/1407-stdlib-crypto) |
| `ssl` | TLS over statically-linked mbedTLS (modern-only policy) | [Crypto](/docs/1407-stdlib-crypto) |
| `uuid` | UUID v4 generation and string utilities (not secure) | [Crypto](/docs/1407-stdlib-crypto) |

### Networking

Sockets, addresses, and the HTTP and URL stacks.

| Module | Purpose | Chapter |
|--------|---------|---------|
| `socket` | TCP/UDP BSD-socket wrappers | [Networking](/docs/1408-stdlib-networking) |
| `ipaddress` | IPv4/IPv6 parse, equality, CIDR membership | [Networking](/docs/1408-stdlib-networking) |
| `http.server` | Pure-Dragon HTTP server (`Router`, `Request`, `Response`) | [Networking](/docs/1408-stdlib-networking) |
| `http.client` | HTTP/1.1 client over `socket.TcpStream` | [Networking](/docs/1408-stdlib-networking) |
| `http.cookies` | Parse and render cookie headers | [Networking](/docs/1408-stdlib-networking) |
| `urllib.parse` | URL parsing and percent-encoding (`urlsplit`, `urlencode`, …) | [Networking](/docs/1408-stdlib-networking) |
| `urllib.request` | High-level `urlopen` over `http.client` | [Networking](/docs/1408-stdlib-networking) |

### Processes and the OS

Spawning children, signals, logging, arguments, and platform facts.

| Module | Purpose | Chapter |
|--------|---------|---------|
| `subprocess` | Spawn child processes with pipe capture | [Processes & OS](/docs/1409-stdlib-processes) |
| `signal` | POSIX signal numbers and delivery helpers | [Processes & OS](/docs/1409-stdlib-processes) |
| `argparse` | Command-line parsing (`ArgumentParser`, `add_argument`) | [Processes & OS](/docs/1409-stdlib-processes) |
| `logging` | A `Logger` plus `debug`/`info`/`warning`/`error`/`critical` | [Processes & OS](/docs/1409-stdlib-processes) |
| `platform` | `system`/`node`/`release`/`version`/… | [Processes & OS](/docs/1409-stdlib-processes) |
| `getpass` | No-echo password input and current-user lookup | [Processes & OS](/docs/1409-stdlib-processes) |

A few areas live outside Part 14 because they have their own home in the
book: the concurrency primitives (`threading`, `collections.concurrent`)
are covered in [Concurrency](/docs/1101-green-threads), the `database`
package in [Databases](/docs/1301-databases), and `unittest` in
[Testing](/docs/1901-unittest).

## At a glance

| If you need to… | Reach for | Covered in |
|-----------------|-----------|------------|
| Read or write a file | `io`, `os.path` | [Files](/docs/1402-stdlib-io) |
| Match or reshape text | `re`, `textwrap`, `string` | [Text](/docs/1403-stdlib-text) |
| Parse JSON / CSV / TOML | `json`, `csv`, `tomllib` | [Data Formats](/docs/1404-stdlib-data) |
| Do math or handle dates | `math`, `datetime`, `statistics` | [Dates & Math](/docs/1405-stdlib-datetime-math) |
| Chain or reduce iterables | `itertools`, `functools` | [Functional Tools](/docs/1406-stdlib-functional) |
| Hash a password or sign data | `hashlib`, `argon2id`, `crypto` | [Crypto](/docs/1407-stdlib-crypto) |
| Open a socket or call HTTP | `socket`, `http.client`, `urllib.parse` | [Networking](/docs/1408-stdlib-networking) |
| Spawn a process or parse args | `subprocess`, `argparse` | [Processes & OS](/docs/1409-stdlib-processes) |
| Look something up alphabetically | - | [Stdlib Index](/docs/2004-appendix-stdlib) |

With the shape of the library in hand, the natural place to start is the
one every program eventually reaches for:
[Files and the Filesystem](/docs/1402-stdlib-io).
