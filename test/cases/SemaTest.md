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
