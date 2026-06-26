# Dragon `datetime` Module - Python Parity Spec

**Module**: `stdlib/datetime.dr`
**Python reference**: https://docs.python.org/3/library/datetime.html
**Overall progress**: **0 / 97** functions (0%)

> Phase 0 = Core (most-used, needed for real programs)
> Phase 1 = Common (frequently used, full formatting/parsing)
> Phase 2 = Extended (full parity, niche/advanced)

---

## Design Decisions for Dragon

### What fits well
- **Immutable value classes** - `Date`, `Time`, `DateTime`, `TimeDelta` are naturally immutable structs with typed fields. Dragon's class system handles this cleanly.
- **Dunder operators** - All comparison (`__eq__`, `__lt__`, etc.) and arithmetic (`__add__`, `__sub__`) dunders are already implemented in Dragon. Date arithmetic maps perfectly.
- **`@staticmethod` constructors** - Python's `datetime.now()`, `date.today()`, `date.fromtimestamp()` map to Dragon `@staticmethod` methods.
- **Field access** - `dt.year`, `dt.month`, etc. are just class field reads. No dynamic properties needed.
- **FFI to C `<time.h>`** - `time()`, `localtime()`, `gmtime()`, `mktime()`, `strftime()`, `strptime()`, `clock_gettime()` are all available via `extern "C"`.

### What does NOT fit / excluded
- **`tzinfo` abstract base class** - Dragon has no ABCs. Replaced by concrete `TimeZone` class with fixed UTC offset. Custom timezone subclassing is out of scope.
- **`fold` parameter** - Extremely niche (DST fold disambiguation). Excluded entirely.
- **Keyword arguments in constructors** - Dragon uses positional params. `replace(year=..., month=...)` becomes individual `with_year()`, `with_month()` methods.
- **`None` return types** - Python's `utcoffset() -> timedelta | None` pattern doesn't fit. Dragon uses sentinel values or skips these methods.
- **`timetuple()` / `struct_time`** - Python-specific named tuple. No equivalent in Dragon. Use individual field access instead.
- **Pickling / `__reduce__`** - Irrelevant for a compiled language.
- **Locale-dependent formatting** - `%c`, `%x`, `%X` depend on C locale. Include in Phase 2 but document as platform-dependent.
- **`divmod(t1, t2)`** - Would require global builtin override. Excluded; use `//` and `%` separately.
- **Deprecated methods** - `utcnow()`, `utcfromtimestamp()` (deprecated in Python 3.12) are excluded.

### Naming convention
Python uses lowercase class names (`datetime`, `timedelta`). Dragon convention is CamelCase for classes (`File`, `Lock`, `SyncList`). This spec uses **CamelCase**: `Date`, `Time`, `DateTime`, `TimeDelta`, `TimeZone`.

### Internal representation
- **`TimeDelta`**: single `int` field - total microseconds. Derived accessors for days/seconds/microseconds.
- **`Date`**: three `int` fields - `year`, `month`, `day`.
- **`Time`**: four `int` fields - `hour`, `minute`, `second`, `microsecond`.
- **`DateTime`**: seven `int` fields - `year`, `month`, `day`, `hour`, `minute`, `second`, `microsecond`, plus `int` for tz offset in minutes (sentinel `-1440` = naive).
- **`TimeZone`**: `int` offset in minutes + `str` name.

### Runtime helpers needed (C)
```c
int64_t  dragon_time_epoch_s();            // time(NULL)
int64_t  dragon_time_epoch_us();           // clock_gettime(CLOCK_REALTIME)
void     dragon_time_to_fields(int64_t epoch_s, int64_t* y, int64_t* mo, int64_t* d,
                               int64_t* h, int64_t* mi, int64_t* s, int64_t* wday, int64_t* yday);
int64_t  dragon_time_from_fields(int64_t y, int64_t mo, int64_t d,
                                 int64_t h, int64_t mi, int64_t s);
const char* dragon_time_strftime(const char* fmt, int64_t y, int64_t mo, int64_t d,
                                 int64_t h, int64_t mi, int64_t s);
void     dragon_time_strptime(const char* s, const char* fmt,
                              int64_t* y, int64_t* mo, int64_t* d,
                              int64_t* h, int64_t* mi, int64_t* sec);
int64_t  dragon_time_days_in_month(int64_t y, int64_t m);
bool     dragon_time_is_leap(int64_t y);
```

---

## Module Constants

| # | Python API | Dragon API | Phase | Status |
|---|-----------|------------|-------|--------|
| 1 | `datetime.MINYEAR` | `MINYEAR: int` (= 1) | 0 | - |
| 2 | `datetime.MAXYEAR` | `MAXYEAR: int` (= 9999) | 0 | - |

---

## `TimeDelta` Class

### Constructors

| # | Python API | Dragon API | Phase | Status |
|---|-----------|------------|-------|--------|
| 3 | `timedelta(days, seconds, microseconds, milliseconds, minutes, hours, weeks)` | `self(days: int, seconds: int, microseconds: int)` | 0 | - |
| 4 | - | `self(days: int)` (convenience: seconds=0, us=0) | 0 | - |
| 5 | - | `self(days: int, seconds: int)` (convenience: us=0) | 0 | - |
| 6 | `timedelta(hours=..., minutes=...)` | `@staticmethod from_hms(hours: int, minutes: int, seconds: int) -> TimeDelta` | 0 | - |
| 7 | `timedelta(weeks=...)` | `@staticmethod from_weeks(weeks: int) -> TimeDelta` | 1 | - |
| 8 | `timedelta(milliseconds=...)` | `@staticmethod from_millis(ms: int) -> TimeDelta` | 1 | - |

> **Note**: Python's `timedelta()` accepts all 7 keyword args and normalizes. Dragon uses arity-overloaded `self()` for the common case (days, seconds, microseconds) and `@staticmethod` named constructors for less common units.

### Fields (read-only via access)

| # | Python API | Dragon API | Phase | Status |
|---|-----------|------------|-------|--------|
| 9 | `td.days` | `td.days` → `int` | 0 | - |
| 10 | `td.seconds` | `td.seconds` → `int` (0-86399) | 0 | - |
| 11 | `td.microseconds` | `td.microseconds` → `int` (0-999999) | 0 | - |

### Instance Methods

| # | Python API | Dragon API | Phase | Status |
|---|-----------|------------|-------|--------|
| 12 | `td.total_seconds()` | `td.total_seconds() -> float` | 0 | - |
| 13 | `str(td)` | `td.__str__() -> str` - `"5 days, 3:02:01"` | 0 | - |
| 14 | `repr(td)` | `td.__repr__() -> str` - `"TimeDelta(5, 10921, 0)"` | 1 | - |
| 15 | `abs(td)` | `td.__abs__() -> TimeDelta` | 0 | - |
| 16 | `bool(td)` | `td.__bool__() -> bool` - false if zero | 0 | - |
| 17 | `hash(td)` | `td.__hash__() -> int` | 1 | - |

### Operators (via dunders)

| # | Python API | Dragon API | Phase | Status |
|---|-----------|------------|-------|--------|
| 18 | `t1 + t2` | `__add__(other: TimeDelta) -> TimeDelta` | 0 | - |
| 19 | `t1 - t2` | `__sub__(other: TimeDelta) -> TimeDelta` | 0 | - |
| 20 | `-t1` | `__neg__() -> TimeDelta` | 0 | - |
| 21 | `+t1` | `__pos__() -> TimeDelta` | 1 | - |
| 22 | `t * n` | `__mul__(n: int) -> TimeDelta` | 0 | - |
| 23 | `t // n` | `__floordiv__(n: int) -> TimeDelta` | 1 | - |
| 24 | `t1 // t2` | - (returns int; overload ambiguity - use method) | 2 | - |
| 25 | `t1 % t2` | `__mod__(other: TimeDelta) -> TimeDelta` | 2 | - |
| 26 | `t1 == t2` | `__eq__(other: TimeDelta) -> bool` | 0 | - |
| 27 | `t1 < t2` | `__lt__(other: TimeDelta) -> bool` | 0 | - |
| 28 | `t1 <= t2` | `__le__(other: TimeDelta) -> bool` (via fallback) | 0 | - |
| 29 | `t1 > t2` | `__gt__(other: TimeDelta) -> bool` (via fallback) | 0 | - |
| 30 | `t1 >= t2` | `__ge__(other: TimeDelta) -> bool` (via fallback) | 0 | - |
| 31 | `t1 != t2` | `__ne__(other: TimeDelta) -> bool` (via fallback) | 0 | - |

### Class Constants

| # | Python API | Dragon API | Phase | Status |
|---|-----------|------------|-------|--------|
| 32 | `timedelta.min` | `@staticmethod min() -> TimeDelta` | 1 | - |
| 33 | `timedelta.max` | `@staticmethod max() -> TimeDelta` | 1 | - |
| 34 | `timedelta.resolution` | `@staticmethod resolution() -> TimeDelta` | 2 | - |

---

## `Date` Class

### Constructors

| # | Python API | Dragon API | Phase | Status |
|---|-----------|------------|-------|--------|
| 35 | `date(year, month, day)` | `self(year: int, month: int, day: int)` | 0 | - |
| 36 | `date.today()` | `@staticmethod today() -> Date` | 0 | - |
| 37 | `date.fromtimestamp(ts)` | `@staticmethod from_timestamp(ts: int) -> Date` | 0 | - |
| 38 | `date.fromordinal(n)` | `@staticmethod from_ordinal(n: int) -> Date` | 2 | - |
| 39 | `date.fromisoformat(s)` | `@staticmethod from_iso(s: str) -> Date` | 1 | - |
| 40 | `date.fromisocalendar(y, w, d)` | `@staticmethod from_isocalendar(year: int, week: int, day: int) -> Date` | 2 | - |

### Fields (read-only)

| # | Python API | Dragon API | Phase | Status |
|---|-----------|------------|-------|--------|
| 41 | `d.year` | `d.year` → `int` | 0 | - |
| 42 | `d.month` | `d.month` → `int` | 0 | - |
| 43 | `d.day` | `d.day` → `int` | 0 | - |

### Instance Methods

| # | Python API | Dragon API | Phase | Status |
|---|-----------|------------|-------|--------|
| 44 | `d.replace(year, month, day)` | `d.with_year(y: int) -> Date` | 1 | - |
| 45 | - | `d.with_month(m: int) -> Date` | 1 | - |
| 46 | - | `d.with_day(d: int) -> Date` | 1 | - |
| 47 | `d.weekday()` | `d.weekday() -> int` (Mon=0, Sun=6) | 0 | - |
| 48 | `d.isoweekday()` | `d.isoweekday() -> int` (Mon=1, Sun=7) | 1 | - |
| 49 | `d.isoformat()` | `d.isoformat() -> str` - `"2024-03-15"` | 0 | - |
| 50 | `str(d)` | `d.__str__() -> str` (= isoformat) | 0 | - |
| 51 | `d.strftime(fmt)` | `d.strftime(fmt: str) -> str` | 1 | - |
| 52 | `d.ctime()` | `d.ctime() -> str` - `"Fri Mar 15 00:00:00 2024"` | 2 | - |
| 53 | `d.toordinal()` | `d.toordinal() -> int` | 2 | - |
| 54 | `d.isocalendar()` | `d.isocalendar() -> list[int]` - `[year, week, weekday]` | 2 | - |
| 55 | `d.timetuple()` | - (excluded: no struct_time in Dragon) | - | N/A |
| 56 | `repr(d)` | `d.__repr__() -> str` | 1 | - |
| 57 | `hash(d)` | `d.__hash__() -> int` | 1 | - |

### Operators

| # | Python API | Dragon API | Phase | Status |
|---|-----------|------------|-------|--------|
| 58 | `d1 + timedelta` | `__add__(td: TimeDelta) -> Date` | 0 | - |
| 59 | `d1 - timedelta` | `__sub__(td: TimeDelta) -> Date` | 0 | - |
| 60 | `d1 - d2` | `d.diff(other: Date) -> TimeDelta` | 0 | - |
| 61 | `d1 == d2` | `__eq__(other: Date) -> bool` | 0 | - |
| 62 | `d1 < d2` | `__lt__(other: Date) -> bool` | 0 | - |
| 63 | `d1 <= d2` | `__le__` (via fallback) | 0 | - |
| 64 | `d1 > d2` | `__gt__` (via fallback) | 0 | - |
| 65 | `d1 >= d2` | `__ge__` (via fallback) | 0 | - |

> **Note on `d1 - d2`**: Python overloads `__sub__` to return either `Date` (when subtracting a `TimeDelta`) or `TimeDelta` (when subtracting a `Date`). Dragon's static type system cannot have `__sub__` return two different types. Solution: `__sub__` takes `TimeDelta` and returns `Date`; a separate `diff()` method takes `Date` and returns `TimeDelta`.

### Class Constants

| # | Python API | Dragon API | Phase | Status |
|---|-----------|------------|-------|--------|
| 66 | `date.min` | `@staticmethod min() -> Date` | 2 | - |
| 67 | `date.max` | `@staticmethod max() -> Date` | 2 | - |

---

## `Time` Class

### Constructors

| # | Python API | Dragon API | Phase | Status |
|---|-----------|------------|-------|--------|
| 68 | `time(hour, minute, second, microsecond)` | `self(hour: int, minute: int, second: int, microsecond: int)` | 1 | - |
| 69 | `time(hour, minute, second)` | `self(hour: int, minute: int, second: int)` (us=0) | 1 | - |
| 70 | `time(hour, minute)` | `self(hour: int, minute: int)` (s=0, us=0) | 1 | - |
| 71 | `time.fromisoformat(s)` | `@staticmethod from_iso(s: str) -> Time` | 2 | - |

### Fields (read-only)

| # | Python API | Dragon API | Phase | Status |
|---|-----------|------------|-------|--------|
| 72 | `t.hour` | `t.hour` → `int` | 1 | - |
| 73 | `t.minute` | `t.minute` → `int` | 1 | - |
| 74 | `t.second` | `t.second` → `int` | 1 | - |
| 75 | `t.microsecond` | `t.microsecond` → `int` | 1 | - |

### Instance Methods

| # | Python API | Dragon API | Phase | Status |
|---|-----------|------------|-------|--------|
| 76 | `t.isoformat()` | `t.isoformat() -> str` - `"14:30:05"` | 1 | - |
| 77 | `str(t)` | `t.__str__() -> str` (= isoformat) | 1 | - |
| 78 | `t.strftime(fmt)` | `t.strftime(fmt: str) -> str` | 2 | - |
| 79 | `t.replace(...)` | `t.with_hour(h: int) -> Time`, etc. | 2 | - |
| 80 | `t1 == t2` | `__eq__(other: Time) -> bool` | 1 | - |
| 81 | `t1 < t2` | `__lt__(other: Time) -> bool` | 1 | - |
| 82 | `hash(t)` | `__hash__() -> int` | 2 | - |

> **Note**: `Time` is Phase 1 because most programs only need `DateTime` or `Date`. Standalone time-of-day handling is less common. Python's `time` class is also the least used of the four.

---

## `DateTime` Class

### Constructors

| # | Python API | Dragon API | Phase | Status |
|---|-----------|------------|-------|--------|
| 83 | `datetime(y, mo, d, h, mi, s, us)` | `self(year: int, month: int, day: int, hour: int, minute: int, second: int)` | 0 | - |
| 84 | `datetime(y, mo, d)` | `self(year: int, month: int, day: int)` (h=m=s=0) | 0 | - |
| 85 | `datetime.now()` | `@staticmethod now() -> DateTime` | 0 | - |
| 86 | `datetime.today()` | - (alias for `now()` in Python, omit) | - | N/A |
| 87 | `datetime.fromtimestamp(ts)` | `@staticmethod from_timestamp(ts: int) -> DateTime` | 0 | - |
| 88 | `datetime.combine(d, t)` | `@staticmethod combine(d: Date, t: Time) -> DateTime` | 1 | - |
| 89 | `datetime.fromisoformat(s)` | `@staticmethod from_iso(s: str) -> DateTime` | 1 | - |
| 90 | `datetime.fromordinal(n)` | `@staticmethod from_ordinal(n: int) -> DateTime` | 2 | - |
| 91 | `datetime.strptime(s, fmt)` | `@staticmethod parse(s: str, fmt: str) -> DateTime` | 1 | - |

> **Excluded**: `datetime.utcnow()`, `datetime.utcfromtimestamp()` - deprecated in Python 3.12. Use `DateTime.now()` with timezone instead.

### Fields (read-only)

| # | Python API | Dragon API | Phase | Status |
|---|-----------|------------|-------|--------|
| 92 | `dt.year` | `dt.year` → `int` | 0 | - |
| 93 | `dt.month` | `dt.month` → `int` | 0 | - |
| 94 | `dt.day` | `dt.day` → `int` | 0 | - |
| 95 | `dt.hour` | `dt.hour` → `int` | 0 | - |
| 96 | `dt.minute` | `dt.minute` → `int` | 0 | - |
| 97 | `dt.second` | `dt.second` → `int` | 0 | - |
| 98 | `dt.microsecond` | `dt.microsecond` → `int` | 1 | - |

### Instance Methods

| # | Python API | Dragon API | Phase | Status |
|---|-----------|------------|-------|--------|
| 99 | `dt.date()` | `dt.date() -> Date` | 0 | - |
| 100 | `dt.time()` | `dt.time() -> Time` | 1 | - |
| 101 | `dt.timestamp()` | `dt.timestamp() -> int` (epoch seconds) | 0 | - |
| 102 | `dt.weekday()` | `dt.weekday() -> int` (Mon=0, Sun=6) | 0 | - |
| 103 | `dt.isoweekday()` | `dt.isoweekday() -> int` (Mon=1, Sun=7) | 1 | - |
| 104 | `dt.isoformat()` | `dt.isoformat() -> str` - `"2024-03-15T14:30:05"` | 0 | - |
| 105 | `str(dt)` | `dt.__str__() -> str` - `"2024-03-15 14:30:05"` (space sep) | 0 | - |
| 106 | `dt.strftime(fmt)` | `dt.strftime(fmt: str) -> str` | 0 | - |
| 107 | `dt.ctime()` | `dt.ctime() -> str` | 2 | - |
| 108 | `repr(dt)` | `dt.__repr__() -> str` | 1 | - |
| 109 | `hash(dt)` | `dt.__hash__() -> int` | 1 | - |
| 110 | `dt.replace(...)` | `dt.with_year(y: int) -> DateTime`, `with_month(m: int) -> DateTime`, etc. | 1 | - |
| 111 | `dt.toordinal()` | `dt.toordinal() -> int` | 2 | - |
| 112 | `dt.isocalendar()` | `dt.isocalendar() -> list[int]` | 2 | - |
| 113 | `dt.timetuple()` | - (excluded: no struct_time) | - | N/A |
| 114 | `dt.utctimetuple()` | - (excluded) | - | N/A |

### Operators

| # | Python API | Dragon API | Phase | Status |
|---|-----------|------------|-------|--------|
| 115 | `dt1 + timedelta` | `__add__(td: TimeDelta) -> DateTime` | 0 | - |
| 116 | `dt1 - timedelta` | `__sub__(td: TimeDelta) -> DateTime` | 0 | - |
| 117 | `dt1 - dt2` | `dt.diff(other: DateTime) -> TimeDelta` | 0 | - |
| 118 | `dt1 == dt2` | `__eq__(other: DateTime) -> bool` | 0 | - |
| 119 | `dt1 < dt2` | `__lt__(other: DateTime) -> bool` | 0 | - |
| 120 | `dt1 <= dt2` | `__le__` (via fallback) | 0 | - |
| 121 | `dt1 > dt2` | `__gt__` (via fallback) | 0 | - |
| 122 | `dt1 >= dt2` | `__ge__` (via fallback) | 0 | - |

---

## `TimeZone` Class

| # | Python API | Dragon API | Phase | Status |
|---|-----------|------------|-------|--------|
| 123 | `timezone(offset, name)` | `self(offset: TimeDelta, name: str)` | 2 | - |
| 124 | `timezone(offset)` | `self(offset: TimeDelta)` (auto-name) | 2 | - |
| 125 | `timezone.utc` | `@staticmethod utc() -> TimeZone` | 2 | - |
| 126 | `tz.utcoffset(dt)` | `tz.utcoffset() -> TimeDelta` | 2 | - |
| 127 | `tz.tzname(dt)` | `tz.tzname() -> str` | 2 | - |
| 128 | `tz.dst(dt)` | - (always returns zero delta for fixed offsets) | - | N/A |

> **Note**: `TimeZone` is Phase 2 because naive datetime is sufficient for most compiled programs. Python's `tzinfo` abstract base class is entirely excluded - Dragon provides only concrete fixed-offset timezones. IANA/Olson timezone database (pytz/zoneinfo) is out of scope.

---

## `strftime` Format Codes

### Phase 0 - supported immediately

| Code | Meaning | Example |
|------|---------|---------|
| `%Y` | 4-digit year | `2024` |
| `%m` | Zero-padded month | `03` |
| `%d` | Zero-padded day | `15` |
| `%H` | Hour (24h) | `14` |
| `%M` | Minute | `30` |
| `%S` | Second | `05` |
| `%%` | Literal `%` | `%` |

### Phase 1 - common additions

| Code | Meaning | Example |
|------|---------|---------|
| `%y` | 2-digit year | `24` |
| `%I` | Hour (12h) | `02` |
| `%p` | AM/PM | `PM` |
| `%A` | Full weekday | `Friday` |
| `%a` | Abbr weekday | `Fri` |
| `%B` | Full month name | `March` |
| `%b` | Abbr month name | `Mar` |
| `%j` | Day of year | `075` |
| `%U` | Week number (Sun start) | `11` |
| `%W` | Week number (Mon start) | `11` |
| `%f` | Microseconds | `000000` |
| `%z` | UTC offset | `+0000` |

### Phase 2 - locale-dependent / niche

| Code | Meaning | Note |
|------|---------|------|
| `%c` | Locale date+time | Platform-dependent |
| `%x` | Locale date | Platform-dependent |
| `%X` | Locale time | Platform-dependent |
| `%Z` | Timezone name | Requires tz database |

> **Implementation**: `strftime` wraps C's `strftime()` via FFI. Phase 0 codes are guaranteed portable. Phase 1/2 codes depend on the platform's C library.

---

## Standalone Module Functions

| # | Python API | Dragon API | Phase | Status |
|---|-----------|------------|-------|--------|
| 129 | - | `now() -> DateTime` (convenience, = `DateTime.now()`) | 0 | - |
| 130 | - | `today() -> Date` (convenience, = `Date.today()`) | 0 | - |
| 131 | - | `timestamp() -> int` (current epoch seconds) | 0 | - |
| 132 | - | `is_leap(year: int) -> bool` | 0 | - |
| 133 | - | `days_in_month(year: int, month: int) -> int` | 0 | - |

---

## Summary by Phase

| Phase | Total | Done | Remaining |
|-------|-------|------|-----------|
| **Phase 0** (core) | 42 | 0 | 42 |
| **Phase 1** (common) | 33 | 0 | 33 |
| **Phase 2** (extended) | 22 | 0 | 22 |
| **Total** | **97** | **0** | **97** |

### Phase 0 - what you get (42 items)

Core classes and operations for everyday datetime work:

- **`TimeDelta`**: constructor (3 arities), fields (days/seconds/microseconds), `total_seconds()`, `__str__`, arithmetic (`+`, `-`, `*`, negation, `abs`), all comparisons
- **`Date`**: constructor, `today()`, `from_timestamp()`, fields, `weekday()`, `isoformat()`, `__str__`, `__add__`/`__sub__` with TimeDelta, `diff()`, all comparisons
- **`DateTime`**: constructor (2 arities), `now()`, `from_timestamp()`, fields (year through second), `date()`, `timestamp()`, `weekday()`, `isoformat()`, `__str__`, `strftime()` (basic codes), `__add__`/`__sub__` with TimeDelta, `diff()`, all comparisons
- **Module functions**: `now()`, `today()`, `timestamp()`, `is_leap()`, `days_in_month()`
- **Constants**: `MINYEAR`, `MAXYEAR`

### Phase 1 - what gets added (33 items)

Formatting, parsing, and the `Time` class:

- **`Time` class**: full constructor/fields/isoformat/comparisons
- **`DateTime.parse()`** (strptime), `from_iso()`, `combine()`, `with_*()` replace methods
- **`Date.from_iso()`**, `strftime()`, `with_*()` replace methods
- **Additional strftime codes**: `%y`, `%I`, `%p`, `%A`, `%a`, `%B`, `%b`, `%j`, `%U`, `%W`, `%f`, `%z`
- **`TimeDelta`**: `from_weeks()`, `from_millis()`, `//`, `__repr__`, `min`/`max`
- **`__repr__`** and **`__hash__`** for all classes

### Phase 2 - full parity (22 items)

Niche features, timezone support, ordinal dates:

- **`TimeZone` class**: fixed-offset timezones, UTC singleton
- **Ordinal conversions**: `toordinal()`, `from_ordinal()`
- **ISO calendar**: `isocalendar()`, `from_isocalendar()`
- **`ctime()`**, locale-dependent format codes
- **`Time.from_iso()`**, `Time.strftime()`, `Time.with_*()`
- **`TimeDelta`**: `%`, `resolution`

---

## Example Usage (Phase 0)

```dragon
from datetime import DateTime, Date, TimeDelta

// Get current date and time
dt: DateTime = DateTime.now()
print(dt)                           // "2024-03-15 14:30:05"
print(dt.isoformat())              // "2024-03-15T14:30:05"
print(dt.strftime("%Y/%m/%d"))     // "2024/03/15"

// Date arithmetic
today: Date = Date.today()
week_later: Date = today + TimeDelta(7)
diff: TimeDelta = week_later.diff(today)
print(diff.days)                    // 7

// Comparisons
d1: Date = Date(2024, 1, 1)
d2: Date = Date(2024, 12, 31)
if d1 < d2 {
    print("d1 is earlier")
}

// Construct from parts
dt2: DateTime = DateTime(2024, 3, 15, 9, 0, 0)
elapsed: TimeDelta = dt.diff(dt2)
print(elapsed.total_seconds())     // seconds between the two datetimes

// Epoch conversion
ts: int = dt.timestamp()
restored: DateTime = DateTime.from_timestamp(ts)

// Utilities
print(is_leap(2024))               // true
print(days_in_month(2024, 2))      // 29
```

---

## Notes

- **No `None` timezone**: Python distinguishes "naive" (no tz) from "aware" (has tz) datetimes and forbids mixing them in comparisons. Dragon Phase 0-1 datetimes are always naive (local time). Phase 2 adds `TimeZone` but naive remains the default.
- **`int` not `float` for timestamps**: Python's `datetime.timestamp()` returns a float (seconds + microsecond fraction). Dragon returns `int` (epoch seconds). For microsecond-precision timestamps, use `dt.microsecond` separately or multiply: `dt.timestamp() * 1_000_000 + dt.microsecond`.
- **`diff()` instead of operator overloading**: Python's `d1 - d2` is polymorphic - it returns `TimeDelta` when the RHS is a date, or `Date` when the RHS is a `TimeDelta`. Dragon's static typing requires a single return type per operator. `__sub__` handles `TimeDelta` operand (returns same type); `diff()` handles same-type operand (returns `TimeDelta`).
- **`with_*()` instead of `replace()`**: Python's `replace(year=..., month=...)` relies on keyword arguments. Dragon uses explicit `with_year()`, `with_month()`, `with_day()` etc. Each returns a new instance (immutability preserved).
- **Microsecond precision**: Phase 0 `DateTime` stores only down to seconds. Phase 1 adds `microsecond` field. The internal representation reserves space from the start so this is a non-breaking addition.
- **Class methods as `@staticmethod`**: Python's `classmethod` constructors (e.g., `datetime.now()`) are `@staticmethod` in Dragon since Dragon doesn't pass the class implicitly for `@classmethod` in a useful way for construction.
- **Module re-exports**: The top-level `now()`, `today()`, `timestamp()` functions are convenience wrappers so users can write `from datetime import now` instead of `from datetime import DateTime` + `DateTime.now()`.
- **`strptime` → `parse`**: Renamed for clarity. `DateTime.parse("2024-03-15", "%Y-%m-%d")` is more readable than `DateTime.strptime(...)`.
- **Leap second handling**: Not supported. Second range is 0-59, consistent with C's `struct tm` and Python's `datetime`.
