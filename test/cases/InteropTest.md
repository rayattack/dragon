# InteropTest cases

Source programs for `test/InteropTest.cpp`. Each block is named by the heading
above it and pulled in with `code("<name>")`.

#### :dragon_imports_dragon

```dr
def double_it(x: int) -> int {
    return x * 2
}
```

#### :cross_module_property_getter_setter

```dr
class Box {
    _v: int = 0
    @property
    def val() -> int { return self._v }
    @val.setter
    def val(n: int) -> None { self._v = n }
    @property
    def twin() -> Box { return self }
    def __str__() -> str { return "box" }
}
```

#### :imported_function_docstring

```dr
"""library doc."""
def hello() -> str {
    """library hello."""
    return "hi"
}
```

#### :imported_module_docstring

```dr
"""library module docstring."""
def hi() -> int { return 0 }
```

#### :dragon_imports_typed_python

```dr
def square(x: int) -> int:
    return x * x
```

#### :diamond_import_compiles

```dr
def base_val() -> int {
    return 10
}
```

#### :diamond_import_compiles_2

```dr
from base import base_val
def left_val() -> int {
    return base_val() + 1
}
```

#### :diamond_import_compiles_3

```dr
from base import base_val
def right_val() -> int {
    return base_val() + 2
}
```

#### :untyped_py_import_rejected

```dr
def process(data):
    return data
```

#### :string_concat_across_modules

```dr
def greet(name: str) -> str {
    return "Hello, " + name
}
```

#### :function_name_across_modules

```dr
def double(x: int) -> int {
    return x * 2
}
```

#### :class_name_across_modules

```dr
class Conflict {
    def(x: int) {
        self.x = x
    }
    def value() -> int {
        return self.x
    }
}
```

#### :class_name_across_modules_2

```dr
class Conflict {
    def(x: int) {
        self.x = x
    }
    def value() -> int {
        return self.x
    }
}
```

#### :cross_module_global_dict

```dr
def _build() -> dict[str, str] {
    m: dict[str, str] = {}
    m["a"] = "alpha"
    m["b"] = "beta"
    return m
}

const TBL: dict[str, str] = _build()

def lookup(k: str) -> str {
    if k in TBL {
        return TBL[k]
    }
    return "missing"
}
```

#### :cross_module_global_list

```dr
def _build() -> list[int] {
    xs: list[int] = []
    xs.append(10)
    xs.append(20)
    xs.append(30)
    return xs
}

const NUMS: list[int] = _build()

def total() -> int {
    s: int = 0
    for n in NUMS {
        s = s + n
    }
    return s
}
```

#### :imported_function_as_value

```dr
def double_it(x: int) -> int {
    return x * 2
}
```

#### :from_package_import_submodule_attr_as_value

```dr
def health_check() -> int {
    return 200
}
```

#### :import_package_dot_submodule_attr_as_value

```dr
def health_check() -> int {
    return 201
}
```

#### :from_package_import_submodule_attr_direct_call

```dr
def health_check() -> int {
    return 202
}
```

#### :import_package_dot_submodule_attr_direct_call

```dr
def health_check() -> int {
    return 203
}
```

#### :d045__cross_package_protected_import_rejected

```dr
_secret: int = 42
public_val: int = 7
```

#### :d045__cross_package_protected_import_rejected_2

```dr
from lib import _secret, public_val
print(_secret)
```

#### :d045__cross_package_public_import_allowed

```dr
def public_add(a: int, b: int) -> int {
    return a + b
}
```

#### :d045__cross_package_public_import_allowed_2

```dr
from lib import public_add
print(public_add(2, 3))
```

#### :d045__cross_package_file_private_import_rejected

```dr
__hidden: int = 99
public_val: int = 1
```

#### :d045__cross_package_file_private_import_rejected_2

```dr
from lib import __hidden
print(__hidden)
```

#### :d045__cross_package_qualified_protected_rejected

```dr
_secret: int = 42
public_val: int = 7
```

#### :d045__cross_package_qualified_protected_rejected_2

```dr
import lib
print(lib._secret)
```

#### :d045__same_package_protected_import_allowed

```dr
from app.internal import _shared
print(_shared)
```
