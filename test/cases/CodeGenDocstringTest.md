# CodeGenDocstringTest cases

Source programs for `test/CodeGenDocstringTest.cpp`. Each block is named by the heading
above it and pulled in with `code("<name>")`.

#### :docstring_class_present

```dr
class Greeter {
    """Says hello to anyone who shows up."""
    name: str
    def(name: str) { self.name = name }
}
print(Greeter.__doc__)
```

#### :docstring_class_absent

```dr
class Plain {
    name: str
    def(name: str) { self.name = name }
}
print(Plain.__doc__)
```

#### :docstring_function_present

```dr
def factorial(n: int) -> int {
    """Compute n! recursively."""
    if n <= 1 { return 1 }
    return n * factorial(n - 1)
}
print(factorial.__doc__)
```

#### :docstring_function_absent

```dr
def bare(n: int) -> int { return n }
print(bare.__doc__)
```

#### :docstring_instance_inherits_class

```dr
class Doc {
    """Class doc."""
    x: int
    def(x: int) { self.x = x }
}
d: Doc = Doc(1)
print(d.__doc__)
```

#### :docstring_instance_none_when_absent

```dr
class NoDoc {
    x: int
    def(x: int) { self.x = x }
}
d: NoDoc = NoDoc(1)
print(d.__doc__)
```

#### :docstring_is_none_narrowing

```dr
class A { """has doc.""" x: int  def(x: int) { self.x = x } }
class B { x: int  def(x: int) { self.x = x } }
if A.__doc__ is None { print("A:none") } else { print("A:doc") }
if B.__doc__ is None { print("B:none") } else { print("B:doc") }
```

#### :docstring_multiline

```dr
def f() -> int {
    """line one
    line two"""
    return 0
}
print(f.__doc__)
```

#### :docstring_f_string_not_lifted

```dr
def f() -> int {
    name: str = "world"
    f"hello {name}"
    return 0
}
print(f.__doc__)
```

#### :docstring_not_first_stmt_is_not_lifted

```dr
def f() -> int {
    n: int = 0
    "this is not a docstring"
    return n
}
print(f.__doc__)
```

#### :docstring_absent_function_emits_no_doc_global

```dr
def bare(n: int) -> int { return n }
print(bare(1))
```

#### :docstring_absent_class_emits_null_doc_ptr

```dr
class Plain {
    x: int
    def(x: int) { self.x = x }
}
p: Plain = Plain(1)
print(p.x)
```

#### :docstring_present_class_emits_doc_global

```dr
class C {
    """has docs."""
    x: int
    def(x: int) { self.x = x }
}
c: C = C(1)
print(c.x)
```

#### :docstring_method_via_class_chain

```dr
class C {
    x: int
    def(x: int) { self.x = x }
    def greet() -> int {
        """method greeting."""
        return self.x
    }
}
print(C.greet.__doc__)
```

#### :docstring_method_via_instance_chain

```dr
class C {
    x: int
    def(x: int) { self.x = x }
    def greet() -> int {
        """method greeting."""
        return self.x
    }
}
c: C = C(1)
print(c.greet.__doc__)
```

#### :docstring_method_absent_returns_none

```dr
class C {
    x: int
    def(x: int) { self.x = x }
    def bare() -> int { return self.x }
}
print(C.bare.__doc__)
```

#### :docstring_bare_name_present

```dr
"""module-level doc."""
print(__doc__)
```

#### :docstring_bare_name_inside_function

```dr
"""mod doc."""
def f() -> int {
    """fn doc."""
    print(__doc__)
    return 0
}
f()
```
