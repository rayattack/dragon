# Standard Library Index


Dragon ships a batteries-included standard library. Every module listed here is
**bundled with the compiler and statically linked into your binary** - there is
no package-manager step, no `pip install`, no runtime dependency to ship
alongside the executable. You write `import json` and the code is already
there.

The guiding principle is **Python parity**: where a module mirrors a CPython
one, the names, signatures, and semantics are meant to read identically. But
Dragon implements a deliberate **subset** - not the whole CPython stdlib, and
within each module not every function or flag. This index documents what
actually exists, grounded in each module's own description. When a module's
surface departs from Python (the `json` shape is the notable case), the
departure is called out.

Many modules are **pure Dragon** - no C dependency at all. Others wrap libc,
libm, or a bundled native library (PCRE2, SQLite, mbedTLS, zlib, zstd) that is
statically linked the same way. Either way, nothing is fetched at build time.

## Text and formatting

| Module | Purpose |
|--------|---------|
| `string` | String constants (`ascii_lowercase`, `digits`, …) and utilities, matching Python's `string` module. Pure Dragon. |
| `textwrap` | Text wrapping and filling - `TextWrapper` plus `wrap`/`fill`/`dedent`/`indent`/`shorten`. Pure Dragon; simplified vs CPython's regex splitter. |
| `re` | Regular expressions backed by bundled **PCRE2 10.44**. `Pattern` class plus `match`/`search`/`sub`/`findall`/`split`. |
| `fnmatch` | Unix filename glob matching (`*`, `?`, `[abc]`, `[!abc]`). Pure Dragon. |
| `html` | HTML escaping/unescaping, tag stripping, entity codecs; also home to the `HTML`/`CSS`/`XML` `template[X]` content types. Pure Dragon. |
| `reprlib` | Size-limited `repr()` for logging and pprint. Typed `repr_*` entry points (static typing can't dispatch on runtime type). |
| `difflib` | Sequence similarity - longest common blocks and ratios over strings, a subset of Python's `difflib`. Pure Dragon. |
| `shlex` | Shell-like lexical splitting with POSIX quoting (`split`/`quote`). Pure Dragon. |
| `template` | The `Template` protocol base that `template[X] { … }` content types subclass. See [Templates](/docs/1201-templates). |

## Data and serialization

| Module | Purpose |
|--------|---------|
| `json` | JSON encode/decode. **Non-Python shape:** the API is *monomorphized* - typed functions like `dumps_int`, `dumps_list_str`, `loads_float`, `loads_list_str`, plus `detect_type`, rather than a single generic `dumps`/`loads`. Handles flat values and homogeneous arrays. Also hosts the `JSON` `template` content type. |
| `csv` | CSV reading/writing with quoted fields, escaped quotes, configurable delimiters. Pure Dragon. |
| `configparser` | INI-file parser - sections, `=`/`:` key-values, `#`/`;` comments, whitespace stripping. Pure Dragon. |
| `tomllib` | Read-only TOML parser matching Python 3.11+ `tomllib` (practical subset; no arrays-of-tables, inline tables, multi-line strings, or datetimes). |
| `drs` | Parser for Dragon Script (`.drs`) config files; returns native `dict[str, Any]` / `list[Any]`. |
| `base64` | RFC 4648 Base64 encode/decode plus URL-safe variants; bytes-oriented to match Python. |
| `binascii` | Binary↔ASCII conversion - `hexlify`/`unhexlify` (a.k.a. `b2a_hex`/`a2b_hex`) and CRC32 over bytes. |
| `struct` | Pack/unpack primitives to/from bytes with the CPython format mini-language (`<`/`>`/`!`/`=`, `b`/`h`/`i`/`q`/`f`/`d`/`s`, …). |
| `quopri` | Quoted-printable (RFC 1521) encode/decode over bytes. Pure Dragon. |
| `operator` | Python operators as plain functions for higher-order use; static typing forces monomorphic (`int`-default, suffixed float/str) variants. |
| `functools` | Higher-order helpers (`reduce`, …) matching Python's `functools`; monomorphic subset forced by static typing. Pure Dragon. |
| `itertools` | Iterator building blocks (`accumulate`/`chain`/`repeat`/`islice`/`pairwise`/`take`), a monomorphic subset of Python's `itertools`. Pure Dragon. |
| `enum` | `Enum`/`IntEnum`/`StrEnum` base classes recognized by the compiler, which lowers the deriving class at compile time (no runtime metaclass). Import-gated, not builtins. |

## Math and numbers

| Module | Purpose |
|--------|---------|
| `math` | Mathematical functions over `float` wrapping libm, with module-level constants (`pi`, …). |
| `statistics` | Common statistics over lists of floats/ints - a subset of Python's `statistics`. Pure Dragon (only libm `sqrt`). |
| `fractions` | Exact rational arithmetic via a `Fraction` class, always stored reduced with sign on the numerator. Pure Dragon. |
| `random` | Pseudo-random floats, ints, and sequence ops over libc `rand`/`srand`. **Not** cryptographically secure. |
| `bisect` | Binary search for maintaining sorted `list[int]` sequences. Pure Dragon. |
| `heapq` | Min-heap (priority queue) operations on `list[int]`. Pure Dragon. |
| `graphlib` | Topological sort via Kahn's algorithm (`TopologicalSorter`). Pure Dragon. |
| `colorsys` | Conversions between RGB and the HSV/HLS/YIQ colour systems, matching Python's `colorsys`. Pure Dragon. |

## Date and time

| Module | Purpose |
|--------|---------|
| `time` | Current time, process/monotonic clocks, and sleep over POSIX time functions. |
| `datetime` | `timedelta`/`date`/`time`/`datetime` with arithmetic, comparison, and ISO-8601 formatting. UTC-only in this version. |
| `calendar` | Pure date math and formatting (Monday=0 … Sunday=6; January=1 … December=12). No C dependency. |
| `sched` | General-purpose event scheduler (`scheduler`), a subset of Python's `sched`. Pure Dragon. |
| `timeit` | Measure execution time of small snippets - `default_timer`/`timeit` over `time.perf_counter`. |

## Operating system and filesystem

| Module | Purpose |
|--------|---------|
| `os` | OS interface over POSIX/libc - filesystem ops, environment access, process management, path utilities. |
| `os.path` | Path utilities (`join`, `basename`, `dirname`, `splitext`, `isabs`, …), the Python `os.path` surface. |
| `pathlib` | Object-oriented paths - a `Path` class wrapping `os.path` with attribute access and `/` joining. POSIX separators only. |
| `glob` | Unix-style pathname expansion returning lists of matching paths. |
| `shutil` | High-level file operations - `copyfile`/`copy`/`copytree`/… - a common subset of Python's `shutil`. |
| `stat` | Interpret `os.stat()` `st_mode` bits - type masks, permission helpers (`S_ISDIR`, …); Python `stat` parity. Pure Dragon. |
| `filecmp` | Compare files and directories (`cmp`, shallow/deep), matching Python's `filecmp`. |
| `fileinput` | Iterate over lines from many input streams as one stream, matching Python's `fileinput`. |
| `linecache` | Cache lines from text files for line-number lookup, matching Python's `linecache`. |
| `mimetypes` | Guess a file's MIME type from its name/extension. Pure Dragon. |
| `io` | File I/O - a `File` class with context-manager support over C stdio. |
| `tempfile` | Temporary file and directory creation over POSIX/libc. |
| `getpass` | Portable no-echo password input and current-user lookup over a native terminal shim plus libc. |
| `subprocess` | Spawn child processes with pipe capture - a Python-parity subset over a native spawn plus a `poll(2)` pump. |
| `signal` | POSIX signal numbers plus delivery helpers, a Python-parity subset. |
| `platform` | Underlying platform identifying data - `system`/`node`/`release`/`version`/…, matching Python's `platform`. |
| `gettext` | Minimal `.po` message-catalog reader for translations, matching Python's `gettext`. Pure Dragon. |
| `netrc` | Parse the `.netrc` credentials file, matching Python's `netrc`. Pure Dragon. |
| `sys` | Process exit, platform info, and system limits. |
| `errno` | Symbolic POSIX error-number constants (Linux/glibc canonicals). Pure Dragon. |
| `syslog` | POSIX `syslog(3)` bindings plus per-severity convenience helpers. Linux/macOS only. |
| `logging` | A `Logger` class plus `debug`/`info`/`warning`/`error`/`critical`; stderr, fixed format, Python numeric levels. |
| `warnings` | Non-fatal warnings - `warn`/`filterwarnings`/`simplefilter`; category names are strings matching Python's class names. |
| `argparse` | Command-line argument parsing - a subset of Python's `argparse` (`ArgumentParser`, `add_argument`, `parse_args`). |

## Networking

| Module | Purpose |
|--------|---------|
| `socket` | TCP/UDP networking - BSD socket wrappers using runtime helpers for sockaddr packing. No external dependency. |
| `ipaddress` | IPv4/IPv6 address and network helpers - parse, round-trip, equality, CIDR membership; Python parity for the common subset. |
| `http` | HTTP package root re-exporting `HTTPServer` and the routing types. |
| `http.server` | Pure-Dragon HTTP server - `Router`, `Request`, `Response`, `Context`; runtime C only for socket I/O and HTTP parsing. |
| `http.client` | HTTP/1.1 client (`HTTPConnection`/`getresponse`/`read`) over `socket.TcpStream`. Plain HTTP. |
| `http.connection` | Connection lifecycle for the server - `ConnReader`/`RequestReader` framing HTTP/1.1 requests off raw socket and TLS streams. Pure Dragon. |
| `http.message` | HTTP request/response message types shared by the server. Pure Dragon. |
| `http.cookies` | Parse and render HTTP cookie headers (common subset). Pure Dragon. |
| `http.cookiejar` | RFC 6265 cookie jar (Python parity, common subset). Pure Dragon. |
| `http.sessions` | Server-side and signed-cookie session crypto plus CSRF token helper (W5/W7). Pure Dragon over `hmac`/`secrets`. |
| `http.multipart` | `multipart/form-data` body parsing (RFC 7578) into binary-safe `Part`s. Pure Dragon. |
| `http.websocket` | RFC 6455 WebSocket handshake plus frame parse/build codec over the binary socket runtime. Pure Dragon. |
| `urllib` | Package root for the `urllib` submodules. |
| `urllib.parse` | URL parsing and percent-encoding (RFC 3986) - `urlsplit`/`urljoin`/`quote`/`parse_qs`/`urlencode`; hosts the `URL` `template` content type. |
| `urllib.request` | High-level HTTP convenience (`urlopen`, `Request`) over `http.client`. |
| `urllib.response` | File-like wrappers returned by `urlopen`, matching Python's `urllib.response`. Pure Dragon. |
| `urllib.error` | Exception classes raised by `urllib` (Python parity). Pure Dragon. |
| `urllib.robotparser` | Parse `robots.txt` access rules, matching Python's `urllib.robotparser`. Pure Dragon. |

## Cryptography and security

| Module | Purpose |
|--------|---------|
| `hashlib` | Python-compatible hashing (SHA-256, SHA-1, MD5) with incremental `update`/`copy`; bytes-in/bytes-out plus `pbkdf2_hmac`. |
| `hmac` | RFC 2104 keyed-hash MAC backed by the mbedTLS runtime engine. |
| `crypto` | String-in / hex-out digest wrappers, and Dragon's home for the **superset** crypto surface Python never exposed - asymmetric sign/verify and AEAD ciphers, each KAT-verified on mbedTLS. |
| `secrets` | Cryptographically strong randomness over the OS CSPRNG - `token_urlsafe`, `token_hex`, `compare_digest`. |
| `argon2id` | Argon2id password hashing (RFC 9106) over the memory-hard runtime core - `hash`/`verify`, raw and encoded forms. **Beyond Python:** not in CPython's stdlib (Python needs `argon2-cffi`). |
| `totp` | RFC 6238 TOTP and RFC 4226 HOTP one-time passwords, pure Dragon (zero FFI). **Beyond Python:** not in CPython's stdlib. |
| `ssl` | TLS/SSL with a CPython-parity surface (`SSLContext`, `wrap_socket`, `create_default_context`, `CERT_*`/`PROTOCOL_*`) over statically-linked mbedTLS; modern-only policy, not user-weakenable. |
| `uuid` | UUID v4 (random) generation and string utilities over libc `rand` - **not** cryptographically secure. |
| `merkle` | RFC 6962 (Certificate Transparency) Merkle tree with `0x00`/`0x01` domain separation - `O(log n)` inclusion and consistency proofs. Pure Dragon. The transparency-log primitive behind the egg registry. |

## Concurrency

See [Concurrency](/docs/1101-green-threads) for the three-tier model.

| Module | Purpose |
|--------|---------|
| `threading` | OS-thread synchronization primitives - `Lock`, `RWLock`, `Semaphore`, `Barrier`, `Condition`, `Event`; all context-manager aware (pthread wrappers). |
| `collections` | Package root re-exporting the concurrent collections. |
| `collections.concurrent` | Thread-safe collection wrappers - Dragon classes over the builtin `SyncList`/`SyncDict` runtime types, subclassable and extendable. |

## Databases

See [Databases](/docs/1301-databases) for the full guide.

| Module | Purpose |
|--------|---------|
| `database` | Database package root - queries are `template[SQL]` values (never strings); `all`/`one`/`val`/`run` reject bare `str`, rows are `dict[str, Any]`. Optional `[T]` on the fetch verbs (`all[Customer]`, `val[int]`) maps rows to a class or scalar. Defines the `open(dsn)` dispatcher. |
| `database.base` | Shared backend-agnostic core - the `SQL` content type, the error hierarchy, `Results`. |
| `database.sqlite` | SQLite backend over the bundled amalgamation (no external dependency). |
| `database.postgres` | Postgres backend speaking the v3 wire protocol directly (SCRAM-SHA-256 auth); no libpq, no C dependency. |
| `database.mysql` | MySQL backend speaking the client/server protocol directly (`mysql_native_password` + `caching_sha2_password`); no libmysqlclient. |
| `sqlite` | Low-level SQLite3 C-API bindings over the bundled amalgamation (the raw layer beneath `database.sqlite`). |

## Testing and development

See [Testing](/docs/1901-unittest) for the full guide.

| Module | Purpose |
|--------|---------|
| `unittest` | Python-parity test framework - `TestCase` subclasses driven by method reflection and deep `Any` equality. |

## Compression and archives

| Module | Purpose |
|--------|---------|
| `gzip` | gzip compress/decompress mirroring CPython's `gzip`; zlib level scale (0-9), default 9. |
| `zstandard` | Zstandard (zstd) compress/decompress - Dragon-specific (not in CPython); mirrors `python-zstandard`'s high-level API. Default level 3. |
| `zipfile` | PKWARE ZIP archive read/write (stored + deflate), matching CPython's `zipfile`. |
| `tarfile` | POSIX USTAR tar read/write composing with gzip/zstd for `.tar`, `.tar.gz`, `.tar.zst`. Pure Dragon header format. |

## Desktop UI

See [Desktop Applications](/docs/1801-desktop-overview) for the full guide.

| Module | Purpose |
|--------|---------|
| `ui` | Dragon's desktop UI toolkit (the design spec) - the `App` lifecycle/main loop and reactive `Signal` primitives. Views are `template[HTML]`; the renderer is the OS webview. |
| `ui.desktop` | The platform `Window` and native shell bindings beneath `ui` (webview host). |

## A note on coverage

This index is a map, not a contract for completeness. Each module aims to make
the **commonly used** part of its Python counterpart feel native, and to do so
at native-code speed because everything compiles ahead of time and links
statically. Where Dragon goes beyond Python - `zstandard`, the `database`
`template[SQL]` layer, the superset `crypto` primitives, and bundled
`argon2id`/`totp` that Python pushes out to third-party packages - it does so to
fill a real gap, not to diverge for its own sake. When you need a function that
isn't here yet, that is a known boundary of the subset, not a bug in your
program.
