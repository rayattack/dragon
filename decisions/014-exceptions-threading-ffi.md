# Decision 014: Exceptions, Threading & C FFI

> **Status:** Done. exceptions live; threading became three-tier concurrency (see 016); C FFI → native-extension ABI.

Three things I had to stop hand-waving about before the HTTP server could survive real traffic:

1. **Exception hierarchy** - expand from 7 hardcoded types to the full Python tree, plus user-defined exceptions
2. **GIL-free threading safety** - no GIL, so concurrent access to lists/dicts is *our* problem
3. **C/C++ FFI** - `extern "C"` for libuv, OpenSSL, database drivers, whatever

No GIL means shared mutable collections will crash if we ignore races. That's not theoretical. I already saw a dict rehash race once while stress-testing the server with four worker threads and stale coffee. It *will* happen on the HTTP server if we pretend CPython's accidental atomicity applies here.

## Part 1: Exception Hierarchy

### Current State

Dragon has 7 hardcoded exception types mapped to integer codes in
[CodeGen.cpp:88-95](src/CodeGen.cpp#L88):

```cpp
int64_t excTypeCode(const std::string& name) {
 if (name == "ValueError") return 2;
 if (name == "TypeError") return 3;
 if (name == "RuntimeError") return 4;
 if (name == "IndexError") return 5;
 if (name == "KeyError") return 6;
 if (name == "ZeroDivisionError") return 7;
 return 1; // Exception (base)
}
```

Runtime exceptions are `fprintf(stderr, ...)` + `exit(1)` - not catchable in many cases. The `setjmp`/`longjmp` machinery exists but only for `try`/`except` blocks.

### Target: Full Python Exception Hierarchy

```
BaseException (0)
├── SystemExit (1)
├── KeyboardInterrupt (2)
├── GeneratorExit (3)
└── Exception (10)
 ├── StopIteration (11)
 ├── ArithmeticError (20)
 │ ├── FloatingPointError (21)
 │ ├── OverflowError (22)
 │ └── ZeroDivisionError (23)
 ├── AssertionError (24)
 ├── AttributeError (25)
 ├── BufferError (26)
 ├── EOFError (27)
 ├── ImportError (30)
 │ └── ModuleNotFoundError (31)
 ├── LookupError (40)
 │ ├── IndexError (41)
 │ └── KeyError (42)
 ├── MemoryError (43)
 ├── NameError (44)
 │ └── UnboundLocalError (45)
 ├── OSError (50)
 │ ├── FileNotFoundError (51)
 │ ├── FileExistsError (52)
 │ ├── IsADirectoryError (53)
 │ ├── NotADirectoryError (54)
 │ ├── PermissionError (55)
 │ ├── TimeoutError (56)
 │ └── ConnectionError (57)
 │ ├── BrokenPipeError (58)
 │ ├── ConnectionAbortedError (59)
 │ ├── ConnectionRefusedError (60)
 │ └── ConnectionResetError (61)
 ├── RuntimeError (70)
 │ ├── NotImplementedError (71)
 │ └── RecursionError (72)
 ├── StopAsyncIteration (73)
 ├── SyntaxError (74)
 ├── TypeError (80)
 ├── ValueError (90)
 │ └── UnicodeError (91)
 │ ├── UnicodeDecodeError (92)
 │ ├── UnicodeEncodeError (93)
 │ └── UnicodeTranslateError (94)
 └── Warning (100)
 ├── DeprecationWarning (101)
 ├── FutureWarning (102)
 ├── ResourceWarning (103)
 ├── RuntimeWarning (104)
 └── UserWarning (105)
```

### Exception Representation

Each exception is a runtime struct:

```c
typedef struct {
 int64_t type_code; // from hierarchy above
 const char* type_name; // "ValueError", "KeyError", etc.
 const char* message; // user message
 const char* filename; // source file where raised
 int64_t lineno; // source line number
 int64_t cause_code; // for "raise X from Y" chaining
 const char* cause_msg; // cause message
} DragonException;
```

### Subtype Matching in `except`

`except ArithmeticError` must catch `ZeroDivisionError`, `OverflowError`, and
`FloatingPointError`. This uses range-based matching:

```cpp
bool isSubtype(int64_t caught, int64_t raised) {
 if (caught == raised) return true;
 // ArithmeticError (20) catches 20-23
 // LookupError (40) catches 40-42
 // OSError (50) catches 50-61
 // etc.
 return raised > caught && raised < nextSiblingCode(caught);
}
```

The type codes are assigned such that children are contiguous ranges after their parent.
This makes `except ParentError` a single range check instead of a linear scan.

### User-Defined Exceptions

```dragon
class HTTPError(Exception) {
 def(status: int, message: str) {
 self.status = status
 self.message = message
 }
}

class NotFoundError(HTTPError) {
 def(resource: str) {
 super(404, f"{resource} not found")
 }
}

// Usage
raise NotFoundError("Customer 42")

try {
 fetch_customer(42)
} except NotFoundError as e {
 print(f"Not found: {e.message}")
} except HTTPError as e {
 print(f"HTTP {e.status}: {e.message}")
}
```

**Implementation:** User-defined exceptions get dynamic type codes starting at 1000. The `excTypeCode` map becomes runtime-extensible. Subtype matching for user exceptions walks the class parent chain (already supported via `classParentNames` in CodeGen).

### Exception Attributes

All exceptions have:

| Attribute | Type | Description |
|-----------|------|-------------|
| `.message` | `str` | Human-readable error message |
| `.args` | `tuple` | Arguments passed to constructor |
| `.__cause__` | `Exception \| None` | From `raise X from Y` |
| `.__traceback__` | `str` | Source location (file:line) |

### `except*` (Exception Groups, PEP 654)

Already parsed. CodeGen needs to: 1. Collect multiple exceptions into a group
2. `except*` matches individual types within the group 3. Unmatched exceptions re-raise

This is Phase 2 - basic `except` must work first.

### Implementation

| Component | Work | LOC |
|-----------|------|-----|
| Runtime: `DragonException` struct | Replace bare int codes with struct | ~50 |
| Runtime: exception hierarchy table | Static array of {code, name, parent_code} | ~80 |
| Runtime: subtype matching | Range-based parent check | ~30 |
| CodeGen: expand `excTypeCode` | Map all 50+ exception names | ~60 |
| CodeGen: user-defined exceptions | Dynamic type codes, parent chain walk | ~80 |
| CodeGen: exception attributes | `.message`, `.args`, `.__cause__` access | ~60 |
| Runtime: `dragon_raise` improvements | Capture filename + lineno from caller | ~40 |
| Tests | Exception catching, hierarchy, user-defined | ~30 |

**Total: ~400 LOC, ~30 tests**

## Part 2: GIL-Free Threading Safety

### The Problem

Python has the Global Interpreter Lock (GIL) - only one thread executes Python code
at a time. This means `list.append` and `dict[key] = value` are effectively atomic
in CPython.

**Dragon has no GIL.** When the HTTP server processes requests on
multiple threads, each thread can simultaneously:
- Read/write to shared `DragonList` (realloc during append = use-after-free)
- Read/write to shared `DragonDict` (rehash during insert = corrupted buckets)
- Read/write to shared `DragonSet` (same as dict)
- Read/write to shared class instance fields (torn reads/writes on non-atomic types)

This will crash. Not "might" - **will**.

### Strategy: Per-Object Granularity, Not Global Lock

A GIL would negate the performance advantage of native compilation. Instead, Dragon
uses per-object locking where needed:

| Approach | When to use | Cost |
|----------|-------------|------|
| **No lock** | Thread-local data, request-scoped (req, res, ctx) | Zero |
| **Atomic operations** | Counters, flags, simple integers | ~1ns per op |
| **Mutex per object** | Shared mutable collections (lists, dicts, sets) | ~25ns per lock/unlock |
| **Read-write lock** | Read-heavy shared collections | ~15ns read, ~25ns write |
| **Lock-free structures** | High-contention queues (thread pool work queue) | Complex but fast |

### Thread-Local by Default

The critical insight (obvious once you say it out loud): **most data in a web server is request-scoped.** The `(req, res, ctx)` trio in is per-request on one thread. Never shared. Most Dragon code needs zero locking.

Only explicitly shared state needs protection:
- App-level storage (`app.keep("db", pool)`)
- Global/module-level mutable variables
- Objects passed between threads via channels

### Shared Collections

When a collection IS shared, Dragon provides thread-safe variants:

```dragon
from threading import Lock, RWLock
from collections.concurrent import SyncList, SyncDict

// Explicit thread-safe collections
const users: SyncList[str] = SyncList
users.append("Alice") // internally mutex-protected

// Or manual locking
const lock: Lock = Lock
const data: list[int] = []

def worker -> None {
 lock.reserve // blocking - waits until lock is available
 data.append(42)
 lock.release
}

// Non-blocking variant
def try_worker -> None {
 if lock.acquire { // returns bool - immediate yes/no
 data.append(42)
 lock.release
 } else {
 print("lock busy, skipping")
 }
}

// Best pattern - scoped (always prefer this)
def worker -> None {
 with lock {
 data.append(42)
 }
}
```

### Single-Operation Atomicity (SyncList/SyncDict)

`SyncList` and `SyncDict` are **single-operation atomic** - each individual method call
(append, pop, get, set) acquires and releases the internal lock. This means:

**Safe - no state dependency:**

```dragon
const data: SyncList[int] = SyncList
data.append(42) // lock, append, unlock - always safe
data.append(43) // lock, append, unlock - always safe
data.pop // lock, pop, unlock - may get empty-list error, but no corruption
```

**Broken - state-dependent logic (TOCTOU):**

```dragon
// DON'T DO THIS - race condition between len and pop
if len(data) > 0 { // lock, read len=1, unlock
 // ← another thread pops here, list is now empty
 const val = data.pop // lock, finds empty list, CRASH
}
```

**The gap between `len` and `pop` is unprotected.** This is a time-of-check-to-
time-of-use (TOCTOU) bug. Each operation is atomic but the combination is not.

**Correct - use explicit locking for multi-step logic:**

```dragon
const raw: list[int] = []
const lock: Lock = Lock

with lock {
 if len(raw) > 0 { // check
 const val = raw.pop // act - still inside same lock, guaranteed safe
 }
}
```

**Rule of thumb:** `SyncList`/`SyncDict` for fire-and-forget operations (append, set).
`with lock {}` for any read-then-act pattern.

### The Compiler's Role

In `.dr` mode, the compiler can **detect potential races** at compile time:

1. If a module-level mutable variable is accessed inside a function that could run on a thread pool pool (e.g., an HTTP handler), emit a warning:```
 warning: shared mutable access to 'counter' without synchronization
 hint: use 'const' for immutable data, or 'SyncDict' for thread-safe access
 ```

2. `const` bindings are inherently thread-safe - immutable data needs
no locking. This is another reason to prefer `const` in Dragon.

3. The `(req, res, ctx)` trio in HTTP handlers is provably thread-local (freshly allocated per request) - no warning needed.

### The `fire` Keyword

Thread creation is a first-class language feature via the `fire` keyword. No imports
needed - `fire` is a builtin that spawns an OS thread and returns a `Thread` handle.

The name is both descriptive ("fire off a task") and on-brand (Dragon fires).

#### Two Forms

**Form 1: Fire a function call**

```dragon
// Fire a named function - most common usage
const t = fire fetch_user(42)

// State fields (read-only atomic bools, set by runtime)
t.started // true - fire always starts immediately
t.done // false - not finished yet

// Result - blocks until thread finishes, returns value
const user: User = t.result

// After completion
t.done // true

// Fire and forget - no handle needed
fire log_event("user_login", user_id)
```

**Form 2: Fire a block (anonymous closure)**

```dragon
// Fire a block - runs on a new thread immediately
fire {
 cleanup_temp_files
 send_notification("done")
}

// Fire a block with a handle
const t = fire {
 const result = heavy_computation
 return result
}
const val = t.result
```

#### Block Capture Semantics

`fire {}` blocks capture variables from the enclosing scope. The capture rule is simple: **`const` shares, everything else copies.**

- **`const` bindings** - shared by reference, zero copy (immutable = thread-safe)
- **Mutable bindings** - snapshotted (value copied) at the moment `fire` is reached

```dragon
const config = load_config // immutable - shared, no copy
counter: int = 0 // mutable - snapshotted

fire {
 print(config.db_host) // shared reference - zero cost
 print(counter) // copied value of 0 at fire-time
}

counter = 5 // does NOT affect the fired block
```

This makes loops safe by default:

```dragon
for i in range(10) {
 fire {
 print(i) // i is mutable, snapshotted per iteration
 }
}
// prints 0,1,2...9 - each thread got its own copy of i
```

The compiler determines `const` vs mutable at compile time - no annotations needed, no runtime overhead for the decision. `const` bindings are another reason to prefer `const` in Dragon: they're not just immutable, they're zero-copy across threads.

#### How `fire` Works

`fire fn(args)` or `fire { body }` is syntactic sugar for: 1. Snapshot any captured mutable variables (copy their current values) 2. Create a `Thread` struct with function pointer (or closure) + args/captures 3. Call `pthread_create` to spawn the thread immediately 4. Return the `Thread` handle (`.started` is always `true`)

The `Thread` handle is just a struct - it does not need to be imported. `fire` always
starts immediately, so `.started` is always `true` on the returned handle. It exists
for cases where a `Thread` is constructed manually (see Advanced section below).

#### Thread State

| State | `started` | `done` |
|-------|-----------|--------|
| Fired (executing) | `true` | `false` |
| Finished | `true` | `true` |

`started` and `done` are atomic bool fields set by the runtime - safe to read from
any thread without locking.

#### Parallel Execution

```dragon
// Fire three threads - all start immediately
const t1 = fire fetch_users
const t2 = fire fetch_orders(customer_id)
const t3 = fire send_email("hi@test.com", "Welcome")

// Collect results - each .result blocks only if that thread isn't done yet
const users = t1.result
const orders = t2.result
const email_status = t3.result

// Check without blocking
if t1.done {
 const users = t1.result // instant - already done
}

// Mix named calls and blocks
const t4 = fire fetch_users
const t5 = fire {
 const raw = read_file("data.csv")
 return parse_csv(raw)
}
```

#### Advanced: Manual Thread Construction

For cases where you need to bind a function now but start later, the `Thread` class is available from the `threading` module:

```dragon
from threading import Thread

const t: Thread = Thread(fetch_user) // bind - does not start
t.started // false
t.start(42) // now it starts
t.started // true
```

Most code should use `fire` - `Thread` exists for edge cases like deferred execution or thread pools.

#### Thread API Summary

| Syntax | What it does | Returns |
|--------|-------------|---------|
| `fire fn(args)` | Spawn thread with function call | `Thread` |
| `fire { body }` | Spawn thread with anonymous block | `Thread` |
| `t.result` | Block until done, return value | `T` |
| `t.started` | Has thread been started? | `bool` |
| `t.done` | Has thread finished? | `bool` |
| `Thread(fn)` | Bind function (deferred start) | `Thread` |
| `t.start(...args)` | Start a deferred thread | `None` |

### Lock & Synchronization Primitives

```dragon
from threading import Lock, RWLock, Event, Semaphore, Barrier

// Mutex
const lock: Lock = Lock
lock.reserve // blocking - waits until lock is available
lock.release

if lock.acquire { // non-blocking - returns bool immediately
 // ... critical section ...
 lock.release
}

// Scoped - best pattern (auto-releases even on exception)
with lock {
 // ... critical section ...
}

// Read-write lock (multiple readers OR one writer)
const rw: RWLock = RWLock
rw.read_reserve // blocking - multiple threads can hold this
rw.read_release
rw.write_reserve // blocking - exclusive
rw.write_release

if rw.read_acquire { rw.read_release } // non-blocking read
if rw.write_acquire { rw.write_release } // non-blocking write

// Event (signal between threads)
const event: Event = Event
event.wait // block until set
event.set // wake all waiters
event.clear // reset

// Semaphore
const sem: Semaphore = Semaphore(max_count=5)
sem.reserve // blocking - waits for available slot
sem.release

if sem.acquire { // non-blocking
 sem.release
}
```

### Lock API Summary

| Method | Behavior | Returns |
|--------|----------|---------|
| `lock.reserve` | Blocks until acquired | `None` |
| `lock.acquire` | Non-blocking attempt | `bool` |
| `lock.release` | Unlock | `None` |
| `with lock { }` | Scoped reserve + auto-release | - |

The naming convention is consistent across all primitives: `reserve` blocks, `acquire` returns immediately with a `bool`. The rationale: "reserve" implies patience (you reserve a table, you wait), "acquire" implies a grab (you either get it or you don't).

### Runtime Implementation

| Primitive | Maps to | Notes |
|-----------|---------|-------|
| `Lock` | `pthread_mutex_t` | `reserve` = blocking, `acquire` = non-blocking bool |
| `RWLock` | `pthread_rwlock_t` | Reader-writer lock |
| `Event` | `pthread_cond_t` + flag | Condition variable |
| `Semaphore` | `sem_t` (POSIX) | `reserve` = blocking, `acquire` = non-blocking bool |
| `Barrier` | `pthread_barrier_t` | N-thread synchronization |
| `fire` / `Thread` | `pthread_create` | `fire` = keyword, `Thread` = deferred; `.result` blocks |
| `SyncList` | `DragonList` + `pthread_mutex_t` | Wrapper with internal lock; single-op atomic |
| `SyncDict` | `DragonDict` + `pthread_rwlock_t` | RW lock for read-heavy; single-op atomic |

### `.py` Mode

Same primitives, Python syntax. `fire` keyword and `reserve`/`acquire` naming are consistent across both modes:

```python
from threading import Lock
from collections.concurrent import SyncList

lock = Lock
data = SyncList

def worker(x):
 with lock:
 raw_list.append(x)
 return x * 2

# fire a named function
t = fire worker(42)

if t.done:
 print(t.result) # instant
else:
 result = t.result # blocks until done

# Fire and forget
fire log_event("login", user_id)

# Fire a block (.py mode uses indentation)
fire:
 cleanup_temp_files
 send_notification("done")

# Fire a block with handle
t = fire:
 result = heavy_computation
 return result

# Lock API - same reserve/acquire convention
lock.reserve # blocking
lock.release

if lock.acquire: # non-blocking bool
 lock.release
```

### Implementation

| Component | Work | LOC |
|-----------|------|-----|
| Runtime: Lock, RWLock, Event, Semaphore, Barrier | pthread wrappers | ~200 |
| Runtime: Thread (result, state fields) | pthread_create, result storage, atomic bools | ~120 |
| Parser + CodeGen: `fire` keyword | `FireExpr` (call + block forms), capture analysis, snapshot emit | ~60 |
| Runtime: SyncList, SyncDict | Mutex-wrapped collections, single-op atomic | ~300 |
| CodeGen: `with lock:` | Phase D (`__enter__`/`__exit__`) | Included in 010 |
| StdlibRegistry: `threading` module | Module registration | ~30 |
| Compiler: shared-access warnings | Detect module-level mutation in threaded contexts | ~150 (Phase 2) |
| Tests | Lock contention, thread safety, race detection | ~25 |

**Total: ~860 LOC, ~25 tests**

## Part 3: C/C++ FFI

### Motivation

Dragon needs to call C libraries: - **libuv** for event-loop-based I/O (Phase 2) - **OpenSSL/LibreSSL** for TLS
- **libpq** for PostgreSQL - **SQLite3** for embedded databases - **PCRE2** for regex - Any C library the user wants

### Syntax

```dragon
// Declare external C function (no body)
extern "C" def malloc(size: int) -> ptr
extern "C" def free(p: ptr) -> None
extern "C" def strlen(s: str) -> int

// With library hint for the linker
extern "C" from "uv" {
 def uv_loop_init(loop: ptr) -> int
 def uv_run(loop: ptr, mode: int) -> int
 def uv_tcp_init(loop: ptr, handle: ptr) -> int
 def uv_tcp_bind(handle: ptr, addr: ptr, flags: int) -> int
 def uv_listen(stream: ptr, backlog: int, cb: ptr) -> int
 def uv_close(handle: ptr, cb: ptr) -> None
}

// Usage - just call them
const loop: ptr = malloc(1024)
uv_loop_init(loop)
```

### Type Mapping for FFI

| Dragon type | C type | LLVM type |
|-------------|--------|-----------|
| `int` | `int64_t` | `i64` |
| `float` | `double` | `double` |
| `bool` | `int64_t` (0 or 1) | `i64` |
| `str` | `const char*` | `i8*` |
| `bytes` | `DragonBytes*` | `%DragonBytes*` |
| `ptr` | `void*` | `i8*` |
| `None` | `void` | `void` |
| `fn(...) -> T` | Function pointer | `ptr` |

The `ptr` type is new - an opaque pointer for FFI. It's `i8*` in LLVM, not
type-checked beyond "is a pointer." This is intentionally unsafe - FFI is the
boundary where Dragon trusts the programmer.

### Compilation and Linking

```bash
# Dragon compiles to object code, then links with external lib
dr build server.dr -l uv # links with -luv
dr build crypto.dr -l ssl -l crypto # links with -lssl -lcrypto
dr build app.dr -L /opt/lib -l pq # custom library path
```

The `-l` and `-L` flags pass through to the `cc` linker invocation that Dragon already uses.

### Structs for FFI

For C structs, Dragon needs to match the memory layout:

```dragon
extern "C" struct sockaddr_in {
 sin_family: int // actually uint16_t - needs size annotations
 sin_port: int // uint16_t
 sin_addr: int // uint32_t
 sin_zero: bytes // char[8]
}
```

This is Phase 2 - for Phase 1, opaque `ptr` handles cover most use cases (libuv passes opaque handles everywhere).

### `.py` Mode

Same syntax - `extern "C"` is valid in both modes:

```python
extern "C" def sqlite3_open(filename: str, db: ptr) -> int
extern "C" def sqlite3_close(db: ptr) -> int
```

### Implementation

| Component | Work | LOC |
|-----------|------|-----|
| Parser: `extern "C" def` declarations | New statement type | ~40 |
| Parser: `extern "C" from "lib" { }` blocks | Grouped declarations | ~30 |
| AST: `ExternDecl` node | Function signature without body | ~15 |
| CodeGen: emit LLVM declarations (not definitions) | `module->getOrInsertFunction` | ~30 |
| TypeChecker: validate FFI signatures | Check param/return types are FFI-safe | ~20 |
| Token: `ptr` type keyword | New primitive type | ~5 |
| CLI: `-l` and `-L` linker flags | Pass through to `cc` invocation | ~15 |
| Tests | FFI declarations, linking, calls | ~15 |

**Total: ~170 LOC, ~15 tests**

## Self-Hosting Strategy: Stdlib in Dragon

### Philosophy

Dragon's stdlib should be written in Dragon wherever possible. C/C++ is reserved for the lowest layer - OS primitives and performance-critical data structures. Everything above that is `.dr` code.

### Layer Architecture

```
┌─────────────────────────────────────────────────────┐
│ Layer 3: Pure Dragon Stdlib (.dr files) │
│ datetime.dr, json.dr, pathlib.dr, collections.dr │
│ http/server.dr, http/router.dr, http/cors.dr │
│ testing/mock.dr, testing/assert.dr │
├─────────────────────────────────────────────────────┤
│ Layer 2: Dragon + FFI (.dr files calling C) │
│ os.dr (calls POSIX via extern "C") │
│ threading.dr (calls pthreads via extern "C") │
│ http/_tcp.dr (calls socket/bind/listen via FFI) │
│ crypto.dr (calls OpenSSL via extern "C") │
├─────────────────────────────────────────────────────┤
│ Layer 1: C Runtime (libdragon_runtime.a) │
│ DragonList, DragonDict, DragonSet, DragonBytes │
│ DragonTuple, string ops, memory allocation │
│ setjmp/longjmp exception machinery │
│ Type tags, format strings, I/O primitives │
├─────────────────────────────────────────────────────┤
│ Layer 0: OS / libc │
│ malloc, free, read, write, socket, pthread_* │
└─────────────────────────────────────────────────────┘
```

### What Moves to Dragon

| Module | Currently | Target | Notes |
|--------|-----------|--------|-------|
| `datetime` | Not implemented | Layer 3 (pure .dr) | Calls `_time` FFI for clock |
| `json` | Not implemented | Layer 3 (pure .dr) | String parsing in Dragon |
| `os.path` | Not implemented | Layer 3 (pure .dr) | String manipulation |
| `os` (syscalls) | Partial in StdlibRegistry | Layer 2 (.dr + FFI) | `extern "C" def mkdir(...)` |
| `http.Router` | Not implemented | Layer 3 (pure .dr) | Class with route table |
| `http._tcp` | Not implemented | Layer 2 (.dr + FFI) | `extern "C" def socket(...)` |
| `threading` | Not implemented | Layer 2 (.dr + FFI) | `extern "C" def pthread_create(...)` |
| `collections` | Not implemented | Layer 3 (pure .dr) | Uses existing list/dict |
| Core types | C runtime | Layer 1 (stays C) | Too performance-critical |

### Example: `datetime.dr` (Layer 3)

```dragon
// lib/stdlib/datetime.dr
extern "C" def _time(tloc: ptr) -> int
extern "C" def _localtime_r(timep: ptr, result: ptr) -> ptr
extern "C" def _strftime(s: ptr, max: int, format: str, tm: ptr) -> int

class datetime {
 year: int
 month: int
 day: int
 hour: int
 minute: int
 second: int

 self(year: int, month: int, day: int,
 hour: int = 0, minute: int = 0, second: int = 0) {
 self.year = year
 self.month = month
 self.day = day
 self.hour = hour
 self.minute = minute
 self.second = second
 }

 static def now -> datetime {
 // Call C time + localtime_r via FFI
 const ts: int = _time(None)
 // ... extract fields from tm struct ...
 return datetime(year, month, day, hour, minute, second)
 }

 def __str__ -> str {
 return f"{self.year}-{self.month:02d}-{self.day:02d} " +
 f"{self.hour:02d}:{self.minute:02d}:{self.second:02d}"
 }

 def __eq__(other: datetime) -> bool {
 return self.year == other.year and self.month == other.month and
 self.day == other.day and self.hour == other.hour and
 self.minute == other.minute and self.second == other.second
 }

 def isoformat -> str {
 return f"{self.year}-{self.month:02d}-{self.day:02d}T" +
 f"{self.hour:02d}:{self.minute:02d}:{self.second:02d}"
 }
}
```

### Benefits

- **Dogfooding:** Every stdlib module is a test of the compiler - **Debuggable:** Developers can read `.dr` stdlib source, not C - **Extensible:** Contributing to stdlib doesn't require C++ knowledge - **Type-safe:** Dragon's type checker validates stdlib code like user code - **Fast:** Compiles to the same LLVM IR as hand-written C, optimizer handles the rest

## Implementation Plan

| Phase | Feature | LOC | Tests | Depends On |
|-------|---------|-----|-------|------------|
| 1 | Exception hierarchy (50+ types) | ~220 | ~15 | Nothing |
| 2 | User-defined exceptions | ~140 | ~10 | Phase 1 |
| 3 | Exception attributes (.message, .__cause__) | ~60 | ~5 | Phase 1 |
| 4 | C FFI (`extern "C" def`) | ~170 | ~15 | Nothing |
| 5 | `fire` keyword (call + block) + Lock/Thread | ~380 | ~15 | Phase 4 (pthreads FFI) |
| 6 | SyncList, SyncDict | ~300 | ~10 | Phase 5 |
| 7 | Compiler shared-access warnings | ~150 | ~5 | Phase 5 |

**Total: ~1,420 LOC, ~75 tests**

Phases 1-3 (exceptions) and Phase 4 (FFI) are independent and can be developed in parallel. Phases 5-7 (threading) depend on FFI for pthreads access.

## Affected Components

- **Runtime:** `DragonException` struct, exception hierarchy table, subtype matching
- **CodeGen:** Expand `excTypeCode`, user-defined exception codes, `extern "C"` decl
 emission
- **Parser:** `extern "C" def` and `extern "C" from "lib" { }` syntax; `fire` expression
- **AST:** `ExternDecl` node type; `FireExpr` node type (call form + block form with captures)
- **Token:** `FIRE` keyword
- **TypeChecker:** FFI type validation, exception subtype checking
- **CLI:** `-l` and `-L` linker pass-through flags
- **StdlibRegistry:** `threading` module registration
- **Tests:** Exception hierarchy, FFI calls, thread safety
