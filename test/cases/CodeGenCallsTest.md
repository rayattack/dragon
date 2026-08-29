# CodeGenCallsTest cases

Source programs for `test/CodeGenCallsTest.cpp`. Each block is named by the heading
above it and pulled in with `code("<name>")`.

#### :print_list_str_dispatch

```dr
x: list[str] = ["a", "b"]
print(x)
```

#### :print_list_float_dispatch

```dr
x: list[float] = [1.0, 2.0]
print(x)
```

#### :extern_c_from_lib_ir

```dr
extern "C" from "mylib" {
  def foo(x: int) -> int
  def bar(s: str) -> ptr
}
```

#### :extern_c_ptr_type_ir

```dr
extern "C" def malloc(size: int) -> ptr
extern "C" def free(p: ptr)
```

#### :expr_stmt_discarding_ptr_return_no_decref_str

```dr
extern "C" def malloc(size: int) -> ptr
extern "C" def memset(s: ptr, c: intc, n: int) -> ptr
extern "C" def free(p: ptr)
const buf: ptr = malloc(64)
memset(buf, 0, 64)
free(buf)
```

#### :isinstance_int

```dr
x: int = 42
print(isinstance(x, int))
```

#### :isinstance_str

```dr
x: str = "hello"
print(isinstance(x, str))
print(isinstance(x, int))
```

#### :isinstance_bool

```dr
x: bool = True
print(isinstance(x, bool))
```

#### :isinstance_list

```dr
x: list[int] = [1, 2, 3]
print(isinstance(x, list))
print(isinstance(x, dict))
```

#### :type_int

```dr
x: int = 42
print(type(x))
```

#### :type_str

```dr
x: str = "hello"
print(type(x))
```

#### :type_float

```dr
x: float = 3.14
print(type(x))
```

#### :type_bool

```dr
x: bool = True
print(type(x))
```

#### :print_list_str

```dr
x: list[str] = ["hello", "world"]
print(x)
```

#### :print_list_float

```dr
x: list[float] = [1.5, 2.0, 3.14]
print(x)
```

#### :print_list_bool

```dr
x: list[bool] = [True, False, True]
print(x)
```

#### :print_list

```dr
x: list[int] = [1, 2, 3]
print(x)
```

#### :min_max_list

```dr
xs: list[int] = [5, 2, 8, 1, 9]
print(min(xs))
print(max(xs))
```

#### :sum_list

```dr
xs: list[int] = [1, 2, 3, 4, 5]
print(sum(xs))
```

#### :any_all_list

```dr
xs: list[int] = [0, 0, 1]
ys: list[int] = [1, 2, 3]
zs: list[int] = [0, 0, 0]
print(any(xs))
print(all(ys))
print(any(zs))
print(all(xs))
```

#### :sorted_reversed

```dr
xs: list[int] = [3, 1, 4, 1, 5]
ys: list[int] = sorted(xs)
zs: list[int] = reversed(xs)
print(ys[0])
print(ys[4])
print(zs[0])
print(zs[4])
```

#### :extern_c_call_puts

```dr
extern "C" def puts(s: str) -> int
puts("hello from C")
```

#### :extern_c_call_abs

```dr
extern "C" def abs(x: int) -> int
x: int = abs(-42)
print(x)
```

#### :hasattr_ir

```dr
class Foo {
  def(x: int) {
    self.x = x
  }
}
f: Foo = Foo(1)
b: bool = hasattr(f, "x")
```

#### :getattr_ir

```dr
class Foo {
  def(x: int) {
    self.x = x
  }
}
f: Foo = Foo(1)
v: int = getattr(f, "x")
```

#### :hasattr_true

```dr
class Point {
  def(x: int, y: int) {
    self.x = x
    self.y = y
  }
}
p: Point = Point(1, 2)
if hasattr(p, "x") {
  print("yes")
}
```

#### :hasattr_false

```dr
class Point {
  def(x: int, y: int) {
    self.x = x
    self.y = y
  }
}
p: Point = Point(1, 2)
if hasattr(p, "z") {
  print("yes")
} else {
  print("no")
}
```

#### :getattr_field

```dr
class Dog {
  def(name: str) {
    self.name = name
  }
}
d: Dog = Dog("Rex")
n: str = getattr(d, "name")
print(n)
```

#### :getattr_int

```dr
class Box {
  def(val: int) {
    self.val = val
  }
}
b: Box = Box(42)
v: int = getattr(b, "val")
print(v)
```

#### :getattr_default

```dr
class Cfg {
  def(port: int) {
    self.port = port
  }
}
c: Cfg = Cfg(8080)
v: int = getattr(c, "missing", 9999)
print(v)
```

#### :hasattr_inherited

```dr
class Base {
  def(x: int) {
    self.x = x
  }
}
class Child(Base) {
  def(x: int, y: int) {
    self.x = x
    self.y = y
  }
}
c: Child = Child(1, 2)
if hasattr(c, "x") {
  print("has x")
}
if hasattr(c, "y") {
  print("has y")
}
```

#### :any_kwarg_to_var_kwargs_function

```dr
def kw(**keys: int | str) -> int {
  n: int = 0
  for k in keys { n = n + 1 }
  return n
}
d: dict[str, int | float | str | bytes | list[int] | list[str] | list[float] | list[list[int]] | list[list[str]] | dict[str, int] | dict[str, str] | tuple[]] = {}
d["id"] = 5
d["name"] = "ada"
print(kw(a=d["id"], b=d["name"]))
```

#### :any_kwarg_keeps_runtime_type_and_value

```dr
def kw(**keys: int | str) -> str {
  return str(keys["a"]) + "|" + str(keys["b"])
}
d: dict[str, int | float | str | bytes | list[int] | list[str] | list[float] | list[list[int]] | list[list[str]] | dict[str, int] | dict[str, str] | tuple[]] = {}
d["id"] = 5
d["name"] = "ada"
print(kw(a=d["id"], b=d["name"]))
```

#### :any_kwarg_to_var_kwargs_method

```dr
class Bag {
  data: dict[str, int | str]
  def(d: dict[str, int | str]) { self.data = d }
  def get(k: str) -> int | str { return self.data[k] }
  def kw(**keys: int | str) -> int {
    n: int = 0
    for k in keys { n = n + 1 }
    return n
  }
}
d: dict[str, int | float | str | bytes | list[int] | list[str] | list[float] | list[list[int]] | list[list[str]] | dict[str, int] | dict[str, str] | tuple[]] = {}
d["id"] = 5
b: Bag = Bag(d)
print(b.kw(a=d["id"], c=b.get("id")))
```

#### :any_kwarg_to_typed_dict_constructor

```dr
class Point(TypedDict) {
  id: int
  name: str
}
d: dict[str, int | float | str | bytes | list[int] | list[str] | list[float] | list[list[int]] | list[list[str]] | dict[str, int] | dict[str, str] | tuple[]] = {}
d["id"] = 7
p: Point = Point(id=d["id"], name="a")
print(p.id)
```

#### :container_kwarg_keeps_its_own_tag

```dr
def kw(**keys: int | str) -> int {
  n: int = 0
  for k in keys { n = n + 1 }
  return n
}
l: list[int] = [1, 2, 3]
d: dict[str, int] = {}
d["k"] = 1
b: bytes = bytes([1, 2])
print(kw(a=l, b=d, c=b))
```

#### :container_kwarg_values_read_back_in_callee

```dr
def kw(**keys: str | list[int]) -> str {
  xs: list[int] = keys["a"]
  name: str = keys["b"]
  return str(xs[0] + xs[1] + xs[2]) + "|" + name
}
l: list[int] = [1, 2, 3]
s: str = "ada"
print(kw(a=l, b=s))
print(l[0])
print(s)
```

#### :instance_kwarg_to_var_kwargs_method

```dr
class Node {
  v: int
  def(v: int) { self.v = v }
}
class Holder {
  def kwm(**keys: int | str) -> int {
    n: int = 0
    for k in keys { n = n + 1 }
    return n
  }
}
h: Holder = Holder()
nd: Node = Node(3)
l: list[int] = [1]
print(h.kwm(a=nd, b=l))
```

#### :typed_dict_kwarg_str_outlives_construction

```dr
class Point(TypedDict) {
  id: int
  name: str
}
def build(n: int) -> str { return "name-" + str(n) }
i: int = 0
last: str = ""
while i < 3 {
  s: str = build(i)
  p: Point = Point(id=i, name=s)
  last = p.name
  i = i + 1
}
print(last)
```

#### :keyword_move_transfers_ownership

```dr
def take(own s: str) -> int { return len(s) }
def mk(n: int) -> str { return "m-" + str(n) }
i: int = 0
t: int = 0
while i < 3 {
  b: str = mk(i)
  t = t + take(s=own b)
  t = t + take(s=mk(i))
  i = i + 1
}
print(t)
```

#### :keyword_move_on_method_and_var_args

```dr
class Sink {
  def take(own s: str) -> int { return len(s) }
}
def va(own s: str, *rest: int) -> int { return len(s) }
def mk(n: int) -> str { return "m-" + str(n) }
k: Sink = Sink()
a: str = mk(1)
b: str = mk(2)
print(k.take(s=own a))
print(va(s=own b))
```

#### :keyword_dub_leaves_source_usable

```dr
def borrows(s: str) -> int { return len(s) }
def mk(n: int) -> str { return "m-" + str(n) }
d: str = mk(7)
print(borrows(s=dub d))
print(d)
```

#### :dub_into_own_param_leaves_source_intact

```dr
def archive(own name: str) -> int { return len(name) }
def count(own xs: list[int]) -> int { return len(xs) }
def tally(own d: dict[str, str]) -> int { return len(d) }
label: str = "report-" + "2026"
xs: list[int] = [1, 2, 3]
d: dict[str, str] = {"a": "1", "b": "2"}
print(archive(dub label))
print(label)
print(count(dub xs))
print(len(xs))
print(tally(dub d))
print(len(d))
```

#### :dub_into_own_param_by_keyword_leaves_source_intact

```dr
class Sink {
  def archive(own name: str) -> int { return len(name) }
}
k: Sink = Sink()
label: str = "report-" + "2026"
print(k.archive(name=dub label))
print(label)
```

#### :str_of_str_stays_zero_copy_ir

`str(s)` and `f"{s}"` on a value that is already a `str` must not allocate: they
hand back the same pointer with one retain. The shared render helper routes
every stringify site, so this pins that it did not turn the identity case into a
copy.

```dr
s: str = "hello"
a: str = str(s)
b: str = f"{s}"
print(a + b)
```
