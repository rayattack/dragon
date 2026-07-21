# Decision 016: Three-Tier Concurrency + Colorless Await

> **Status:** Implemented (with deviations. See Current State). Supersedes the original LLVM-coroutines + colored-async draft.

I got tired of the Python async refactor spiral - one `async def` leaf and suddenly fifty signatures turn red. Bob Nystrom's "what color is your function" essay plus too many late-night gdb sessions on pthread stacks convinced me we need three concurrency tiers and an `await` that doesn't infect every caller:

1. **Green threads (default):** `fire fn` / `fire { block }`. M:N via minicoro. Colorless. I/O yields the context instead of blocking the OS thread. (We ended up on raw epoll/kqueue, not libuv. See deviation below.)
2. **Scoped OS threads:** `thread { block }`. RAII pthread, auto-join at scope exit.
3. **Manual OS threads:** `Thread(target=fn)` with `t.start`, `t.join`, `t.is_alive`. Python `threading.Thread` shape for daemons and custom pools.

**The Dragon Rule** is the bit I'm proudest of: `await` works in *any* function, not just `async def`. Kills the coloring virus, keeps suspension visible at the call site so you're not guessing where context switches happen.

## Current State

| Layer | Status |
|-------|--------|
| Lexer | Done - `ASYNC`/`AWAIT`/`FIRE` tokens recognized |
| Parser | Done - `async def` sets `FunctionDecl.isAsync`, `await expr` → `AwaitExpr`, `fire expr` → `FireExpr` |
| AST | Done - `AwaitExpr`, `FireExpr` nodes + `isAsync` flag exist |
| TypeChecker | **Done** - `Task[T]` type; `fire f` → `Task[R]`, `async def f -> T` → `Task[T]`, `await Task[T]` → `T`, `t.join` → `T`, `t.is_alive` → `bool`. Awaiting a non-Task (e.g. a sync call) is a compile error. |
| CodeGen | **Done** - `fire`/`async def` spawn green threads via `dragon_vthread_spawn_typed` (per-callsite typed trampoline); `await`/`.join` call `dragon_vthread_join` and reinterpret the i64 result slot at the native result type T per (bitcast float, inttoptr ptr-shaped, truncate bool). `Task[T]` erases to a bare `ptr`. |
| Runtime | **Green threads (minicoro M:N)** - `dragon_vthread_spawn_typed/join/yield/sleep/is_alive`, per-vthread exception stacks. I/O yielding via **raw epoll/kqueue** (see deviation below). |
| Stdlib | `threading.dr` - Lock, RWLock, Semaphore, Barrier, Condition, Event |

**`Task[T]` design .** `Task[T]` is an *erased generic*: it carries the
result type `T` through the type system but lowers to a bare `ptr` (the
`DragonVThread*`) at LLVM - zero runtime cost, no boxing . Surface rules: the
binding annotation is **mandatory** (`t: Task[int] = fire work`); bare `t: Task = fire
work` is allowed and the result type is **pinned from the concrete RHS** (a task yields
exactly one typed value), so `t.join` / `await t` recover the native `T`. `t = fire
work` (no annotation) is rejected by the mandatory-declaration rule. This closes the
former soundness hole where `await`/`fire` produced `unknown` and `r: str = await
fetch` (an `int` task) compiled and miscompiled.

**Deviation - raw epoll/kqueue, not libuv.** The libuv integration described below was not adopted. Scheduler I/O uses raw `epoll` (Linux) / `kqueue` (macOS) / `WSAPoll` (Windows) in `lib/Runtime/runtime_concurrency.cpp`. No libuv dep. Libuv section below is historical rationale; behavior matches intent. Occassionally we get asked why not libuv - fewer deps, same epoll/kqueue underneath anyway.

## The Dragon Rule: Separating Color from Clarity

### The Problem with Colored Functions

Python's `async def` / `await` creates two worlds:

```python
# Python: the coloring virus
async def fetch: # RED function
 return await get(url)

async def process: # Must be RED because it calls fetch
 data = await fetch

async def main: # Must be RED because it calls process
 await process

asyncio.run(main) # Only way to call RED from BLUE
```

One async leaf function forces every caller up the chain to become async. You end up with duplicate APIs: `read` and `async_read`, `Lock` and `AsyncLock`. This is the "function coloring" problem described by Bob Nystrom.

### Dragon's Solution: No Coloring, Yes Clarity

Dragon keeps `await` as a **callsite marker** but removes the requirement that the calling function must also be `async def`.

| Language | Parent must be async? (coloring) | Must use await? (clarity) | Suspension visible? |
|----------|----------------------------------|---------------------------|---------------------|
| Python | Yes (viral) | Yes | Yes |
| Go | No | No | No (invisible) |
| **Dragon** | **No** | **Yes** | **Yes** |

### The Rules

1. **`async def` always returns a Task handle** (`DragonVThread*`), not the direct value
2. **`await` is the only way to unwrap a Task handle into a value**
3. **`await` is allowed in ANY function** - `def` or `async def`
4. **`await` on a non-async function is a compile error** - prevents "await drift"

```dragon
async def fetch_data(url: str) -> str {
 // ... libuv-backed I/O, runs on green thread ...
 return "Data"
}

def process {
 // Normal def. Uses 'await' to explicitly mark the context-switch boundary.
 // No coloring virus - process is NOT async.
 result: str = await fetch_data("http://example.com")
 print(result)
}

// The caller of process doesn't need to know or care!
process
```

### Calling async def WITHOUT await

You can call an `async def` without `await` to get the raw Task handle:

```dragon
async def fetch(url: str) -> str { ... }

// With await: blocks current context until done, returns str
data: str = await fetch("http://example.com")

// Without await: returns Task handle immediately (starts the green thread)
task = fetch("http://example.com") // task is Task[str]
// ... do other work ...
data: str = await task // unwrap later
```

### Compiler Errors

**Error 1: `await` on a synchronous function**
```
error: 'do_math' is a synchronous function and cannot be awaited
 --> main.dr:2:10
 |
2 | result = await do_math(5)
 | ^^^^^^^^^^^^^^^^^
 |
note: only 'async def' functions return an awaitable Task
hint: remove 'await' - this function returns its value directly
```

**Error 2: Forgetting `await` when assigning to typed variable**
```
error: cannot assign Task[str] to str
 --> main.dr:2:13
 |
2 | data: str = fetch
 | ^^^^^^^ returns Task[str], not str
 |
hint: use 'await fetch' to unwrap the Task into a str
hint: or use 'task = fetch' to store the Task handle
```

**Error 3: `await` on a non-Task expression**
```
error: 'await' requires a Task expression, got int
 --> main.dr:2:5
 |
2 | y = await x
 | ^^^^^^^ int is not awaitable
 |
note: 'await' can only be used with values returned by 'async def' functions
```

### How await Works at the Backend Level

Since the backend is minicoro (stackful green threads), `await` desugars to:

1. Call the `async def` function (which spawns a green thread, returns `DragonVThread*`) 2. If the task isn't done, call the scheduler to `swapcontext` (yield current context) 3. Once the scheduler wakes us back up, extract the result from the task handle 4. Return the unwrapped value as the expression result

### fire vs async def - When to Use Which

| | `fire fn` | `async def` + `await` |
|---|---|---|
| **Returns** | Task handle (join manually) | Task handle (unwrap with await) |
| **Intent** | "Run this in parallel" | "This does I/O, pause me until done" |
| **Typical use** | CPU-bound fan-out | Sequential I/O, network calls |
| **Joining** | `t.join` (explicit) | `await` (inline) |

```dragon
// Parallel fan-out with fire
t1 = fire compute(data1)
t2 = fire compute(data2)
r1: int = t1.join
r2: int = t2.join

// Sequential I/O with async + await
async def fetch_user(id: int) -> User { ... }
async def fetch_orders(user: User) -> list[Order] { ... }

def show_dashboard(id: int) {
 user = await fetch_user(id) // pause, resume when done
 orders = await fetch_orders(user) // pause, resume when done
 render(user, orders)
}

// Hybrid: parallel async
def show_dashboard_fast(id: int) {
 user_task = fetch_user(id) // start without await (runs in parallel)
 orders_task = fetch_orders_by_id(id) // start without await
 user = await user_task // collect first result
 orders = await orders_task // collect second result
 render(user, orders)
}
```

### Why three tiers (restated)

- **Green threads** (Tier 1): Lightweight, M:N scheduled. ~64KB per thread, supports thousands concurrent. Default for most concurrent work. Colorless - any function works. - **Scoped OS threads** (Tier 2): When you need true hardware parallelism with RAII cleanup. One-shot background tasks that auto-join at scope exit. - **Manual OS threads** (Tier 3): Standard Python `threading.Thread` API for long-running daemons, custom thread pools, or when you need explicit lifecycle control.

### Why not OS threads alone

Dragon already has `fire fn` spawning OS threads . But OS threads are expensive: ~1MB stack each, OS-limited to thousands, context switches cost ~1μs. Green threads cost ~64KB each, support tens of thousands concurrent, and switch in ~200 cycles.

### Why not LLVM coroutines (original)

The original proposed LLVM stackless coroutines (`llvm.coro.*` intrinsics). This
approach has colored functions (Python-style). After analysis, Dragon rejects coloring
because:

- Coloring creates a viral refactoring burden (one async leaf → 50 signature changes)
- Coloring splits the ecosystem (sync Lock vs AsyncLock, sync read vs async read)
- Dragon is a *compiled* language with full control - we can do better

Stackful green threads via minicoro give us colorless concurrency with explicit `await`
as a callsite marker. The `await` keyword serves the same readability purpose without
the signature virus.

## Research: How Other Languages Do It

### Comparison table

| Language | Coroutine Type | Platform I/O | Colored? | Callsite Visible? |
|---|---|---|---|---|
| Rust | Stackless (compiler) | mio | Yes | Yes (`await`) |
| Swift | Stackless (LLVM coro) | libdispatch | Yes (mitigated) | Yes (`await`) |
| Go | Stackful (goroutines) | netpoller | No | No |
| Node.js | Stackless (V8 promises) | libuv | Yes | Yes (`await`) |
| Julia | Stackful (Tasks) | libuv | No | No |
| C++20 | Stackless (LLVM coro) | Boost.Asio | Yes | Yes (`co_await`) |
| Zig | Redesigning | io_uring | No (planned) | TBD |
| **Dragon** | **Stackful (minicoro)** | **libuv** | **No** | **Yes (`await`)** |

Dragon is unique: stackful green threads (no coloring) + explicit `await` (callsite clarity).

### Why minicoro over other stackful approaches

| Option | Pros | Cons |
|--------|------|------|
| **ucontext** | POSIX standard | Deprecated on macOS, signal mask conflicts with setjmp |
| **minicoro** | ~300 LOC, cross-platform (x86-64/ARM64), no signal mask issues | Third-party (but tiny, MIT, vendorable) |
| **Custom asm** | Maximum control | Massive per-platform effort |
| **Go-style** | Growable stacks | Requires GC for pointer adjustment during stack moves |

**Dragon chooses minicoro** because:
- Cross-platform: x86-64, ARM64, RISC-V, WASM via small assembly stubs
- Single-header C, vendorable like SQLite3 and PCRE2
- Raw assembly context switches - no signal mask save/restore (no conflict with setjmp/longjmp exceptions)
- Configurable stack size (default 64KB, guard pages for overflow detection)

## Architecture

### Three-Tier Overview

```
┌──────────────────────────────────────────────────────────────────┐
│ Dragon Source Code │
│ │
│ fire fetch(url) thread { compute } │
│ data = await fetch(url) t = Thread(target=fn) │
│ fire { cleanup } t.start; t.join │
│ │
│ Tier 1: Green Threads Tier 2: Scoped Tier 3: Manual│
└──────────────┬────────────────────┬──────────────────┬───────────┘
 │ │ │
 ▼ ▼ ▼
┌──────────────────────────┐ ┌───────────┐ ┌───────────────────┐
│ DragonScheduler (C) │ │ pthread │ │ DragonOSThread │
│ M:N scheduling │ │ auto-join │ │ start/join/alive │
│ N worker OS threads │ │ at scope │ │ manual lifecycle │
│ run queue + libuv loop │ │ exit │ │ │
├──────────────────────────┤ └─────┬─────┘ └────────┬──────────┘
│ minicoro (~300 LOC C) │ │ │
│ mco_coro + mco_resume │ │ │
│ mco_yield + context asm │ │ │
├──────────────────────────┤ │ │
│ libuv (~400KB static) │ │ │
│ epoll/kqueue/IOCP │ │ │
│ timers, TCP, DNS, FS │ │ │
└──────────────┬───────────┘ │ │
 │ │ │
 └────────────────────┴──────────────────┘
 POSIX pthreads
```

### Per-VThread Exception State

Current exception state is global (NOT thread-safe):

```c
static jmp_buf __dragon_exc_stack[32];
static int __dragon_exc_sp = -1;
static int __dragon_exc_type = 0;
static const char* __dragon_exc_msg = "";
```

For green threads, multiple vthreads share one OS thread. `__thread` TLS is insufficient. The solution is a dual-path approach:

```c
typedef struct DragonVThread {
 mco_coro* coro; // minicoro coroutine handle
 jmp_buf exc_stack[32]; // per-vthread exception stack
 int exc_sp;
 int exc_type;
 const char* exc_msg;
 int64_t result;
 volatile int8_t done;
 struct DragonVThread* next; // scheduler run queue linkage
} DragonVThread;

static __thread DragonVThread* __current_vthread = NULL;

// Exception functions check vthread first, then TLS fallback
void* dragon_exc_push_frame {
 if (__current_vthread) {
 __current_vthread->exc_sp++;
 return &__current_vthread->exc_stack[__current_vthread->exc_sp];
 }
 __tls_exc_sp++;
 return &__tls_exc_stack[__tls_exc_sp];
}
```

### How `async def` Compiles

```dragon
async def fetch(url: str) -> str {
 resp = http_get(url)
 return resp.body
}
```

Compiles to (conceptual): 1. Wrap the function body in a green thread spawn spawn 2. Return `DragonVThread*` handle immediately

```c
// Ramp: spawns green thread, returns handle
DragonVThread* fetch(char* url) {
 DragonVThread* vt = dragon_vthread_spawn(fetch_body, url);
 return vt;
}

// Body: runs on green thread
int64_t fetch_body(char* url) {
 char* resp = http_get(url); // may yield to scheduler via libuv
 return (int64_t)resp;
}
```

### How `await` Compiles

```dragon
data: str = await fetch("http://example.com")
```

Compiles to:
```c
DragonVThread* task = fetch("http://example.com"); // get handle
int64_t result = dragon_vthread_join(task); // block until done
char* data = (char*)result; // extract value
```

If the caller is itself on a green thread, `dragon_vthread_join` yields to the scheduler instead of blocking the OS thread.

## Three Tiers in Detail

### Tier 1: Green Threads (`fire`)

```dragon
// fire a function call - starts green thread, returns Task handle
t = fire compute(data)
result: int = t.join

// fire a block - runs on green thread
fire {
 cleanup_temp_files
 send_notification("done")
}

// fire and forget - no handle
fire log_event("user_login", user_id)

// fire with handle from block
t = fire {
 result = heavy_computation
 return result
}
val: int = t.join
```

Both `fire fn` and `fire { block }` forms are supported. The block form desugars to an anonymous lambda.

### Tier 2: Scoped OS Threads (`thread { }`)

```dragon
// Spawns a real OS thread, auto-joins at scope exit
thread {
 compute_heavy_task
 write_results_to_disk
}
// ^ thread has joined here - guaranteed

// Multiple scoped threads
thread { process_batch_a }
thread { process_batch_b }
// ^ both joined before continuing
```

`thread { block }` compiles to: 1. Create anonymous function from block body 2. `pthread_create` to spawn OS thread 3. Insert `pthread_join` at the end of the enclosing scope (RAII)

### Tier 3: Manual OS Threads (`Thread`)

```dragon
from threading import Thread

t = Thread(target=worker_fn, args=(42, "hello"))
t.start // spawns the OS thread
// ... do other work ...
if t.is_alive {
 print("still running")
}
result = t.join // blocks, returns value (Dragon extension)
```

Standard Python `threading.Thread` API with Dragon's extension that `join` returns the thread function's return value.

## API Normalization

All non-standard threading APIs are normalized to Python conventions:

| Old (non-standard) | New (Python-standard) | Notes |
|----|----|----|
| `t.result` | `t.join` | `join` returns value (Dragon extension) |
| `t.done` | `t.is_alive` | Inverted semantics |
| `t.started` | (removed) | Not needed |
| `Lock.reserve` | `Lock.acquire` | Blocking acquire |
| `Lock.acquire` (trylock) | `Lock.try_lock` | Non-blocking attempt |

## Type System Additions

```
Task[T] - returned by async def, wraps DragonVThread* + type T
```

TypeChecker changes: - `async def foo -> int` has return type `Task[int]` - `await expr` requires operand to be `Task[T]`, result type is `T` - `await` is valid in **any** function (the Dragon Rule - no parent `isAsync` check) - `await` on non-`Task` type is a hard compile error - Assignment of `Task[T]` to `T` without `await` is a type error with helpful hint

## libuv Integration

### Build integration

```cmake
include(FetchContent)
FetchContent_Declare(
 libuv
 GIT_REPOSITORY https://github.com/libuv/libuv.git
 GIT_TAG v1.49.2
)
FetchContent_MakeAvailable(libuv)
target_link_libraries(dragon_runtime uv_a) # static link
```

### What libuv gives us

| Feature | libuv API | Dragon use |
|---|---|---|
| Event loop | `uv_run` | Scheduler I/O loop |
| Timers | `uv_timer_start` | `sleep` in green threads |
| TCP | `uv_tcp_connect`, `uv_read_start` | Network I/O |
| UDP | `uv_udp_send`, `uv_udp_recv_start` | UDP sockets |
| DNS | `uv_getaddrinfo` | Name resolution |
| Filesystem | `uv_fs_open`, `uv_fs_read` | Non-blocking file I/O |
| Child processes | `uv_spawn` | Subprocess management |
| Thread pool | `uv_queue_work` | CPU-bound offloading |

### Platform coverage

| Platform | I/O Backend | Status |
|---|---|---|
| Linux | epoll (+ io_uring on 5.13+) | Primary target |
| macOS | kqueue | Supported |
| Windows | IOCP | Supported |
| FreeBSD/OpenBSD | kqueue | Supported |

### Size budget

| Component | Size |
|---|---|
| libuv static lib | ~300-500 KB |
| minicoro | ~500 lines (vendored) |
| Dragon scheduler + I/O glue | ~800-1000 lines C |
| **Total addition** | **~500 KB** |

## Implementation Phases

### Phase 0: API Normalization

Rename all non-standard APIs. No behavioral changes.

**Outputs:** - `t.result` → `t.join`, `t.done` → `t.is_alive` in CodeGen dispatch - `Lock.reserve` → `Lock.acquire`, `Lock.acquire` → `Lock.try_lock` in stdlib - All ~24 threading tests updated

### Phase 1: Thread-Safe Exception State

Make exceptions safe for concurrent execution. **Blocks all subsequent phases.**

**Outputs:** - Convert 4 global exception statics to `__thread` TLS - Add `DragonVThread` struct with per-vthread exception state - Add `__thread DragonVThread* __current_vthread = NULL` - All 5 exception functions check vthread first, TLS fallback

### Phase 2: `thread { block }` Scoped OS Threads (Tier 2)

Add the simplest new tier.

**Outputs:**
- `THREAD` token + keyword
- `ThreadStmt` AST node with body block
- Parser: `threadStatement` - parse `thread { block }`
- CodeGen: ThreadStmt visitor - anon fn + pthread_create + auto-join at scope exit

### Phase 3: Green Thread Runtime (Tier 1)

Transform `fire` from OS threads to M:N green threads.

**Outputs:**
- Vendor minicoro (`lib/Minicoro/minicoro.h`)
- `DragonScheduler`: run queue, N worker OS threads, lazy init via `pthread_once`
- `dragon_vthread_spawn`, `dragon_vthread_yield`, `dragon_vthread_join`, `dragon_vthread_is_alive`
- `fire` keyword calls `dragon_vthread_spawn` instead of `dragon_thread_fire`
- `fire { block }` form (desugar to anonymous lambda)
- Both `fire fn` and `fire { block }` supported

### Phase 4: Colorless async/await (The Dragon Rule) - done Implemented

Implement await without function coloring.

**Outputs (all done):**
- done `Task[T]` type in TypeChecker (`Kind::Task` + `TaskType`) - an *erased generic*: carries result type `T` at the type level, lowers to a bare `ptr` (`DragonVThread*`) at LLVM. Zero runtime cost, no boxing .
- done `async def foo -> T` gets return type `Task[T]`; `fire f` gets `Task[<R of f>]`.
- done `async def` / `fire` CodeGen: spawn via `dragon_vthread_spawn_typed` (per-callsite typed trampoline), return handle.
- done `await expr` / `.join` CodeGen: call `dragon_vthread_join(handle)`, then reinterpret the i64 result slot at the native result type `T` (bitcast float, inttoptr ptr-shaped, truncate bool - the inverse of `resultToI64`, *not* `coerceArg` which would int-convert floats).
- done `await` validation: operand must be `Task[T]` (result type `T`), allowed in ANY function; awaiting a non-Task (e.g. a sync call) is a hard compile error.

**Surface rule (refinement of the original draft):** the binding annotation is **mandatory** - `t: Task[int] = fire work`. Bare `t: Task = fire work` is allowed and the result type is pinned from the concrete RHS (a task yields exactly one typed value). `t = fire work` (no annotation) is rejected by Dragon's mandatory-declaration rule (`:` declares, `=` reassigns). This deliberately drops the "infer the handle type, no annotation needed" idea - typing is not optional.

### Phase 5: Thread Class + SyncList/Dict Wrappers (Tier 3)

Manual Thread API + stdlib concurrent collection wrappers.

**Outputs:**
- `Thread` class in `stdlib/threading.dr` with `start`, `join`, `is_alive`
- Runtime: `dragon_osthread_new/start/join/is_alive`
- `stdlib/collections/concurrent.dr` - Dragon class wrappers for SyncList/SyncDict
- Builtins remain for performance; stdlib wrappers for extensibility

### Phase 6: libuv I/O Yielding

Green threads yield on I/O instead of blocking OS worker threads.

**Outputs:**
- libuv via CMake FetchContent (v1.49.2, static link)
- I/O interception: `dragon_vthread_sleep`, `dragon_vthread_read`, `dragon_vthread_write`
- I/O functions check `__current_vthread`: non-NULL → libuv path; NULL → blocking
- Stdlib I/O functions updated to use vthread-aware runtime calls

### Phase 7: Cleanup

Remove deprecated code, finalize.

**Outputs:**
- Remove old `dragon_thread_fire/join/is_done`
- Remove legacy `__Thread` CodeGen dispatch
- Update `needsPthread` → `needsThreading`

## Alternatives Considered

### LLVM stackless coroutines (original)

LLVM `llvm.coro.*` intrinsics transform `async def` into zero-cost state machines.
Same approach as Swift and C++20.

**Rejected:** Requires colored functions. Dragon values developer experience over
micro-optimization. Stackful green threads (64KB) are lightweight enough for all
practical workloads. The coloring virus is a worse cost than 64KB per task.

### Go-style goroutines

Stackful coroutines with growable stacks (2KB→1GB), M:N scheduling, work-stealing,
preemption.

**Rejected:** Requires GC-aware pointer adjustment during stack growth (Dragon has no
GC). Also provides no callsite clarity - suspension is invisible.

### setjmp/longjmp coroutines

Dragon already uses setjmp/longjmp for exceptions. Could reuse for coroutines.

**Rejected:** Conflicts with existing exception handling. Signal mask save/restore
adds overhead. minicoro's raw assembly context switches are cleaner.

### ucontext_t

POSIX standard for context switching.

**Rejected:** Deprecated on macOS. Signal mask save/restore conflicts with
setjmp/longjmp exception handling. minicoro avoids both issues.

### Pure colored async/await (Python-style)

`await` only inside `async def`. Standard Python compatibility.

**Rejected:** Dragon is a *typed, compiled Python variant* - it can improve on Python's
design. The coloring virus is the single most complained-about aspect of Python's async.
Dragon keeps `await` syntax while eliminating the coloring constraint.

### No async/await (threads only)

Use OS threads for all concurrency.

**Rejected:** ~1MB per thread, OS-limited to thousands. Cannot support modern server
workloads (10K+ connections).

## Risk Assessment

| Risk | Severity | Mitigation |
|---|---|---|
| Per-vthread exceptions corrupt state | **Critical** | Dual-path: vthread struct + TLS fallback. Concurrent try/except tests |
| Green thread stack overflow (64KB) | High | Guard pages via mprotect. Configurable stack size |
| Deadlock: vthread holds mutex and yields | High | Document: Lock must not be held across yield |
| libuv + M:N scheduler interaction | **Very High** | Dedicated I/O thread pattern. Integration tests |
| `await` type validation edge cases | Medium | Thorough TypeChecker tests for Task[T] |
| M:N scheduler correctness | High | Start single worker thread, scale to N later |

## References

- [What Color is Your Function? - Bob Nystrom](https://journal.stuffwithstuff.com/2015/02/01/what-color-is-your-function)
- [libuv documentation](https://docs.libuv.org/en/v1.x)
- [minicoro - Minimal coroutine library](https://github.com/edubart/minicoro)
- [LLVM Coroutines documentation](https://llvm.org/docs/Coroutines.html)
- [Async Functions in Swift - LLVM Dev Meeting 2021](https://llvm.org/devmtg/2021-11/slides/2021-AsyncFunctionsInSwift.pdf)
- [Rust Async Book](https://rust-lang.github.io/async-book)
- [Zig's New Async I/O - Loris Cro](https://kristoff.it/blog/zig-new-async-io)
- [How Stacks are Handled in Go - Cloudflare](https://blog.cloudflare.com/how-stacks-are-handled-in-go)
- [uvloop: Python asyncio on libuv](https://github.com/MagicStack/uvloop)
- [Project Loom: Virtual Threads in Java](https://openjdk.org/jeps/444)
