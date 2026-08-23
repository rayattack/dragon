# ModuleInitOrderTest cases

Source programs for `test/ModuleInitOrderTest.cpp`. Each block is named by the heading
above it and pulled in with `code("<name>")`.

#### :direct_forward_const_read__class_typed

```dr
class Dep {
    v: int
    def(x: int) { self.v = x }
}
class Svc {
    d: Dep
    def(dep: Dep) { self.d = dep }
    def get() -> int { return self.d.v }
}
const S: Svc = Svc(D)
const D: Dep = Dep(42)
print(S.get())
```

#### :direct_forward_const_read__scalar

```dr
const A: int = B + 1
const B: int = 41
print(A)
```

#### :interproc_forward_const_read

```dr
def read_later() -> int { return LATER }
const A: int = read_later()
const LATER: int = 42
print(A)
```

#### :two_const_init_cycle

```dr
const X: int = Y + 1
const Y: int = X + 1
print(X)
print(Y)
```

#### :interproc_init_cycle

```dr
def f() -> int { return B }
def g() -> int { return A }
const A: int = f()
const B: int = g()
print(A)
```

#### :correct_order_const_chain__class_typed

```dr
class Dep {
    v: int
    def(x: int) { self.v = x }
}
class Svc {
    d: Dep
    def(dep: Dep) { self.d = dep }
    def get() -> int { return self.d.v }
}
const D: Dep = Dep(42)
const S: Svc = Svc(D)
print(S.get())
```

#### :correct_order_const_chain__scalar

```dr
const B: int = 41
const A: int = B + 1
print(A)
```

#### :forward_function_ref_pure_helper

```dr
const A: int = compute()
def compute() -> int { return 7 }
print(A)
```

#### :forward_function_reads_const_but_not_called_during_init

```dr
def check(n: int) -> bool { return n < LIMIT }
const LIMIT: int = 100
print(LIMIT)
```

#### :interproc_const_read_const_defined_earlier

```dr
const DATA_DIR: str = "data"
def ensure() -> str { return DATA_DIR }
const READY: str = ensure()
print(READY)
```

#### :main_at_top_calls_helpers_below

```dr
def main() -> None { helper() }
def helper() -> None { print("hi") }
main()
```
