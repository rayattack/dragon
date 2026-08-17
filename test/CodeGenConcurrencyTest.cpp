#include "CodeGenTestHelpers.h"

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

TEST(CodeGenTest, LockAcquireNonblockingBoolIR) {
    auto ir = generateIR(
        "from threading import Lock\n"
        "m: Lock = Lock()\n"
        "ok: bool = m.acquire(blocking=False)\n"
    );
    EXPECT_NE(ir.find("dragon_lock_acquire_ex"), std::string::npos);
    EXPECT_NE(ir.find("icmp ne i64"), std::string::npos);
}

TEST(CodeGenTest, LockAcquireTimeoutIR) {
    auto ir = generateIR(
        "from threading import Lock\n"
        "m: Lock = Lock()\n"
        "ok: bool = m.acquire(blocking=True, timeout=0.5)\n"
    );
    EXPECT_NE(ir.find("dragon_lock_acquire_ex"), std::string::npos);
    EXPECT_NE(ir.find("double"), std::string::npos);
}

TEST(CodeGenTest, LockFastPathNoOverheadIR) {
    auto ir = generateIR(
        "from threading import Lock\n"
        "lock: Lock = Lock()\n"
        "lock.acquire()\n"
        "lock.release()\n"
    );
    EXPECT_NE(ir.find("dragon_lock_new"), std::string::npos);
    EXPECT_EQ(ir.find("threading__Lock___init__"), std::string::npos);
    EXPECT_EQ(ir.find("_dragon_Lock"), std::string::npos);
}

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

TEST(CodeGenTest, FireBlockIR) {
    auto ir = generateIR(
        "fire {\n"
        "  x: int = 42\n"
        "}\n"
    );
    EXPECT_NE(ir.find("dragon_vthread_spawn"), std::string::npos);
    EXPECT_NE(ir.find("__dragon_fire_"), std::string::npos);
}

TEST(CodeGenTest, FireVthreadSpawnIR) {
    auto ir = generateIR(
        "def work() -> int {\n"
        "  return 42\n"
        "}\n"
        "t: Task[int] = fire work()\n"
    );
    EXPECT_NE(ir.find("dragon_vthread_spawn"), std::string::npos);
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

TEST(CodeGenTest, AsyncDefIR) {
    auto ir = generateIR(
        "async def fetch() -> int {\n"
        "  return 42\n"
        "}\n"
    );
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
    auto out = compileAndRun(
        "def announce(n: int) {\n"
        "    print(n)\n"
        "}\n"
        "t: Task = fire announce(7)\n"
        "t.join()\n"
    );
    EXPECT_EQ(out, "7\n");
}

TEST(CodeGenTest, ThreadBlockBasicE2E) {
    auto out = compileAndRun(
        "thread {\n"
        "  print(\"from thread\")\n"
        "}\n"
        "print(\"after join\")\n"
    );
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
    auto out = compileAndRun(
        "fire {\n"
        "  print(\"bg\")\n"
        "}\n"
        "x: int = 0\n"
        "while x < 1000000 {\n"
        "  x = x + 1\n"
        "}\n"
        "print(\"main\")\n"
    );
    EXPECT_NE(out.find("bg"), std::string::npos);
    EXPECT_NE(out.find("main"), std::string::npos);
}

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

// 10 fire workers x 100k allocations hit every GC race at once (parallel gc_track realloc, concurrent collects,
// decref vs refcount capture: each a double-free without gc_lock). Flat list[int] only: nested literals hit a pre-existing elem-incref bug, not these races.
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

TEST(CodeGenE2E, VThreadDoneFlagSynchronizes) {
    auto out = compileAndRun(
        "def work(n: int) -> int { return n * 2 }\n"
        "t: Task[int] = fire work(21)\n"
        "r: int = t.join()\n"
        "print(r)\n"
    );
    EXPECT_EQ(out, "42\n");
}

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
    EXPECT_EQ(out, "0\n-1\n");
}

TEST(CodeGenIR, VthreadSleepInt64Param) {
    auto ir = generateIR(
        "extern \"C\" def dragon_vthread_sleep(ms: int)\n"
        "dragon_vthread_sleep(2147483648)\n"
    );
    EXPECT_NE(ir.find("dragon_vthread_sleep(i64"), std::string::npos)
        << "Expected i64 ms parameter on dragon_vthread_sleep\nIR:\n" << ir;
}

TEST(CodeGenE2E, VthreadSleepShortStillWorks) {
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
    auto out = compileAndRun(
        "extern \"C\" def dragon_vthread_sleep(ms: int)\n"
        "def long_sleeper() -> int {\n"
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
    EXPECT_EQ(out, "42\n");
}

TEST(CodeGenE2E, NbRecvBadFdReturnsMinusOne) {
    auto out = compileAndRun(
        "extern \"C\" def dragon_nb_recv(fd: int, buf: ptr, max_len: int) -> int\n"
        "extern \"C\" def malloc(n: int) -> ptr\n"
        "extern \"C\" def free(p: ptr)\n"
        "buf: ptr = malloc(64)\n"
        "r: int = dragon_nb_recv(-1, buf, 64)\n"
        "free(buf)\n"
        "print(r)\n"
    );
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
    auto out = compileAndRun(
        "extern \"C\" def dragon_nb_accept(fd: int, addr: ptr, addrlen: ptr) -> int\n"
        "r: int = dragon_nb_accept(-1, none, none)\n"
        "print(r)\n"
    );
    EXPECT_EQ(out, "-1\n");
}

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
    EXPECT_EQ(out, "48\n");
}

// Fired workers hammer incref/decref on the SAME heap strings: fire-site SHARED marking must
// dispatch atomic RC ops, else the torn refcount frees a string the shared list still points at.
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
    EXPECT_EQ(out, "480000\n");
}

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
    EXPECT_EQ(out,
              "vthread terminated by uncaught ValueError: intentional\n"
              "20\n0\n40\nalive\n");
}

TEST(CodeGenE2E, TaskIntJoinRecoversNativeInt) {
    auto out = compileAndRun(
        "def work() -> int { return 21 }\n"
        "t: Task[int] = fire work()\n"
        "r: int = t.join()\n"
        "print(r)\n");
    EXPECT_EQ(out, "21\n");
}

TEST(CodeGenE2E, TaskFloatJoinBitcastsNotConverts) {
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
