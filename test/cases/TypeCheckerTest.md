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

#### :duplicate_ctor_arity_rejected

Constructors dispatch by argument count alone, so two constructors with the
same count cannot both be reachable. Left in, they used to miscompile into an
arity switch with duplicate cases; the class must be rejected at type check.

```dr
class P {
    tag: str = ""
    def (a: int, b: int) {
        self.tag = "int-int"
    }
    def (a: str, b: int) {
        self.tag = "str-int"
    }
}
p: P = P("hello", 1)
```

#### :distinct_ctor_arities_ok

```dr
class P {
    tag: str = ""
    def (a: int) {
        self.tag = "one"
    }
    def (a: int, b: int) {
        self.tag = "two"
    }
}
x: P = P(1)
y: P = P(1, 2)
```

#### :ctor_overload_arity_mismatch_rejected

A call whose argument count matches no constructor overload used to pass the
type checker and die at codegen as an internal error with an LLVM verifier
dump; it must be a plain type-check error like the single-constructor case.

```dr
class Q {
    a: int = 0
    def (a: int) {
        self.a = a
    }
    def (a: int, b: int) {
        self.a = a + b
    }
}
q: Q = Q(1, 2, 3)
```

#### :ctor_overload_zero_args_rejected

```dr
class Q {
    a: int = 0
    def (a: int) {
        self.a = a
    }
    def (a: int, b: int) {
        self.a = a + b
    }
}
q: Q = Q()
```

#### :ctor_overlapping_defaults_rejected

Two constructors whose defaulted parameters overlap make some call counts
ambiguous (a 1-argument call below fits both). Never guess: the class is
rejected where it is declared, with both constructors on screen.

```dr
class Q {
    v: int = 0
    def (a: int, b: int = 1) {
        self.v = a + b
    }
    def (x: int, y: int = 1, z: int = 2) {
        self.v = x + y + z
    }
}
q: Q = Q(5)
```

#### :ctor_exact_shadow_of_defaults_ok

An exact-arity constructor beside a defaulted one is legal: exact match always
wins, so no call count is ambiguous.

```dr
class Q {
    v: int = 0
    def (a: int) {
        self.v = a
    }
    def (a: int, b: int = 5) {
        self.v = a + b
    }
}
x: Q = Q(1)
y: Q = Q(1, 2)
```

#### :ctor_overload_kwargs_ok

```dr
class Q {
    v: int = 0
    def (a: int) {
        self.v = a
    }
    def (a: int, b: int) {
        self.v = a + b
    }
}
w: Q = Q(a=1, b=2)
x: Q = Q(1, b=2)
y: Q = Q(a=9)
```

#### :ctor_overload_kwargs_unknown_name_rejected

```dr
class Q {
    v: int = 0
    def (a: int) {
        self.v = a
    }
    def (a: int, b: int) {
        self.v = a + b
    }
}
q: Q = Q(a=1, zz=2)
```

#### :ctor_overload_kwargs_missing_required_rejected

```dr
class Q {
    v: int = 0
    def (a: int) {
        self.v = a
    }
    def (a: int, b: int) {
        self.v = a + b
    }
}
q: Q = Q(b=2)
```

#### :ctor_overload_default_range_ok

A defaulted trailing parameter widens the overload's accepted count downward,
so a two-argument call legally dispatches to the three-parameter constructor.

```dr
class R {
    v: int = 0
    def (a: int) {
        self.v = a
    }
    def (a: int, b: int, c: int = 100) {
        self.v = a + b + c
    }
}
r: R = R(1, 2)
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
xs: list[int | str] = names
```

#### :concrete_list_arg_not_assignable_to_list_any_param

```dr
def first(xs: list[int | str]) -> int {
    return len(xs)
}
def run() -> None {
    names: list[str] = ["a", "b"]
    n: int = first(names)
}
```

#### :fresh_literal_arg_still_passable_to_list_any_param

```dr
def first(xs: list[int | str]) -> int {
    return len(xs)
}
def run() -> None {
    n: int = first(["a", "b"])
}
```

#### :list_any_not_assignable_to_concrete_list

```dr
xs: list[int | float | str | bytes | list[int] | list[str] | list[float] | list[list[int]] | list[list[str]] | dict[str, int] | dict[str, str] | tuple[]] = ["a", "b"]
names: list[str] = xs
```

#### :dict_value_covariance_to_union_rejected

```dr
m: dict[str, int] = {"a": 1}
d: dict[str, int | str] = m
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
up: Callable[[str], str] = s.upper
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
async def gives(n: int) -> int | str {
    return n
}
```

#### :fire_on_any_returning_callee_rejected

```dr
def gives(n: int) -> int | str {
    return n
}
t: Task[int | str] = fire gives(1)
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

#### :exception_subclass_rejects_non_message_arg

A subclass of a builtin exception that declares no constructor is the
synthesized message family: it takes an optional `str`. An `int` there has no
constructor to bind to, so it is refused instead of reaching codegen.

```dr
class NumErr(Exception) {}
def boom() {
    raise NumErr(42)
}
```

#### :exception_subclass_rejects_extra_args

```dr
class TwoErr(Exception) {}
def boom() {
    raise TwoErr("a", "b")
}
```

#### :exception_message_field_must_be_str

`message` is the field the synthesized exception constructor writes and the
field an exception renders from, so an exception subclass may not redeclare it
at another type. Before this was refused, the declaration silently shared the
inherited `str` slot and reading it back returned the message text.

```dr
class A(Exception) {}
class C(A) {
    message: int = 3
}
c: C = C()
```

#### :exception_message_field_must_be_str_when_inferred

```dr
class A(Exception) {}
class E(A) {
    def(m: int) { self.message = m }
}
e: E = E(5)
```

#### :exception_message_field_str_is_ok

```dr
class A(Exception) {}
class C(A) {
    message: str = "z"
}
c: C = C()
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

#### :str_contains_method_rejected

`str.contains()` was retired in favour of the `in` operator, which is the same
operation. The diagnostic names the replacement.

```dr
s: str = "hello"
print(s.contains("ell"))
```

#### :str_substring_in_accepted

```dr
s: str = "hello"
print("ell" in s)
print("zzz" not in s)
```

#### :class_object_in_list_arg_rejected

Passing a class where an instance is expected is caught in a scalar slot and in
an annotated container, but a call argument used to slip through and segfault at
runtime instead. The element type is checked now.

```dr
class Thing {
    n: int
    def() { self.n = 1 }
}

def take(items: list[Thing]) -> int { return len(items) }

print(take([Thing]))
```

#### :instance_in_list_arg_accepted

```dr
class Thing {
    n: int
    def() { self.n = 1 }
}

def take(items: list[Thing]) -> int { return len(items) }

print(take([Thing(), Thing()]))
```

#### :generic_ctor_without_args_rejected

A bare construction of a generic class with no annotation and no explicit type
arguments has nothing to infer from, and stays a compile error after the
DRG-85 scope-resolution fix routed same-named plain classes around this path.

```dr
class Box[T] {
    def(v: T) {
        self.v: T = v
    }
}

b: Box = Box(5)
```

#### :generic_ctor_explicit_accepted

```dr
class Box[T] {
    def(v: T) {
        self.v: T = v
    }
}

b: Box[int] = Box[int](5)
```

#### :attr_on_unnarrowed_optional_rejected

An attribute access through `T | None` used to fall through the checker as
silent Unknown, and the boxing fallback then stamped an int tag on a str
pointer (DRG-101). Possibly-none receivers must be narrowed first.

```dr
class Box {
    name: str
    def(name: str) { self.name = name }
}

ob: Optional[Box] = Box("x")
print(ob.name)
```

#### :attr_on_plain_union_rejected

```dr
v: int | str = "x"
print(v.upper())
```

#### :method_call_on_unnarrowed_optional_rejected

```dr
class Box {
    name: str
    def(name: str) { self.name = name }
    def tag() -> str { return self.name }
}

ob: Optional[Box] = Box("x")
print(ob.tag())
```

#### :attr_after_complement_guard_accepted

```dr
class Box {
    name: str
    def(name: str) { self.name = name }
}

def probe() -> str {
    ob: Optional[Box] = Box("x")
    if not isinstance(ob, Box) {
        return ""
    }
    return ob.name
}
```

#### :attr_after_else_return_accepted

```dr
class Box {
    name: str
    def(name: str) { self.name = name }
}

def probe() -> str {
    ob: Optional[Box] = Box("x")
    if isinstance(ob, Box) {
        pass
    } else {
        return ""
    }
    return ob.name
}
```

#### :min_varargs_concrete_mismatch_rejected

`min` over concrete int arguments types as int, so assigning it to str is the
same compile error as `name: int = "boy"`.

```dr
s: str = min(5, 2, 8)
```

#### :render_class_without_str_print_rejected

A class with no `__str__` or `__repr__` has no text form. Printing one used to
emit `<Point instance>` while `str()` and f-strings pasted the first byte of the
object header, so one value had three different answers. There is one rule now:
the compiler refuses, and names the fix.

```dr
class Point {
    x: int
    def(x: int) {
        self.x = x
    }
}
p: Point = Point(1)
print(p)
```

#### :render_class_without_str_str_call_rejected

```dr
class Point {
    x: int
    def(x: int) {
        self.x = x
    }
}
p: Point = Point(1)
s: str = str(p)
```

#### :render_class_without_str_fstring_rejected

```dr
class Point {
    x: int
    def(x: int) {
        self.x = x
    }
}
p: Point = Point(1)
s: str = f"{p}"
```

#### :render_class_without_str_splice_rejected

```dr
class Point {
    x: int
    def(x: int) {
        self.x = x
    }
}
p: Point = Point(1)
page: str = template {<p>!{p}</p>}
```

#### :render_class_with_str_accepted

One `__str__` makes the value renderable everywhere: `print`, `str()`,
f-strings, and template splices share a single rule.

```dr
class Money {
    amount: int
    def(amount: int) {
        self.amount = amount
    }
    def __str__() -> str {
        return str(self.amount)
    }
}
m: Money = Money(5)
print(m)
s: str = str(m)
f: str = f"{m}"
page: str = template {<p>!{m}</p>}
```

#### :render_container_accepted

A container renders structurally, the same text an f-string produces.

```dr
xs: list[int] = [1, 2]
d: dict[str, int] = {"a": 1}
print(xs)
s: str = str(d)
page: str = template {<p>!{xs}</p>}
```

#### :spread_of_nested_container_rejected

A nested-container element cannot be rendered at a known type, so it cannot be
escaped honestly. Render each element yourself instead.

```dr
xs: list[list[int]] = [[1], [2]]
page: str = template {<p>!{*xs}</p>}
```

#### :spread_of_non_list_rejected

```dr
d: dict[str, int] = {"a": 1}
page: str = template {<p>!{*d}</p>}
```

#### :spread_of_nested_list_rejected

```dr
rows: list[list[int]] = [[1], [2]]
page: str = template {<p>!{*rows}</p>}
```

#### :spread_of_class_without_str_rejected

```dr
class Point {
    x: int
    def(x: int) {
        self.x = x
    }
}
ps: list[Point] = [Point(1)]
page: str = template {<p>!{*ps}</p>}
```

#### :join_of_class_without_str_rejected

```dr
class Point {
    x: int
    def(x: int) {
        self.x = x
    }
}
ps: list[Point] = [Point(1)]
page: str = template {<p>!{ps | join(", ")}</p>}
```

#### :join_separator_without_str_rejected

The separator travels the same rendering rule as the elements, so an
unrenderable separator is the same compile error.

```dr
class Point {
    x: int
    def(x: int) {
        self.x = x
    }
}
sep: Point = Point(1)
parts: list[str] = ["a", "b"]
page: str = template {<p>!{parts | join(sep)}</p>}
```

#### :spread_of_str_list_accepted

```dr
parts: list[str] = ["a", "b"]
page: str = template {<p>!{*parts}</p>}
raw_page: str = template {<p>!{*parts | raw}</p>}
joined: str = template {<p>!{parts | join(", ")}</p>}
ints: list[int] = [1, 2]
numbers: str = template {<p>!{*ints}</p>}
```

#### :render_contract_typed_value_rejected

A contract names a shape, not a class, so there is no `__str__` to dispatch to
at the splice site. Before, the pointer was pasted as if it were text.

```dr
type Speaker {
    def speak() -> str
}

class Dog -> Speaker {
    name: str
    def(name: str) {
        self.name = name
    }
    def speak() -> str {
        return self.name + " barks"
    }
    def __str__() -> str {
        return self.name
    }
}

def render(s: Speaker) -> str {
    return f"{s}"
}
```

#### :walrus_rebind_wrong_type_rejected

A `:=` declaration fixes the binding's type exactly like an annotated one; a
later `=` at a different type must be refused, not silently reinterpret the
value's bits.

```dr
n := 5
n = "x"
```

#### :walrus_rebind_container_wrong_type_rejected

```dr
d: dict[str, int] = {"id": 7}
cur := d
cur = 10
```

#### :walrus_rebind_same_type_accepted

```dr
n := 5
n = 6
xs := [1, 2, 3]
xs = [4, 5]
```

#### :recursive_type_alias_accepted

A recursive alias is the honest spelling for JSON-shaped data: the arms are
closed and every descent is a checked narrowing.

```dr
type Data = str | int | float | bool | none | list[Data] | dict[str, Data]

d: dict[str, Data] = {"user": {"name": "Ada"}, "id": 7}
xs: list[Data] = [1, "two", {"k": 3}]
```

#### :recursive_type_alias_self_only_rejected

```dr
type T = T
```

#### :recursive_type_alias_bare_arm_rejected

A recursive reference must sit inside a container; a bare self-arm has no
base case and no representation.

```dr
type T = int | T
```

#### :union_element_nested_literal_accepted

A nested container literal is matched against the union's container arm, so a
literal can populate a `dict[str, Data]` without an explicit cast.

```dr
type Data = str | int | list[Data] | dict[str, Data]

d: dict[str, Data] = {"a": {"b": "c"}, "n": 1}
```

#### :intc_local_rejected

`intc` is C's 32-bit int and exists only so `extern "C"` signatures can spell the
C ABI. As a Dragon local it has no honest meaning: the checker models it as
`int`, codegen stores it as `i32`, and the widths disagree.

```dr
def main() {
    v: intc = 5
    print(v)
}
main()
```

#### :intc_field_rejected

```dr
class Holder {
    n: intc
    def () {
        self.n = 7
    }
}
```

#### :intc_param_in_dragon_fn_rejected

```dr
def take(n: intc) -> int {
    return 1
}
```

#### :intc_return_in_dragon_fn_rejected

```dr
def give() -> intc {
    return 1
}
```

#### :intc_in_extern_signature_ok

The one place `intc` belongs. Both the parameter and the return type are part of
the C ABI here, and the value widens back to `int` on the Dragon side.

```dr
extern "C" def usleep(usec: intc) -> intc

def main() {
    r: int = usleep(1)
    print(r)
}
main()
```

#### :deque_assigned_to_list_rejected

A `deque` is a `DragonDeque` at runtime, not a `DragonList`. Letting it flow into a
`list[int]` slot means every later `len`/index/iterate reads a list header off a
deque struct and prints whatever lands there.

```dr
from collections import deque

def main() {
    d: deque[int] = deque()
    d.append(1)
    xs: list[int] = d
    print(len(xs))
}
main()
```

#### :list_assigned_to_deque_rejected

```dr
from collections import deque

def main() {
    xs: list[int] = [1, 2, 3]
    d: deque[int] = xs
    print(len(d))
}
main()
```

#### :deque_passed_to_list_param_rejected

```dr
from collections import deque

def total(xs: list[int]) -> int {
    return len(xs)
}

def main() {
    d: deque[int] = deque([1, 2, 3])
    print(total(d))
}
main()
```

#### :deque_returned_as_list_rejected

```dr
from collections import deque

def build() -> list[int] {
    d: deque[int] = deque([1, 2])
    return d
}

def main() {
    print(len(build()))
}
main()
```

#### :deque_element_type_mismatch_rejected

```dr
from collections import deque

def main() {
    d: deque[int] = deque()
    d.append("nope")
}
main()
```

#### :deque_flows_as_deque_accepted

```dr
from collections import deque

def drain(q: deque[str]) -> int {
    n: int = 0
    while len(q) > 0 {
        q.popleft()
        n = n + 1
    }
    return n
}

def build() -> deque[str] {
    q: deque[str] = deque()
    q.append("a")
    q.append("b")
    return q
}

def main() {
    print(drain(build()))
}
main()
```

#### :deque_for_loop_rejected

There is no deque iteration protocol in the runtime, so a `for` over one used to
walk a `DragonList` header laid over a `DragonDeque` and yield garbage past the
real end.

```dr
from collections import deque

def main() {
    d: deque[int] = deque([1, 2, 3])
    for x in d {
        print(x)
    }
}
main()
```

#### :deque_comprehension_rejected

```dr
from collections import deque

def main() {
    d: deque[int] = deque([1, 2, 3])
    xs: list[int] = [x for x in d]
    print(len(xs))
}
main()
```

#### :deque_sorted_rejected

```dr
from collections import deque

def main() {
    d: deque[int] = deque([3, 1, 2])
    xs: list[int] = sorted(d)
    print(len(xs))
}
main()
```

#### :deque_index_rejected

A `DragonDeque` is a circular buffer with a head offset, not a `DragonList`, so
indexing one used to read the struct's own fields back as an element.

```dr
from collections import deque

def main() {
    d: deque[int] = deque([1, 2])
    x: int = d[0]
    print(x)
}
main()
```

#### :user_sorted_shadowing_builtin_accepted

A user function may take a `deque` and be named after a sequence builtin. The
deque-is-not-iterable gate must fire on the builtin, not on the bare name.

```dr
from collections import deque

def sorted(q: deque[int]) -> int {
    return len(q)
}

def main() {
    d: deque[int] = deque()
    d.append(1)
    print(sorted(d))
}
main()
```

#### :user_min_shadowing_builtin_accepted

```dr
from collections import deque

def min(q: deque[str]) -> str {
    return q.popleft()
}

def main() {
    d: deque[str] = deque(["a", "b"])
    print(min(d))
}
main()
```

#### :list_arg_to_defaulted_str_param_rejected

An argument bound to a declared parameter is compared to that parameter's type
whether or not the call supplies the trailing defaulted parameters. Before, a
call that leaned on a default skipped argument checking entirely, so a
`list[str]` reached a `str` parameter and rendered as the empty string.

```dr
def contains(needle: str, haystack: str, msg: str = "") -> bool {
    return needle in haystack
}
xs: list[str] = ["a", "b"]
print(contains("a", xs))
```

#### :int_arg_to_defaulted_str_param_rejected

```dr
def note(text: str, msg: str = "") {
    print(text)
}
note(1)
```

#### :list_arg_to_defaulted_method_param_rejected

```dr
class Log {
    def note(text: str, msg: str = "") {
        print(text)
    }
}
l: Log = Log()
xs: list[str] = ["a"]
l.note(xs)
```

#### :list_arg_to_defaulted_self_method_param_rejected

```dr
class Log {
    def note(text: str, msg: str = "") {
        print(text)
    }
    def go() {
        xs: list[str] = ["a"]
        self.note(xs)
    }
}
l: Log = Log()
l.go()
```

#### :list_arg_to_defaulted_ctor_param_rejected

```dr
class Box {
    label: str
    def (label: str, note: str = "") {
        self.label = label
    }
}
xs: list[str] = ["a"]
b: Box = Box(xs)
print(b.label)
```

#### :arg_before_keyword_arg_type_checked

```dr
def note(text: str, msg: str = "") {
    print(text)
}
xs: list[str] = ["a"]
note(xs, msg="m")
```

#### :defaulted_call_with_matching_args_accepted

```dr
def note(text: str, msg: str = "") {
    print(text)
}
class Log {
    def say(text: str, msg: str = "") {
        print(text)
    }
}
def each(first: str, *rest: str) {
    print(first)
}
note("a")
note("a", "b")
l: Log = Log()
l.say("a")
each("a", "b", "c")
xs: list[str] = ["a"]
each(*xs)
```

#### :keyword_arg_value_type_checked

A keyword argument binds a declared parameter, so its value is compared to that
parameter's type exactly as a positional argument is.

```dr
def note(text: str, msg: str = "") {
    print(text)
}
note(text=1)
```

#### :keyword_arg_list_to_str_param_rejected

```dr
def note(text: str, msg: str = "") {
    print(text)
}
xs: list[str] = ["a"]
note(text=xs)
```

#### :keyword_arg_on_method_type_checked

```dr
class Log {
    def say(text: str, msg: str = "") {
        print(text)
    }
}
l: Log = Log()
xs: list[str] = ["a"]
l.say(text=xs)
```

#### :keyword_arg_on_ctor_type_checked

```dr
class Box {
    label: str
    def (label: str, note: str = "") {
        self.label = label
    }
}
xs: list[str] = ["a"]
b: Box = Box(label=xs)
print(b.label)
```

#### :keyword_arg_on_ctor_overload_type_checked

```dr
class Tag {
    name: str
    def (name: str) {
        self.name = name
    }
    def (name: str, extra: str) {
        self.name = name + extra
    }
}
xs: list[str] = ["a"]
t: Tag = Tag(name=xs, extra="b")
print(t.name)
```

#### :keyword_arg_trailing_default_value_type_checked

```dr
def note(text: str, msg: str = "") {
    print(text)
}
note("a", msg=1)
```

#### :keyword_call_with_matching_args_accepted

```dr
def note(text: str, msg: str = "") {
    print(text)
}
def each(first: str, *rest: str) {
    print(first)
}
class Log {
    def say(text: str, msg: str = "") {
        print(text)
    }
    def wrap(**opts: str) {
        print(str(len(opts)))
    }
}
class Box {
    label: str
    def (label: str, note: str = "") {
        self.label = label
    }
}
note(text="a")
note("a", msg="b")
note(text="a", msg="b")
l: Log = Log()
l.say(text="a", msg="b")
l.wrap(anything="a")
b: Box = Box(label="a", note="n")
print(b.label)
xs: list[str] = ["a"]
each(*xs)
```
