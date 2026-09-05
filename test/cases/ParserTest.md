# ParserTest cases

Source programs for `test/ParserTest.cpp`. Each block is named by the heading
above it and pulled in with `code("<name>")`.

#### :function_docstring_lifted

```dr
def foo() -> int:
    """foo's doc."""
    return 0
```

#### :class_docstring_lifted

```dr
class C:
    """class doc."""
    pass
```

#### :f_string_not_lifted_as_docstring

```dr
def foo() -> int:
    f"hello {1}"
    return 0
```

#### :function_with_body

```dr
def add(a: int, b: int) -> int {
  return a + b
}
```

#### :class_with_methods

```dr
class Point {
  def(x: int, y: int) {
    self.x = x
    self.y = y
  }
  def distance() -> float {
    return 0.0
  }
}
```

#### :for_with_range

```dr
for i in range(10) {
  print(i)
}
```

#### :nested_if_while

```dr
if True {
  while x {
    break
  }
}
```

#### :try_catch_finally_full

```dr
try {
  pass
} catch ValueError as e {
  pass
} finally {
  pass
}
```

#### :implicit_self_method

```dr
class Point {
  def distance(other: Point) -> float {
    return 0.0
  }
}
```

#### :explicit_self_in_dragon_is_error

```dr
class Foo {
  def bar(self) {
    pass
  }
}
```

#### :explicit_self_in_py_mode_ok

```py
class Foo:
    def bar(self):
        pass
```

#### :py_match_int_literal_cases

```py
match x:
    case 1:
        pass
    case 2:
        pass
```

#### :py_match_wildcard_case

```py
match x:
    case 1:
        pass
    case _:
        pass
```

#### :py_match_or_pattern

```py
match x:
    case 1 | 2 | 3:
        pass
```

#### :py_match_sequence_pattern

```py
match point:
    case [x, y]:
        pass
```

#### :py_match_with_guard

```py
match x:
    case n if n > 0:
        pass
```

#### :py_match_multiple_bodies

```py
match x:
    case 1:
        pass
        pass
    case _:
        pass
```

#### :py_except_star_parsed

```py
try:
    pass
except* ValueError:
    pass
```

#### :py_except_star_with_as

```py
try:
    pass
except* ValueError as eg:
    pass
```

#### :py_multiple_inheritance_parsed

```py
class Child(Base1, Base2):
    pass
```

#### :py_class_method_explicit_self

```py
class Dog(Animal):
    def __init__(self, x):
        self.x = x
    def speak(self):
        return self.x
```

#### :py_with_statement

```py
with open("test.txt", "r") as f:
    pass
```

#### :py_for_loop

```py
for i in range(10):
    pass
```

#### :py_try_except_else_finally

```py
try:
    pass
except ValueError:
    pass
else:
    pass
finally:
    pass
```

#### :py_function_pos_only_kw_only

```py
def foo(a, b, /, c, *, d, e):
    pass
```

#### :py_walrus_operator

```py
if (n := 10) > 5:
    pass
```

#### :def_ctor_multiple

```dr
class Point {
  def(x: int, y: int) {
    self.x = x
    self.y = y
  }
  def(xy: int) {
    self.x = xy
    self.y = xy
  }
}
```

#### :dunder_init_in_dragon_is_error

```dr
class Foo {
  def __init__(x: int) {
    self.x = x
  }
}
```

#### :dunder_init_in_py_mode_ok

```py
class Foo:
    def __init__(self, x: int) -> None:
        self.x = x
```

#### :extern_c_from_lib

```dr
extern "C" from "curl" {
  def curl_easy_init() -> ptr
  def curl_easy_cleanup(handle: ptr) -> int
}
```

#### :extern_c_with_ptr_type

```dr
extern "C" def malloc(size: int) -> ptr
extern "C" def free(p: ptr)
```

#### :staticmethod_decorator_py

```py
class Foo:
    @staticmethod
    def bar(x: int) -> int:
        return x * 2
```

#### :classmethod_decorator_py

```py
class Foo:
    @classmethod
    def create(cls) -> Foo:
        pass
```

#### :staticmethod_decorator_dr

```dr
class Foo {
  @staticmethod
  def bar(x: int) -> int {
    return x * 2
  }
}
```

#### :non_method_decorator_no_effect

```dr
@staticmethod
def foo(x: int) -> int {
  return x
}
```

#### :defer_function_call_parses

```dr
def f(n: int) -> None { pass }
def g() -> None {
    defer f(1)
}
```

#### :defer_method_call_parses

```dr
def g(s: str) -> None {
    defer s.upper()
}
```

#### :defer_non_call_rejected

```dr
def g() -> None {
    x: int = 1
    defer x
}
```

#### :defer_binary_expr_rejected

```dr
def f() -> int { return 1 }
def g() -> None {
    defer f() + f()
}
```

#### :defer_remains_usable_as_identifier

```dr
def defer_task(defer: int) -> int { return defer }
def g() -> int {
    defer: int = 3
    return defer_task(defer)
}
```

#### :own_on_call_result_teaches_the_rule

```dr
def take(own s: str) -> int {
  return len(s)
}
def mk() -> str {
  return "a" + "b"
}
print(take(own mk()))
```

#### :dub_on_call_result_teaches_the_rule

```dr
def borrows(s: str) -> int {
  return len(s)
}
def mk() -> str {
  return "a" + "b"
}
print(borrows(dub mk()))
```

#### :own_on_field_teaches_the_rule_without_cascade

```dr
class W {
  v: str
  def(v: str) {
    self.v = v
  }
}
def take(own s: str) -> int {
  return len(s)
}
w: W = W("a")
print(take(own w.v))
```

#### :dub_on_field_is_a_copy_not_an_error

```dr
class W {
  v: str
  def(v: str) {
    self.v = v
  }
}
def borrows(s: str) -> int {
  return len(s)
}
w: W = W("a")
print(borrows(dub w.v))
```

#### :dub_on_element_teaches_the_rule

```dr
xs: list[str] = ["a"]
x: str = dub xs[0]
```

#### :own_mark_on_call_result_teaches_the_rule_by_keyword

```dr
def take(own s: str) -> int {
  return len(s)
}
def mk() -> str {
  return "a" + "b"
}
print(take(s=own mk()))
```

#### :own_and_dub_remain_ordinary_identifiers

```dr
def f(own: int) -> int {
  return own
}
print(f(own=3))
```

#### :own_on_call_result_in_assignment_teaches_the_rule

```dr
def mk() -> str {
  return "a" + "b"
}
x: str = own mk()
```

#### :dub_on_field_in_assignment_is_a_copy

```dr
class W {
  v: str
  def(v: str) {
    self.v = v
  }
}
w: W = W("a")
x: str = dub w.v
```

#### :dub_on_nested_field_path_is_a_copy

```dr
class Inner {
  xs: list[str]
  def() {
    self.xs = ["a"]
  }
}
class Outer {
  inner: Inner
  def() {
    self.inner = Inner()
  }
}
o: Outer = Outer()
for x in dub o.inner.xs {
  print(x)
}
```

#### :own_and_dub_on_bare_name_in_assignment_still_parse

```dr
def mk() -> str {
  return "a" + "b"
}
b: str = mk()
x: str = dub b
```

#### :own_and_dub_on_bare_name_in_assignment_still_parse_2

```dr
def mk() -> str {
  return "a" + "b"
}
b: str = mk()
x: str = own b
```

#### :reserved_word_as_declaration_name

`from` starts an import, so the parser used to read this line as a broken import
and reported the construct the programmer never wrote.

```dr
from: str = "a"
print("unused")
```

#### :reserved_word_as_assignment_target

```dr
x: int = 1
del = 2
print(x)
```

#### :reserved_word_as_parameter_name

```dr
def f(from: str) -> int {
    return 1
}
print(f("a"))
```

#### :reserved_word_as_attribute_name

```dr
class Box {
    v: int
    def () {
        self.v = 1
    }
}
b: Box = Box()
print(b.from)
```

#### :reserved_word_as_class_declaration_name

```dr
class: str = "a"
print("unused")
```

#### :reserved_word_as_name_then_a_real_error

The reserved word must not swallow the rest of the file: the missing ')' on the
last line is a separate, real parse error and has to survive.

```dr
from: str = "a"
y: int = 2
print(y
```

#### :soft_keywords_remain_ordinary_names

`case` is not a reserved word, so it keeps working as an ordinary name.

```dr
case: int = 2
print(case)
```

#### :dotted_except_type

A module-qualified exception class is a type name like any other, so the except
clause must read it whole instead of stopping at the dot.

```dr
try {
  pass
} except mod.MyErr as e {
  pass
}
```

#### :dotted_except_type_group

```dr
try {
  pass
} except (KeyError, pkg.mod.MyErr) as e {
  pass
}
```

#### :py_dotted_except_type

```py
try:
    pass
except mod.MyErr as e:
    pass
```
