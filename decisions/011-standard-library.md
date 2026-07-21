# Decision 011: Standard Library - Phase 1

Done. All five Phase-1 modules shipped. Ongoing stdlib coverage and dogfooding rollout tracked elsewhere (which absorbed the former follow-up doc).

Five stdlib modules for Dragon: `json`, `datetime`, `math` (complete existing), `os` (extend existing), and `sys`. No new language features required - built entirely on existing runtime + StdlibRegistry infrastructure.

Dragon compiles and runs but the stdlib was basically empty beyond string/list/dict methods and basic I/O when I wrote this. Without `json` you can't talk to web APIs. Without `datetime` you can't handle timestamps. Without `sys` you can't read argv or exit cleanly. Without these five you can't really call Dragon a useful compiled language yet, just a fast toy.

## Module Designs

### 1. `json`

**Import:** `from json import dumps, loads` or `import json`

**Functions:**

| Function | Signature | Description |
|----------|-----------|-------------|
| `json.dumps(obj)` | `(Any) -> str` | Serialize Dragon value to JSON string |
| `json.dumps(obj, indent)` | `(Any, int) -> str` | Pretty-printed JSON |
| `json.loads(s)` | `(str) -> Any` | Parse JSON string to Dragon value |

**Type mapping:**

| JSON type | Dragon type |
|-----------|-------------|
| `number` (int) | `int` |
| `number` (float) | `float` |
| `string` | `str` |
| `boolean` | `bool` |
| `null` | `None` |
| `array` | `list` |
| `object` | `dict[str, Any]` |

**Implementation:** Runtime C functions `dragon_json_dumps(tagged_value) -> char*` and
`dragon_json_loads(char*) -> tagged_value`. The tagged value uses Dragon's existing
`VarKind` enum for type dispatch. For custom classes, `json.dumps` calls `__str__` if
defined, or `vars(obj)` to produce a dict from struct fields.

**Estimted size:** ~400 LOC in runtime (recursive descent JSON parser + serializer).

### 2. `datetime`

**Import:** `from datetime import datetime, timedelta, date, time`

**Classes:**

```dragon
class datetime {
 // Fields
 year: int
 month: int
 day: int
 hour: int
 minute: int
 second: int
 microsecond: int

 // Constructors
 self(year: int, month: int, day: int,
 hour: int = 0, minute: int = 0, second: int = 0)

 // Class methods
 static def now -> datetime
 static def utcnow -> datetime
 static def fromtimestamp(ts: float) -> datetime
 static def fromisoformat(s: str) -> datetime

 // Instance methods
 def timestamp -> float
 def isoformat -> str
 def strftime(fmt: str) -> str

 // Operators
 def __sub__(other: datetime) -> timedelta
 def __add__(delta: timedelta) -> datetime
 def __eq__(other: datetime) -> bool
 def __lt__(other: datetime) -> bool
 def __str__ -> str
 def __repr__ -> str
}

class timedelta {
 days: int
 seconds: int
 microseconds: int

 self(days: int = 0, seconds: int = 0, microseconds: int = 0)
 def total_seconds -> float
 def __add__(other: timedelta) -> timedelta
 def __sub__(other: timedelta) -> timedelta
 def __mul__(n: int) -> timedelta
 def __str__ -> str
}

class date {
 year: int
 month: int
 day: int

 self(year: int, month: int, day: int)
 static def today -> date
 static def fromisoformat(s: str) -> date
 def isoformat -> str
 def strftime(fmt: str) -> str
 def __sub__(other: date) -> timedelta
 def __eq__(other: date) -> bool
 def __lt__(other: date) -> bool
 def __str__ -> str
}

class time {
 hour: int
 minute: int
 second: int
 microsecond: int

 self(hour: int = 0, minute: int = 0, second: int = 0)
 def isoformat -> str
 def strftime(fmt: str) -> str
 def __eq__(other: time) -> bool
 def __lt__(other: time) -> bool
 def __str__ -> str
}
```

**Implementation:** Runtime C functions wrapping `<time.h>` (`time`, `localtime`,
`gmtime`, `strftime`, `mktime`). The `datetime` struct is a Dragon class whose
fields map to `struct tm` components. ~300 LOC in runtime.

**Depends on:** Phase A (`__str__`, `__eq__`, `__lt__`) and Phase B
(`__add__`, `__sub__`).

### 3. `math` (extend existing)

**Current state:** StdlibRegistry maps `math.sqrt`, `math.sin`, `math.cos`, etc. to
C `<math.h>` functions.

**Missing:**

| Function/Constant | Maps to |
|-------------------|---------|
| `math.pi` | `3.14159265358979323846` |
| `math.e` | `2.71828182845904523536` |
| `math.inf` | `INFINITY` |
| `math.nan` | `NAN` |
| `math.isnan(x)` | `isnan` |
| `math.isinf(x)` | `isinf` |
| `math.isfinite(x)` | `isfinite` |
| `math.log(x)` | `log` |
| `math.log2(x)` | `log2` |
| `math.log10(x)` | `log10` |
| `math.exp(x)` | `exp` |
| `math.gcd(a, b)` | Runtime impl |
| `math.factorial(n)` | Runtime impl |
| `math.comb(n, k)` | Runtime impl |
| `math.perm(n, k)` | Runtime impl |
| `math.degrees(x)` | `x * 180.0 / M_PI` |
| `math.radians(x)` | `x * M_PI / 180.0` |

**Implementation:** Constants emitted as LLVM `ConstantFP` globals. Functions added to
StdlibRegistry. `gcd`, `factorial`, `comb`, `perm` as ~50 LOC in runtime.

### 4. `os` (extend existing)

**Current state:** StdlibRegistry has `os.getcwd`.

**Additions:**

| Function | Maps to | Notes |
|----------|---------|-------|
| `os.path.join(a, b)` | Runtime | Concatenate with `/` separator |
| `os.path.exists(p)` | `access(p, F_OK)` | |
| `os.path.isfile(p)` | `stat` + `S_ISREG` | |
| `os.path.isdir(p)` | `stat` + `S_ISDIR` | |
| `os.path.basename(p)` | Runtime | String manipulation |
| `os.path.dirname(p)` | Runtime | String manipulation |
| `os.path.abspath(p)` | `realpath` | |
| `os.path.splitext(p)` | Runtime | Returns `(root, ext)` tuple |
| `os.listdir(path)` | `opendir` / `readdir` | Returns `list[str]` |
| `os.mkdir(path)` | `mkdir(path, 0755)` | |
| `os.makedirs(path)` | Recursive `mkdir` | |
| `os.remove(path)` | `unlink` | |
| `os.rmdir(path)` | `rmdir` | |
| `os.rename(src, dst)` | `rename` | |
| `os.getenv(key)` | `getenv` | Returns `str` or `None` |
| `os.environ` | Runtime dict wrapper | `dict[str, str]` |
| `os.system(cmd)` | `system` | Returns exit code |

**Implementation:** ~200 LOC in runtime. Path functions are string manipulation +
POSIX calls.

### 5. `sys`

**Import:** `import sys`

| Name | Type | Maps to |
|------|------|---------|
| `sys.argv` | `list[str]` | `main(argc, argv)` passthrough |
| `sys.exit(code)` | `(int) -> Never` | `exit` |
| `sys.platform` | `str` | `"linux"` / `"darwin"` / `"win32"` (compile-time) |
| `sys.version` | `str` | Dragon version string |
| `sys.maxsize` | `int` | `INT64_MAX` |
| `sys.stdin` | `File` | `fdopen(0)` |
| `sys.stdout` | `File` | `fdopen(1)` |
| `sys.stderr` | `File` | `fdopen(2)` |
| `sys.path` | `list[str]` | Module search paths |

**`sys.argv` implementation:** The Dragon entry point `main(argc, argv)` currently
discards arguments. Change CodeGen to:
1. Emit `main(i32 argc, i8** argv)` instead of `main`
2. Build a `DragonList` from `argv[0..argc]`
3. Store in a global `@sys_argv`

## Build order

| Phase | Module | Estimated LOC | Depends On |
|-------|--------|---------------|------------|
| 1 | `math` (complete) | ~50 runtime + StdlibRegistry entries | Nothing |
| 2 | `sys` | ~100 runtime + CodeGen main change | Nothing |
| 3 | `os` (extend) | ~200 runtime + StdlibRegistry entries | Nothing |
| 4 | `json` | ~400 runtime | Nothing (but benefits from `__str__`) |
| 5 | `datetime` | ~300 runtime + Dragon class definitions | Phase A+B |

**Total: ~1,050 LOC of runtime C code, ~30 StdlibRegistry entries, ~50 tests**

## Later (Phase 2+)

These require additional language features and are deferred:

| Module | Blocker | Priority |
|--------|---------|----------|
| `functools` | Needs `__call__` dunder (Phase F), closures as values | Medium |
| `collections` | Needs `__missing__` dunder, `__getitem__` (Phase C) | Medium |
| `re` | Needs regex engine (embed PCRE2 or hand-roll) | Medium |
| `pathlib` | Needs `__truediv__` for `/` operator (Phase B) | Low |
| `itertools` | Needs lazy generators (`yield` codegen) | Low |
| `hashlib` | Needs `bytes` type or wrap OpenSSL | Low |
| `inspect` | Needs RTTI / runtime type metadata tables | Very Low |

## Affected Components

- **Runtime (`lib/Runtime/runtime.cpp`):** New C functions for json, datetime, os, sys
- **StdlibRegistry:** Module registration for import resolution
- **CodeGen:** `main` signature change for `sys.argv`, module constant emission
- **Tests:** ~50 new tests across CodeGenTest (IR + E2E)
