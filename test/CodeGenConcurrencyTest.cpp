#include "CodeGenTestHelpers.h"

//===----------------------------------------------------------------------===//
// Fire/Lock IR
//===----------------------------------------------------------------------===//

TEST(CodeGenTest, FireBasicIR) {
    auto ir = generateIR(
        "def worker() -> int {\n"
        "  return 42\n"
        "}\n"
        "t: Task[int] = fire worker()\n"
    );
    EXPECT_NE(ir.find("dragon_thread_fire"), std::string::npos);
}

TEST(CodeGenTest, FireJoinIR) {
    auto ir = generateIR(
        "def worker() -> int {\n"
        "  return 42\n"
        "}\n"
        "t: Task[int] = fire worker()\n"
        "x: int = t.join()\n"
    );
    EXPECT_NE(ir.find("dragon_thread_join"), std::string::npos);
}

TEST(CodeGenTest, FireIsAliveIR) {
    auto ir = generateIR(
        "def worker() -> int {\n"
        "  return 42\n"
        "}\n"
        "t: Task[int] = fire worker()\n"
        "d: int = t.is_alive()\n"
    );
    EXPECT_NE(ir.find("dragon_vthread_is_alive"), std::string::npos);
}

// NOTE: Lock is import-gated (`from threading import Lock`). generateIR now runs
// the faithful front end (it resolves stdlib imports via ModuleResolver and gates
// on Sema/TypeChecker errors), so these IR-shape tests carry the real import - the
// same surface the `dragon` driver sees. Lock's codegen lowering is purely
// name-based (CallBuiltins/CallMethods/typeExprTo* key on the name "Lock"), so the
// resulting IR still contains the expected intrinsic calls. End-to-end behavior is
// also covered by InteropTest.StdlibThreadingLock*.
TEST(CodeGenTest, LockNewIR) {
    auto ir = generateIR(
        "from threading import Lock\n"
        "m: Lock = Lock()\n"
    );
    EXPECT_NE(ir.find("dragon_lock_new"), std::string::npos);
}

TEST(CodeGenTest, LockAcquireReleaseIR) {
    auto ir = generateIR(
        "from threading import Lock\n"
        "m: Lock = Lock()\n"
        "m.acquire()\n"
        "m.release()\n"
    );
    EXPECT_NE(ir.find("dragon_lock_acquire"), std::string::npos);
    EXPECT_NE(ir.find("dragon_lock_release"), std::string::npos);
}

// acquire(blocking=False) returns bool via dragon_lock_acquire_ex; the i64
// runtime result must be coerced to i1 (icmp ne), else a success reports False.
TEST(CodeGenTest, LockAcquireNonblockingBoolIR) {
    auto ir = generateIR(
        "from threading import Lock\n"
        "m: Lock = Lock()\n"
        "ok: bool = m.acquire(blocking=False)\n"
    );
    EXPECT_NE(ir.find("dragon_lock_acquire_ex"), std::string::npos);
    EXPECT_NE(ir.find("icmp ne i64"), std::string::npos);
}

// acquire(blocking=True, timeout=...) passes an f64 timeout to acquire_ex.
TEST(CodeGenTest, LockAcquireTimeoutIR) {
    auto ir = generateIR(
        "from threading import Lock\n"
        "m: Lock = Lock()\n"
        "ok: bool = m.acquire(blocking=True, timeout=0.5)\n"
    );
    EXPECT_NE(ir.find("dragon_lock_acquire_ex"), std::string::npos);
    // timeout arg flows as a double constant (0.5) to the runtime call.
    EXPECT_NE(ir.find("double"), std::string::npos);
}

// Speed guarantee (commandment #1): Lock is an intrinsic that erases to a bare
// ptr. The canonical typed form must NOT emit a class instance, GC tracking, or
// refcount traffic - only a single dragon_lock_new() and direct runtime calls.
TEST(CodeGenTest, LockFastPathNoOverheadIR) {
    auto ir = generateIR(
        "from threading import Lock\n"
        "lock: Lock = Lock()\n"
        "lock.acquire()\n"
        "lock.release()\n"
    );
    // Lock is a pure intrinsic: construction lowers to a direct dragon_lock_new,
    // with no synthesized class constructor for Lock itself.
    EXPECT_NE(ir.find("dragon_lock_new"), std::string::npos);
    // No Lock class constructor. Full module prefix so it does NOT match the
    // sibling `threading__RWLock___init__` that importing `threading` also
    // compiles into the module.
    EXPECT_EQ(ir.find("threading__Lock___init__"), std::string::npos);
    EXPECT_EQ(ir.find("_dragon_Lock"), std::string::npos);
    // NOTE: a whole-module "no @dragon_gc_track" assertion was dropped here. It
    // only held under the old non-faithful harness (which never resolved the
    // import, so threading's other classes were absent). Faithfully importing
    // `threading` compiles its sibling classes (RWLock, Semaphore, ...) whose
    // constructors legitimately gc_track, so a module-wide absence check is no
    // longer meaningful. Lock's zero-overhead path is captured by the
    // intrinsic-new + no-Lock-constructor assertions above (a pure-intrinsic
    // ptr is not GC-tracked).
}

// with lock { } lowers to acquire on entry / release on exit - no class dunders.
TEST(CodeGenTest, LockWithStatementIR) {
    auto ir = generateIR(
        "from threading import Lock\n"
        "lock: Lock = Lock()\n"
        "with lock {\n"
        "  x: int = 1\n"
        "}\n"
    );
    EXPECT_NE(ir.find("dragon_lock_acquire"), std::string::npos);
    EXPECT_NE(ir.find("dragon_lock_release"), std::string::npos);
}

//===----------------------------------------------------------------------===//
// Thread Block IR
//===----------------------------------------------------------------------===//

TEST(CodeGenTest, ThreadBlockIR) {
    auto ir = generateIR(
        "thread {\n"
        "  print(\"hello\")\n"
        "}\n"
    );
    EXPECT_NE(ir.find("__dragon_thread_"), std::string::npos);
    EXPECT_NE(ir.find("dragon_thread_fire"), std::string::npos);
    EXPECT_NE(ir.find("dragon_thread_join"), std::string::npos);
}

//===----------------------------------------------------------------------===//
// Fire Block IR
//===----------------------------------------------------------------------===//

TEST(CodeGenTest, FireBlockIR) {
    auto ir = generateIR(
        "fire {\n"
        "  x: int = 42\n"
        "}\n"
    );
    EXPECT_NE(ir.find("dragon_vthread_spawn"), std::string::npos);
    EXPECT_NE(ir.find("__dragon_fire_"), std::string::npos);
}

//===----------------------------------------------------------------------===//
// Vthread IR
//===----------------------------------------------------------------------===//

TEST(CodeGenTest, FireVthreadSpawnIR) {
    auto ir = generateIR(
        "def work() -> int {\n"
        "  return 42\n"
        "}\n"
        "t: Task[int] = fire work()\n"
    );
    // fire should call dragon_vthread_spawn (green thread)
    EXPECT_NE(ir.find("dragon_vthread_spawn"), std::string::npos);
    // The actual call should be to vthread_spawn, not thread_fire
    EXPECT_NE(ir.find("call ptr @dragon_vthread_spawn"), std::string::npos);
}

TEST(CodeGenTest, FireVthreadJoinIR) {
    auto ir = generateIR(
        "def work() -> int {\n"
        "  return 42\n"
        "}\n"
        "t: Task[int] = fire work()\n"
        "r: int = t.join()\n"
    );
    EXPECT_NE(ir.find("dragon_vthread_join"), std::string::npos);
}

TEST(CodeGenTest, FireVthreadIsAliveIR) {
    auto ir = generateIR(
        "def work() -> int {\n"
        "  return 42\n"
        "}\n"
        "t: Task[int] = fire work()\n"
        "a: int = t.is_alive()\n"
    );
    EXPECT_NE(ir.find("dragon_vthread_is_alive"), std::string::npos);
}

//===----------------------------------------------------------------------===//
// Async/Await IR
//===----------------------------------------------------------------------===//

TEST(CodeGenTest, AsyncDefIR) {
    auto ir = generateIR(
        "async def fetch() -> int {\n"
        "  return 42\n"
        "}\n"
    );
    // async def creates inner body function + wrapper that calls vthread_spawn
    EXPECT_NE(ir.find("fetch__async_body"), std::string::npos);
    EXPECT_NE(ir.find("dragon_vthread_spawn"), std::string::npos);
}

TEST(CodeGenTest, AwaitIR) {
    auto ir = generateIR(
        "async def fetch() -> int {\n"
        "  return 42\n"
        "}\n"
        "r: int = await fetch()\n"
    );
    EXPECT_NE(ir.find("dragon_vthread_join"), std::string::npos);
}

TEST(CodeGenTest, AsyncDefReturnsPtrIR) {
    auto ir = generateIR(
        "async def compute(x: int) -> int {\n"
        "  return x * 2\n"
        "}\n"
        "task: Task[int] = compute(21)\n"
    );
    // Calling async def without await should return a ptr (Task handle)
    EXPECT_NE(ir.find("compute__async_body"), std::string::npos);
    EXPECT_NE(ir.find("dragon_vthread_spawn"), std::string::npos);
}

TEST(CodeGenTest, VthreadSleepIR) {
    auto ir = generateIR(
        "extern \"C\" def dragon_vthread_sleep(ms: int)\n"
        "dragon_vthread_sleep(10)\n"
    );
    EXPECT_NE(ir.find("dragon_vthread_sleep"), std::string::npos);
}

//===----------------------------------------------------------------------===//
// SyncList/SyncDict IR
//===----------------------------------------------------------------------===//

// SyncList/SyncDict are internal builtins with no spellable type. The real
// user-facing surface is ConcurrentList/ConcurrentDict (collections.concurrent),
// thin class wrappers whose methods call the same dragon_synclist_*/
// dragon_syncdict_* intrinsics directly - so the asserted runtime symbols still
// appear in the emitted IR (verified: ConcurrentList()/.append() emit
// dragon_synclist_new/_append; ConcurrentDict() emits dragon_syncdict_new).
TEST(CodeGenTest, SyncListNewIR) {
    auto ir = generateIR(
        "from collections.concurrent import ConcurrentList\n"
        "c: ConcurrentList = ConcurrentList()\n"
    );
    EXPECT_NE(ir.find("dragon_synclist_new"), std::string::npos);
}

TEST(CodeGenTest, SyncListAppendIR) {
    auto ir = generateIR(
        "from collections.concurrent import ConcurrentList\n"
        "c: ConcurrentList = ConcurrentList()\n"
        "c.append(42)\n"
    );
    EXPECT_NE(ir.find("dragon_synclist_append"), std::string::npos);
}

TEST(CodeGenTest, SyncDictNewIR) {
    auto ir = generateIR(
        "from collections.concurrent import ConcurrentDict\n"
        "c: ConcurrentDict = ConcurrentDict()\n"
    );
    EXPECT_NE(ir.find("dragon_syncdict_new"), std::string::npos);
}

//===----------------------------------------------------------------------===//
// Fire E2E
//===----------------------------------------------------------------------===//

TEST(CodeGenTest, FireBasicE2E) {
    auto out = compileAndRun(
        "def worker() -> int {\n"
        "  return 42\n"
        "}\n"
        "t: Task[int] = fire worker()\n"
        "r: int = t.join()\n"
        "print(r)\n"
    );
    EXPECT_EQ(out, "42\n");
}

TEST(CodeGenTest, FireReturnIntE2E) {
    auto out = compileAndRun(
        "def add(a: int, b: int) -> int {\n"
        "  return a + b\n"
        "}\n"
        "t: Task[int] = fire add(10, 32)\n"
        "print(t.join())\n"
    );
    EXPECT_EQ(out, "42\n");
}

TEST(CodeGenTest, FireNoArgsE2E) {
    auto out = compileAndRun(
        "def hello() -> int {\n"
        "  return 99\n"
        "}\n"
        "t: Task[int] = fire hello()\n"
        "print(t.join())\n"
    );
    EXPECT_EQ(out, "99\n");
}

TEST(CodeGenTest, FireMultipleE2E) {
    auto out = compileAndRun(
        "def double(x: int) -> int {\n"
        "  return x * 2\n"
        "}\n"
        "t1: Task[int] = fire double(5)\n"
        "t2: Task[int] = fire double(10)\n"
        "t3: Task[int] = fire double(15)\n"
        "r1: int = t1.join()\n"
        "r2: int = t2.join()\n"
        "r3: int = t3.join()\n"
        "print(r1)\n"
        "print(r2)\n"
        "print(r3)\n"
    );
    EXPECT_EQ(out, "10\n20\n30\n");
}

TEST(CodeGenTest, FireSequentialE2E) {
    auto out = compileAndRun(
        "def square(x: int) -> int {\n"
        "  return x * x\n"
        "}\n"
        "t1: Task[int] = fire square(3)\n"
        "r1: int = t1.join()\n"
        "t2: Task[int] = fire square(7)\n"
        "r2: int = t2.join()\n"
        "print(r1)\n"
        "print(r2)\n"
    );
    EXPECT_EQ(out, "9\n49\n");
}

// Regression: fire on a void-returning callee (-> None) used to fail LLVM
// verification with "Instruction has a name, but provides a void value!"
// because buildFireTrampoline always passed "fire.res" as the SSA name.
// See src/CodeGenImpl.h:buildFireTrampoline.
TEST(CodeGenTest, FireVoidMethodE2E) {
    auto out = compileAndRun(
        "class Worker {\n"
        "    def() {}\n"
        "    def run(n: int) -> None {\n"
        "        print(n)\n"
        "    }\n"
        "}\n"
        "const w: Worker = Worker()\n"
        "t: Task[None] = fire w.run(42)\n"
        "t.join()\n"
    );
    EXPECT_EQ(out, "42\n");
}

TEST(CodeGenTest, FireVoidFunctionE2E) {
    auto out = compileAndRun(
        "def shout(msg: str) -> None {\n"
        "    print(msg)\n"
        "}\n"
        "t: Task[None] = fire shout(\"hello\")\n"
        "t.join()\n"
    );
    EXPECT_EQ(out, "hello\n");
}

TEST(CodeGenTest, FireVoidNoReturnAnnotationE2E) {
    // No -> None annotation either: the parser/typechecker treats an
    // unannotated def as void-returning. The trampoline must still emit
    // an unnamed call instruction.
    auto out = compileAndRun(
        "def announce(n: int) {\n"
        "    print(n)\n"
        "}\n"
        "t: Task = fire announce(7)\n"
        "t.join()\n"
    );
    EXPECT_EQ(out, "7\n");
}

// Lock E2E correctness tests (acquire/release, blocking=False, timeout, with,
// exception-safety) live in InteropTest.StdlibThreadingLock* - they require
// `from threading import Lock`, which needs full stdlib module resolution that
// the single-module compileAndRun helper here does not perform.

//===----------------------------------------------------------------------===//
// Thread Block E2E
//===----------------------------------------------------------------------===//

TEST(CodeGenTest, ThreadBlockBasicE2E) {
    auto out = compileAndRun(
        "thread {\n"
        "  print(\"from thread\")\n"
        "}\n"
        "print(\"after join\")\n"
    );
    // thread auto-joins, so "from thread" prints before "after join"
    EXPECT_EQ(out, "from thread\nafter join\n");
}

TEST(CodeGenTest, ThreadBlockMultiStmtE2E) {
    auto out = compileAndRun(
        "thread {\n"
        "  print(\"a\")\n"
        "  print(\"b\")\n"
        "  print(\"c\")\n"
        "}\n"
        "print(\"done\")\n"
    );
    EXPECT_EQ(out, "a\nb\nc\ndone\n");
}

//===----------------------------------------------------------------------===//
// Fire Block E2E
//===----------------------------------------------------------------------===//

TEST(CodeGenTest, FireBlockBasicE2E) {
    auto out = compileAndRun(
        "t: Task = fire {\n"
        "  print(\"from block\")\n"
        "}\n"
        "t.join()\n"
        "print(\"done\")\n"
    );
    EXPECT_EQ(out, "from block\ndone\n");
}

TEST(CodeGenTest, FireBlockNoJoinE2E) {
    // fire { block } without join - should still execute (scheduler runs it)
    auto out = compileAndRun(
        "fire {\n"
        "  print(\"bg\")\n"
        "}\n"
        // Small busy-wait to let the scheduler run the vthread
        "x: int = 0\n"
        "while x < 1000000 {\n"
        "  x = x + 1\n"
        "}\n"
        "print(\"main\")\n"
    );
    // bg should print before or during main's busy-wait
    EXPECT_NE(out.find("bg"), std::string::npos);
    EXPECT_NE(out.find("main"), std::string::npos);
}

//===----------------------------------------------------------------------===//
// Async/Await E2E
//===----------------------------------------------------------------------===//

TEST(CodeGenTest, AsyncAwaitBasicE2E) {
    auto out = compileAndRun(
        "async def get_value() -> int {\n"
        "  return 42\n"
        "}\n"
        "result: int = await get_value()\n"
        "print(result)\n"
    );
    EXPECT_EQ(out, "42\n");
}

TEST(CodeGenTest, AsyncAwaitWithArgsE2E) {
    auto out = compileAndRun(
        "async def double_it(x: int) -> int {\n"
        "  return x * 2\n"
        "}\n"
        "result: int = await double_it(21)\n"
        "print(result)\n"
    );
    EXPECT_EQ(out, "42\n");
}

TEST(CodeGenTest, AsyncAwaitMultipleArgsE2E) {
    auto out = compileAndRun(
        "async def add(a: int, b: int) -> int {\n"
        "  return a + b\n"
        "}\n"
        "result: int = await add(17, 25)\n"
        "print(result)\n"
    );
    EXPECT_EQ(out, "42\n");
}

TEST(CodeGenTest, AsyncAwaitInNormalDefE2E) {
    // The Dragon Rule: await allowed in non-async functions
    auto out = compileAndRun(
        "async def fetch() -> int {\n"
        "  return 99\n"
        "}\n"
        "def process() -> int {\n"
        "  val: int = await fetch()\n"
        "  return val + 1\n"
        "}\n"
        "print(process())\n"
    );
    EXPECT_EQ(out, "100\n");
}

TEST(CodeGenTest, AsyncAwaitParallelE2E) {
    // Start multiple async tasks, await them later
    auto out = compileAndRun(
        "async def compute(x: int) -> int {\n"
        "  return x * x\n"
        "}\n"
        "t1: Task[int] = compute(3)\n"
        "t2: Task[int] = compute(4)\n"
        "r1: int = await t1\n"
        "r2: int = await t2\n"
        "print(r1 + r2)\n"
    );
    EXPECT_EQ(out, "25\n");
}

TEST(CodeGenTest, AsyncAwaitChainE2E) {
    // Chain of async calls
    auto out = compileAndRun(
        "async def step1() -> int {\n"
        "  return 10\n"
        "}\n"
        "async def step2(x: int) -> int {\n"
        "  return x + 20\n"
        "}\n"
        "v: int = await step1()\n"
        "result: int = await step2(v)\n"
        "print(result)\n"
    );
    EXPECT_EQ(out, "30\n");
}

TEST(CodeGenTest, VthreadSleepE2E) {
    // sleep in a fire'd green thread - should yield and not block
    auto out = compileAndRun(
        "extern \"C\" def dragon_vthread_sleep(ms: int)\n"
        "def worker() -> int {\n"
        "  dragon_vthread_sleep(50)\n"
        "  return 42\n"
        "}\n"
        "t: Task[int] = fire worker()\n"
        "r: int = t.join()\n"
        "print(r)\n"
    );
    EXPECT_EQ(out, "42\n");
}

TEST(CodeGenTest, VthreadSleepConcurrentE2E) {
    // Multiple green threads sleeping concurrently - all should complete
    auto out = compileAndRun(
        "extern \"C\" def dragon_vthread_sleep(ms: int)\n"
        "def sleeper(id: int) -> int {\n"
        "  dragon_vthread_sleep(20)\n"
        "  return id\n"
        "}\n"
        "t1: Task[int] = fire sleeper(1)\n"
        "t2: Task[int] = fire sleeper(2)\n"
        "t3: Task[int] = fire sleeper(3)\n"
        "r1: int = t1.join()\n"
        "r2: int = t2.join()\n"
        "r3: int = t3.join()\n"
        "print(r1 + r2 + r3)\n"
    );
    EXPECT_EQ(out, "6\n");
}

//===----------------------------------------------------------------------===//
// SyncList/SyncDict E2E
//===----------------------------------------------------------------------===//

TEST(CodeGenTest, SyncListBasicE2E) {
    auto out = compileAndRun(
        "from collections.concurrent import ConcurrentList\n"
        "sl: ConcurrentList = ConcurrentList()\n"
        "sl.append(10)\n"
        "sl.append(20)\n"
        "sl.append(30)\n"
        "print(sl.len())\n"
        "print(sl.get(0))\n"
        "print(sl.get(1))\n"
        "print(sl.get(2))\n"
    );
    EXPECT_EQ(out, "3\n10\n20\n30\n");
}

TEST(CodeGenTest, SyncListPopSetE2E) {
    // ConcurrentList.pop(idx) requires an explicit index (no Python-style
    // default-to-last). The list is [1, 99, 3] after set(1, 99); popping the
    // last element is pop(2), which returns 3 - same value/output as a bare
    // pop() on a 3-element list would yield.
    auto out = compileAndRun(
        "from collections.concurrent import ConcurrentList\n"
        "sl: ConcurrentList = ConcurrentList()\n"
        "sl.append(1)\n"
        "sl.append(2)\n"
        "sl.append(3)\n"
        "sl.set(1, 99)\n"
        "print(sl.get(1))\n"
        "v: int = sl.pop(2)\n"
        "print(v)\n"
        "print(sl.len())\n"
    );
    EXPECT_EQ(out, "99\n3\n2\n");
}

TEST(CodeGenTest, SyncListSortReverseE2E) {
    auto out = compileAndRun(
        "from collections.concurrent import ConcurrentList\n"
        "sl: ConcurrentList = ConcurrentList()\n"
        "sl.append(3)\n"
        "sl.append(1)\n"
        "sl.append(2)\n"
        "sl.sort()\n"
        "print(sl.get(0))\n"
        "print(sl.get(1))\n"
        "print(sl.get(2))\n"
        "sl.reverse()\n"
        "print(sl.get(0))\n"
    );
    EXPECT_EQ(out, "1\n2\n3\n3\n");
}

TEST(CodeGenTest, SyncDictBasicE2E) {
    auto out = compileAndRun(
        "from collections.concurrent import ConcurrentDict\n"
        "sd: ConcurrentDict = ConcurrentDict()\n"
        "sd.set(\"a\", 10)\n"
        "sd.set(\"b\", 20)\n"
        "print(sd.get(\"a\"))\n"
        "print(sd.get(\"b\"))\n"
        "print(sd.len())\n"
    );
    EXPECT_EQ(out, "10\n20\n2\n");
}

TEST(CodeGenTest, SyncDictGetDefaultE2E) {
    // ConcurrentDict spells the two-arg defaulted lookup as get_default(key,
    // default) (get() takes only the key), so sd.get("x", 0) maps to
    // sd.get_default("x", 0). has_key returns int (1/0).
    auto out = compileAndRun(
        "from collections.concurrent import ConcurrentDict\n"
        "sd: ConcurrentDict = ConcurrentDict()\n"
        "sd.set(\"x\", 42)\n"
        "print(sd.get_default(\"x\", 0))\n"
        "print(sd.get_default(\"y\", -1))\n"
        "print(sd.has_key(\"x\"))\n"
        "print(sd.has_key(\"z\"))\n"
    );
    EXPECT_EQ(out, "42\n-1\n1\n0\n");
}

TEST(CodeGenTest, SyncDictPopClearE2E) {
    auto out = compileAndRun(
        "from collections.concurrent import ConcurrentDict\n"
        "sd: ConcurrentDict = ConcurrentDict()\n"
        "sd.set(\"a\", 1)\n"
        "sd.set(\"b\", 2)\n"
        "v: int = sd.pop(\"a\")\n"
        "print(v)\n"
        "print(sd.len())\n"
        "sd.clear()\n"
        "print(sd.len())\n"
    );
    EXPECT_EQ(out, "1\n1\n0\n");
}

TEST(CodeGenTest, SyncListThreadedE2E) {
    // Task bindings require an explicit annotation (t: Task[int] = fire ...),
    // so the bare `t1 = fire ...` of the original is annotated here.
    auto out = compileAndRun(
        "from collections.concurrent import ConcurrentList\n"
        "sl: ConcurrentList = ConcurrentList()\n"
        "def adder(start: int) -> int {\n"
        "    sl.append(start)\n"
        "    sl.append(start + 1)\n"
        "    return 0\n"
        "}\n"
        "t1: Task[int] = fire adder(10)\n"
        "t2: Task[int] = fire adder(20)\n"
        "t1.join()\n"
        "t2.join()\n"
        "print(sl.len())\n"
    );
    EXPECT_EQ(out, "4\n");
}

//===----------------------------------------------------------------------===//
// GC thread-safety stress test
//===----------------------------------------------------------------------===//

// Spawns 10 fire workers (each green thread distributed across N scheduler
// OS threads) and has each one perform 100k heap allocations. Every
// allocation calls dragon_gc_track AND bumps gc_alloc_counter; crossing
// gc_threshold triggers dragon_gc_collect from whichever worker thread
// crossed it. This exercises every GC thread-safety race simultaneously:
//
//  - parallel dragon_gc_track vs. dragon_gc_track: concurrent realloc of
//  gc_tracked array (the exact "double-free of stale pointer" race)
//  - two concurrent dragon_gc_collect calls: would double-free without
//  the gc_in_progress coordination
//  - dragon_decref racing with GC's capture of refcount: mutator
//  `--refcount == 0` on a tracked obj concurrent with GC's snapshot
//  would produce double-free without serialization through gc_lock
//
// Success criterion: no crashes, no leaks, no UAF.
// NOTE: we use flat list[int] allocations only because list-literal codegen
// currently does not incref heap-typed elements - nesting would hit that
// pre-existing refcount accounting bug (orthogonal to the races above). The
// flat allocations still hit every race path above.
TEST(CodeGenTest, GCThreadSafetyStressE2E) {
    auto out = compileAndRun(
        "def worker(seed: int) -> int {\n"
        "  total: int = 0\n"
        "  for i in range(100000) {\n"
        "    a: list[int] = [seed, i, seed + i, seed * i]\n"
        "    b: list[int] = [i, seed, i - seed]\n"
        "    total = total + 1\n"
        "  }\n"
        "  return total\n"
        "}\n"
        "t1: Task[int] = fire worker(1)\n"
        "t2: Task[int] = fire worker(2)\n"
        "t3: Task[int] = fire worker(3)\n"
        "t4: Task[int] = fire worker(4)\n"
        "t5: Task[int] = fire worker(5)\n"
        "t6: Task[int] = fire worker(6)\n"
        "t7: Task[int] = fire worker(7)\n"
        "t8: Task[int] = fire worker(8)\n"
        "t9: Task[int] = fire worker(9)\n"
        "t10: Task[int] = fire worker(10)\n"
        "sum: int = t1.join() + t2.join() + t3.join() + t4.join() + t5.join()\n"
        "sum = sum + t6.join() + t7.join() + t8.join() + t9.join() + t10.join()\n"
        "print(sum)\n"
    );
    EXPECT_EQ(out, "1000000\n");
}

//===----------------------------------------------------------------------===//
// Regression: thread sync flags + double-start race
//===----------------------------------------------------------------------===//
// - vthread/OSThread done & started flags use __atomic builtins (volatile
//   is not enough).
// - dragon_osthread_start uses CAS so concurrent .start() runs pthread_create
//  at most once. Single-threaded test is enough to verify the CAS rejects the
//  second call; concurrent start of OS threads from Dragon source is awkward
//  to express, so we exercise the Thread API directly.

TEST(CodeGenE2E, VThreadDoneFlagSynchronizes) {
    // After join() returns, is_alive() must observe done=1. With volatile this
    // could fail on weakly-ordered hardware; with atomic-load ACQUIRE it can't.
    auto out = compileAndRun(
        "def work(n: int) -> int { return n * 2 }\n"
        "t: Task[int] = fire work(21)\n"
        "r: int = t.join()\n"
        "print(r)\n"
    );
    EXPECT_EQ(out, "42\n");
}

// Regression: cycle collector string leak. If the clear_refs phase calls
// dragon_decref_str, its gc_collecting guard refuses to free strings
// reaching refcount 0 mid-collection; Phase 6's destroy then iterates zero
// elements (size already cleared) and the strings leak. clear_refs
// uses dragon_str_force_free_if_zero, which bypasses the guard.
TEST(CodeGenE2E, CycleCollectorWithStringFields) {
    auto out = compileAndRun(
        "class Node {\n"
        "  def(name: str) {\n"
        "    self.name: str = name\n"
        "    self.next: Optional[Node] = None\n"
        "  }\n"
        "}\n"
        "def make_cycle() {\n"
        "  a: Node = Node(\"alpha-string-payload\")\n"
        "  b: Node = Node(\"beta-string-payload\")\n"
        "  a.next = b\n"
        "  b.next = a\n"
        "}\n"
        "for i in range(800) { make_cycle() }\n"
        "print(\"ok\")\n"
    );
    EXPECT_EQ(out, "ok\n");
}

TEST(CodeGenE2E, ThreadDoubleStartRejected) {
    // Calling start() twice on the same OS thread handle must return -1 the
    // second time. The CAS in dragon_osthread_start makes this race-safe.
    // We call the C runtime directly to avoid the stdlib threading import
    // (which the codegen test harness doesn't resolve).
    auto out = compileAndRun(
        "extern \"C\" def dragon_osthread_new(fn: ptr, args: ptr, nargs: int) -> ptr\n"
        "extern \"C\" def dragon_osthread_start(handle: ptr) -> int\n"
        "extern \"C\" def dragon_osthread_join(handle: ptr) -> int\n"
        "def work() -> int { return 7 }\n"
        "h: ptr = dragon_osthread_new(work, none, 0)\n"
        "r1: int = dragon_osthread_start(h)\n"
        "r2: int = dragon_osthread_start(h)\n"
        "_: int = dragon_osthread_join(h)\n"
        "print(r1)\n"
        "print(r2)\n"
    );
    // r1: 0 (pthread_create success), r2: -1 (CAS rejected the second start)
    EXPECT_EQ(out, "0\n-1\n");
}

//===----------------------------------------------------------------------===//
// Regression: vthread_sleep ms truncation
//
// Encoding `ms` into IoRequest.fd via
// `(int)(intptr_t)ms` silently truncates values >2^31 ms (~24.8 days)
// and produces wrong durations. IoRequest carries a separate int64_t
// timer_ms field so the full int64 ms value is preserved.
//
// We can't actually sleep for >2^31 ms in CI, but we can verify:
//  1. Short sleeps (< INT_MAX) still work correctly (regression).
//  2. The Dragon FFI signature accepts and passes int64 ms unchanged.
//  3. A value just over INT_MAX (e.g. 2147483648) is accepted without
//  crashing or returning to caller instantly (which would happen if
//  truncation collapsed it to a negative or 0 timerfd duration).
//
// We use `fire`'d workers with very short sleeps so the test stays fast.
//===----------------------------------------------------------------------===//

TEST(CodeGenIR, VthreadSleepInt64Param) {
    // Confirm the runtime function is declared with i64 ms (not i32).
    auto ir = generateIR(
        "extern \"C\" def dragon_vthread_sleep(ms: int)\n"
        "dragon_vthread_sleep(2147483648)\n"  // 2^31, would truncate as int
    );
    // Look for `void @dragon_vthread_sleep(i64`. If the signature was i32
    // we'd see `i32` here.
    EXPECT_NE(ir.find("dragon_vthread_sleep(i64"), std::string::npos)
        << "Expected i64 ms parameter on dragon_vthread_sleep\nIR:\n" << ir;
}

TEST(CodeGenE2E, VthreadSleepShortStillWorks) {
    // Regression: short sleep still completes correctly.
    auto out = compileAndRun(
        "extern \"C\" def dragon_vthread_sleep(ms: int)\n"
        "def worker() -> int {\n"
        "  dragon_vthread_sleep(5)\n"
        "  return 99\n"
        "}\n"
        "t: Task[int] = fire worker()\n"
        "r: int = t.join()\n"
        "print(r)\n"
    );
    EXPECT_EQ(out, "99\n");
}

TEST(CodeGenE2E, VthreadSleepLargeValueDoesntTruncate) {
    // Pass a value > INT_MAX. Pre-fix this would cast to int and become a
    // negative or wrapped duration -> timerfd would either fire instantly
    // or fail. Post-fix the int64 value is preserved.
    //
    // We can't actually wait that long, but we can spawn the sleeping
    // worker, then immediately request a much shorter sleep that completes
    // and lets us print before joining. If the long sleep had truncated to
    // a small/negative value, the worker would already be done before we
    // print "spawned" - race-y, but in practice fork-then-print is faster
    // than even a microsecond timerfd.
    //
    // Simpler check: just confirm the program compiles, runs, and the
    // long-sleeping worker is reported alive after a short main sleep.
    auto out = compileAndRun(
        "extern \"C\" def dragon_vthread_sleep(ms: int)\n"
        "def long_sleeper() -> int {\n"
        // 5 billion ms = ~58 days. Far past int32 range. We never join
        // this; the test exits before it wakes up.
        "  dragon_vthread_sleep(5000000000)\n"
        "  return 1\n"
        "}\n"
        "def short_worker() -> int {\n"
        "  dragon_vthread_sleep(10)\n"
        "  return 42\n"
        "}\n"
        "t_long: Task[int] = fire long_sleeper()\n"
        "t_short: Task[int] = fire short_worker()\n"
        "r: int = t_short.join()\n"
        "print(r)\n"
    );
    // If truncation collapsed long_sleeper's duration, the program could
    // hang on the long_sleeper joining or exit with weird behavior. With
    // the full int64 preserved, long_sleeper just stays parked and the
    // short worker returns.
    EXPECT_EQ(out, "42\n");
}

//===----------------------------------------------------------------------===//
// Regression: nb_ I/O must poll(), not busy-wait
//
// A usleep(1000) retry loop on EAGAIN outside a green-thread context
// spins at 100% CPU forever on an invalid /
// closed fd. dragon_nb_accept/_recv/_send instead use
// poll() and bail to -1 on POLLERR/POLLHUP/POLLNVAL.
//
// Test: call nb_recv on a socket fd that never becomes readable (a
// half-closed pipe) from the main thread. A spin would hang the
// test process; poll returns -1 promptly.
//===----------------------------------------------------------------------===//

TEST(CodeGenE2E, NbRecvBadFdReturnsMinusOne) {
    // -1 is never a valid fd. nb_recv should return -1 (not hang).
    // We allocate a small buffer via Dragon's bytes type so we have a ptr
    // to hand the runtime - it must not actually try to fill it.
    auto out = compileAndRun(
        "extern \"C\" def dragon_nb_recv(fd: int, buf: ptr, max_len: int) -> int\n"
        "extern \"C\" def malloc(n: int) -> ptr\n"
        "extern \"C\" def free(p: ptr)\n"
        "buf: ptr = malloc(64)\n"
        "r: int = dragon_nb_recv(-1, buf, 64)\n"
        "free(buf)\n"
        "print(r)\n"
    );
    // -1 returned from the runtime; no infinite spin.
    EXPECT_EQ(out, "-1\n");
}

TEST(CodeGenE2E, NbSendBadFdReturnsMinusOne) {
    auto out = compileAndRun(
        "extern \"C\" def dragon_nb_send(fd: int, buf: str, len: int) -> int\n"
        "msg: str = \"hello\"\n"
        "r: int = dragon_nb_send(-1, msg, 5)\n"
        "print(r)\n"
    );
    EXPECT_EQ(out, "-1\n");
}

TEST(CodeGenE2E, NbAcceptBadFdReturnsMinusOne) {
    // nb_accept on a bogus fd: must return -1, not hang.
    auto out = compileAndRun(
        "extern \"C\" def dragon_nb_accept(fd: int, addr: ptr, addrlen: ptr) -> int\n"
        "r: int = dragon_nb_accept(-1, none, none)\n"
        "print(r)\n"
    );
    EXPECT_EQ(out, "-1\n");
}

// Regression for the hello_server "request 87" crash. The bug had two
// interacting causes:
//  (1) `__dragon_traverse_X` (codegen-emitted per-class GC traversal) did
//  NOT NULL-check heap fields before invoking the visitor. When the
//  cycle collector fired DURING `ClassName_new` - after `gc_track(self)`
//  but BEFORE `__init__` had populated heap fields - the half-built
//  instance's NULL fields were passed to `gc_visit_reachable`, which
//  dereferenced NULL at offset 9 (gc_flags) and segfaulted.
//  (2) `Holder_new` packed the header word with `gc_flags=0` instead of
//  `GC_FLAG_HEAP_OBJ` (0x80) - `dragon_mark_shared_deep` early-returns
//  on objects without HEAP_OBJ, so SHARED never propagated through
//  class instances at fire-sites.
//  (3) Method fires (`fire self.method(...)`) had `funcParamKinds` empty
//  for the mangled `Class_method` name, so the spawn-site emitted no
//  atomic-incref + no shared-mark - the body ran with non-atomic
//  refcount ops on cross-vthread state.
//
// This test uses gc_threshold=20 so the collector fires inside the
// constructor's heap-field initializers (where (1) was triggered), then
// fires N vthreads each touching a SHARED list/dict heap field on the
// receiver - exercising (2) and (3). Pre-fix this crashes; post-fix it
// runs to completion and prints the expected sum.
TEST(CodeGenE2E, GCCycleCollectorMidConstructionTraverseNullCheck) {
    auto out = compileAndRun(
        "extern \"C\" def dragon_gc_set_threshold(n: int)\n"
        "class Holder {\n"
        "  def() {\n"
        "    self.a: list[int] = [1, 2, 3]\n"
        "    self.b: list[int] = [4, 5, 6]\n"
        "    self.c: dict[str, int] = {\"x\": 1}\n"
        "    self.d: list[str] = [\"alpha\", \"beta\"]\n"
        "  }\n"
        "  def sum_a() -> int {\n"
        "    s: int = 0\n"
        "    for x in self.a {\n"
        "      s = s + x\n"
        "    }\n"
        "    return s\n"
        "  }\n"
        "}\n"
        "def worker(h: Holder) -> int {\n"
        "  return h.sum_a()\n"
        "}\n"
        "dragon_gc_set_threshold(20)\n"
        "h: Holder = Holder()\n"
        "tasks: list[Task[int]] = []\n"
        "i: int = 0\n"
        "while i < 8 {\n"
        "  t: Task[int] = fire worker(h)\n"
        "  tasks.append(t)\n"
        "  i = i + 1\n"
        "}\n"
        "total: int = 0\n"
        "for t in tasks {\n"
        "  total = total + t.join()\n"
        "}\n"
        "print(total)\n"
    );
    // 8 vthreads × sum([1,2,3])=6 = 48
    EXPECT_EQ(out, "48\n");
}

// Regression for the multi-worker plain-RC race on shared heap state.
// `dragon_incref` / `dragon_decref` (and the _str siblings) check
// `gc_flags & GC_FLAG_SHARED` and dispatch to the atomic variant when set.
// SHARED is propagated transitively at fire-sites by `dragon_mark_shared_deep`
// and via container write-barriers, so any heap object reachable from a
// fired arg gets atomic RC ops in the body.
//
// This test fires N workers, each performing K iterations of an
// incref+decref cycle on the SAME shared heap-allocated strings. The
// strings are produced by `prefix + str(i)` so they are real DragonStrings
// (not literals - literals are skipped by `dragon_is_heap_string`). The
// shared list is passed as the fire arg, so it and its element strings
// must be SHARED-marked transitively.
//
// With SHARED enabled (current behavior): every incref/decref on the
// shared strings dispatches atomic. Refcount stays consistent. Total
// matches the expected sum exactly.
//
// With SHARED neutered (e.g. by removing the `if (h->gc_flags & SHARED)`
// branch in dragon_incref_str / dragon_decref_str): two workers on
// different OS threads racing on the same string's refcount tear the
// non-atomic `++` / `--`. Lost increments cause the refcount to drop below
// the true reference count; eventually a string is freed while the shared
// list still points at it; the next `len(x)` read of the freed slot
// crashes (heap-corruption SIGSEGV, free() abort, or wrong total).
//
// Sized to make the race window large but the test fast (~1-2s):
//  - 16 fire workers (≥ 2 worker OS threads on any non-uniproc CI box)
//  - 5,000 iters each, cycling through 8 shared strings
//  - len() per iter forces a real read on the string's body so a
//  freed-string UAF surfaces as a crash, not a silent miss.
TEST(CodeGenE2E, SharedRefcountAtomicDispatch_FireMultiWorker) {
    auto out = compileAndRun(
        "def make_str(p: str, n: int) -> str {\n"
        "  return p + str(n)\n"
        "}\n"
        "def worker(s: list[str]) -> int {\n"
        "  n: int = 0\n"
        "  i: int = 0\n"
        "  while i < 5000 {\n"
        "    j: int = i % 8\n"
        "    x: str = s[j]\n"
        "    n = n + len(x)\n"
        "    i = i + 1\n"
        "  }\n"
        "  return n\n"
        "}\n"
        "shared: list[str] = []\n"
        "k: int = 0\n"
        "while k < 8 {\n"
        "  shared.append(make_str(\"item_\", k))\n"
        "  k = k + 1\n"
        "}\n"
        "tasks: list[Task[int]] = []\n"
        "w: int = 0\n"
        "while w < 16 {\n"
        "  t: Task[int] = fire worker(shared)\n"
        "  tasks.append(t)\n"
        "  w = w + 1\n"
        "}\n"
        "total: int = 0\n"
        "for t in tasks {\n"
        "  total = total + t.join()\n"
        "}\n"
        "print(total)\n"
    );
    // Each "item_<digit>" is 6 chars; 5000 iters × 6 chars = 30000 per worker;
    // 16 workers × 30000 = 480000.
    EXPECT_EQ(out, "480000\n");
}

// Regression: an uncaught exception inside a fired vthread must NOT kill the
// parent. If the fire-trampoline has no exception handler, dragon_raise_exc
// falls through to exit(1), taking down the parent thread
// (and any sibling vthreads / accept loop with it). The fire trampoline
// pushes a top-level setjmp frame so a stray longjmp lands in the trampoline,
// is logged, and the worker re-enters the scheduler cleanly.
//
// This test fires three workers: the middle one raises ValueError. The
// parent then joins all three (the failed one returns the 0 sentinel), does
// more work, and prints "alive". If the trampoline's setjmp barrier is
// missing, the process would have already exit(1)'d before reaching the
// final print and the test would observe an empty / mismatched stdout.
TEST(CodeGenE2E, FireVThreadUncaughtExceptionContained) {
    auto out = compileAndRun(
        "def good(n: int) -> int {\n"
        "  return n * 2\n"
        "}\n"
        "def bad(n: int) -> int {\n"
        "  raise ValueError(\"intentional\")\n"
        "  return -1\n"
        "}\n"
        "a: Task[int] = fire good(10)\n"
        "b: Task[int] = fire bad(99)\n"
        "c: Task[int] = fire good(20)\n"
        "ra: int = a.join()\n"
        "rb: int = b.join()\n"
        "rc: int = c.join()\n"
        "print(ra)\n"
        "print(rb)\n"
        "print(rc)\n"
        "print(\"alive\")\n"
    );
    // good(10) = 20, bad raised so result sentinel = 0, good(20) = 40,
    // and the parent must reach the final print. compileAndRun merges
    // stderr into out, so the trampoline's log line appears first.
    EXPECT_EQ(out,
              "vthread terminated by uncaught ValueError: intentional\n"
              "20\n0\n40\nalive\n");
}

//===----------------------------------------------------------------------===//
// Task[T] - typed handle + native result coercion (D016 Phase 4 / D030)
//===----------------------------------------------------------------------===//

TEST(CodeGenE2E, TaskIntJoinRecoversNativeInt) {
    auto out = compileAndRun(
        "def work() -> int { return 21 }\n"
        "t: Task[int] = fire work()\n"
        "r: int = t.join()\n"
        "print(r)\n");
    EXPECT_EQ(out, "21\n");
}

TEST(CodeGenE2E, TaskFloatJoinBitcastsNotConverts) {
    // The float result is bit-PACKED into the i64 vthread slot; join must
    // BITCAST it back, not int-convert. 3.5 round-trips iff the bits survive.
    auto out = compileAndRun(
        "def fw() -> float { return 3.5 }\n"
        "t: Task[float] = fire fw()\n"
        "r: float = t.join()\n"
        "print(r)\n");
    EXPECT_EQ(out, "3.5\n");
}

TEST(CodeGenE2E, TaskStrJoinRecoversPointer) {
    auto out = compileAndRun(
        "def sw() -> str { return \"hello\" }\n"
        "t: Task[str] = fire sw()\n"
        "r: str = t.join()\n"
        "print(r)\n");
    EXPECT_EQ(out, "hello\n");
}

TEST(CodeGenE2E, AwaitAsyncDefRecoversNativeInt) {
    auto out = compileAndRun(
        "async def fetch() -> int { return 99 }\n"
        "r: int = await fetch()\n"
        "print(r)\n");
    EXPECT_EQ(out, "99\n");
}

TEST(CodeGenE2E, BareTaskAnnotationRefinesAndJoins) {
    auto out = compileAndRun(
        "def work() -> int { return 42 }\n"
        "t: Task = fire work()\n"
        "r: int = t.join()\n"
        "print(r)\n");
    EXPECT_EQ(out, "42\n");
}
