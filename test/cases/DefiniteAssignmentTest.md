# DefiniteAssignmentTest cases

Source programs for `test/DefiniteAssignmentTest.cpp`. Each block is named by the heading
above it and pulled in with `code("<name>")`.

#### :assigned_in_both_branches_ok

```dr
def f(c: bool) -> int {
    x: int
    if c { x = 1 } else { x = 2 }
    return x
}
```

#### :assigned_in_only_one_branch_errors

```dr
def f(c: bool) -> int {
    x: int
    if c { x = 1 }
    return x
}
```

#### :else_returns_guard_ok

```dr
def f(c: bool) -> int {
    x: int
    if c { x = 1 } else { return 0 }
    return x
}
```

#### :while_true_break_assigns_ok

```dr
def f() -> int {
    x: int
    while True { x = 42 break }
    return x
}
```

#### :general_while_may_not_run_errors

```dr
def f(c: bool) -> int {
    x: int
    while c { x = 1 }
    return x
}
```

#### :aug_assign_on_unassigned_errors

```dr
def f() -> int {
    x: int
    x += 1
    return x
}
```

#### :parameter_is_assigned_ok

```dr
def f(x: int) -> int {
    return x
}
```

#### :global_declared_name_ok

```dr
g: int = 0
def f() -> int {
    return g
}
```

#### :ctor_forgets_field_errors

```dr
class C {
    x: int
    items: list[int]
    def(x: int) { self.x = x }
}
```

#### :ctor_assigns_all_fields_ok

```dr
class C {
    x: int
    y: int
    def(x: int, y: int) { self.x = x self.y = y }
}
```

#### :field_with_default_ok

```dr
class C {
    x: int
    items: list[int] = []
    def(x: int) { self.x = x }
}
```

#### :field_assigned_in_one_ctor_branch_errors

```dr
class C {
    x: int
    items: list[int]
    def(x: int) {
        self.x = x
        if x > 0 { self.items = [1] }
    }
}
```

#### :deferred_init_via_method_ok

```dr
class Engine {
    n: int
    def(n: int) { self.n = n }
}
class C {
    x: int
    engine: Engine
    def(x: int) { self.x = x }
    def install() { self.engine = Engine(8) }
}
```

#### :ctor_assigns_via_self_helper_ok

```dr
class C {
    x: int
    items: list[int]
    def(x: int) { self.x = x self._setup() }
    def _setup() { self.items = [1] }
}
```

#### :static_field_not_required_ok

```dr
class C {
    static count: int = 0
    x: int
    def(x: int) { self.x = x }
}
```

#### :early_raise_before_assign_ok

```dr
class C {
    x: int
    def(x: int) {
        if x < 0 { raise ValueError("bad") }
        self.x = x
    }
}
```
