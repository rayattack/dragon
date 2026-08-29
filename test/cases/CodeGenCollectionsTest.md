# CodeGenCollectionsTest cases

Source programs for `test/CodeGenCollectionsTest.cpp`. Each block is named by the heading
above it and pulled in with `code("<name>")`.

#### :dict_dot_access_read_ir

```dr
d: dict = {"x": 1}
v: int = d.x
```

#### :dict_dot_access_write_ir

```dr
d: dict = {"x": 1}
d.x = 2
```

#### :dict_get_checked_ir

```dr
d: dict = {name: "Jon", age: 10}
x: int = d["age"]
```

#### :dict_get_checked_dot_access_ir

```dr
d: dict = {name: "Jon", age: 10}
x: int = d.age
```

#### :typed_dict_ir

```dr
class Config(TypedDict) {
    host: str
    port: int
}
cfg: Config = Config({host: "localhost", port: 8080})
```

#### :typed_dict_checked_access_ir

```dr
class Config(TypedDict) {
    host: str
    port: int
}
cfg: Config = Config({host: "localhost", port: 8080})
h: str = cfg["host"]
```

#### :list_basic

```dr
x: list[int] = [10, 20, 30]
print(x[0])
print(x[1])
print(x[2])
```

#### :list_append

```dr
x: list[int] = [1, 2]
x.append(3)
x.append(4)
print(len(x))
print(x[2])
print(x[3])
```

#### :list_negative_index

```dr
x: list[int] = [10, 20, 30]
print(x[-1])
print(x[-2])
```

#### :list_in_loop

```dr
x: list[int] = [0, 0, 0, 0, 0]
for i in range(5) {
  x.append(i * i)
}
print(x[5])
print(x[8])
print(len(x))
```

#### :list_insert

```dr
x: list[int] = [1, 3, 4]
x.insert(1, 2)
print(x[0])
print(x[1])
print(x[2])
print(x[3])
```

#### :list_remove

```dr
x: list[int] = [1, 2, 3, 2, 4]
x.remove(2)
print(len(x))
print(x[1])
```

#### :list_pop

```dr
x: list[int] = [10, 20, 30]
v: int = x.pop()
print(v)
print(len(x))
```

#### :list_pop_index

```dr
x: list[int] = [10, 20, 30]
v: int = x.pop(0)
print(v)
print(x[0])
```

#### :list_clear

```dr
x: list[int] = [1, 2, 3]
x.clear()
print(len(x))
```

#### :list_extend

```dr
x: list[int] = [1, 2]
y: list[int] = [3, 4, 5]
x.extend(y)
print(len(x))
print(x[3])
```

#### :list_index

```dr
x: list[int] = [10, 20, 30, 40]
print(x.index(30))
```

#### :list_count

```dr
x: list[int] = [1, 2, 3, 2, 1, 2]
print(x.count(2))
```

#### :list_sort

```dr
x: list[int] = [3, 1, 4, 1, 5, 9, 2, 6]
x.sort()
print(x[0])
print(x[1])
print(x[7])
```

#### :list_reverse

```dr
x: list[int] = [1, 2, 3, 4]
x.reverse()
print(x[0])
print(x[3])
```

#### :list_copy

```dr
x: list[int] = [1, 2, 3]
y: list[int] = x.copy()
x.append(4)
print(len(x))
print(len(y))
```

#### :dict_basic

```dr
d: dict[str, int] = {"a": 1, "b": 2}
print(d["a"])
print(len(d))
```

#### :dict_values

```dr
d: dict[str, int] = {"a": 1, "b": 2}
v: list[int] = d.values()
print(len(v))
```

#### :dict_pop

```dr
d: dict[str, int] = {"a": 1, "b": 2, "c": 3}
v: int = d.pop("b")
print(v)
print(len(d))
```

#### :dict_clear

```dr
d: dict[str, int] = {"a": 1, "b": 2}
d.clear()
print(len(d))
```

#### :dict_setdefault

```dr
d: dict[str, int] = {"a": 1}
v: int = d.setdefault("a", 99)
print(v)
w: int = d.setdefault("b", 42)
print(w)
print(len(d))
```

#### :dict_copy

```dr
d: dict[str, int] = {"a": 1, "b": 2}
e: dict[str, int] = d.copy()
print(len(e))
print(e["a"])
```

#### :bare_key_dict_basic

```dr
d: dict = {name: "Jon", age: 10}
print(d["name"])
print(d["age"])
```

#### :bare_key_dict_mixed

```dr
d: dict = {host: "localhost", "port": 8080}
print(d["host"])
print(d["port"])
```

#### :computed_key_dict

```dr
field: str = "age"
d: dict = {name: "Jon", (field): 10}
print(d["name"])
print(d["age"])
```

#### :dict_dot_access_read

```dr
d: dict = {"name": "Jon", "age": 25}
print(d.name)
print(d.age)
```

#### :dict_dot_access_read_bare_key

```dr
d: dict = {name: "Jon", age: 25}
print(d.name)
print(d.age)
```

#### :dict_dot_access_write

```dr
d: dict = {"x": 1}
d.x = 42
print(d.x)
```

#### :dict_dot_access_write_new

```dr
d: dict = {"x": 1}
d.y = 99
print(d["y"])
```

#### :dict_methods_still_work

```dr
d: dict = {name: "Jon", age: 25}
v: str = d.get("name")
print(v)
```

#### :dict_items_tuple_unpacking

```dr
d: dict[str, int] = {"a": 1, "b": 2, "c": 3}
for k, v in d.items() {
    print(k)
    print(v)
}
```

#### :dict_popitem_lifo_order

```dr
d: dict[str, int] = {"a": 1, "b": 2, "c": 3}
d.popitem()
print(len(d))
print("c" in d)
print("a" in d)
```

#### :dict_popitem_repeated

```dr
d: dict[str, int] = {"a": 1, "b": 2, "c": 3}
d.popitem()
d.popitem()
print(len(d))
print("a" in d)
```

#### :dict_fromkeys_with_default

```dr
keys: list[str] = ["x", "y", "z"]
d: dict[str, int] = dict.fromkeys(keys, 99)
print(len(d))
print(d["x"])
print(d["y"])
print(d["z"])
```

#### :dict_fromkeys_without_default

```dr
d: dict[str, int] = dict.fromkeys(["a", "b"])
print(len(d))
print("a" in d)
print("b" in d)
```

#### :tuple_unpack_assignment

```dr
def pair() -> tuple[int, int] {
    return (3, 4)
}
a, b = pair()
print(a + b)
```

#### :dict_items_value_var_kind_str

```dr
d: dict[str, str] = {"a": "alpha", "b": "beta"}
for k, v in d.items() {
    print(k)
    print(v)
}
```

#### :dict_items_value_var_kind_int

```dr
d: dict[str, int] = {"a": 1, "b": 2}
for k, v in d.items() {
    print(v + 10)
}
```

#### :bare_key_dict_single_entry

```dr
d: dict = {status: 200}
print(d["status"])
```

#### :dict_dot_access_read_write_combined

```dr
d: dict = {count: 0}
d.count = 5
d.label = "items"
print(d.count)
print(d.label)
```

#### :dict_with_nested_containers_e2_e

```dr
d: dict = {"a": 1, "b": 2}
print(d["a"])
print(d["b"])
```

#### :dict_get_checked_correct_type

```dr
d: dict = {name: "Jon", age: 10}
x: int = d["age"]
print(x)
```

#### :dict_get_checked_string_correct

```dr
d: dict = {name: "Jon", age: 10}
x: str = d["name"]
print(x)
```

#### :dict_get_checked_dot_access

```dr
d: dict = {name: "Jon", age: 10}
x: int = d.age
print(x)
```

#### :typed_dict_basic

```dr
class Config(TypedDict) {
    host: str
    port: int
}
cfg: Config = Config({host: "localhost", port: 8080})
print(cfg["host"])
print(cfg["port"])
```

#### :typed_dict_dot_access

```dr
class Config(TypedDict) {
    host: str
    port: int
    debug: bool
}
cfg: Config = Config({host: "localhost", port: 8080, debug: True})
print(cfg.host)
print(cfg.port)
print(cfg.debug)
```

#### :typed_dict_mixed_types

```dr
class Settings(TypedDict) {
    name: str
    count: int
    ratio: float
    active: bool
}
s: Settings = Settings({name: "test", count: 42, ratio: 3.14, active: True})
print(s["name"])
print(s["count"])
```

#### :typed_dict_annotated_access

```dr
class Config(TypedDict) {
    host: str
    port: int
}
cfg: Config = Config({host: "localhost", port: 8080})
h: str = cfg["host"]
p: int = cfg["port"]
print(h)
print(p)
```

#### :typed_dict_without_annotation

```dr
class Config(TypedDict) {
    host: str
    port: int
}
cfg: Config = Config({host: "localhost", port: 8080})
print(cfg["host"])
print(cfg["port"])
```

#### :typed_dict_double_star_spread

```dr
class Customer(TypedDict) {
    id: int
    name: str
}
row: dict[str, int | float | str | bytes | list[int] | list[str] | list[float] | list[list[int]] | list[list[str]] | dict[str, int] | dict[str, str] | tuple[]] = {}
row["id"] = 1
row["name"] = "Ada"
c: Customer = Customer(**row)
print(c["id"])
print(c["name"])
```

#### :typed_dict_double_star_spread_in_comprehension

```dr
class Customer(TypedDict) {
    id: int
    name: str
}
rows: list[dict[str, int | str]] = []
r1: dict[str, int | float | str | bytes | list[int] | list[str] | list[float] | list[list[int]] | list[list[str]] | dict[str, int] | dict[str, str] | tuple[]] = {}
r1["id"] = 1
r1["name"] = "Ada"
rows.append(r1)
cs: list[Customer] = [Customer(**r) for r in rows]
for c in cs { print(c["name"]) }
```

#### :tuple_create_and_print

```dr
t: tuple[int, int, int] = (1, 2, 3)
print(t)
```

#### :tuple_single_element

```dr
t: tuple[int] = (42,)
print(t)
```

#### :tuple_index_access

```dr
t: tuple[int, int, int] = (10, 20, 30)
print(t[0])
print(t[1])
print(t[2])
```

#### :tuple_negative_index

```dr
t: tuple[int, int, int] = (10, 20, 30)
print(t[-1])
print(t[-2])
```

#### :tuple_len

```dr
t: tuple[int, int, int, int, int] = (1, 2, 3, 4, 5)
print(len(t))
```

#### :tuple_empty_len

```dr
t: int | float | str | bytes | list[int] | list[str] | list[float] | list[list[int]] | list[list[str]] | dict[str, int] | dict[str, str] | tuple[] = ()
print(len(t))
```

#### :tuple_in_function

```dr
def make_pair(a: int, b: int) -> int {
    t: tuple[int, int] = (a, b)
    return t[0] + t[1]
}
print(make_pair(3, 4))
```

#### :tuple_unpack_simple

```dr
a, b = (1, 2)
print(a)
print(b)
```

#### :tuple_unpack_three

```dr
a, b, c = (10, 20, 30)
print(a)
print(b)
print(c)
```

#### :tuple_unpack_from_rhs_tuple

```dr
a, b = 100, 200
print(a)
print(b)
```

#### :starred_unpack_first

```dr
first, *rest = [10, 20, 30, 40]
print(first)
print(len(rest))
```

#### :starred_unpack_last

```dr
*init, last = [10, 20, 30, 40]
print(len(init))
print(last)
```

#### :for_loop_tuple_unpack

```dr
pairs: list[tuple[int, int]] = [(1, 10), (2, 20), (3, 30)]
for a, b in pairs {
    print(a + b)
}
```

#### :set_len

```dr
s: set = {10, 20, 30}
print(len(s))
```

#### :set_deduplicate

```dr
s: set = {1, 2, 2, 3, 3, 3}
print(len(s))
```

#### :set_empty_len

```dr
s: set = {42}
print(len(s))
```

#### :set_contains_via_in

```dr
s: set = {10, 20, 30}
if 20 in s {
    print(1)
} else {
    print(0)
}
```

#### :set_not_contains_via_in

```dr
s: set = {10, 20, 30}
if 99 in s {
    print(1)
} else {
    print(0)
}
```

#### :immortal_object_survives_decref

```dr
extern "C" def dragon_make_immortal(obj: list[int]) -> None
extern "C" def dragon_is_immortal_obj(obj: list[int]) -> int
x: list[int] = [1, 2, 3]
dragon_make_immortal(x)
result: int = dragon_is_immortal_obj(x)
print(result)
```

#### :non_immortal_object_reports_zero

```dr
extern "C" def dragon_is_immortal_obj(obj: list[int]) -> int
x: list[int] = [10, 20]
result: int = dragon_is_immortal_obj(x)
print(result)
```

#### :list_spread_basic

```dr
a: list[int] = [1, 2]
b: list[int] = [3, 4]
c: list[int] = [*a, *b]
print(len(c))
```

#### :list_spread_with_literals

```dr
a: list[int] = [2, 3]
c: list[int] = [1, *a, 4]
print(len(c))
```

#### :dict_spread_basic

```dr
a: dict = {"x": 1}
b: dict = {"y": 2}
c: dict = {**a, **b}
print(len(c))
```

#### :dict_spread_with_entries

```dr
base: dict = {"host": "localhost"}
merged: dict = {**base, "port": 8080}
print(len(merged))
```

#### :dict_spread_override

```dr
a: dict = {"x": 1}
b: dict = {"x": 99}
c: dict = {**a, **b}
print(c["x"])
```

#### :deque_append_popleft

```dr
d: deque[int] = deque()
d.append(1)
d.append(2)
d.append(3)
print(d.popleft())
print(d.popleft())
print(d.popleft())
```

#### :deque_len

```dr
d: deque[int] = deque()
d.append(10)
d.append(20)
print(len(d))
d.popleft()
print(len(d))
```

#### :deque_appendleft

```dr
d: deque[int] = deque()
d.appendleft(1)
d.appendleft(2)
d.appendleft(3)
print(d.popleft())
print(d.popleft())
```

#### :deque_pop

```dr
d: deque[int] = deque()
d.append(1)
d.append(2)
d.append(3)
print(d.pop())
print(d.pop())
```

#### :deque_from_list

```dr
items: list[int] = [10, 20, 30]
d: deque[int] = deque(items)
print(len(d))
print(d.popleft())
```

#### :list_repeat_bool

```dr
x: list[bool] = [True] * 5
print(len(x))
print(x[0])
print(x[4])
```

#### :list_repeat_int

```dr
x: list[int] = [1, 2] * 3
print(len(x))
print(x[0])
print(x[1])
print(x[2])
print(x[5])
```

#### :list_repeat_str

```dr
x: list[str] = ["hello"] * 2
print(len(x))
print(x[0])
print(x[1])
```

#### :list_repeat_empty

```dr
x: list[int] = [1, 2] * 0
print(len(x))
```

#### :list_repeat_single_int

```dr
x: list[int] = [0] * 4
print(len(x))
print(x[0])
print(x[3])
```

#### :discarded_list_str_pop_emits_decref

```dr
x: list[str] = ["hello", "world"]
x.pop()
```

#### :discarded_list_str_pop_loop_bounded

```dr
x: list[str] = ["a", "b", "c", "d"]
for i in range(100000) {
  x.append("hello" + "_world")
  x.pop()
}
print(len(x))
```

#### :discarded_list_list_int_pop_emits_decref

```dr
x: list[list[int]] = [[1, 2], [3, 4]]
x.pop()
```

#### :discarded_list_list_int_pop_loop_bounded

```dr
x: list[list[int]] = [[1], [2], [3]]
for i in range(50000) {
  x.append([10, 20, 30])
  x.pop()
}
print(len(x))
```

#### :discarded_dict_str_str_pop_emits_decref

```dr
d: dict[str, str] = {"k": "v"}
d.pop("k")
```

#### :discarded_dict_str_str_pop_loop_bounded

```dr
d: dict[str, str] = {}
for i in range(50000) {
  d["k"] = "value_" + "x"
  d.pop("k")
}
print(len(d))
```

#### :discarded_list_int_pop_no_decref

```dr
x: list[int] = [1, 2, 3]
x.pop()
```

#### :discarded_list_int_pop_runs

```dr
x: list[int] = [10, 20, 30]
x.pop()
print(len(x))
```

#### :assigned_list_str_pop_no_double_free

```dr
x: list[str] = ["hello", "world"]
v: str = x.pop()
print(v)
print(len(x))
```

#### :assigned_list_str_pop_loop_no_double_free

```dr
x: list[str] = ["a", "b", "c", "d"]
last: str = ""
for i in range(50000) {
  x.append("hello")
  last = x.pop()
}
print(last)
print(len(x))
```

#### :ann_assign_consumes_pop_no_double_free

```dr
x: list[str] = ["hello", "world"]
y: str = x.pop()
z: str = y
print(z)
```

#### :dict_values_uniform_loop_bounded

```dr
d: dict[str, str] = {}
d["a"] = "x"
d["b"] = "y"
for _ in range(10000) { _v: list[str] = d.values() }
print("ok")
```

#### :list_extend_adopts_tag

```dr
src: list[str] = ["a", "b", "c"]
dest: list[str] = []
dest.extend(src)
print(dest[2])
```

#### :enumerate_zip_balanced_refcount

```dr
for i in range(1000) {
  src: list[str] = ["a", "b", "c"]
  o: list[str] = ["x", "y", "z"]
  p1: list = enumerate(src)
  p2: list = zip(src, o)
}
print("ok")
```

#### :dict_get_checked_type_error_message

```dr
d: dict = {age: "ten"}
try {
  x: int = d["age"]
  print("no error")
} except TypeError as e {
  print("caught")
}
```

#### :dict_get_checked_type_error_loop_bounded

```dr
d: dict = {age: "ten"}
errors: int = 0
for i in range(100000) {
  try {
    x: int = d["age"]
  } except TypeError {
    errors = errors + 1
  }
}
print(errors)
```

#### :dict_get_checked_alternating_errors

```dr
d: dict = {age: "ten", name: 42}
errors: int = 0
for i in range(50000) {
  try {
    a: int = d["age"]
  } except TypeError {
    errors = errors + 1
  }
  try {
    b: str = d["name"]
  } except TypeError {
    errors = errors + 1
  }
}
print(errors)
```

#### :set_str_contains_content_hashed

```dr
s: set[str] = {"hello", "world"}
if "hello" in s {
    print("a")
}
x: str = "hel" + "lo"
if x in s {
    print("b")
}
```

#### :set_str_add_dedup

```dr
s: set[str] = {"a", "b"}
y: str = "a"
z: str = "a" + ""
s.add(y)
s.add(z)
print(len(s))
```

#### :set_method_add_remove_discard

```dr
s: set[int] = {1, 2}
s.add(3)
print(len(s))
s.remove(1)
print(len(s))
s.discard(99)
print(len(s))
if 3 in s {
    print("y")
}
```

#### :set_union_intersection_difference

```dr
a: set[int] = {1, 2, 3}
b: set[int] = {2, 3, 4}
u: set[int] = a.union(b)
i: set[int] = a.intersection(b)
d: set[int] = a.difference(b)
sd: set[int] = a.symmetric_difference(b)
print(len(u))
print(len(i))
print(len(d))
print(len(sd))
```

#### :set_issubset_issuperset_isdisjoint

```dr
a: set[int] = {1, 2}
b: set[int] = {1, 2, 3}
c: set[int] = {4, 5}
print(a.issubset(b))
print(b.issubset(a))
print(b.issuperset(a))
print(a.isdisjoint(c))
print(a.isdisjoint(b))
```

#### :set_utf8_keys

```dr
s: set[str] = {"café", "tea", "日本"}
if "café" in s {
    print("a")
}
if "日本" in s {
    print("b")
}
if "missing" in s {
    print("x")
} else {
    print("c")
}
t: set[str] = {"tea", "coffee"}
u: set[str] = s.union(t)
print(len(u))
```

#### :set_clear_and_copy

```dr
a: set[int] = {1, 2, 3}
b: set[int] = a.copy()
a.clear()
print(len(a))
print(len(b))
```

#### :set_update

```dr
a: set[int] = {1, 2}
b: set[int] = {2, 3}
a.update(b)
print(len(a))
if 3 in a {
    print("y")
}
```

#### :print_and_len_on_chained_subscript

```dr
a: list[list[str]] = []
inner1: list[str] = ["hello", "world"]
a.append(inner1)
inner2: list[str] = ["foo"]
a.append(inner2)
print(a[0])
print(len(a[0]))
print(len(a[1]))
```

#### :method_call_on_subscripted_str

```dr
xs: list[str] = ["/usr/lib", "home"]
print(xs[0].startswith("/"))
parts: list[str] = xs[0].split("/")
print(len(parts))
```

#### :for_in_over_list_of_class_instances

```dr
class Box {
    def(name: str) {
        self.name = name
    }
}
xs: list[Box] = [Box("a"), Box("b"), Box("c")]
for f in xs {
    print(f.name)
}
```

#### :str_bool_predicates_print_true_false

```dr
s: str = "/foo"
print(s.startswith("/"))
print(s.endswith("x"))
print("abc".isalpha())
print("123".isalpha())
```

#### :dict_set_from_borrowed_local_str

```dr
def build() -> dict[str, str] {
    const params: dict[str, str] = {}
    const k: str = "name"
    const v: str = "dragon"
    params[k] = v
    return params
}
const d: dict[str, str] = build()
const filler: list[str] = ["a", "b", "c", "d"]
print(d["name"])
```

#### :list_set_from_borrowed_local_str

```dr
def fill(target: list[str]) -> None {
    const v: str = "x"
    target[0] = v
}
const xs: list[str] = ["a", "b"]
fill(xs)
const filler: list[str] = ["p", "q", "r", "s"]
print(xs[0])
print(xs[1])
```

#### :tuple_return_from_borrowed_dict_local

```dr
def make() -> tuple[bool, dict[str, str]] {
    const d: dict[str, str] = {}
    d["k"] = "v"
    return (true, d)
}
const r: tuple[bool, dict[str, str]] = make()
const filler: list[str] = ["a", "b", "c", "d"]
const got: dict[str, str] = r[1]
print(got["k"])
```

#### :list_eq_int_elementwise

```dr
a: list[int] = [1, 2, 3]
b: list[int] = [1, 2, 3]
c: list[int] = [1, 2, 4]
print(a == b)
print(a == c)
print(a != c)
```

#### :list_eq_str_elementwise

```dr
a: list[str] = ["foo", "bar"]
b: list[str] = ["foo", "bar"]
c: list[str] = ["foo", "baz"]
print(a == b)
print(a == c)
```

#### :list_eq_nested

```dr
a: list[list[int]] = [[1, 2], [3, 4]]
b: list[list[int]] = [[1, 2], [3, 4]]
c: list[list[int]] = [[1, 2], [3, 5]]
print(a == b)
print(a == c)
```

#### :list_eq_different_lengths

```dr
a: list[int] = [1, 2, 3]
b: list[int] = [1, 2]
print(a == b)
```

#### :dict_eq_str_keyed_order_independent

```dr
a: dict[str, int] = {"x": 1, "y": 2}
b: dict[str, int] = {"y": 2, "x": 1}
c: dict[str, int] = {"x": 1, "y": 3}
print(a == b)
print(a == c)
```

#### :dict_eq_int_keyed

```dr
a: dict[int, str] = {1: "a", 2: "b"}
b: dict[int, str] = {2: "b", 1: "a"}
c: dict[int, str] = {1: "a", 2: "c"}
print(a == b)
print(a == c)
```

#### :boxed_list_eq_via_any

```dr
a: int | float | str | bytes | list[int] | list[str] | list[float] | list[list[int]] | list[list[str]] | dict[str, int] | dict[str, str] | tuple[] = [1, 2, 3]
b: int | float | str | bytes | list[int] | list[str] | list[float] | list[list[int]] | list[list[str]] | dict[str, int] | dict[str, str] | tuple[] = [1, 2, 3]
c: int | float | str | bytes | list[int] | list[str] | list[float] | list[list[int]] | list[list[str]] | dict[str, int] | dict[str, str] | tuple[] = [1, 2, 4]
print(a == b)
print(a == c)
```

#### :boxed_dict_eq_via_any

```dr
a: int | dict[str, int] = {"k": 1}
b: int | dict[str, int] = {"k": 1}
c: int | dict[str, int] = {"k": 2}
print(a == b)
print(a == c)
```

#### :boxed_bytes_eq_via_any

```dr
a: int | bytes = b"hello"
b: int | bytes = b"hello"
c: int | bytes = b"world"
print(a == b)
print(a == c)
```

#### :const_assign_from_list_subscript

```dr
def first(xs: list[str]) -> str {
    const x: str = xs[0]
    return x
}
const result: str = first(["hello", "world"])
const filler: list[str] = ["a", "b", "c", "d"]
print(result)
```

#### :list_any_mixed_literal_indexed_prints_values

```dr
x: list[int | float | str | bytes | list[int] | list[str] | list[float] | list[list[int]] | list[list[str]] | dict[str, int] | dict[str, str] | tuple[]] = [10, "hi", 2.5]
print(x[0])
print(x[1])
print(x[2])
```

#### :list_any_homogeneous_int_literal_indexed

```dr
x: list[int | float | str | bytes | list[int] | list[str] | list[float] | list[list[int]] | list[list[str]] | dict[str, int] | dict[str, str] | tuple[]] = [1, 2, 3]
print(x[0])
print(x[2])
```

#### :list_any_element_is_castable_via_isinstance

```dr
x: list[int | str] = [10, "hi"]
v: int | str = x[0]
if isinstance(v, int) {
  n: int = v
  print(n + 5)
}
```

#### :list_any_iteration

```dr
x: list[int | float | str | bytes | list[int] | list[str] | list[float] | list[list[int]] | list[list[str]] | dict[str, int] | dict[str, str] | tuple[]] = [1, "two", 3.0]
for item in x {
  print(item)
}
```

#### :list_any_holding_str_list_prints_tag_aware

```dr
xs: list[int | float | str | bytes | list[int] | list[str] | list[float] | list[list[int]] | list[list[str]] | dict[str, int] | dict[str, str] | tuple[]] = [["a", "b"], [1, 2], [1.5, 2.5]]
print(xs)
```

#### :dict_str_any_holding_containers_prints_tag_aware

```dr
d: dict[str, int | float | str | bytes | list[int] | list[str] | list[float] | list[list[int]] | list[list[str]] | dict[str, int] | dict[str, str] | tuple[]] = {"a": ["x", "y"], "b": {"inner": "v"}}
print(d)
```

#### :dict_int_any_holding_list_prints_tag_aware

```dr
d: dict[int, int | float | str | bytes | list[int] | list[str] | list[float] | list[list[int]] | list[list[str]] | dict[str, int] | dict[str, str] | tuple[]] = {1: ["x", "y"]}
print(d)
```

#### :str_of_bytes_does_not_crash

```dr
a: bytes = bytes([1, 2, 3])
sa: str = str(a)
print(sa)
```

#### :str_of_bytes_is_injective

```dr
seen: dict[str, int] = {}
n: int = 0
i: int = 0
while i < 256 {
  seen[str(bytes([i]))] = i
  n = n + 1
  i = i + 1
}
j: int = 0
while j < 64 {
  k: int = 0
  while k < 4 {
    seen[str(bytes([j, k]))] = j
    n = n + 1
    k = k + 1
  }
  j = j + 1
}
print(n)
print(len(seen))
```

#### :str_of_bytes_survives_nul_and_high_bytes

```dr
b: bytes = bytes([2, 128, 0, 0, 10])
c: bytes = bytes([2, 128, 0, 0, 99])
print(str(b))
print(str(c))
print(str(b) == str(c))
```

#### :str_of_bytes_agrees_with_print_and_interpolation

```dr
t: bytes = bytes([9, 10, 13, 65])
print(t)
print(str(t))
print(f"{t}")
print(str([t]))
```

#### :dict_literal_closure_value_survives_scope

```dr
cap: str = "held"
cb: Callable[[], None] = lambda () -> None { print(len(cap)) }
d: dict[str, Callable[[], None]] = {"f": cb}
d["f"]()
cb()
```

#### :dict_comprehension_container_values_keep_their_tag

```dr
src: list[str] = ["a", "bb"]
d: dict[str, list[int]] = {k: [len(k), 1] for k in src}
print(len(d))
print(d["a"][0])
print(d["bb"][0])
```

#### :dict_comprehension_shared_value_outlives_source

```dr
src: list[str] = ["a", "bb"]
shared: list[int] = [7, 8]
d: dict[str, list[int]] = {k: shared for k in src}
print(len(d))
print(d["a"][0] + d["bb"][1])
print(shared[0])
```

#### :dict_dot_assign_str_outlives_source

```dr
class Point(TypedDict) {
  id: int
  name: str
}
def build(n: int) -> str { return "n-" + str(n) }
p: Point = Point({id: 0, name: "x"})
s: str = build(4)
p.name = s
print(p.name)
print(s)
```
