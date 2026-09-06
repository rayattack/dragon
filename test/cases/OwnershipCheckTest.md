# OwnershipCheckTest cases

Source programs for `test/OwnershipCheckTest.cpp`. Each block is named by the heading
above it and pulled in with `code("<name>")`.

#### :del_after_container_store_errors

```dr
def f() -> int {
    cache: dict[str, str] = {}
    buf: str = "abc" + "def"
    cache["k"] = buf
    del buf
    return 1
}
```

#### :del_of_captured_local_errors

```dr
def f() -> int {
    x: str = "a" + "b"
    g: Callable[[], str] = lambda () -> str { return x }
    del x
    return 1
}
```

#### :del_of_aliased_owner_errors

```dr
def f() -> int {
    x: str = "a" + "b"
    y: str = x
    del x
    return len(y)
}
```

#### :double_del_errors

```dr
def f() -> int {
    x: str = "a" + "b"
    del x
    del x
    return 1
}
```

#### :del_of_borrowed_element_errors

```dr
def f() -> int {
    xs: list[str] = ["aa", "bb"]
    e: str = xs[0]
    del e
    return 1
}
```

#### :del_of_field_errors

```dr
class C {
    buf: str = ""
    def clear() {
        del self.buf
    }
}
```

#### :conditional_del_then_use_errors

```dr
def f(c: bool) -> int {
    x: str = "a" + "b"
    if c {
        del x
    }
    return len(x)
}
```

#### :del_of_outer_binding_in_loop_errors

```dr
def f() -> int {
    x: str = "a" + "b"
    i: int = 0
    while i < 3 {
        del x
        i = i + 1
    }
    return 1
}
```

#### :use_after_del_errors

```dr
def f() -> int {
    x: str = "a" + "b"
    del x
    return len(x)
}
```

#### :del_of_plain_param_errors

```dr
def f(s: str) -> int {
    del s
    return 1
}
```

#### :del_of_with_subject_errors

```dr
def f() -> int {
    with open("x") as r {
        del r
    }
    return 1
}
```

#### :own_field_borrow_store_errors

```dr
class Box {
    own _s: str
    def(s: str) {
        self._s = s
    }
}
```

#### :own_param_plain_store_into_own_field_compiles

```dr
class Box {
    own _s: str
    def(own s: str) {
        self._s = s
    }
}
```

#### :fresh_owned_local_plain_store_into_own_field_compiles

```dr
class Box {
    own _s: str
    def() {
        v: str = "a" + "b"
        self._s = v
    }
}
```

#### :plain_store_into_own_field_then_use_is_use_after_move

```dr
class Box {
    own _s: str
    def(own s: str) -> int {
        self._s = s
        return len(s)
    }
}
```

#### :plain_store_of_escaped_owner_into_own_field_errors

```dr
class Box {
    own _s: str
    def(own s: str, sink: dict[str, str]) {
        sink["k"] = s
        self._s = s
    }
}
```

#### :own_scalar_field_errors

```dr
class Box {
    own n: int = 0
    def() { self.n = 1 }
}
```

#### :own_local_errors

```dr
def f() {
    own x: str = "a"
}
```

#### :plain_lock_field_errors

```dr
class Router {
    _storage_lock: Lock
    def() { self._storage_lock = Lock() }
}
```

#### :undeclared_lock_field_store_errors

```dr
class Cache {
    def() {
        self.lk = Lock()
    }
}
```

#### :conditional_del_no_use_errors_at_join

```dr
def branchy(cond: bool) {
    x: str = "a" + "b"
    if cond {
        del x
    }
}
```

#### :use_after_move_errors

```dr
def consume(own s: str) -> int { return len(s) }
def f() -> int {
    b: str = "a" + "b"
    n: int = consume(own b)
    return n + len(b)
}
```

#### :conditional_move_no_use_errors_at_join

```dr
def consume(own s: str) -> int { return len(s) }
def f(c: bool) {
    x: str = "a" + "b"
    if c {
        consume(own x)
    }
}
```

#### :move_to_alias_destination_errors

```dr
def f() {
    x: str = "a" + "b"
    y: str = own x
}
```

#### :double_move_errors

```dr
def consume(own s: str) -> int { return len(s) }
def f() {
    x: str = "a" + "b"
    consume(own x)
    consume(own x)
}
```

#### :move_of_escaped_owner_errors

```dr
def consume(own s: str) -> int { return len(s) }
def f() {
    cache: dict[str, str] = {}
    x: str = "a" + "b"
    cache["k"] = x
    consume(own x)
}
```

#### :mutation_during_iteration_errors

```dr
def f() {
    names: list[str] = ["a", "tmp1", "tmp2", "b"]
    for name in names {
        names.remove(name)
    }
}
```

#### :subscript_store_during_iteration_errors

```dr
def f() {
    xs: list[int] = [1, 2, 3]
    for x in xs {
        xs[0] = x
    }
}
```

#### :mutating_a_different_container_compiles

```dr
def f() {
    xs: list[int] = [1, 2, 3]
    out: list[int] = []
    for x in xs {
        out.append(x)
    }
}
```

#### :dub_immutable_iterable_errors

```dr
def f() {
    s: str = "abc" + "def"
    for c in dub s {
        print(c)
    }
}
```

#### :touch_while_lent_errors

```dr
class Op { name: str
 def(n: str) { self.name = n } }
def work(o: Op) -> int { o.name = "x"
 return len(o.name) }
def f() -> int {
    o: Op = Op("inc")
    t: Task[int] = fire work(o)
    n: int = len(o.name)
    const r: int = await t
    return r + n
}
```

#### :discarded_handle_borrow_errors

```dr
class Op { name: str
 def(n: str) { self.name = n } }
def work(o: Op) -> int { o.name = "x"
 return len(o.name) }
def f() {
    o: Op = Op("inc")
    fire work(o)
}
```

#### :still_lent_at_scope_end_errors

```dr
class Op { name: str
 def(n: str) { self.name = n } }
def work(o: Op) -> int { o.name = "x"
 return len(o.name) }
def f() {
    o: Op = Op("inc")
    t: Task[int] = fire work(o)
}
```

#### :task_rebind_while_lent_errors

```dr
class Op { name: str
 def(n: str) { self.name = n } }
def work(o: Op) -> int { o.name = "x"
 return len(o.name) }
def f() -> int {
    o: Op = Op("inc")
    t: Task[int] = fire work(o)
    t = fire work(o)
    return await t
}
```

#### :lend_then_await_then_use_compiles

```dr
class Op { name: str
 def(n: str) { self.name = n } }
def work(o: Op) -> int { return len(o.name) }
def f() -> int {
    o: Op = Op("inc")
    t: Task[int] = fire work(o)
    const r: int = await t
    return r + len(o.name)
}
```

#### :fire_read_only_list_fan_out_accepted

```dr
def worker(s: list[str]) -> int {
    n: int = 0
    i: int = 0
    while i < len(s) {
        n = n + len(s[i])
        i = i + 1
    }
    return n
}
def run() -> int {
    shared: list[str] = ["a", "b"]
    tasks: list[Task[int]] = []
    w: int = 0
    while w < 4 {
        t: Task[int] = fire worker(shared)
        tasks.append(t)
        w = w + 1
    }
    total: int = 0
    for t in tasks { total = total + t.join() }
    return total
}
```

#### :fire_read_only_discarded_handle_accepted

```dr
def worker(s: list[str]) -> int { return len(s) }
def run() -> None {
    shared: list[str] = ["a", "b"]
    fire worker(shared)
}
```

#### :fire_mutating_worker_in_loop_rejected

```dr
def mutate(s: list[int]) -> int {
    s.append(1)
    return len(s)
}
def run() -> int {
    shared: list[int] = [1, 2, 3]
    tasks: list[Task[int]] = []
    w: int = 0
    while w < 4 {
        t: Task[int] = fire mutate(shared)
        tasks.append(t)
        w = w + 1
    }
    return len(tasks)
}
```

#### :del_after_read_only_share_rejected

```dr
def worker(s: list[str]) -> int { return len(s) }
def run() -> int {
    shared: list[str] = ["a", "b"]
    t: Task[int] = fire worker(shared)
    del shared
    return t.join()
}
```

A borrowing callee that writes its parameter closes door 5. The refusal must
say which write closed it, offer `dub` only when the type is dubable, and never
offer a bare `own` at the call site: `own` on a borrowing parameter is E14, so
the honest fix is to declare the parameter own in the callee first.

#### :fire_borrowing_writer_rejected

```dr
def mutate(s: list[int]) -> int {
    s.append(1)
    return len(s)
}
def run() -> None {
    shared: list[int] = [1, 2, 3]
    fire mutate(shared)
}
```

#### :fire_keyword_argument_read_only_accepted

```dr
def worker(s: list[str]) -> int { return len(s) }
def run() -> None {
    shared: list[str] = ["a", "b"]
    fire worker(s=shared)
}
```

#### :fire_non_dubable_writer_rejected

```dr
class Box {
    items: list[int]
    def() { self.items = [] }
}
def fill(b: Box) -> int {
    b.items = [1]
    return len(b.items)
}
def run() -> None {
    b: Box = Box()
    fire fill(b)
}
```

The read-only proof is a property of the callee body, so it must travel with an
imported function exactly as it does for a same-file one. These cases share one
external module, registered under the name `readers`.

#### :fire_readers_module

```dr
def read_only(d: dict[str, int]) -> int { return len(d) }
def writer(d: dict[str, int]) -> int {
    d["x"] = 1
    return len(d)
}
def taker(own d: dict[str, int]) -> int { return len(d) }
```

#### :fire_imported_read_only_accepted

```dr
from readers import read_only
def run() -> None {
    doc: dict[str, int] = {"a": 1}
    fire read_only(doc)
}
```

#### :fire_imported_aliased_read_only_accepted

```dr
from readers import read_only as ro
def run() -> None {
    doc: dict[str, int] = {"a": 1}
    fire ro(doc)
}
```

#### :fire_imported_qualified_read_only_accepted

```dr
import readers
def run() -> None {
    doc: dict[str, int] = {"a": 1}
    fire readers.read_only(doc)
}
```

#### :fire_imported_writer_rejected

```dr
from readers import writer
def run() -> None {
    doc: dict[str, int] = {"a": 1}
    fire writer(doc)
}
```

#### :fire_imported_own_taker_rejected

```dr
from readers import taker
def run() -> None {
    doc: dict[str, int] = {"a": 1}
    fire taker(doc)
}
```

#### :use_after_defer_own_move_errors

```dr
def sink(own d: list[int]) -> None { }
def f() -> int {
    d: list[int] = [1, 2]
    defer sink(own d)
    return len(d)
}
```

#### :double_move_after_defer_own_errors

```dr
def sink(own d: list[int]) -> None { }
def f() -> None {
    d: list[int] = [1, 2]
    defer sink(own d)
    sink(own d)
}
```

#### :own_move_of_defer_pinned_arg_errors

```dr
def use(d: list[int]) -> None { }
def sink(own d: list[int]) -> None { }
def f() -> None {
    d: list[int] = [1, 2]
    defer use(d)
    sink(own d)
}
```

#### :own_move_of_deferred_receiver_errors

```dr
class R {
    def() { }
    def close() -> None { }
}
def sink(own r: R) -> None { }
def f() -> None {
    r: R = R()
    defer r.close()
    sink(own r)
}
```

#### :del_of_defer_pinned_binding_errors

```dr
def use(d: list[int]) -> None { }
def f() -> None {
    d: list[int] = [1, 2]
    defer use(d)
    del d
}
```

#### :pin_expires_with_defer_scope

```dr
def use(d: list[int]) -> None { }
def sink(own d: list[int]) -> None { }
def f(flag: bool) -> None {
    d: list[int] = [1, 2]
    if flag {
        defer use(d)
    }
    sink(own d)
}
```

#### :defer_borrow_then_continued_use_accepted

```dr
def use(d: list[int]) -> None { }
def f() -> int {
    d: list[int] = [1, 2]
    defer use(d)
    d.append(3)
    return len(d)
}
```

#### :defer_own_with_no_later_use_accepted

```dr
def sink(own d: list[int]) -> None { }
def f() -> None {
    d: list[int] = [1, 2]
    defer sink(own d)
}
```

#### :defer_dub_leaves_source_live

```dr
def use(d: list[int]) -> None { }
def sink(own d: list[int]) -> None { }
def f() -> None {
    d: list[int] = [1, 2]
    defer use(dub d)
    sink(own d)
}
```

#### :own_ctor_move_then_use_errors

```dr
def f() -> int {
    h: H = H(4)
    r: R = R(own h)
    return h.fd()
}
```

#### :own_ctor_double_move_errors

```dr
def f() -> int {
    h: H = H(4)
    r1: R = R(own h)
    r2: R = R(own h)
    return r1.probe()
}
```

#### :own_ctor_fresh_temporary_accepted

```dr
def f() -> int {
    r: R = R(H(4))
    return r.probe()
}
```

#### :own_ctor_move_accepted

```dr
def f() -> int {
    h: H = H(4)
    r: R = R(own h)
    return r.probe()
}
```

#### :del_of_lent_task_errors_at_del_site

```dr
class Counter {
    n: int
    def(n: int) {
        self.n = n
    }
}
def worker(c: Counter) -> int {
    c.n = c.n + 1
    return c.n
}
def run() -> int {
    c: Counter = Counter(7)
    t: Task[int] = fire worker(c)
    del t
    return 0
}
```

#### :del_of_unlent_task_accepted

```dr
async def make(n: int) -> str {
    return "x"
}
def run() -> None {
    t: Task[str] = make(1)
    del t
}
```

#### :double_await_rejected

```dr
async def make(n: int) -> str {
    return str(n)
}
def run() -> str {
    t: Task[str] = make(1)
    a: str = await t
    b: str = await t
    return a + b
}
```

#### :await_then_join_rejected

```dr
async def make(n: int) -> str {
    return str(n)
}
def run() -> str {
    t: Task[str] = make(1)
    a: str = await t
    return t.join()
}
```

#### :is_alive_after_await_rejected

```dr
async def make(n: int) -> str {
    return str(n)
}
def run() -> bool {
    t: Task[str] = make(1)
    a: str = await t
    return t.is_alive()
}
```

#### :await_in_loop_of_outer_task_rejected

```dr
async def make(n: int) -> str {
    return str(n)
}
def run() -> int {
    t: Task[str] = make(1)
    total: int = 0
    i: int = 0
    while i < 3 {
        s: str = await t
        total = total + len(s)
        i = i + 1
    }
    return total
}
```

#### :conditional_await_then_second_await_rejected

```dr
async def make(n: int) -> str {
    return str(n)
}
def run(ready: bool) -> str {
    t: Task[str] = make(1)
    early: str = ""
    if ready {
        early = await t
    }
    late: str = await t
    return early + late
}
```

#### :conditional_await_alone_accepted

```dr
async def make(n: int) -> str {
    return str(n)
}
def run(ready: bool) -> str {
    t: Task[str] = make(1)
    if ready {
        return await t
    }
    return "skipped"
}
```

#### :poll_then_await_accepted

```dr
async def make(n: int) -> str {
    return str(n)
}
def run() -> str {
    t: Task[str] = make(1)
    while t.is_alive() {
        pass
    }
    return await t
}
```

#### :rebind_then_await_accepted

```dr
async def make(n: int) -> str {
    return str(n)
}
def run() -> str {
    t: Task[str] = make(1)
    a: str = await t
    t = make(2)
    b: str = await t
    return a + b
}
```

#### :keyword_move_marks_binding_moved

```dr
def take(own s: str) -> int { return len(s) }
def f() -> int {
    b: str = "abc" + "def"
    take(s=own b)
    return len(b)
}
```

#### :keyword_move_without_reuse_accepted

```dr
def take(own s: str) -> int { return len(s) }
def f() -> int {
    b: str = "abc" + "def"
    return take(s=own b)
}
```

#### :keyword_dub_keeps_binding_usable

```dr
def borrows(s: str) -> int { return len(s) }
def f() -> int {
    b: str = "abc" + "def"
    return borrows(s=dub b) + len(b)
}
```

#### :dub_into_own_param_keeps_binding_usable

```dr
def take(own s: str) -> int { return len(s) }
def f() -> int {
    b: str = "abc" + "def"
    return take(dub b) + len(b)
}
```

#### :returning_an_own_collection_field_errors

```dr
class Config {
    own headers: dict[str, str]
    def() { self.headers = {"Accept": "text/html"} }
    def get_headers() -> dict[str, str] { return self.headers }
}
```

#### :returning_an_own_field_via_a_local_still_errors

```dr
class Config {
    own headers: dict[str, str]
    def() { self.headers = {"Accept": "text/html"} }
    def leak() -> dict[str, str] {
        h: dict[str, str] = self.headers
        return h
    }
}
```

#### :returning_an_own_instance_field_advises_a_method

```dr
class Inner {
    n: int
    def() { self.n = 1 }
}
class Holder {
    own inst: Inner
    def() { self.inst = Inner() }
    def leak() -> Inner { return self.inst }
}
```

#### :dubbing_an_own_field_out_is_the_sanctioned_exit

```dr
class Config {
    own headers: dict[str, str]
    def() { self.headers = {"Accept": "text/html"} }
    def snapshot() -> dict[str, str] { return dub self.headers }
    def header(k: str) -> str { return self.headers.get(k, "") }
    def set_header(k: str, v: str) -> None { self.headers[k] = v }
    def count() -> int { return len(self.headers) }
}
```

#### :immutable_own_fields_are_not_sealed

```dr
class Sink {
    own payload: str
    def(own payload: str) { self.payload = own payload }
    def read() -> str { return self.payload }
}
```

#### :plain_collection_field_stays_unsealed

```dr
class Config {
    headers: dict[str, str]
    def() { self.headers = {"Accept": "text/html"} }
    def get_headers() -> dict[str, str] { return self.headers }
}
```

Door 4 (an internally locked type) is a property of the class, so it must hold
wherever the class was declared. The shadowed case keeps the rule conservative:
when two classes share a bare name and only one carries the lock, neither is
admitted through door 4.

#### :fire_locked_module

```dr
class Guarded {
    own lock: Lock
    hits: int
    def() {
        self.lock = Lock()
        self.hits = 0
    }
}
```

#### :fire_imported_locked_type_accepted

```dr
from guarded import Guarded
def bump(g: Guarded) -> None {
    with g.lock {
        g.hits = g.hits + 1
    }
}
def run() -> None {
    g: Guarded = Guarded()
    fire bump(g)
}
```

#### :fire_shadowed_locked_name_refused

```dr
class Guarded {
    hits: int
    def() { self.hits = 0 }
}
def bump(g: Guarded) -> None {
    g.hits = g.hits + 1
}
def run() -> None {
    g: Guarded = Guarded()
    fire bump(g)
}
```
