# TypeCheckerTest cases

Source programs for `test/TypeCheckerTest.cpp`. Each block is named by the heading
above it and pulled in with `code("<name>")`.

#### :no_ctor_class_rejects_args

```dr
class A {
    x: int
}
a: A = A(5)
```

#### :no_ctor_class_zero_args_ok

```dr
class A {
    x: int
}
a: A = A()
```

#### :no_ctor_subclass_rejects_parent_args

```dr
class B {
    x: int
    def (x: int) {
        self.x = x
    }
}
class C(B) {
    def describe() -> str {
        return "c"
    }
}
c: C = C(5)
```

#### :no_ctor_subclass_zero_args_ok

```dr
class B {
    x: int
    def (x: int) {
        self.x = x
    }
}
class C(B) {
    def describe() -> str {
        return "c"
    }
}
c: C = C()
```

#### :function_value_subscript_rejected

```dr
def args() -> list[str] {
    return ["a"]
}
x: str = args[1]
```

#### :function_value_len_rejected

```dr
def args() -> list[str] {
    return ["a"]
}
n: int = len(args)
```

#### :function_value_iteration_rejected

```dr
def args() -> list[str] {
    return ["a"]
}
for a in args {
    print(a)
}
```

#### :called_function_result_stays_usable

```dr
def args() -> list[str] {
    return ["a"]
}
x: str = args()[0]
n: int = len(args())
for a in args() {
    print(a)
}
```

#### :fresh_list_literal_covariant_to_base

```dr
class Animal { def() {} }
class Dog(Animal) { def() {} }
pets: list[Animal] = [Dog()]
```

#### :named_list_stays_invariant

```dr
class Animal { def() {} }
class Dog(Animal) { def() {} }
xs: list[Dog] = [Dog()]
ys: list[Animal] = xs
```

#### :fresh_list_non_subclass_rejected

```dr
class Animal { def() {} }
class Plant { def() {} }
pets: list[Animal] = [Plant()]
```

#### :named_concrete_list_not_assignable_to_list_any

```dr
names: list[str] = ["a", "b"]
xs: list[Any] = names
```

#### :concrete_list_arg_not_assignable_to_list_any_param

```dr
def first(xs: list[Any]) -> int {
    return len(xs)
}
def run() -> None {
    names: list[str] = ["a", "b"]
    n: int = first(names)
}
```

#### :fresh_literal_arg_still_passable_to_list_any_param

```dr
def first(xs: list[Any]) -> int {
    return len(xs)
}
def run() -> None {
    n: int = first(["a", "b"])
}
```

#### :list_any_not_assignable_to_concrete_list

```dr
xs: list[Any] = ["a", "b"]
names: list[str] = xs
```

#### :dict_value_covariance_to_any_still_allowed

```dr
m: dict[str, int] = {"a": 1}
d: dict[str, Any] = m
```

#### :str_arg_to_int_param_rejected

```dr
def sq(n: int) -> int { return n * n }
sq("x")
```

#### :float_arg_to_int_param_rejected

```dr
def g(n: int) -> int { return n + 1 }
g(3.5)
```

#### :scalar_arg_to_container_param_rejected

```dr
def f(xs: list[int]) -> int { return len(xs) }
f("hello")
```

#### :int_arg_to_float_param_ok

```dr
def area(w: float, h: float) -> float { return w * h }
area(3, 4)
```

#### :subclass_arg_to_base_param_ok

```dr
class Animal { def() {} }
class Dog(Animal) { def() {} }
def greet(a: Animal) -> int { return 1 }
d: Dog = Dog()
greet(d)
```

#### :fresh_list_literal_arg_to_base_list_param_ok

```dr
class Animal { def() {} }
class Dog(Animal) { def() {} }
def feed(pets: list[Animal]) -> int { return len(pets) }
feed([Dog()])
```

#### :generic_call_concrete_param_mismatch_rejected

```dr
def first[T](xs: list[T], tag: str) -> T { return xs[0] }
first([1, 2], 5)
```

#### :generic_call_concrete_param_ok_still_infers

```dr
def first[T](xs: list[T], tag: str) -> T { return xs[0] }
first([1, 2], "label")
```

#### :match_type_test_pattern_ok

```dr
v: int | str = 5
match v {
    case int() { print("i") }
    case str() { print("s") }
}
```

#### :match_class_pattern_arity_mismatch_rejected

```dr
class Point { def(x: int) { self.x = x } }
p: Point = Point(1)
match p { case Point(a, b, c) { print("x") } }
```

#### :match_class_pattern_unknown_type_rejected

```dr
x: int = 5
match x { case Nope() { print("x") } }
```

#### :match_non_exhaustive_union_rejected

```dr
v: int | str = 5
match v {
    case int() { print("i") }
}
```

#### :match_non_exhaustive_bool_rejected

```dr
b: bool = True
match b {
    case True { print("t") }
}
```

#### :match_unreachable_after_wildcard_rejected

```dr
x: int = 1
match x {
    case _ { print("any") }
    case 1 { print("one") }
}
```

#### :match_duplicate_literal_rejected

```dr
x: int = 1
match x {
    case 1 { print("a") }
    case 1 { print("b") }
    case _ { print("c") }
}
```

#### :function_decl_ok

```dr
def add(x: int, y: int) -> int {
  return x + y
}
```

#### :function_return_type_mismatch

```dr
def foo() -> int {
  return "hello"
}
```

#### :function_return_none_from_int_func

```dr
def foo() -> int {
  return
}
```

#### :function_return_none_ok

```dr
def foo() -> None {
  return
}
```

#### :function_call_return_type

```dr
def add(x: int, y: int) -> int {
  return x + y
}
add(1, 2)
```

#### :function_param_types

```dr
def greet(name: str) -> str {
  return "Hello " + name
}
```

#### :function_param_bad_return

```dr
def foo(x: str) -> int {
  return x
}
```

#### :if_statement

```dr
x = 10
if x > 5 {
  print(x)
}
```

#### :while_loop

```dr
x = 0
while x < 10 {
  x = x + 1
}
```

#### :for_loop

```dr
for i in range(10) {
  print(i)
}
```

#### :try_statement

```dr
try {
  x = 10 / 0
} catch Exception as e {
  print("error")
}
```

#### :class_decl

```dr
class Point {
  def(x: int, y: int) -> None {
    pass
  }
}
```

#### :implicit_self_method_type

```dr
class Point {
  def(x: int, y: int) -> None {
    self.x = x
    self.y = y
  }
  def distance() -> float {
    return 0.0
  }
}
```

#### :implicit_self_field_access

```dr
class Point {
  def(x: int, y: int) -> None {
    self.x = x
    self.y = y
  }
}
```

#### :explicit_self_py_mode_type

```py
class Point:
    def distance(self) -> float:
        return 0.0
```

#### :class_instantiation

```dr
class Foo {
  pass
}
Foo()
```

#### :fibonacci_function

```dr
def fib(n: int) -> int {
  if n <= 1 {
    return n
  }
  return fib(n - 1) + fib(n - 2)
}
```

#### :annotated_variable_used_in_expression

```dr
x: int = 10
y: int = x + 5
print(y)
```

#### :multiple_statements

```dr
x = 1
y = 2
z = x + y
print(z)
```

#### :register_external_module_resolves_import

```dr
from utils import add
result: int = add(1, 2)
```

#### :get_exports_returns_defined_functions

```dr
def multiply(a: int, b: int) -> int {
    return a * b
}
x: int = 42
```

#### :cross_module_type_info_flows_to_call_expr

```dr
from math_utils import square
result = square(5)
```

#### :typed_template_requires_template_base

```dr
class Foo {
  def(inner: str) { self._inner = inner }
}
x: str = "a"
y = template[Foo] {!{x}}
```

#### :typed_template_struct_template_reserved_for_d037

```dr
class StructTemplate {
  def() { pass }
}
class Widget(StructTemplate) {
  def() { pass }
}
y = template[Widget] {hi}
```

#### :fire_produces_task_of_callee_return

```dr
def work() -> int { return 21 }
t: Task[int] = fire work()
r: int = t.join()
```

#### :await_unwraps_async_def_return

```dr
async def fetch() -> int { return 99 }
r: int = await fetch()
```

#### :bare_task_annotation_refines_from_rhs

```dr
def work() -> int { return 1 }
t: Task = fire work()
r: int = t.join()
```

#### :bare_task_annotation_refines_from_rhs_2

```dr
def work() -> int { return 1 }
t: Task = fire work()
r: str = t.join()
```

#### :await_int_result_not_assignable_to_str

```dr
async def fetch() -> int { return 99 }
r: str = await fetch()
```

#### :await_on_sync_function_is_error

```dr
def work() -> int { return 5 }
r: int = await work()
```

#### :wrong_explicit_task_param_is_error

```dr
def work() -> int { return 5 }
t: Task[str] = fire work()
```

#### :dict_int_key_int_index_ok

```dr
d: dict[int, str] = {1: "a"}
x: str = d[1]
```

#### :dict_int_key_str_index_is_error

```dr
d: dict[int, str] = {1: "a"}
x: str = d["1"]
```

#### :dict_str_key_str_index_ok

```dr
d: dict[str, str] = {"a": "b"}
x: str = d["a"]
```

#### :dict_str_key_int_index_is_error

```dr
d: dict[str, str] = {"a": "b"}
x: str = d[1]
```

#### :dict_wrong_key_type_on_assign_is_error

```dr
d: dict[int, str] = {1: "a"}
d["1"] = "b"
```

#### :d045__same_class_private_access_ok

```dr
class A {
    __secret: int = 5
    def() {}
    def get() -> int { return self.__secret }
}
a: A = A()
print(a.get())
```

#### :d045__same_package_protected_access_ok

```dr
class A {
    _shared: int = 5
    def() {}
}
def peek(a: A) -> int { return a._shared }
```

#### :d045__subclass_inherited_protected_ok

```dr
class Base {
    _x: int = 1
    def() {}
}
class Derived(Base) {
    def() { super() }
    def use() -> int { return self._x }
}
```

#### :d045__recognized_dunders_ok

```dr
class A {
    def() {}
    def __str__() -> str { return "a" }
    def __eq__(other: A) -> bool { return true }
    def __len__() -> int { return 0 }
}
```

#### :d045__private_across_classes_rejected

```dr
class A {
    __secret: int = 5
    def() {}
}
def peek(a: A) -> int { return a.__secret }
```

#### :d045__subclass_cannot_touch_parent_private

```dr
class Base {
    __secret: int = 1
    def() {}
}
class Derived(Base) {
    def() { super() }
    def peek() -> int { return self.__secret }
}
```

#### :d045__private_from_module_top_level_rejected

```dr
class A {
    __secret: int = 5
    def() {}
}
a: A = A()
print(a.__secret)
```

#### :d045__unrecognized_class_dunder_rejected

```dr
class A {
    def() {}
    def __frobnicate__() -> int { return 1 }
}
```

#### :generic_class_and_function_accepted

```dr
class Box[T] {
    def(v: T) { self.value = v }
    def get() -> T { return self.value }
}
def first[T](xs: list[T]) -> T { return xs[0] }
b: Box[int] = Box[int](5)
n: int = b.get()
xs: list[str] = ["a"]
s: str = first(xs)
```

#### :generic_unbounded_method_call_rejected

```dr
def f[T](t: T) -> int { return t.foo() }
x: int = f[int](5)
```

#### :generic_unbounded_subscript_rejected

```dr
def f[T](t: T) -> int { return t[0] }
x: int = f[int](5)
```

#### :generic_polymorphic_recursion_capped

```dr
class Foo[T] {
    def(v: T) { self.v = v }
    def deeper() -> Foo[list[T]] { return Foo[list[T]]([self.v]) }
}
x: Foo[int] = Foo[int](1)
```

#### :generic_class_arity_mismatch_rejected

```dr
class Pair[K, V] {
    def(k: K, v: V) { self.k = k; self.v = v }
}
p: Pair[int] = Pair[int](1)
```

#### :generic_equality_allowed_ordering_rejected

```dr
def eq[T](a: T, b: T) -> bool { return a == b }
x: bool = eq[int](1, 1)
```

#### :generic_equality_allowed_ordering_rejected_2

```dr
def lt[T](a: T, b: T) -> bool { return a < b }
x: bool = lt[int](1, 2)
```

#### :generic_method_unsolved_type_param_rejected

```dr
class Reg {
    def make[T]() -> T { return T() }
}
r: Reg = Reg()
r.make()
```

#### :generic_method_dual_definition_rejected

```dr
class Conn {
    def one() -> int { return 0 }
    def one[T](id: int) -> T { return T(id) }
}
b: Conn = Conn()
```

#### :generic_method_arity_mismatch_rejected

```dr
class Reg {
    def pair[K, V](k: K, v: V) -> V { return v }
}
r: Reg = Reg()
x: int = r.pair[int](1, 2)
```

#### :generic_method_unbounded_member_access_rejected

```dr
class Reg {
    def grow[T](x: T) -> int { return x.size() }
}
r: Reg = Reg()
n: int = r.grow[int](3)
```

#### :generic_method_on_generic_class_double_mono_ok

```dr
class Container[T] {
    def(v: T) { self.v = v }
    def wrap[U](x: U) -> U { return x }
    def pair[U](x: U) -> tuple[T, U] { return (self.v, x) }
}
c: Container[int] = Container[int](7)
p: tuple[int, str] = c.pair[str]("hi")
```

#### :generic_method_shadows_class_type_param_rejected

```dr
class C[T] {
    def() { }
    def m[T](x: T) -> T { return x }
}
```

#### :generic_method_polymorphic_recursion_capped

```dr
class R[T] {
    def() { }
    def go[U](x: U, n: int) -> int {
        if n <= 0 { return 0 }
        r2: R[T] = R[T]()
        ys: list[U] = [x]
        return 1 + r2.go[list[U]](ys, n - 1)
    }
}
r: R[int] = R[int]()
print(r.go[int](5, 3))
```

#### :generic_instantiation_subclass_rejected

```dr
class Animal[T] {
    def(v: T) { self.tag = v }
}
class Dog(Animal[str]) {
    def(v: str) { self.tag = v }
}
```

#### :generic_union_type_argument_accepted

```dr
class Box[T] {
    def(v: T) { self.value = v }
    def get() -> T { return self.value }
}
a: Box[int | str] = Box[int | str](5)
b: Box[int | str] = Box(7)
```

#### :bounded_type_param_member_access_ok

```dr
class Animal {
    name: str
    def(n: str) { self.name = n }
    def speak() -> str { return self.name }
}
def describe[T: Animal](x: T) -> str { return x.name + x.speak() }
a: Animal = Animal("k")
s: str = describe[Animal](a)
```

#### :bounded_type_param_subclass_arg_accepted

```dr
class Animal { def() { } def speak() -> str { return "a" } }
class Dog(Animal) { def() { } def speak() -> str { return "woof" } }
def describe[T: Animal](x: T) -> str { return x.speak() }
d: Dog = Dog()
s: str = describe[Dog](d)
```

#### :bounded_generic_class_member_access_ok

```dr
class Animal { def() { } def speak() -> str { return "a" } }
class Shelter[T: Animal] {
    occupant: T
    def(o: T) { self.occupant = o }
    def announce() -> str { return self.occupant.speak() }
}
sh: Shelter[Animal] = Shelter[Animal](Animal())
```

#### :bounded_type_param_arg_violates_bound_rejected

```dr
class Animal { def() { } def speak() -> str { return "a" } }
class Cat { def() { } def meow() -> str { return "m" } }
def describe[T: Animal](x: T) -> str { return x.speak() }
c: Cat = Cat()
s: str = describe[Cat](c)
```

#### :bounded_generic_class_arg_violates_bound_rejected

```dr
class Animal { def() { } def speak() -> str { return "a" } }
class Shelter[T: Animal] {
    occupant: T
    def(o: T) { self.occupant = o }
}
sh: Shelter[int] = Shelter[int](5)
```

#### :bounded_generic_method_ok

```dr
class Animal { def() { } def speak() -> str { return "a" } }
class Registry {
    def() { }
    def loudest[T: Animal](a: T) -> str { return a.speak() }
}
r: Registry = Registry()
s: str = r.loudest[Animal](Animal())
```

#### :lambda_body_bad_binding_rejected

```dr
f: Callable[[], int] = lambda () -> int {
    bad: int = "boy"
    return bad
}
```

#### :lambda_body_return_type_mismatch_rejected

```dr
f: Callable[[], int] = lambda () -> int {
    return "boy"
}
```

#### :lambda_body_well_typed_ok

```dr
f: Callable[[int], int] = lambda (n: int) -> int {
    doubled: int = n * 2
    return doubled
}
```

#### :method_on_any_receiver_rejected

```dr
def worker(n: int) -> int { return n }
tasks: list = []
a: Task[int] = fire worker(1)
tasks.append(a)
for t in tasks {
    x: int = t.join()
}
```

#### :method_on_typed_task_list_ok

```dr
def worker(n: int) -> int { return n }
tasks: list[Task[int]] = []
a: Task[int] = fire worker(1)
tasks.append(a)
for t in tasks {
    x: int = t.join()
}
```

#### :field_read_on_any_receiver_rejected

```dr
xs: list = []
for e in xs {
    y: int = e.value
}
```

#### :defer_own_arg_to_borrowing_param_rejected

```dr
def use(d: list[int]) -> None { }
def f() -> None {
    d: list[int] = [1, 2]
    defer use(own d)
}
```

#### :defer_missing_own_at_own_param_rejected

```dr
def sink(own d: list[int]) -> None { }
def f() -> None {
    d: list[int] = [1, 2]
    defer sink(d)
}
```

#### :defer_arg_type_mismatch_rejected

```dr
def use(d: list[int]) -> None { }
def f() -> None {
    defer use("not a list")
}
```

#### :defer_well_typed_call_ok

```dr
def sink(own d: list[int]) -> None { }
def use(d: list[int]) -> None { }
def f() -> None {
    d: list[int] = [1, 2]
    e: list[int] = [3]
    defer use(d)
    defer sink(own e)
}
```

#### :identity_resource_borrow_into_own_ctor_rejected

```dr
class H {
    _fd: int
    def(fd: int) { self._fd = fd }
}
class R {
    own _h: H
    def(own h: H) { self._h = h }
}
def f() -> None {
    h: H = H(4)
    r: R = R(h)
}
```

#### :identity_resource_dub_rejected

```dr
class H {
    _fd: int
    def(fd: int) { self._fd = fd }
}
def f() -> None {
    h: H = H(4)
    h2: H = dub h
}
```

#### :identity_resource_blessed_spellings_ok

```dr
class H {
    _fd: int
    def(fd: int) { self._fd = fd }
}
class R {
    own _h: H
    def(own h: H) { self._h = h }
}
def f() -> None {
    fresh: R = R(H(4))
    h: H = H(5)
    moved: R = R(own h)
}
```

#### :method_self_reassign_in_ctor_rejected

```dr
class T2 {
    def(x: int) {
        self.x: int = x
        self.m = "shadow"
    }
    def m() { print("m") }
}
```

#### :bare_builtin_method_read_rejected

```dr
s: str = "abc"
print(s.upper)
```

#### :bare_task_join_read_rejected

```dr
def work() -> int { return 7 }
t: Task[int] = fire work()
print(t.join)
```

#### :builtin_method_bound_to_callable_rejected

```dr
s: str = "abc"
up: Callable[[Any], str] = s.upper
```

#### :builtin_method_calls_still_ok

```dr
def work() -> int { return 7 }
s: str = "abc"
u: str = s.upper()
parts: list[str] = ["a", "b"]
j: str = ",".join(parts)
parts.append("c")
d: dict[str, int] = {"k": 1}
v: int = d.get("k", 0)
t: Task[int] = fire work()
r: int = t.join()
```

#### :unknown_static_method_call_on_class_rejected

```dr
class Box {
    @staticmethod
    def real() -> None {
        pass
    }
}
Box.nope()
```

#### :unknown_class_attribute_read_rejected

```dr
class Box {
    limit: int = 3
}
x: int = Box.missing
```

#### :static_method_call_on_class_ok

```dr
class Box {
    limit: int = 3
    @staticmethod
    def real() -> int {
        return 7
    }
}
x: int = Box.real()
lim: int = Box.limit
```

#### :inherited_static_method_through_subclass_ok

```dr
class Base {
    @staticmethod
    def make() -> int {
        return 1
    }
}
class Sub(Base) {
    pass
}
x: int = Sub.make()
```

#### :unknown_member_on_subclass_chain_rejected

```dr
class Base {
    @staticmethod
    def make() -> int {
        return 1
    }
}
class Sub(Base) {
    pass
}
Sub.fabricate()
```

#### :contract_composition_conflict_rejected

```dr
type A {
    def m() -> str
}
type B {
    def m() -> int
}
type C(A, B) {}
```

#### :sum_rejects_non_numeric_elements

```dr
xs: list[str] = ["a", "b"]
s: str = sum(xs)
```

#### :sum_rejects_non_numeric_elements_2

```dr
xs: list[float] = [1.5, 2.5]
t: float = sum(xs)
```

#### :min_max_reject_container_elements

```dr
xs: list[list[int]] = [[1], [2]]
m: list[int] = min(xs)
```

#### :min_max_reject_container_elements_2

```dr
xs: list[str] = ["a", "b"]
m: str = min(xs)
```

#### :generic_and_monomorphic_same_name_rejected

```dr
def f(x: int) -> int {
    return x + 1
}
def f[T](x: T) -> T {
    return x
}
```

#### :generic_and_monomorphic_same_name_rejected_2

```dr
def f[T](x: T) -> T {
    return x
}
def f(x: int) -> int {
    return x + 1
}
```

#### :generic_and_monomorphic_same_name_rejected_3

```dr
def f[T](x: T) -> T {
    return x
}
def f[T](x: T, y: T) -> T {
    return x
}
```

#### :iter_returning_class_without_next_rejected

```dr
class Bag {
    def() {
        self.n: int = 0
    }
    def __iter__() -> Sack {
        return Sack()
    }
}
class Sack {
    def() {
        self.n: int = 0
    }
}
def f() -> int {
    total: int = 0
    for x in Bag() {
        total = total + 1
    }
    return total
}
```

#### :in_on_scalar_rejected

```dr
x: int = 5
b: bool = x in 3
```

#### :in_on_scalar_rejected_2

```dr
xs: list[int] = [1, 2]
b: bool = 1 in xs
```

#### :in_on_scalar_rejected_3

```dr
d: dict[str, int] = {"a": 1}
b: bool = "a" in d
```

#### :in_on_tuple_rejected

```dr
t: tuple[int, int, int] = (1, 2, 3)
b: bool = 2 in t
```

#### :in_on_instance_needs_contains

```dr
class Box {
    v: int
    def(v: int) { self.v = v }
}
b: Box = Box(5)
ok: bool = 1 in b
```

#### :in_on_instance_needs_contains_2

```dr
class Bag {
    xs: list[int]
    def(xs: list[int]) { self.xs = xs }
    def __contains__(v: int) -> bool { return v in self.xs }
}
b: Bag = Bag([1])
ok: bool = 1 in b
```

#### :isinstance_second_arg_must_be_type

```dr
x: int = 5
n: int = 3
b: bool = isinstance(x, n)
```

#### :isinstance_second_arg_must_be_type_2

```dr
x: int = 5
b: bool = isinstance(x, int)
```

#### :isinstance_second_arg_must_be_type_3

```dr
class Cow {
    v: int
    def(v: int) { self.v = v }
}
c: Cow = Cow(1)
b: bool = isinstance(c, Cow)
```

#### :non_callable_callee_rejected

```dr
xs: list[int] = [10, 20]
print(xs[0](3))
```

#### :set_type_is_honest

```dr
s: set[int] = {1, 2}
s
```

#### :set_operators_typed

```dr
a: set[int] = {1, 2}
b: set[int] = {2, 3}
c: set[int] = a | b
d: set[int] = a & b
e: set[int] = a - b
f: set[int] = a ^ b
```

#### :set_operators_typed_2

```dr
a: set[int] = {1, 2}
b: set[int] = {2, 3}
c: set[int] = a + b
```

#### :set_operators_typed_3

```dr
a: set[int] = {1, 2}
xs: list[int] = [1]
c: set[int] = a | xs
```

#### :set_operators_typed_4

```dr
a: set[int] = {1, 2}
b: set[str] = {"x"}
c: set[int] = a | b
```

#### :discarded_task_statement_rejected

```dr
async def fetch(url: str) -> str {
    return url
}
fetch("http://x")
```

#### :bare_fire_statement_accepted

```dr
def work(n: int) -> int {
    return n
}
fire work(1)
```

#### :bound_task_declaration_accepted

```dr
async def fetch(url: str) -> str {
    return url
}
def go() -> str {
    t: Task[str] = fetch("http://x")
    return await t
}
```

#### :async_any_return_rejected

```dr
async def gives(n: int) -> Any {
    return n
}
```

#### :fire_on_any_returning_callee_rejected

```dr
def gives(n: int) -> Any {
    return n
}
t: Task[Any] = fire gives(1)
```

#### :dub_of_task_rejected

```dr
async def fetch(url: str) -> str {
    return url
}
def go() -> str {
    t: Task[str] = fetch("http://x")
    u: Task[str] = dub t
    return await u
}
```

#### :raise_from_cause_rejected

```dr
class E(Exception) {
    def(m: str) { self.message = m }
}
try {
    raise ValueError("low")
} except ValueError as e {
    raise E("high") from e
}
```

#### :own_param_by_keyword_requires_own

```dr
def take(own s: str) -> int { return len(s) }
def mk() -> str { return "a" + "b" }
def main() {
    b: str = mk()
    print(take(s=b))
}
main()
```

#### :own_param_by_keyword_accepts_own_marked_arg

```dr
def take(own s: str) -> int { return len(s) }
def mk() -> str { return "a" + "b" }
def main() {
    b: str = mk()
    print(take(s=own b))
}
main()
```

#### :own_param_by_keyword_accepts_fresh_value

```dr
def take(own s: str) -> int { return len(s) }
def mk() -> str { return "a" + "b" }
def main() {
    print(take(s=mk()))
}
main()
```

#### :borrow_param_by_keyword_rejects_own

```dr
def borrows(s: str) -> int { return len(s) }
def mk() -> str { return "a" + "b" }
def main() {
    b: str = mk()
    print(borrows(s=own b))
}
main()
```

#### :dub_satisfies_own_param

```dr
def take(own s: str) -> int { return len(s) }
def mk() -> str { return "a" + "b" }
def main() {
    b: str = mk()
    print(take(dub b))
    print(len(b))
}
main()
```

#### :dub_satisfies_own_param_by_keyword

```dr
def take(own s: str) -> int { return len(s) }
def mk() -> str { return "a" + "b" }
def main() {
    b: str = mk()
    print(take(s=dub b))
    print(len(b))
}
main()
```

#### :unmarked_binding_still_rejected_by_own_param

```dr
def take(own s: str) -> int { return len(s) }
def mk() -> str { return "a" + "b" }
def main() {
    b: str = mk()
    print(take(b))
}
main()
```

#### :exception_subclass_message_construction_ok

```dr
class error(Exception) {}
def boom() {
    raise error("bad")
}
```

#### :builtin_attr_unknown_on_int

A builtin receiver has a closed member set, so an attribute that is not one of
its methods is rejected at check time rather than deferred to codegen.

```dr
n: int = 5
print(n.no_such_attr)
```

#### :builtin_attr_unknown_on_str

```dr
s: str = "x"
print(s.no_such_attr)
```

#### :builtin_attr_unknown_method_on_str

```dr
s: str = "x"
print(s.no_such_method())
```

#### :builtin_attr_unknown_on_list

```dr
xs: list[int] = [1]
print(xs.no_such_attr)
```

#### :builtin_attr_known_methods_accepted

The member tables must stay complete, or valid programs break. These are all
real methods and must type-check cleanly.

```dr
s: str = "ab"
xs: list[int] = [1]
b: bytes = b"a,b"
st: set[int] = {1}
print(s.upper())
print(b.join([b"x", b"y"]))
print(len(b.split(b",")))
print(b.replace(b"a", b"z"))
xs.append(2)
st.update({2})
print(len(xs) + len(st))
```

#### :dict_dot_access_is_not_an_attribute_error

A dict has open members because dot-access reads string keys, so an unknown
name on a dict receiver must not be rejected.

```dr
ages: dict[str, int] = {"Ada": 36}
print(ages.Ada)
```
