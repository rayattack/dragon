# TypeHintEnforcerTest cases

Source programs for `test/TypeHintEnforcerTest.cpp`. Each block is named by the heading
above it and pulled in with `code("<name>")`.

#### :typed_function_passes

```dr
def add(x: int, y: int) -> int:
    return x + y
```

#### :typed_function_no_params

```dr
def greet() -> str:
    return "hello"
```

#### :typed_function_returns_none

```dr
def do_stuff(x: int) -> None:
    pass
```

#### :missing_param_type

```dr
def add(x, y: int) -> int:
    return x + y
```

#### :missing_param_type_2

```dr
def add(x, y: int) -> int:
    return x + y
```

#### :all_params_missing_types

```dr
def process(a, b, c) -> int:
    return 0
```

#### :missing_return_type

```dr
def add(x: int, y: int):
    return x + y
```

#### :missing_return_type_2

```dr
def add(x: int, y: int):
    return x + y
```

#### :init_no_return_type_ok

```dr
class Foo:
    def __init__(self, x: int):
        pass
```

#### :method_self_exempt

```dr
class Foo:
    def bar(self, x: int) -> int:
        return x
```

#### :class_method_cls_exempt

```dr
class Foo:
    def create(cls, name: str) -> str:
        return name
```

#### :method_non_self_param_missing_type

```dr
class Foo:
    def bar(self, x) -> int:
        return 0
```

#### :disable_param_type_check

```dr
def add(x, y) -> int:
    return 0
```

#### :disable_return_type_check

```dr
def add(x: int, y: int):
    return x + y
```

#### :multiple_errors

```dr
x = 5
def foo(a, b):
    return 0
```

#### :dragon_file_with_types_ok

```dr
def add(x: int, y: int) -> int {
    return x + y
}
```

#### :mixed_functions

```dr
def typed(x: int) -> int:
    return x
def untyped(y):
    return y
```
