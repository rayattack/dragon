# Dates, Times, and Math

This chapter covers the part of Dragon's standard library that deals with
clocks, calendars, and numbers: `datetime`, `time`, `calendar`, `math`,
`random`, `statistics`, and `fractions`.

These modules are written in Dragon (with thin `extern "C"` bridges to libc and
libm where the work genuinely belongs to the C runtime). Because they are
ordinary Dragon source, the surfaces are typed and concrete - and they do not
always match Python name-for-name. Each section below documents what the code
*actually exposes*. Read the "Differs from Python" note in each section before
you port code over from CPython.

A module is used by importing it and qualifying the call (`import math` then
`math.sqrt(2.0)`), or by pulling names in directly
(`from math import sqrt`). Both forms are shown throughout.

---

## `datetime` - dates, times, and durations

The `datetime` module provides four classes - `timedelta`, `date`,
`time_of_day`, and `datetime` - plus a set of module-level constructor
functions. Everything is **UTC-only**; there is no `tzinfo` and no
local-timezone conversion in this version.

The classes are plain Dragon classes, so construction is positional and fully
typed:

| Constructor | Signature |
| --- | --- |
| `timedelta(days, seconds, microseconds)` | all `int`; normalized to canonical form |
| `date(year, month, day)` | all `int` |
| `time_of_day(hour, minute, second, microsecond)` | all `int` |
| `datetime(year, month, day, hour, minute, second, microsecond)` | all `int` |

Current-time values come from **free functions**, not classmethods:

| Function | Returns |
| --- | --- |
| `datetime_now()` / `datetime_utcnow()` | current UTC `datetime` |
| `datetime_fromtimestamp(ts: float)` | `datetime` from a Unix timestamp |
| `date_today()` | today's UTC `date` |
| `date_fromtimestamp(ts: float)` | `date` from a Unix timestamp |

The classes carry the methods you would expect: `isoformat()` on `date`,
`time_of_day`, and `datetime`; `timestamp()` and `date_part()`/`time_part()` on
`datetime`; `toordinal()` on `date`; and `total_seconds()` on `timedelta`.
Arithmetic is wired through operator dunders, so `+` and `-` work directly:
`datetime + timedelta` yields a `datetime`, and `datetime - datetime` yields a
`timedelta`.

```dragon
import datetime

const dt: datetime.datetime = datetime.datetime(2026, 6, 1, 14, 30, 5, 0)
print(dt.isoformat())            # 2026-06-01T14:30:05
print(dt.date_part().isoformat()) # 2026-06-01
print(dt.timestamp())            # 1780324205.0

const td: datetime.timedelta = datetime.timedelta(7, 3600, 0)
print(td.total_seconds())        # 608400.0

const later: datetime.datetime = dt + td
print(later.isoformat())         # 2026-06-08T15:30:05

const gap: datetime.timedelta = later - dt
print(gap.total_seconds())       # 608400.0
```

The constructor functions read the wall clock through `time.now_float()`:

```dragon
from datetime import datetime, date, datetime_now, date_today, datetime_fromtimestamp

const now: datetime = datetime_now()
print(now.year > 2000)           # True

const today: date = date_today()
print(today.isoformat())         # e.g. 2026-06-01

const epoch: datetime = datetime_fromtimestamp(0.0)
print(epoch.isoformat())         # 1970-01-01T00:00:00
```

**Differs from Python.** There are no classmethods: use `datetime_now()`
instead of `datetime.now()`, and `date_today()` instead of `date.today()`. The
time-of-day class is named `time_of_day`, not `time` (the latter is a sibling
module). There is **no** `strftime`/`strptime` - formatting is `isoformat()`
only. The whole module is UTC-only: no `tzinfo`, no `astimezone`, and
`datetime_utcnow()` is just an alias for `datetime_now()`.

---

## `time` - clocks and sleeping

The `time` module wraps the POSIX clocks. The reusable surface is a set of
free functions; the bare `time()`/`sleep()` names are `extern "C"` ABI
declarations, so call the Dragon wrappers below rather than those.

| Function | Returns | Meaning |
| --- | --- | --- |
| `now()` | `int` | seconds since the Unix epoch |
| `now_float()` | `float` | epoch seconds with sub-second precision (`CLOCK_REALTIME`) |
| `monotonic()` | `float` | monotonic clock, immune to wall-clock changes |
| `perf_counter()` | `float` | high-resolution counter (alias for `monotonic`) |
| `process_time()` | `float` | CPU time used by this process |
| `sleep_secs(seconds: int)` | `int` | sleep whole seconds |
| `sleep_ms(milliseconds: int)` | `int` | sleep milliseconds |
| `sleep_float(seconds: float)` | `int` | sleep with sub-second precision |

Use `monotonic()` (or `perf_counter()`) for measuring elapsed time, and
`now()`/`now_float()` when you need a real-world timestamp.

```dragon
import time

const start: float = time.monotonic()
time.sleep_float(0.01)
const elapsed: float = time.monotonic() - start
print(elapsed >= 0.0)            # True

print(time.now() > 1700000000)   # True
```

**Differs from Python.** The single Python `time.sleep(secs)` is split into
three typed wrappers - `sleep_secs`, `sleep_ms`, and `sleep_float` - because
Dragon does not overload on argument type. `time.time()` is `now()` /
`now_float()`. The sleep wrappers return an `int` status code, not `None`.

---

## `calendar` - calendar math and formatting

`calendar` is pure date arithmetic with no C dependencies. Weekdays are
numbered **0 = Monday … 6 = Sunday**; months are **1 = January … 12 =
December**. The module exports named constants (`MONDAY`…`SUNDAY`,
`JANUARY`…`DECEMBER`) for both.

| Function | Returns |
| --- | --- |
| `isleap(year: int)` | `bool` |
| `leapdays(y1: int, y2: int)` | `int` - leap years in `[y1, y2)` |
| `weekday(year, month, day)` | `int` - `0`=Monday … `6`=Sunday |
| `monthrange(year, month)` | `list[int]` - `[first_weekday, num_days]` |
| `monthcalendar(year, month)` | `list[list[int]]` - weeks of day numbers (`0` = padding) |
| `day_name(d)` / `day_abbr(d)` | `str` |
| `month_name(m)` / `month_abbr(m)` | `str` |
| `formatmonth(year, month)` | `str` - multi-line month grid |
| `formatyear(year)` | `str` - all twelve months |

```dragon
import calendar

print(calendar.isleap(2024))         # True
print(calendar.weekday(2026, 6, 1))  # 0  (a Monday)
print(calendar.month_name(6))        # June

const mr: list[int] = calendar.monthrange(2026, 6)
print(mr[0])                         # 0   first day is Monday
print(mr[1])                         # 30  days in June

print(calendar.formatmonth(2026, 6))
# Output:
#      June 2026
# Mo Tu We Th Fr Sa Su
#  1  2  3  4  5  6  7
#  8  9 10 11 12 13 14
# 15 16 17 18 19 20 21
# 22 23 24 25 26 27 28
# 29 30
```

**Differs from Python.** `monthrange` returns a two-element `list[int]` rather
than a tuple, and the day/month name lookups are functions (`day_name(0)`)
rather than the array-like `calendar.day_name[0]` objects in Python. There is no
`Calendar`/`TextCalendar`/`HTMLCalendar` class hierarchy - `formatmonth` and
`formatyear` are the formatting surface.

---

## `math` - numeric functions

`math` is a libm bridge plus a layer of idiomatic Dragon wrappers. All the
trigonometric, exponential, and rounding functions take and return `float`. The
integer helpers (`gcd`, `factorial`, …) take and return `int`.

Constants: `pi`, `e`, `tau`, `inf`, `nan`.

The libm-backed functions include `sqrt`, `fabs`, `fmod`, `remainder`, `pow`,
`exp`, `exp2`, `expm1`, `log`, `log2`, `log10`, `log1p`, `cbrt`, `hypot`; the
trig family `sin`, `cos`, `tan`, `asin`, `acos`, `atan`, `atan2`; the hyperbolic
family `sinh`, `cosh`, `tanh`, `asinh`, `acosh`, `atanh`; rounding `ceil`,
`floor`, `trunc`, `round`, `copysign`; and the classifiers `isnan`, `isinf`,
`isfinite`.

The Dragon-side wrappers add the conveniences Python keeps in `math`:

| Function | Returns | Notes |
| --- | --- | --- |
| `degrees(x)` / `radians(x)` | `float` | angle conversion |
| `dist(p, q)` | `float` | Euclidean distance over `list[float]` |
| `clamp(x, low, high)` / `lerp(a, b, t)` | `float` | |
| `gcd(a, b)` / `lcm(a, b)` | `int` | |
| `factorial(n)` | `int` | `-1` for negative input |
| `comb(n, k)` / `perm(n, k)` | `int` | |
| `isqrt(n)` | `int` | integer square root |
| `log_int(x, base)` | `int` | integer logarithm |
| `sum_float(items)` / `prod(items)` | `float` | over `list[float]` |

```dragon
import math

print(math.sqrt(2.0))        # 1.4142135623730951
print(math.floor(3.7))       # 3.0
print(math.ceil(3.2))        # 4.0
print(math.hypot(3.0, 4.0))  # 5.0
print(math.gcd(12, 18))      # 6
print(math.factorial(5))     # 120
print(math.isqrt(50))        # 7
print(math.comb(5, 2))       # 10
print(math.degrees(math.pi)) # 180.0
```

**Differs from Python.** `floor`, `ceil`, `trunc`, and `round` come straight
from libm and therefore return a **`float`**, not an `int` - wrap them in
`int(...)` if you need an integer. `round` rounds half away from zero (libm
`round`), not Python's banker's rounding. `factorial(-1)` returns `-1` rather
than raising. The classifiers (`isnan`, etc.) return an `intc` (`0`/`1`), not a
`bool`.

---

## `random` - pseudo-random numbers

`random` wraps libc's `rand()`/`srand()`. It is fast but **not**
cryptographically secure - reach for `secrets` when you need unpredictable
values.

| Function | Returns | Meaning |
| --- | --- | --- |
| `seed(s: int)` | - | seed the generator deterministically |
| `seed_time()` | - | seed from the current time |
| `random()` | `float` | a value in `[0.0, 1.0)` |
| `randint(a, b)` | `int` | `N` with `a <= N <= b` (inclusive) |
| `randrange(start, stop)` | `int` | `N` with `start <= N < stop` |
| `uniform(a, b)` | `float` | `N` with `a <= N <= b` |
| `choice(items: list[str])` | `str` | a random element |
| `choice_int(items: list[int])` | `int` | a random element |
| `shuffle(items: list[int])` | `list[int]` | a **new** shuffled list |
| `sample(items: list[int], k)` | `list[int]` | `k` elements without replacement |

```dragon
import random

random.seed(42)                       # reproducible runs

const r: float = random.random()
print(r >= 0.0 and r < 1.0)           # True

const roll: int = random.randint(1, 6)
print(roll >= 1 and roll <= 6)        # True

const colors: list[str] = ["red", "green", "blue"]
print(random.choice(colors) in colors)  # True

const order: list[int] = random.shuffle([1, 2, 3, 4, 5])
print(len(order))                     # 5
```

The exact draws above are not shown because they depend on the seed and the
platform's `rand()`. Call `random.seed(n)` first if you need a run to be
reproducible.

**Differs from Python.** `choice` is monomorphic per element type:
`choice(list[str])` and `choice_int(list[int])` are separate functions because
Dragon does not have a runtime-polymorphic sequence type. `shuffle` returns a
new list rather than shuffling in place (and is `int`-only), and `sample` is
`int`-only. The generator is libc's, so the stream for a given seed will not
match CPython's Mersenne Twister.

---

## `statistics` - summary statistics

`statistics` operates on `list[float]` (with `int`-list variants where it
matters). Functions raise `ValueError` on empty input, and `variance`/`stdev`
require at least two points.

| Function | Returns | Notes |
| --- | --- | --- |
| `mean(data)` / `fmean(data)` | `float` | arithmetic mean |
| `geometric_mean(data)` | `float` | positive values only |
| `harmonic_mean(data)` | `float` | positive values only |
| `median(data)` | `float` | average of the middle two when even |
| `median_low(data)` / `median_high(data)` | `float` | |
| `mode_float(data)` / `mode_int(data)` | `float` / `int` | first value seen wins a tie |
| `variance(data)` / `stdev(data)` | `float` | **sample** (divides by `n-1`) |
| `pvariance(data)` / `pstdev(data)` | `float` | **population** (divides by `n`) |
| `quantiles(data, n)` | `list[float]` | `n-1` cut points (exclusive method) |

```dragon
import statistics

const data: list[float] = [2.0, 4.0, 4.0, 4.0, 5.0, 5.0, 7.0, 9.0]
print(statistics.mean(data))      # 5.0
print(statistics.median(data))    # 4.5
print(statistics.variance(data))  # 4.571428571428571
print(statistics.stdev(data))     # 2.138089935299395
print(statistics.pstdev(data))    # 2.0
print(statistics.mode_float(data))# 4.0

print(statistics.quantiles([1.0, 2.0, 3.0, 4.0, 5.0, 6.0], 4))
# [1.5, 3.0, 4.5]
```

**Differs from Python.** `mode` is split into `mode_float` and `mode_int`. The
data argument is a concrete `list[float]` (or `list[int]`), not an arbitrary
iterable, and there is no `NormalDist` class or `correlation`/`covariance`
suite. `geometric_mean` and `harmonic_mean` reject non-positive values with a
`ValueError`.

---

## `fractions` - exact rational arithmetic

`fractions` provides a `Fraction` class storing reduced rationals with the sign
on the numerator. It is pure Dragon and uses **named methods** rather than
operators.

`Fraction(num: int, den: int)` constructs and immediately reduces; a zero
denominator raises `ValueError`. The module-level `from_float(f: float)`
approximates a float via continued fractions (denominator capped at `10^6`).

| Method | Returns | Meaning |
| --- | --- | --- |
| `add` / `sub` / `mul` / `div` | `Fraction` | arithmetic (each takes one `Fraction`) |
| `eq` / `lt` / `le` / `gt` / `ge` | `bool` | comparison |
| `to_float()` | `float` | decimal value |
| `to_str()` | `str` | `"num/den"`, or just `"num"` when the denominator is 1 |
| `abs()` / `neg()` / `reciprocal()` | `Fraction` | |
| `limit_denominator(max_den)` | `Fraction` | closest fraction with a bounded denominator |

```dragon
from fractions import Fraction, from_float

const half: Fraction = Fraction(1, 2)
const third: Fraction = Fraction(1, 3)

const total: Fraction = half.add(third)
print(total.to_str())      # 5/6
print(total.to_float())    # 0.8333333333333334

print(Fraction(6, 8).to_str())   # 3/4   (auto-reduced)
print(half.lt(third))            # False

const quarter: Fraction = from_float(0.25)
print(quarter.to_str())          # 1/4
```

**Differs from Python.** There are no operator overloads - write `a.add(b)` and
`a.lt(b)`, not `a + b` and `a < b`. Construction is `Fraction(num, den)` with two
integer arguments; there is no string parser (`Fraction("3/4")`) and no
mixed-type construction. `to_str()` / `to_float()` replace Python's `str()` /
`float()` conversions, and the numerator/denominator are the public fields
`num` and `den`.

---

## At a glance

| Module | Import | Key surface | Watch out for |
| --- | --- | --- | --- |
| `datetime` | `import datetime` | `datetime`/`date`/`timedelta`/`time_of_day`, `datetime_now()`, `date_today()` | UTC-only; no `strftime`; current time via free functions |
| `time` | `import time` | `now()`, `now_float()`, `monotonic()`, `sleep_float()` | `sleep` is split into `sleep_secs`/`_ms`/`_float` |
| `calendar` | `import calendar` | `isleap`, `weekday`, `monthrange`, `formatmonth` | weekday `0`=Monday; `monthrange` returns a list |
| `math` | `import math` | `sqrt`, `floor`, `gcd`, `factorial`, `comb`, `pi` | `floor`/`ceil`/`round` return `float` |
| `random` | `import random` | `seed`, `random`, `randint`, `choice`, `shuffle` | not secure; `choice`/`shuffle` typed per element |
| `statistics` | `import statistics` | `mean`, `median`, `stdev`, `variance`, `mode_float` | takes `list[float]`; `mode` split by type |
| `fractions` | `from fractions import Fraction` | `Fraction(n, d)`, `add`/`mul`/`to_str` | named methods, no operator overloads |

With dates, clocks, and numbers covered, the next chapter turns to the tools
for composing and transforming data: [Functional Tools](/docs/1406-stdlib-functional).
