# SemaTest cases

Source programs for `test/SemaTest.cpp`. Each block is named by the heading
above it and pulled in with `code("<name>")`.

#### :function_defines_name

```dr
def foo() {
  pass
}
foo()
```

#### :function_params

```dr
def add(x: int, y: int) -> int {
  return x + y
}
```

#### :function_param_not_in_outer_scope

```dr
def foo(x: int) {
  pass
}
print(x)
```

#### :class_defines_name

```dr
class Foo {
  pass
}
Foo()
```

#### :for_loop_defines_variable

```dr
for i in range(10) {
  print(i)
}
```

#### :import_defines_name

```dr
import os
print(os)
```

#### :from_import_defines_name

```dr
from os import path
print(path)
```

#### :import_alias

```dr
import numpy as np
print(np)
```

#### :global_statement

```dr
global x
print(x)
```

#### :break_inside_loop

```dr
while True {
  break
}
```

#### :continue_inside_loop

```dr
while True {
  continue
}
```

#### :return_inside_function

```dr
def foo() {
  return 42
}
```

#### :break_in_for

```dr
for i in range(10) {
  if i == 5 {
    break
  }
}
```

#### :nested_loop_break

```dr
while True {
  for i in range(10) {
    break
  }
  break
}
```

#### :try_catch_defines_handler_var

```dr
try {
  pass
} catch ValueError as e {
  print(e)
}
```

#### :with_statement_defines_var

```dr
class Ctx {
  def __enter__() -> Ctx { return self }
  def __exit__() -> int { return 0 }
}
with Ctx() as f {
  print(f)
}
```

#### :class_with_method_using_self

```dr
class Foo {
  def bar() {
    print(self)
  }
}
```

#### :implicit_self_resolves_in_body

```dr
class Point {
  def move(dx: int) {
    self.x = dx
  }
}
```

#### :implicit_self_not_in_params

```dr
class Point {
  def move(dx: int) {
    self.x = dx
  }
}
```

#### :function_calling_function

```dr
def add(a: int, b: int) -> int {
  return a + b
}
def main() {
  x: int = add(1, 2)
  print(x)
}
```

#### :assignment_in_condition

```dr
x: int = 10
if x {
  print(x)
}
```

#### :def_ctor_ok

```dr
class Foo {
  def(x: int) {
    self.x = x
  }
}
```

#### :match_capture_in_or_sub_pattern

```dr
x: int = 5
match x {
  case 1 | n { print(n) }
}
```

#### :module_level_defer_rejected

```dr
def f() -> None { pass }
defer f()
```

#### :defer_inside_function_accepted

```dr
def f() -> None { pass }
def g() -> None {
    defer f()
}
```

#### :except_as_target_is_handler_local

```dr
try {
    raise ValueError("x")
} except ValueError as ex {
    print(ex)
}
print(ex)
```

#### :except_as_target_is_handler_local_2

```dr
e: str = "outer"
try {
    raise ValueError("x")
} except ValueError as e {
    print(e)
}
print(e)
```

#### :template_interp_undefined_name

A name inside a `!{...}` interpolation is resolved like any other name, so
`dragon check` rejects it instead of deferring the failure to codegen.

```dr
greeting: str = template {Hello !{missing_name}}
print(greeting)
```

#### :template_interp_undefined_call

```dr
out: str = template {Total: !{missing_fn()}}
print(out)
```

#### :template_block_undefined_iterable

The iterable of a `for` block inside a template is resolved too.

```dr
out: str = template {!{ for x in missing_items { :{ item } } }}
print(out)
```

#### :template_block_binds_loop_variable

A loop variable declared by a template block is in scope for the interpolations
nested inside that block, so this must resolve cleanly.

```dr
def render(items: list[str], title: str) -> str {
    return template {<h1>!{title}</h1>!{ for it in items { :{ <li>!{it}</li> } } }}
}
print(render(["a"], "T"))
```

#### :bare_dict_key_shadowed_by_const

`{ K_A: 1 }` keys the dict on the text "K_A", not on the value of the constant
`K_A`. Both readings are plausible while a binding of that name is visible, so
the program must say which one it means.

```dr
const K_A: str = "view.load"
literal: dict[str, int] = { K_A: 1 }
print(len(literal))
```

#### :bare_dict_key_with_no_binding_is_text

```dr
literal: dict[str, int] = { view_load: 1, other: 2 }
print(len(literal))
```

#### :bare_dict_key_parenthesised_is_the_value

```dr
const K_A: str = "view.load"
computed: dict[str, int] = { (K_A): 1 }
print(len(computed))
```

#### :bare_dict_key_shadowed_by_function

```dr
def handler() -> int {
    return 1
}
routes: dict[str, int] = { handler: 1 }
print(len(routes))
```

#### :bare_dict_key_shadowed_by_class

```dr
class Widget {
    size: int
}
kinds: dict[str, int] = { Widget: 1 }
print(len(kinds))
```

#### :bare_dict_key_shadowed_by_parameter

```dr
def index(label: str) -> int {
    counts: dict[str, int] = { label: 1 }
    return len(counts)
}
print(index("a"))
```

#### :bare_dict_key_shadowed_in_nested_dict

```dr
const K_A: str = "view.load"
nested: dict[str, dict[str, int]] = { outer: { K_A: 1 } }
print(len(nested))
```

#### :bare_dict_key_shadowed_in_default_argument

```dr
const K_A: str = "view.load"
def index(counts: dict[str, int] = { K_A: 1 }) -> int {
    return len(counts)
}
print(index())
```

#### :bare_dict_key_shadowed_by_imported_name

```dr
from json import dumps
payload: dict[str, int] = { dumps: 1 }
print(len(payload))
```

#### :bare_dict_key_named_like_a_class_field_is_text

A class field is not in scope as a bare name inside a method, so a key spelled
like one is unambiguously text.

```dr
class Row {
    label: str
    def () {
        self.label = "a"
    }
    def counts() -> int {
        seen: dict[str, int] = { label: 1 }
        return len(seen)
    }
}
r: Row = Row()
print(r.counts())
```

#### :bare_dict_key_shadowed_in_typeddict_literal

```dr
class Point(TypedDict) {
    id: int
    score: float
}
const score: str = "s"
p: Point = Point({id: 1, score: 2.5})
print(p.id)
```

#### :bare_dict_key_in_typeddict_literal_with_no_binding

```dr
class Point(TypedDict) {
    id: int
    score: float
}
p: Point = Point({id: 1, score: 2.5})
print(p.id)
```
