# CodeGenClassesTest cases

Source programs for `test/CodeGenClassesTest.cpp`. Each block is named by the heading
above it and pulled in with `code("<name>")`.

#### :method_reflection_globals_emitted

```dr
class Foo {
    x: int
    def(x: int) { self.x = x }
    def bar() -> int { return self.x + 1 }
    def baz() -> int { return self.x * 2 }
}
f: Foo = Foo(10)
```

#### :subclass_inherits_parent_field_layout

```dr
class Res { n: int
 def() { self.n = 7 } }
class Base { r: Res
 def() { self.r = Res() } }
class Sub(Base) { def() {} }
def assign_into(b: Base) { b.r = Res() }
s: Sub = Sub()
assign_into(s)
print("ok")
```

#### :default_constructor_synthesis_bare

```dr
class Bare {
    def greet() { print("hi") }
}
b: Bare = Bare()
b.greet()
```

#### :default_constructor_synthesis_subclass_delegates

```dr
class Base {
    tag: int
    def() { self.tag = 42 }
}
class Sub(Base) {
    def show() { print(self.tag) }
}
s: Sub = Sub()
s.show()
```

#### :list_covariance_fresh_literal

```dr
class Animal { tag: int
 def() { self.tag = 0 } }
class Dog(Animal) { def() { self.tag = 1 } }
class Cat(Animal) { def() { self.tag = 2 } }
pets: list[Animal] = [Dog(), Cat()]
print(len(pets))
for p in pets { print(p.tag) }
```

#### :hasattr_finds_method

```dr
class Counter {
    n: int
    def(n: int) { self.n = n }
    def bump() { self.n = self.n + 1 }
}
c: Counter = Counter(0)
print(hasattr(c, "n"))
print(hasattr(c, "bump"))
print(hasattr(c, "nope"))
```

#### :getattr_bound_method_invocation_module_scope

```dr
class Counter {
    n: int
    def(n: int) { self.n = n }
    def bump() { self.n = self.n + 1 }
}
c: Counter = Counter(5)
m: Callable[[], None] = getattr(c, "bump")
m()
m()
print(c.n)
```

#### :getattr_bound_method_invocation_function_scope

```dr
class Counter {
    n: int
    def(n: int) { self.n = n }
    def add(k: int) { self.n = self.n + k }
}
def go() {
    c: Counter = Counter(0)
    addm: Callable[[int], None] = getattr(c, "add")
    addm(5)
    addm(3)
    print(c.n)
}
go()
```

#### :getattr_method_from_inherited_parent

```dr
class Animal {
    name: str
    def(name: str) { self.name = name }
    def greet() { print(self.name) }
}
class Dog(Animal) {
    def(name: str) { self.name = name }
}
d: Dog = Dog("Rex")
m: Callable[[], None] = getattr(d, "greet")
m()
```

#### :dir_instance_sorted_with_dunder_init

```dr
class Foo {
    x: int
    def(x: int) { self.x = x }
    def bar() -> int { return self.x }
    def baz() -> int { return self.x * 2 }
}
f: Foo = Foo(5)
names: list[str] = dir(f)
for n in names { print(n) }
```

#### :dir_class_descriptor

```dr
class Foo {
    def(x: int) { self.x = x }
    def bar() -> int { return 1 }
    x: int
}
names: list[str] = dir(Foo)
for n in names { print(n) }
```

#### :dir_walks_parent_chain

```dr
class Animal {
    name: str
    def(name: str) { self.name = name }
    def speak() -> str { return "sound" }
}
class Dog(Animal) {
    def(name: str) { self.name = name }
    def fetch() -> str { return "got" }
}
d: Dog = Dog("Rex")
names: list[str] = dir(d)
for n in names { print(n) }
```

#### :method_find_walks_parent_chain

```dr
class Animal {
    name: str
    def(name: str) { self.name = name }
    def speak() -> str { return "generic sound" }
}
class Dog(Animal) {
    def(name: str) { self.name = name }
    def fetch() -> str { return "got it" }
}
d: Dog = Dog("Rex")
print(d.speak())
print(d.fetch())
```

#### :class_decl_struct_type

```dr
class Counter {
  def(n: int) {
    self.val = n
  }
}
```

#### :class_method_decl

```dr
class Adder {
  def(x: int) {
    self.x = x
  }
  def get() -> int {
    return self.x
  }
}
```

#### :class_constructor_call

```dr
class Box {
  def(v: int) {
    self.v = v
  }
}
b: Box = Box(10)
```

#### :class_field_access

```dr
class Pair {
  def(a: int, b: int) {
    self.a = a
    self.b = b
  }
  def first() -> int {
    return self.a
  }
}
```

#### :class_method_call

```dr
class Val {
  def(n: int) {
    self.n = n
  }
  def get() -> int {
    return self.n
  }
}
v: Val = Val(42)
print(v.get())
```

#### :class_module_verifies

```dr
class Point {
  def(x: int, y: int) {
    self.x = x
    self.y = y
  }
  def sum() -> int {
    return self.x + self.y
  }
}
p: Point = Point(3, 4)
print(p.sum())
```

#### :class_gc_header_in_struct

```dr
class Widget {
  def(v: int) {
    self.v = v
  }
}
```

#### :class_gc_header_init

```dr
class Obj {
  def(x: int) {
    self.x = x
  }
}
```

#### :class_instance_decref_at_scope_exit

```dr
class Foo {
  def(x: int) {
    self.x = x
  }
}
def make() {
  f: Foo = Foo(42)
  print(f.x)
}
make()
```

#### :fire_list_arg_atomic_incref

```dr
def process(data: list[int]) -> int {
  return 0
}
items: list[int] = [1, 2, 3]
t: Task[int] = fire process(items)
```

#### :fire_int_arg_no_atomic_incref

```dr
def compute(n: int) -> int {
  return n + 1
}
t: Task[int] = fire compute(42)
```

#### :fire_trampoline_decrefs_heap_args

```dr
def process(items: list[int]) -> int {
  return 1
}
data: list[int] = [1, 2, 3]
t: Task[int] = fire process(data)
```

#### :gc_phase5_functions_decl

```dr
x: list[int] = [1, 2, 3]
print(x[0])
```

#### :class_dealloc_and_traverse

```dr
class Node {
  def(val: int, child: Node) {
    self.val: int = val
    self.child: Node = child
  }
}
```

#### :class_clear_zeros_fields

```dr
class Container {
  def(name: str, items: list[int]) {
    self.name: str = name
    self.items: list[int] = items
  }
}
```

#### :acyclic_class_not_tracked

```dr
class Obj {
  def(x: int, name: str) {
    self.x = x
    self.name = name
  }
}
```

#### :cyclic_capable_class_tracked

```dr
class Node {
  kids: list[int]
  def() {
    self.kids = []
  }
}
```

#### :stack_alloc_non_escaping_instance

```dr
class P {
  def(x: int) { self.x = x }
}
t: int = 0
i: int = 0
while i < 10 {
  p: P = P(i)
  t = t + p.x
  i = i + 1
}
print(t)
```

#### :escaping_instance_stays_heap

```dr
class P {
  def(x: int) { self.x = x }
}
def make(v: int) -> P {
  p: P = P(v)
  return p
}
print(make(5).x)
```

#### :stack_instance_field_reads_correct

```dr
class Pt {
  x: int
  y: int
  def(x: int, y: int) { self.x = x
    self.y = y }
}
total: int = 0
i: int = 0
while i < 1000 {
  p: Pt = Pt(i, i + 1)
  total = total + p.x + p.y
  i = i + 1
}
print(total)
```

#### :escaping_instances_distinct_after_return

```dr
class P {
  def(x: int) { self.x = x }
}
def make(v: int) -> P {
  p: P = P(v)
  return p
}
a: P = make(10)
b: P = make(20)
c: P = make(30)
print(a.x + b.x + c.x)
```

#### :const_ir

```dr
const MAX: int = 42
print(MAX)
```

#### :static_field_ir

```dr
class Counter {
  static count: int = 0
  def() {
    pass
  }
}
print(Counter.count)
```

#### :static_method_ir

```dr
class MathUtil {
  def() {
    pass
  }
  static def add(a: int, b: int) -> int {
    return a + b
  }
}
x: int = MathUtil.add(3, 4)
print(x)
```

#### :multi_constructor_ir

```dr
class Point {
  def(x: int, y: int) {
    self.x = x
    self.y = y
  }
  def() {
    self.x = 0
    self.y = 0
  }
}
p1: Point = Point(3, 4)
p2: Point = Point()
```

#### :single_constructor_unchanged_ir

```dr
class Simple {
  def(x: int) {
    self.x = x
  }
}
s: Simple = Simple(42)
```

#### :super_call_ir

```dr
class Animal {
    def(x: int) {
        self.x = x
    }
    def speak() -> int {
        return self.x
    }
}
```

#### :mro_method_lookup_ir

```dr
class A {
    def(v: int) {
        self.v = v
    }
    def greet() -> int {
        return self.v
    }
}
class B(A) {
    def(v: int) {
        self.v = v
    }
}
b: B = B(5)
print(b.greet())
```

#### :py_class_inheritance_ir

```py
class Base:
    def __init__(self, x: int):
        self.x = x
    def get(self) -> int:
        return self.x
class Child(Base):
    def __init__(self, x: int):
        self.x = x
c: Child = Child(7)
print(c.get())
```

#### :first_class_class_static_dispatch_unchanged

```dr
class Point {
    def(x: int, y: int) {
        self.x = x
        self.y = y
    }
}
p: Point = Point(3, 4)
print(p.x)
print(p.y)
```

#### :first_class_class_descriptor_ir

```dr
class Foo {
    def(x: int) {
        self.x = x
    }
}
```

#### :first_class_class_type_param

```dr
class Animal {
    def(name: str) {
        self.name = name
    }
}
def make(cls: type, n: str) -> Animal {
    a: Animal = cls(n)
    return a
}
x: Animal = make(Animal, "hi")
print(x.name)
```

#### :first_class_class_return_type_annotation

```dr
class Animal {
    def(name: str) {
        self.name = name
    }
}
def pick() -> type {
    return Animal
}
cls: type = pick()
inst: Animal = cls("rex")
print(inst.name)
```

#### :first_class_class_list_iteration

```dr
class Animal {
    def(name: str) {
        self.name = name
    }
}
classes: list[type] = [Animal, Animal]
for c in classes {
    inst: Animal = c("hi")
    print(inst.name)
}
```

#### :first_class_class_dict_lookup

```dr
class Animal {
    def(name: str) {
        self.name = name
    }
}
registry: dict[str, type] = {"a": Animal}
cls: type = registry["a"]
inst: Animal = cls("rex")
print(inst.name)
```

#### :class_value_binding_rejected

```dr
class Router {
    def(name: str) {
        self.name = name
    }
}
App: type = Router
a: Router = App("hi")
print(a.name)
```

#### :first_class_class_unannotated_param_errors

```dr
class Animal {
    def(name: str) {
        self.name = name
    }
}
def make(cls, n: str) {
    return cls(n)
}
x: Animal = make(Animal, "hi")
```

#### :class_decorator_identity

```dr
def identity(cls: type) -> type {
    return cls
}
@identity
class Greeter {
    def(name: str) {
        self.name = name
    }
}
g: Greeter = Greeter("world")
print(g.name)
```

#### :class_decorator_registry

```dr
registry: list[type] = []
def reg(cls: type) -> type {
    registry.append(cls)
    return cls
}
@reg
class Foo {
    def(n: str) {
        self.n = n
    }
}
@reg
class Bar {
    def(n: str) {
        self.n = n
    }
}
print(len(registry))
for c in registry {
    inst: Foo = c("hi")
    print(inst.n)
}
```

#### :class_decorator_stacking

```dr
log: list[str] = []
def t1(cls: type) -> type {
    log.append("t1")
    return cls
}
def t2(cls: type) -> type {
    log.append("t2")
    return cls
}
@t1
@t2
class Foo {
    def(n: str) {
        self.n = n
    }
}
f: Foo = Foo("x")
for s in log {
    print(s)
}
```

#### :class_decorator_isinstance_works

```dr
def identity(cls: type) -> type { return cls }
@identity
class Foo {
    def(n: str) {
        self.n = n
    }
}
f: Foo = Foo("hi")
if isinstance(f, Foo) {
    print("yes")
} else {
    print("no")
}
```

#### :class_decorator_preserves_attribute

```dr
def identity(cls: type) -> type { return cls }
@identity
class Point {
    def(x: int, y: int) {
        self.x = x
        self.y = y
    }
}
p: Point = Point(3, 4)
print(p.x)
print(p.y)
```

#### :dataclass_construction

```dr
@dataclass
class Point {
    x: int
    y: int
}
p: Point = Point(3, 4)
print(p.x)
print(p.y)
```

#### :dataclass_equal_by_fields

```dr
@dataclass
class Point {
    x: int
    y: int
}
p1: Point = Point(3, 4)
p2: Point = Point(3, 4)
p3: Point = Point(5, 4)
if p1 == p2 { print("eq12") } else { print("ne12") }
if p1 == p3 { print("eq13") } else { print("ne13") }
```

#### :dataclass_repr_format

```dr
@dataclass
class Point {
    x: int
    y: int
}
p: Point = Point(3, 4)
print(repr(p))
```

#### :dataclass_default_values

```dr
@dataclass
class Config {
    name: str
    timeout: int = 30
}
c1: Config = Config("server", 60)
c2: Config = Config("client")
print(c1.timeout)
print(c2.timeout)
```

#### :dataclass_user_init_overrides

```dr
@dataclass
class Box {
    x: int
    def(v: int) {
        self.x = v * 2
    }
}
b: Box = Box(7)
print(b.x)
```

#### :dataclass_mixed_field_types

```dr
@dataclass
class Person {
    name: str
    age: int
    active: bool
}
p1: Person = Person("Ada", 36, True)
p2: Person = Person("Ada", 36, True)
p3: Person = Person("Bob", 36, True)
if p1 == p2 { print("eq12") }
if p1 == p3 { print("BAD") } else { print("ne13") }
print(repr(p1))
```

#### :named_tuple_construction

```dr
class Vec(NamedTuple) {
    x: int
    y: int
}
v: Vec = Vec(1, 2)
print(v.x)
print(v.y)
print(repr(v))
```

#### :dataclass_with_runtime_decorator

```dr
registry: list[type] = []
def reg(cls: type) -> type {
    registry.append(cls)
    return cls
}
@reg
@dataclass
class Item {
    name: str
    qty: int
}
x: Item = Item("apple", 3)
print(x.qty)
print(len(registry))
```

#### :vtable_global_in_ir

```dr
class Dog {
    def(name: str) {
        self.name = name
    }
    def speak() -> str {
        return "Woof"
    }
}
```

#### :vtable_struct_layout

```dr
class Point {
    def(x: int, y: int) {
        self.x = x
        self.y = y
    }
    def sum() -> int {
        return self.x + self.y
    }
}
```

#### :vtable_dynamic_method_dispatch

```dr
class Dog {
    def(name: str) {
        self.name = name
    }
    def speak() -> str {
        return "Woof"
    }
}
obj: Dog = Dog("Rex")
print(obj.speak())
```

#### :vtable_inheritance_dispatch

```dr
class Animal {
    def(name: str) {
        self.name = name
    }
    def speak() -> str {
        return "..."
    }
}
class Dog(Animal) {
    def(name: str) {
        self.name = name
    }
    def speak() -> str {
        return "Woof"
    }
}
obj: Dog = Dog("Rex")
print(obj.speak())
```

#### :vtable_inherited_method

```dr
class Animal {
    def(name: str) {
        self.name = name
    }
    def speak() -> str {
        return "..."
    }
}
class Dog(Animal) {
    def(name: str) {
        self.name = name
    }
}
obj: Dog = Dog("Buddy")
print(obj.speak())
```

#### :vtable_static_dispatch_unchanged

```dr
class Cat {
    def(name: str) {
        self.name = name
    }
    def speak() -> str {
        return "Meow"
    }
}
obj: Cat = Cat("Whiskers")
print(obj.speak())
```

#### :vtable_multiple_methods

```dr
class Calc {
    def(v: int) {
        self.v = v
    }
    def add(x: int) -> int {
        return self.v + x
    }
    def mul(x: int) -> int {
        return self.v * x
    }
}
obj: Calc = Calc(10)
print(obj.add(5))
print(obj.mul(3))
```

#### :vtable_void_method

```dr
class Printer {
    def(msg: str) {
        self.msg = msg
    }
    def show() {
        print(self.msg)
    }
}
obj: Printer = Printer("hello")
obj.show()
```

#### :bug_repro_str_field_cmp

```dr
class Greeter {
  def(name: str) {
    self.name = name
  }
  def greet() -> str {
    if self.name == "world" {
      return "Hello, World!"
    }
    return "Hello!"
  }
}
g: Greeter = Greeter("world")
print(g.greet())
```

#### :float_field_inference

```dr
class Pt {
  def(x: float, y: float) {
    self.x = x
    self.y = y
  }
  def sum() -> float {
    return self.x + self.y
  }
}
```

#### :bool_field_inference

```dr
class Flag {
  def(active: bool) {
    self.active = active
  }
  def is_on() -> bool {
    return self.active
  }
}
f: Flag = Flag(true)
print(f.is_on())
```

#### :str_field_literal_init

```dr
class Config {
  def() {
    self.mode = "default"
  }
  def get_mode() -> str {
    if self.mode == "default" {
      return "ok"
    }
    return "custom"
  }
}
```

#### :class_basic

```dr
class Point {
  def(x: int, y: int) {
    self.x = x
    self.y = y
  }
  def sum() -> int {
    return self.x + self.y
  }
}
p: Point = Point(3, 4)
print(p.sum())
```

#### :class_gc_field_access

```dr
class Point {
  def(x: int, y: int) {
    self.x = x
    self.y = y
  }
}
p: Point = Point(3, 4)
print(p.x)
print(p.y)
```

#### :class_gc_method_access

```dr
class Counter {
  def(v: int) {
    self.v = v
  }
  def get() -> int {
    return self.v
  }
}
c: Counter = Counter(99)
print(c.get())
```

#### :class_gc_multiple_fields

```dr
class Rect {
  def(w: int, h: int) {
    self.w = w
    self.h = h
  }
  def area() -> int {
    return self.w * self.h
  }
}
r: Rect = Rect(5, 10)
print(r.area())
```

#### :class_gc_field_mutation

```dr
class Box {
  def(v: int) {
    self.v = v
  }
  def set(n: int) {
    self.v = n
  }
  def get() -> int {
    return self.v
  }
}
b: Box = Box(1)
print(b.get())
b.set(42)
print(b.get())
```

#### :class_gc_return_instance

```dr
class Val {
  def(n: int) {
    self.n = n
  }
}
def make(x: int) -> Val {
  v: Val = Val(x)
  return v
}
r: Val = make(7)
```

#### :cycle_collector_frees_objects

```dr
class Node {
  def(val: int) {
    self.val: int = val
    self.next: Node | None = None
  }
}
def make_cycle() {
  a: Node = Node(1)
  b: Node = Node(2)
  a.next = b
  b.next = a
}
make_cycle()
print("ok")
```

#### :cycle_collector_reentrancy_guard

```dr
class Node {
  def(val: int) {
    self.val: int = val
    self.next: Node | None = None
    self.tag: str = "node"
  }
}
def make_cycle(i: int) {
  a: Node = Node(i)
  b: Node = Node(i + 1)
  a.next = b
  b.next = a
}
for i in range(2000) {
  make_cycle(i)
}
print("ok")
```

#### :class_instance_create_destroy_e2_e

```dr
class Box {
  def(v: int) {
    self.v = v
  }
}
def test() {
  b: Box = Box(42)
  print(b.v)
}
test()
```

#### :multi_constructor_dispatch_e2_e

```dr
class Point {
  def(x: int, y: int) {
    self.x = x
    self.y = y
  }
  def() {
    self.x = 0
    self.y = 0
  }
}
p1: Point = Point(10, 20)
print(p1.x)
print(p1.y)
p2: Point = Point()
print(p2.x)
print(p2.y)
```

#### :three_constructors_e2_e

```dr
class Vec {
  def() {
    self.x = 0
    self.y = 0
    self.z = 0
  }
  def(x: int) {
    self.x = x
    self.y = x
    self.z = x
  }
  def(x: int, y: int, z: int) {
    self.x = x
    self.y = y
    self.z = z
  }
}
v1: Vec = Vec()
print(v1.x)
v2: Vec = Vec(5)
print(v2.y)
v3: Vec = Vec(1, 2, 3)
print(v3.z)
```

#### :multi_constructor_with_methods_e2_e

```dr
class Rect {
  def(w: int, h: int) {
    self.w = w
    self.h = h
  }
  def() {
    self.w = 1
    self.h = 1
  }
  def area() -> int {
    return self.w * self.h
  }
}
r1: Rect = Rect(3, 4)
print(r1.area())
r2: Rect = Rect()
print(r2.area())
```

#### :self_ctor_single_e2_e

```dr
class Point {
  def(x: int, y: int) {
    self.x = x
    self.y = y
  }
  def sum() -> int {
    return self.x + self.y
  }
}
p: Point = Point(10, 32)
print(p.sum())
```

#### :def_init_backcompat_e2_e

```dr
class Box {
  def __init__(val: int) {
    self.val = val
  }
  def get() -> int {
    return self.val
  }
}
b: Box = Box(99)
print(b.get())
```

#### :static_field_e2_e

```dr
class Counter {
  static count: int = 0
  def() {
    Counter.count = Counter.count + 1
  }
}
c1: Counter = Counter()
c2: Counter = Counter()
print(Counter.count)
```

#### :staticmethod_dr

```dr
class Math {
  @staticmethod
  def double(x: int) -> int {
    return x * 2
  }
}
print(Math.double(21))
```

#### :staticmethod_py

```py
class Math:
    @staticmethod
    def double(x: int) -> int:
        return x * 2
print(Math.double(21))
```

#### :classmethod_py

```py
class Counter:
    def __init__(self, n: int):
        self.n = n
    def get(self) -> int:
        return self.n
    @classmethod
    def zero(cls) -> Counter:
        return Counter(0)
c: Counter = Counter.zero()
print(c.get())
```

#### :static_and_instance_method_coexist

```dr
class Calc {
  def(base: int) {
    self.base = base
  }
  def add(n: int) -> int {
    return self.base + n
  }
  @staticmethod
  def pi() -> int {
    return 3
  }
}
c: Calc = Calc(10)
print(c.add(5))
print(Calc.pi())
```

#### :super_method_call

```dr
class Base {
    def(x: int) {
        self.x = x
    }
    def get_val() -> int {
        return self.x
    }
}
class Child(Base) {
    def(x: int) {
        self.x = x
    }
    def doubled() -> int {
        return super.get_val() * 2
    }
}
c: Child = Child(21)
print(c.doubled())
```

#### :mro_inherited_method

```dr
class Animal {
    def(x: int) {
        self.x = x
    }
    def get_x() -> int {
        return self.x
    }
}
class Dog(Animal) {
    def(x: int) {
        self.x = x
    }
}
d: Dog = Dog(42)
print(d.get_x())
```

#### :py_mro_inherited_method_e2_e

```py
class Animal:
    def __init__(self, x: int):
        self.x = x
    def get_x(self) -> int:
        return self.x
class Dog(Animal):
    def __init__(self, x: int):
        self.x = x
d: Dog = Dog(42)
print(d.get_x())
```

#### :py_super_method_e2_e

```py
class Base:
    def __init__(self, x: int):
        self.x = x
    def get_val(self) -> int:
        return self.x
class Child(Base):
    def __init__(self, x: int):
        self.x = x
    def doubled(self) -> int:
        return super().get_val() * 2
c: Child = Child(21)
print(c.doubled())
```

#### :str_field_cmp_e2_e

```dr
class Greeter {
  def(name: str) {
    self.name = name
  }
  def greet() -> str {
    if self.name == "world" {
      return "Hello, World!"
    }
    return "Hello!"
  }
}
g: Greeter = Greeter("world")
print(g.greet())
```

#### :str_field_cmp_not_equal

```dr
class Tag {
  def(label: str) {
    self.label = label
  }
  def check() -> str {
    if self.label != "admin" {
      return "not admin"
    }
    return "admin"
  }
}
t: Tag = Tag("user")
print(t.check())
```

#### :union_param_isinstance_narrowing

```dr
def show(x: int | str) {
    if isinstance(x, int) {
        print(x + 1)
    } else {
        print(x)
    }
}
show(41)
show("hello")
```

#### :union_param_print_dispatch

```dr
def show(x: int | str) {
    print(x)
}
show(42)
show("world")
```

#### :union_param_int_float

```dr
def show(x: int | float) {
    if isinstance(x, int) {
        print(x + 1)
    } else {
        print(x)
    }
}
show(10)
show(3.14)
```

#### :union_param_multiple

```dr
def add_or_cat(a: int | str, b: int | str) {
    if isinstance(a, int) {
        if isinstance(b, int) {
            print(a + b)
        }
    }
}
add_or_cat(10, 20)
```

#### :union_type_builtin

```dr
def show_type(x: int | str) {
    print(type(x))
}
show_type(42)
show_type("hi")
```

#### :union_three_types

```dr
def show(x: int | str | float) {
    if isinstance(x, int) {
        print("int")
    } elif isinstance(x, str) {
        print("str")
    } elif isinstance(x, float) {
        print("float")
    }
}
show(1)
show("a")
show(2.5)
```

#### :union_narrowed_arithmetic

```dr
def double_it(x: int | str) -> int {
    if isinstance(x, int) {
        return x * 2
    }
    return 0
}
print(double_it(21))
```

#### :union_pass_through

```dr
def inner(x: int | str) {
    print(x)
}
def outer(x: int | str) {
    inner(x)
}
outer(99)
outer("pass")
```

#### :union_local_variable

```dr
def test() {
    x: int | str = 42
    print(x)
    x = "hello"
    print(x)
}
test()
```

#### :union_param_bool

```dr
def show(x: int | bool) {
    if isinstance(x, bool) {
        print(x)
    } else {
        print(x + 1)
    }
}
show(True)
show(10)
```

#### :print_class_descriptor

```dr
class Foo {
    def() {}
}
print(Foo)
```

#### :property_getter_bare_access

```dr
class Box {
  def(v: int) {
    self._v = v
  }
  @property
  def value() -> int {
    return self._v + 1
  }
}
b: Box = Box(41)
print(b.value)
```

#### :property_setter_round_trip

```dr
class Box {
  def(v: int) {
    self._v = v
  }
  @property
  def value() -> int {
    return self._v
  }
  @value.setter
  def value(x: int) {
    self._v = x * 2
  }
}
b: Box = Box(10)
print(b.value)
b.value = 5
print(b.value)
```

#### :property_computed_derived

```dr
class Rect {
  def(w: int, h: int) {
    self.w = w
    self.h = h
  }
  @property
  def area() -> int {
    return self.w * self.h
  }
}
r: Rect = Rect(3, 4)
print(r.area)
```

#### :property_returns_string

```dr
class Greeter {
  def(n: str) {
    self.n = n
  }
  @property
  def greeting() -> str {
    return "hi " + self.n
  }
}
g: Greeter = Greeter("world")
print(g.greeting)
```

#### :property_inherited_from_base

```dr
class Base {
  def(v: int) {
    self._v = v
  }
  @property
  def value() -> int {
    return self._v
  }
}
class Derived(Base) {
  def(v: int) {
    self._v = v + 1
  }
}
d: Derived = Derived(6)
print(d.value)
```

#### :property_assigned_to_typed_local

```dr
class Box {
  def(v: int) {
    self._v = v
  }
  @property
  def value() -> int {
    return self._v
  }
}
def use() -> int {
  b: Box = Box(11)
  x: int = b.value
  return x + 1
}
print(use())
```

#### :property_getter_called_once

```dr
class Counter {
  def() {
    self.n = 0
  }
  @property
  def tick() -> int {
    self.n = self.n + 1
    return self.n
  }
}
c: Counter = Counter()
print(c.tick)
print(c.tick)
print(c.tick)
```

#### :property_emits_method_call_not_field_load

```dr
class Box {
  def(v: int) {
    self._v = v
  }
  @property
  def value() -> int {
    return self._v
  }
}
b: Box = Box(1)
x: int = b.value
```

#### :enum_auto_numbered

```dr
enum Color {
    RED,
    GREEN,
    BLUE
}
print(Color.RED)
print(Color.GREEN)
print(Color.BLUE)
```

#### :enum_explicit_values

```dr
enum Status {
    OK = 200,
    NOT_FOUND = 404,
    SERVER_ERROR = 500
}
print(Status.OK)
print(Status.NOT_FOUND)
print(Status.SERVER_ERROR)
```

#### :enum_mixed_auto_and_explicit

```dr
enum Mixed {
    A,
    B = 10,
    C,
    D
}
print(Mixed.A)
print(Mixed.B)
print(Mixed.C)
print(Mixed.D)
```

#### :enum_with_match_statement

```dr
enum Color {
    RED,
    GREEN,
    BLUE
}
def name(c: int) -> str {
    match c {
        case Color.RED { return "red" }
        case Color.GREEN { return "green" }
        case Color.BLUE { return "blue" }
        case _ { return "unknown" }
    }
    return "unreachable"
}
print(name(Color.RED))
print(name(Color.GREEN))
print(name(Color.BLUE))
print(name(99))
```

#### :enum_negative_value

```dr
enum Sign {
    NEG = -1,
    ZERO,
    POS
}
print(Sign.NEG)
print(Sign.ZERO)
print(Sign.POS)
```

#### :enum_used_in_if_elif

```dr
enum Level {
    DEBUG,
    INFO,
    WARNING,
    ERROR
}
def label(l: int) -> str {
    if l == Level.DEBUG {
        return "D"
    } elif l == Level.INFO {
        return "I"
    } elif l == Level.WARNING {
        return "W"
    }
    return "E"
}
print(label(Level.DEBUG))
print(label(Level.INFO))
print(label(Level.WARNING))
print(label(Level.ERROR))
```

#### :enum_trailing_comma_optional

```dr
enum E { A, B, C, }
print(E.A)
print(E.B)
print(E.C)
```

#### :property_setter_mangled_in_vtable

```dr
class Box {
  def(v: int) {
    self._v = v
  }
  @property
  def value() -> int {
    return self._v
  }
  @value.setter
  def value(x: int) {
    self._v = x
  }
}
b: Box = Box(1)
b.value = 5
```

#### :for_in_class_field_dict_with_explicit_field_annotation

```dr
class Foo {
    data: dict[str, str]
    def() { self.data = {"a": "1", "b": "2"} }
    def show() -> None {
        for k in self.data {
            print(k)
            print(self.data[k])
        }
    }
}
const f: Foo = Foo()
f.show()
```

#### :for_in_class_field_dict_inferred_from_dict_literal

```dr
class Foo {
    def() { self.data = {"a": "1", "b": "2"} }
    def show() -> None {
        for k in self.data {
            print(k)
            print(self.data[k])
        }
    }
}
const f: Foo = Foo()
f.show()
```

#### :for_in_other_object_dict_field

```dr
class Foo {
    data: dict[str, str]
    def() { self.data = {"x": "y", "p": "q"} }
}
def use(f: Foo) -> None {
    for k in f.data {
        print(k)
        print(f.data[k])
    }
}
const f: Foo = Foo()
use(f)
```

#### :for_in_class_field_string

```dr
class Foo {
    body: str
    def() { self.body = "hi" }
    def show() -> None { for c in self.body { print(c) } }
}
const f: Foo = Foo()
f.show()
```

#### :for_in_class_field_dict_keys_method_on_attribute

```dr
class Foo {
    data: dict[str, str]
    def() { self.data = {"a": "1", "b": "2"} }
    def show() -> None {
        for k in self.data.keys() { print(k) }
    }
}
const f: Foo = Foo()
f.show()
```

#### :for_in_class_field_dict_heterogeneous_stays_polymorphic

```dr
class Foo {
    def() { self.data = {"a": "x", "b": 1} }
    def show() -> None {
        for k in self.data { print(k) }
    }
}
const f: Foo = Foo()
f.show()
```

#### :capturing_closure_stored_on_class_field_invoked_through_field

```dr
class Holder {
    def(handler: Callable[[int], int]) {
        self.handler = handler
    }
}
def make(x: int) -> Holder {
    def inner(y: int) -> int {
        return x + y
    }
    return Holder(inner)
}
const h: Holder = make(10)
const r: int = h.handler(5)
print(r)
```

#### :bare_fn_pointer_on_class_field_invoked_through_field

```dr
class Holder {
    def(handler: Callable[[int], int]) {
        self.handler = handler
    }
}
def add5(y: int) -> int {
    return y + 5
}
const h: Holder = Holder(add5)
const r: int = h.handler(10)
print(r)
```

#### :own_ptr_field_default_registered_allocator_releases

An `own` field default whose initializer is a registered allocator call registers
the matching releaser exactly as a constructor assignment does, so the class
deallocator destroys the resource.

```dr
extern "C" def dragon_lock_new() -> ptr

class Guard {
    own h: ptr = dragon_lock_new()
}

g: Guard = Guard()
```

#### :own_field_default_unregistered_callee_rejected

An `own` field default from a callee the releaser registry does not know must
still be rejected: the compiler cannot generate the release, and accepting it
would leak the resource silently.

```dr
extern "C" def dragon_lock_new() -> ptr

def make_handle() -> ptr {
    return dragon_lock_new()
}

class Holder {
    own h: ptr = make_handle()
}

h: Holder = Holder()
```

#### :own_release_inherited_by_subclass_dealloc

A subclass instance runs only its own dealloc, so the child dealloc must carry
the parent's own raw-resource releasers: one destroy call in the base dealloc
and one in the child dealloc.

```dr
extern "C" def dragon_lock_new() -> ptr

class Base {
    own h: ptr = dragon_lock_new()
}

class Child(Base) {
    extra: int = 0
}

c: Child = Child()
```

#### :own_release_transitive_through_grandparent

The stored releaser list is transitive through the parent chain: a grandchild
whose grandparent owns the resource still destroys it, giving one destroy call
per class dealloc.

```dr
extern "C" def dragon_lock_new() -> ptr

class Base {
    own h: ptr = dragon_lock_new()
}

class Mid(Base) {
    a: int = 0
}

class Leaf(Mid) {
    b: int = 0
}

l: Leaf = Leaf()
```
