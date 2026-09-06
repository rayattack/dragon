# CodeGenConcurrencyTest cases

Source programs for `test/CodeGenConcurrencyTest.cpp`. Each block is named by the heading
above it and pulled in with `code("<name>")`.

#### :fire_basic_ir

```dr
def worker() -> int {
  return 42
}
t: Task[int] = fire worker()
```

#### :fire_join_ir

```dr
def worker() -> int {
  return 42
}
t: Task[int] = fire worker()
x: int = t.join()
```

#### :fire_is_alive_ir

```dr
def worker() -> int {
  return 42
}
t: Task[int] = fire worker()
d: int = t.is_alive()
```

#### :lock_new_ir

```dr
from threading import Lock
m: Lock = Lock()
```

#### :lock_acquire_release_ir

```dr
from threading import Lock
m: Lock = Lock()
m.acquire()
m.release()
```

#### :lock_acquire_nonblocking_bool_ir

```dr
from threading import Lock
m: Lock = Lock()
ok: bool = m.acquire(blocking=False)
```

#### :lock_acquire_timeout_ir

```dr
from threading import Lock
m: Lock = Lock()
ok: bool = m.acquire(blocking=True, timeout=0.5)
```

#### :lock_fast_path_no_overhead_ir

```dr
from threading import Lock
lock: Lock = Lock()
lock.acquire()
lock.release()
```

#### :lock_with_statement_ir

```dr
from threading import Lock
lock: Lock = Lock()
with lock {
  x: int = 1
}
```

#### :thread_block_ir

```dr
thread {
  print("hello")
}
```

#### :fire_block_ir

```dr
fire {
  x: int = 42
}
```

#### :fire_vthread_spawn_ir

```dr
def work() -> int {
  return 42
}
t: Task[int] = fire work()
```

#### :fire_vthread_join_ir

```dr
def work() -> int {
  return 42
}
t: Task[int] = fire work()
r: int = t.join()
```

#### :fire_vthread_is_alive_ir

```dr
def work() -> int {
  return 42
}
t: Task[int] = fire work()
a: int = t.is_alive()
```

#### :async_def_ir

```dr
async def fetch() -> int {
  return 42
}
```

#### :await_ir

```dr
async def fetch() -> int {
  return 42
}
r: int = await fetch()
```

#### :async_def_returns_ptr_ir

```dr
async def compute(x: int) -> int {
  return x * 2
}
task: Task[int] = compute(21)
```

#### :vthread_sleep_ir

```dr
extern "C" def dragon_vthread_sleep(ms: int)
dragon_vthread_sleep(10)
```

#### :sync_list_new_ir

```dr
from collections.concurrent import ConcurrentList
c: ConcurrentList = ConcurrentList()
```

#### :sync_list_append_ir

```dr
from collections.concurrent import ConcurrentList
c: ConcurrentList = ConcurrentList()
c.append(42)
```

#### :sync_dict_new_ir

```dr
from collections.concurrent import ConcurrentDict
c: ConcurrentDict = ConcurrentDict()
```

#### :fire_basic_e2_e

```dr
def worker() -> int {
  return 42
}
t: Task[int] = fire worker()
r: int = t.join()
print(r)
```

#### :fire_return_int_e2_e

```dr
def add(a: int, b: int) -> int {
  return a + b
}
t: Task[int] = fire add(10, 32)
print(t.join())
```

#### :fire_no_args_e2_e

```dr
def hello() -> int {
  return 99
}
t: Task[int] = fire hello()
print(t.join())
```

#### :fire_multiple_e2_e

```dr
def double(x: int) -> int {
  return x * 2
}
t1: Task[int] = fire double(5)
t2: Task[int] = fire double(10)
t3: Task[int] = fire double(15)
r1: int = t1.join()
r2: int = t2.join()
r3: int = t3.join()
print(r1)
print(r2)
print(r3)
```

#### :fire_sequential_e2_e

```dr
def square(x: int) -> int {
  return x * x
}
t1: Task[int] = fire square(3)
r1: int = t1.join()
t2: Task[int] = fire square(7)
r2: int = t2.join()
print(r1)
print(r2)
```

#### :fire_void_method_e2_e

```dr
class Worker {
    def() {}
    def run(n: int) -> None {
        print(n)
    }
}
const w: Worker = Worker()
t: Task[None] = fire w.run(42)
t.join()
```

#### :fire_void_function_e2_e

```dr
def shout(msg: str) -> None {
    print(msg)
}
t: Task[None] = fire shout("hello")
t.join()
```

#### :fire_void_no_return_annotation_e2_e

```dr
def announce(n: int) {
    print(n)
}
t: Task = fire announce(7)
t.join()
```

#### :thread_block_basic_e2_e

```dr
thread {
  print("from thread")
}
print("after join")
```

#### :thread_block_multi_stmt_e2_e

```dr
thread {
  print("a")
  print("b")
  print("c")
}
print("done")
```

#### :fire_block_basic_e2_e

```dr
t: Task = fire {
  print("from block")
}
t.join()
print("done")
```

#### :fire_block_no_join_e2_e

```dr
fire {
  print("bg")
}
x: int = 0
while x < 1000000 {
  x = x + 1
}
print("main")
```

#### :async_await_basic_e2_e

```dr
async def get_value() -> int {
  return 42
}
result: int = await get_value()
print(result)
```

#### :async_await_with_args_e2_e

```dr
async def double_it(x: int) -> int {
  return x * 2
}
result: int = await double_it(21)
print(result)
```

#### :async_await_multiple_args_e2_e

```dr
async def add(a: int, b: int) -> int {
  return a + b
}
result: int = await add(17, 25)
print(result)
```

#### :async_await_in_normal_def_e2_e

```dr
async def fetch() -> int {
  return 99
}
def process() -> int {
  val: int = await fetch()
  return val + 1
}
print(process())
```

#### :async_await_parallel_e2_e

```dr
async def compute(x: int) -> int {
  return x * x
}
t1: Task[int] = compute(3)
t2: Task[int] = compute(4)
r1: int = await t1
r2: int = await t2
print(r1 + r2)
```

#### :async_await_chain_e2_e

```dr
async def step1() -> int {
  return 10
}
async def step2(x: int) -> int {
  return x + 20
}
v: int = await step1()
result: int = await step2(v)
print(result)
```

#### :vthread_sleep_e2_e

```dr
extern "C" def dragon_vthread_sleep(ms: int)
def worker() -> int {
  dragon_vthread_sleep(50)
  return 42
}
t: Task[int] = fire worker()
r: int = t.join()
print(r)
```

#### :vthread_sleep_concurrent_e2_e

```dr
extern "C" def dragon_vthread_sleep(ms: int)
def sleeper(id: int) -> int {
  dragon_vthread_sleep(20)
  return id
}
t1: Task[int] = fire sleeper(1)
t2: Task[int] = fire sleeper(2)
t3: Task[int] = fire sleeper(3)
r1: int = t1.join()
r2: int = t2.join()
r3: int = t3.join()
print(r1 + r2 + r3)
```

#### :sync_list_basic_e2_e

```dr
from collections.concurrent import ConcurrentList
sl: ConcurrentList = ConcurrentList()
sl.append(10)
sl.append(20)
sl.append(30)
print(sl.len())
print(sl.get(0))
print(sl.get(1))
print(sl.get(2))
```

#### :sync_list_pop_set_e2_e

```dr
from collections.concurrent import ConcurrentList
sl: ConcurrentList = ConcurrentList()
sl.append(1)
sl.append(2)
sl.append(3)
sl.set(1, 99)
print(sl.get(1))
v: int = sl.pop(2)
print(v)
print(sl.len())
```

#### :sync_list_sort_reverse_e2_e

```dr
from collections.concurrent import ConcurrentList
sl: ConcurrentList = ConcurrentList()
sl.append(3)
sl.append(1)
sl.append(2)
sl.sort()
print(sl.get(0))
print(sl.get(1))
print(sl.get(2))
sl.reverse()
print(sl.get(0))
```

#### :sync_dict_basic_e2_e

```dr
from collections.concurrent import ConcurrentDict
sd: ConcurrentDict = ConcurrentDict()
sd.set("a", 10)
sd.set("b", 20)
print(sd.get("a"))
print(sd.get("b"))
print(sd.len())
```

#### :sync_dict_get_default_e2_e

```dr
from collections.concurrent import ConcurrentDict
sd: ConcurrentDict = ConcurrentDict()
sd.set("x", 42)
print(sd.get_default("x", 0))
print(sd.get_default("y", -1))
print(sd.has_key("x"))
print(sd.has_key("z"))
```

#### :sync_dict_pop_clear_e2_e

```dr
from collections.concurrent import ConcurrentDict
sd: ConcurrentDict = ConcurrentDict()
sd.set("a", 1)
sd.set("b", 2)
v: int = sd.pop("a")
print(v)
print(sd.len())
sd.clear()
print(sd.len())
```

#### :sync_list_threaded_e2_e

```dr
from collections.concurrent import ConcurrentList
sl: ConcurrentList = ConcurrentList()
def adder(start: int) -> int {
    sl.append(start)
    sl.append(start + 1)
    return 0
}
t1: Task[int] = fire adder(10)
t2: Task[int] = fire adder(20)
t1.join()
t2.join()
print(sl.len())
```

#### :gc_thread_safety_stress_e2_e

```dr
def worker(seed: int) -> int {
  total: int = 0
  for i in range(100000) {
    a: list[int] = [seed, i, seed + i, seed * i]
    b: list[int] = [i, seed, i - seed]
    total = total + 1
  }
  return total
}
t1: Task[int] = fire worker(1)
t2: Task[int] = fire worker(2)
t3: Task[int] = fire worker(3)
t4: Task[int] = fire worker(4)
t5: Task[int] = fire worker(5)
t6: Task[int] = fire worker(6)
t7: Task[int] = fire worker(7)
t8: Task[int] = fire worker(8)
t9: Task[int] = fire worker(9)
t10: Task[int] = fire worker(10)
sum: int = t1.join() + t2.join() + t3.join() + t4.join() + t5.join()
sum = sum + t6.join() + t7.join() + t8.join() + t9.join() + t10.join()
print(sum)
```

#### :v_thread_done_flag_synchronizes

```dr
def work(n: int) -> int { return n * 2 }
t: Task[int] = fire work(21)
r: int = t.join()
print(r)
```

#### :cycle_collector_with_string_fields

```dr
class Node {
  def(name: str) {
    self.name: str = name
    self.next: Optional[Node] = None
  }
}
def make_cycle() {
  a: Node = Node("alpha-string-payload")
  b: Node = Node("beta-string-payload")
  a.next = b
  b.next = a
}
for i in range(800) { make_cycle() }
print("ok")
```

#### :thread_double_start_rejected

```dr
extern "C" def dragon_osthread_new(fn: ptr, args: ptr, nargs: int) -> ptr
extern "C" def dragon_osthread_start(handle: ptr) -> int
extern "C" def dragon_osthread_join(handle: ptr) -> int
def work() -> int { return 7 }
h: ptr = dragon_osthread_new(work, none, 0)
r1: int = dragon_osthread_start(h)
r2: int = dragon_osthread_start(h)
_: int = dragon_osthread_join(h)
print(r1)
print(r2)
```

#### :vthread_sleep_int64_param

```dr
extern "C" def dragon_vthread_sleep(ms: int)
dragon_vthread_sleep(2147483648)
```

#### :vthread_sleep_short_still_works

```dr
extern "C" def dragon_vthread_sleep(ms: int)
def worker() -> int {
  dragon_vthread_sleep(5)
  return 99
}
t: Task[int] = fire worker()
r: int = t.join()
print(r)
```

#### :vthread_sleep_large_value_doesnt_truncate

```dr
extern "C" def dragon_vthread_sleep(ms: int)
def long_sleeper() -> int {
  dragon_vthread_sleep(5000000000)
  return 1
}
def short_worker() -> int {
  dragon_vthread_sleep(10)
  return 42
}
t_long: Task[int] = fire long_sleeper()
t_short: Task[int] = fire short_worker()
r: int = t_short.join()
print(r)
```

#### :nb_recv_bad_fd_returns_minus_one

```dr
extern "C" def dragon_nb_recv(fd: int, buf: ptr, max_len: int) -> int
extern "C" def malloc(n: int) -> ptr
extern "C" def free(p: ptr)
buf: ptr = malloc(64)
r: int = dragon_nb_recv(-1, buf, 64)
free(buf)
print(r)
```

#### :nb_send_bad_fd_returns_minus_one

```dr
extern "C" def dragon_nb_send(fd: int, buf: str, len: int) -> int
msg: str = "hello"
r: int = dragon_nb_send(-1, msg, 5)
print(r)
```

#### :nb_accept_bad_fd_returns_minus_one

```dr
extern "C" def dragon_nb_accept(fd: int, addr: ptr, addrlen: ptr) -> int
r: int = dragon_nb_accept(-1, none, none)
print(r)
```

#### :gc_cycle_collector_mid_construction_traverse_null_check

```dr
extern "C" def dragon_gc_set_threshold(n: int)
class Holder {
  def() {
    self.a: list[int] = [1, 2, 3]
    self.b: list[int] = [4, 5, 6]
    self.c: dict[str, int] = {"x": 1}
    self.d: list[str] = ["alpha", "beta"]
  }
  def sum_a() -> int {
    s: int = 0
    for x in self.a {
      s = s + x
    }
    return s
  }
}
def worker(h: Holder) -> int {
  return h.sum_a()
}
dragon_gc_set_threshold(20)
h: Holder = Holder()
tasks: list[Task[int]] = []
i: int = 0
while i < 8 {
  t: Task[int] = fire worker(h)
  tasks.append(t)
  i = i + 1
}
total: int = 0
for t in tasks {
  total = total + t.join()
}
print(total)
```

#### :shared_refcount_atomic_dispatch__fire_multi_worker

```dr
def make_str(p: str, n: int) -> str {
  return p + str(n)
}
def worker(s: list[str]) -> int {
  n: int = 0
  i: int = 0
  while i < 5000 {
    j: int = i % 8
    x: str = s[j]
    n = n + len(x)
    i = i + 1
  }
  return n
}
shared: list[str] = []
k: int = 0
while k < 8 {
  shared.append(make_str("item_", k))
  k = k + 1
}
tasks: list[Task[int]] = []
w: int = 0
while w < 16 {
  t: Task[int] = fire worker(shared)
  tasks.append(t)
  w = w + 1
}
total: int = 0
for t in tasks {
  total = total + t.join()
}
print(total)
```

#### :fire_v_thread_uncaught_exception_contained

```dr
def good(n: int) -> int {
  return n * 2
}
def bad(n: int) -> int {
  raise ValueError("intentional")
  return -1
}
a: Task[int] = fire good(10)
b: Task[int] = fire bad(99)
c: Task[int] = fire good(20)
ra: int = a.join()
rb: int = b.join()
rc: int = c.join()
print(ra)
print(rb)
print(rc)
print("alive")
```

#### :task_int_join_recovers_native_int

```dr
def work() -> int { return 21 }
t: Task[int] = fire work()
r: int = t.join()
print(r)
```

#### :task_float_join_bitcasts_not_converts

```dr
def fw() -> float { return 3.5 }
t: Task[float] = fire fw()
r: float = t.join()
print(r)
```

#### :task_str_join_recovers_pointer

```dr
def sw() -> str { return "hello" }
t: Task[str] = fire sw()
r: str = t.join()
print(r)
```

#### :await_async_def_recovers_native_int

```dr
async def fetch() -> int { return 99 }
r: int = await fetch()
print(r)
```

#### :bare_task_annotation_refines_and_joins

```dr
def work() -> int { return 42 }
t: Task = fire work()
r: int = t.join()
print(r)
```

#### :lock_own_field_with_ir

```dr
from threading import Lock

class Counter {
    value: int = 0
    own lock: Lock = Lock()
    def bump() -> None {
        with self.lock {
            self.value = self.value + 1
        }
    }
}

c: Counter = Counter()
c.bump()
print(c.value)
```

#### :lock_other_object_field_with_ir

```dr
from threading import Lock

class Counter {
    value: int = 0
    own lock: Lock = Lock()
}

def bump_other(c: Counter) -> None {
    with c.lock {
        c.value = c.value + 1
    }
}

c: Counter = Counter()
bump_other(c)
print(c.value)
```

#### :lock_nested_field_with_ir

```dr
from threading import Lock

class Inner {
    own lock: Lock = Lock()
}

class Outer {
    value: int = 0
    inner: Inner = Inner()
    def bump() -> None {
        with self.inner.lock {
            self.value = self.value + 1
        }
    }
}

o: Outer = Outer()
o.bump()
print(o.value)
```

#### :lock_parameter_with_ir

```dr
from threading import Lock

class Counter {
    value: int = 0
    own lock: Lock = Lock()
}

def bump_guarded(l: Lock, c: Counter) -> None {
    with l {
        c.value = c.value + 1
    }
}

c: Counter = Counter()
bump_guarded(c.lock, c)
print(c.value)
```

#### :lock_local_bound_from_field_with_ir

```dr
from threading import Lock

class Counter {
    value: int = 0
    own lock: Lock = Lock()
    def bump() -> None {
        borrowed: Lock = self.lock
        with borrowed {
            self.value = self.value + 1
        }
    }
}

c: Counter = Counter()
c.bump()
print(c.value)
```

#### :lock_owned_local_still_destroyed_ir

```dr
from threading import Lock

def guarded(n: int) -> int {
    mine: Lock = Lock()
    total: int = 0
    with mine {
        total = n + 1
    }
    return total
}

print(guarded(1))
```

#### :lock_field_acquire_release_ir

```dr
from threading import Lock

class Counter {
    value: int = 0
    own lock: Lock = Lock()
    def bump() -> None {
        self.lock.acquire()
        self.value = self.value + 1
        self.lock.release()
    }
}

c: Counter = Counter()
c.bump()
print(c.value)
```

#### :lock_shared_across_fire_ir

```dr
from threading import Lock

class Shared {
    seen: list[str] = []
    own lock: Lock = Lock()
    def add(tag: str) -> None {
        with self.lock {
            self.seen.append(tag)
        }
    }
}

s: Shared = Shared()
t: Task[None] = fire s.add("a")
s.add("b")
t.join()
print(len(s.seen))
```
