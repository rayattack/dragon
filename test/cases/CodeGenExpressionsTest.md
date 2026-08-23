# CodeGenExpressionsTest cases

Source programs for `test/CodeGenExpressionsTest.cpp`. Each block is named by the heading
above it and pulled in with `code("<name>")`.

#### :chained_comp_int_less_less

```dr
x: int = 5
y: bool = 1 < x < 10
```

#### :chained_comp_three_operands

```dr
a: int = 1
b: int = 2
c: int = 3
r: bool = a < b < c
```

#### :chained_comp_two_operands

```dr
a: int = 1
b: int = 2
r: bool = a < b
```

#### :walrus_basic_int

```dr
x: int = 0
y: int = (x := 42)
```

#### :arithmetic

```dr
x: int = 10
y: int = 20
print(x + y)
print(x * y)
```

#### :string_repeat

```dr
n: int = 4
print("*" * 3)
print(3 * "*")
print("ab" * n)
print(n * "-")
print("[" + "x" * 0 + "]")
print("[" + "x" * -5 + "]")
print(("a" + "b") * 2)
```

#### :string_repeat_augmented

```dr
s: str = "yo"
s *= 3
print(s)
```

#### :if_else_chain

```dr
x: int = 15
if x > 20 {
  print(1)
} elif x > 10 {
  print(2)
} else {
  print(3)
}
```

#### :while_loop

```dr
x: int = 0
total: int = 0
while x < 5 {
  total += x
  x += 1
}
print(total)
```

#### :if_float_truthiness

```dr
x: float = 1.5
if x {
  print("truthy")
} else {
  print("falsy")
}
```

#### :if_float_zero_falsy

```dr
x: float = 0.0
if x {
  print("truthy")
} else {
  print("falsy")
}
```

#### :elif_float_condition

```dr
x: int = 0
y: float = 3.14
if x {
  print("x")
} elif y {
  print("y")
} else {
  print("none")
}
```

#### :while_float_condition

```dr
x: float = 3.0
while x {
  print(x)
  x = x - 1.0
}
```

#### :ternary_expression

```dr
x: int = 10
y: int = 1 if x > 5 else 0
print(y)
z: int = 1 if x > 20 else 0
print(z)
```

#### :chained_comp_int_all_true

```dr
x: int = 5
if 1 < x < 10 {
    print(1)
} else {
    print(0)
}
```

#### :chained_comp_int_false_first

```dr
x: int = 15
if 1 < x < 10 {
    print(1)
} else {
    print(0)
}
```

#### :chained_comp_int_false_second

```dr
x: int = 0
if 1 < x < 10 {
    print(1)
} else {
    print(0)
}
```

#### :chained_comp_less_equal

```dr
x: int = 5
if 0 <= x <= 10 {
    print(1)
} else {
    print(0)
}
```

#### :chained_comp_equal_equal

```dr
x: int = 5
if 5 == x == 5 {
    print(1)
} else {
    print(0)
}
```

#### :chained_comp_four_operands

```dr
if 1 < 2 < 3 < 4 {
    print(1)
} else {
    print(0)
}
```

#### :chained_comp_four_operands_fail

```dr
if 1 < 2 < 3 < 2 {
    print(1)
} else {
    print(0)
}
```

#### :chained_comp_mixed_ops

```dr
x: int = 5
if 0 < x <= 5 {
    print(1)
} else {
    print(0)
}
```

#### :chained_comp_middle_evaluated_once

```dr
counter: int = 0
def mid() -> int {
    global counter
    counter = counter + 1
    return 5
}
if 1 < mid() < 10 {
    print("ok")
}
print(counter)
```

#### :chained_comp_short_circuits_middle_not_reevaluated

```dr
counter: int = 0
def mid() -> int {
    global counter
    counter = counter + 1
    return 0
}
if 1 < mid() < 10 {
    print("yes")
} else {
    print("no")
}
print(counter)
```

#### :and_or_short_circuit_skips_unsafe_rhs

```dr
def access_first(xs: list[int]) -> bool {
    return len(xs) > 0 and xs[0] > 0
}
def divide_safe(x: int, y: int) -> bool {
    return y != 0 and (x // y) > 0
}
def or_short(x: int) -> bool {
    return x > 100 or (1000000 // (x - x)) > 0
}
print(access_first([]))
print(access_first([5]))
print(access_first([-1]))
print(divide_safe(10, 2))
print(divide_safe(10, 0))
print(or_short(200))
```

#### :bool_assign_from_i64_returning_expr

```dr
def flag(name: str, ty: str) -> bool {
    const opt: bool = name.startswith("-")
    const ck: bool = ty == "bool"
    const f: bool = opt and ck
    return f
}
print(flag("--debug", "bool"))
print(flag("port",     "bool"))
print(flag("--debug", "int"))
```

#### :walrus_assign_and_use

```dr
y: int = (x := 42)
print(x)
print(y)
```

#### :walrus_in_if_condition

```dr
x: int = 10
if (n := x) > 5 {
    print(n)
} else {
    print(0)
}
```

#### :overflow_off_silently_wraps

```dr
a: int = 9000000000000000000
b: int = a + a
print(b)
```

#### :overflow_add_caught

```dr
a: int = 9000000000000000000
try {
    b: int = a + a
    print(b)
} except OverflowError {
    print("caught")
}
```

#### :overflow_mul_caught

```dr
a: int = 9000000000000000000
try {
    b: int = a * 3
    print(b)
} except OverflowError {
    print("caught")
}
```

#### :overflow_pow_caught

```dr
b: int = 2
e: int = 100
try {
    c: int = b ** e
    print(c)
} except OverflowError {
    print("caught")
}
```

#### :overflow_sub_caught

```dr
a: int = -9000000000000000000
b: int = 1000000000000000000
try {
    c: int = a - b
    print(c)
} except OverflowError {
    print("caught")
}
```

#### :overflow_normal_arith_unaffected

```dr
a: int = 100
b: int = 200
print(a + b)
print(a * b)
print(a - b)
print(2 ** 10)
```

#### :overflow_caught_by_arithmetic_error_parent

```dr
a: int = 9000000000000000000
try {
    b: int = a + a
    print(b)
} except ArithmeticError {
    print("caught_arith")
}
```

#### :ternary_class_field_dict_subscript_str_then_branch

```dr
class Req {
    def() { self.params: dict[str, str] = {} }
}
def serve(req: Req) -> None {
    const has: bool = "*" in req.params
    const wc: str = req.params["*"] if has else "fallback"
    print(wc)
}
serve(Req())
```

#### :ternary_class_field_dict_subscript_str_else_branch

```dr
class Req {
    def() { self.params: dict[str, str] = {} }
}
def serve(req: Req) -> None {
    const has: bool = "*" in req.params
    const wc: str = "default" if not has else req.params["*"]
    print(wc)
}
serve(Req())
```

#### :ternary_class_field_dict_subscript_str_both_branches

```dr
class Req {
    def() {
        self.params: dict[str, str] = {"x": "FROM_PARAMS"}
        self.headers: dict[str, str] = {"y": "FROM_HEADERS"}
    }
}
def serve(req: Req, pick_p: bool) -> None {
    const v: str = req.params["x"] if pick_p else req.headers["y"]
    print(v)
}
serve(Req(), True)
serve(Req(), False)
```

#### :ternary_class_field_dict_subscript_int_value

```dr
class Req {
    def() { self.counts: dict[str, int] = {} }
}
def serve(req: Req) -> None {
    const has: bool = "x" in req.counts
    const v: int = req.counts["x"] if has else 0
    print(v)
}
serve(Req())
```

#### :ternary_class_field_dict_subscript_float_value

```dr
class Cfg {
    def() { self.tunables: dict[str, float] = {} }
}
def lookup(c: Cfg) -> None {
    const has: bool = "rate" in c.tunables
    const v: float = c.tunables["rate"] if has else 1.5
    print(v)
}
lookup(Cfg())
```

#### :ternary_class_field_dict_subscript_instance_value

```dr
class Item {
    def(n: str) { self.name = n }
}
class Reg {
    def() { self.items: dict[str, Item] = {} }
}
def lookup(r: Reg, fallback: Item) -> None {
    const has: bool = "k" in r.items
    const it: Item = r.items["k"] if has else fallback
    print(it.name)
}
lookup(Reg(), Item("miss"))
```

#### :ternary_list_subscript_refcount_no_caller_alias

```dr
def f(types: list[str]) -> dict[str, str] {
    const t: str = types[0] if 0 < len(types) else "str"
    print(f"  t=[{t}]")
    const params: dict[str, str] = {}
    params["x"] = "y"
    return params
}
const types: list[str] = ["int"]
const a: dict[str, str] = f(types)
print(f"after a: types[0]=[{types[0]}]")
const b: dict[str, str] = f(types)
print(f"after b: types[0]=[{types[0]}]")
```

#### :ternary_local_dict_subscript_refcount_no_alias

```dr
def f(h: dict[str, str]) -> None {
    if true {
        const got: str = h["host"] if "host" in h else "none"
        print(f"  got=[{got}]")
    }
    const filler: dict[str, str] = {}
    filler["k"] = "REUSED!!"
}
const h: dict[str, str] = {}
h["host"] = "ORIGINAL"
f(h)
const after: str = h["host"]
print(f"after=[{after}]")
```

#### :in_op_class_field_dict_membership

```dr
class Req {
    def() { self.params: dict[str, str] = {"x": "FOUND"} }
}
def main() -> None {
    const r: Req = Req()
    print("x" in r.params)
    print("y" in r.params)
}
main()
```

#### :in_op_class_field_dict_bare_keys

```dr
class Req {
    def() { self.params: dict[str, str] = {x: "FOUND"} }
}
def main() -> None {
    const r: Req = Req()
    print("x" in r.params)
    print("y" in r.params)
}
main()
```

#### :in_op_self_field_dict_membership

```dr
class Store {
    def() { self.data: dict[str, str] = {"a": "1"} }
    def has(k: str) -> bool {
        return k in self.data
    }
}
const s: Store = Store()
print(s.has("a"))
print(s.has("b"))
```

#### :nested_def_captures_via_ternary

```dr
def outer() -> None {
    const base: str = "BASE"
    def inner(flag: bool) -> None {
        const v: str = base if flag else "alt"
        print(v)
    }
    inner(True)
    inner(False)
}
outer()
```

#### :nested_def_captures_via_f_string

```dr
def outer() -> None {
    const base: str = "BASE"
    def inner() -> None {
        const s: str = f"prefix={base}"
        print(s)
    }
    inner()
}
outer()
```

#### :nested_def_captures_via_subscript

```dr
class Bag {
    def() { self.kv: dict[str, str] = {"k": "V"} }
}
def outer() -> None {
    const base: str = "k"
    def inner(b: Bag) -> None {
        const v: str = b.kv[base]
        print(v)
    }
    inner(Bag())
}
outer()
```

#### :nested_def_captures_via_ternary_and_f_string

```dr
class Req {
    def() { self.params: dict[str, str] = {"*": "sub/path"} }
}
class App {
    def() {}
    def assets(folder: str) -> None {
        const base: str = folder
        def serve(req: Req) -> None {
            const wc: str = req.params["*"] if "*" in req.params else ""
            const fp: str = f"{base}/{wc}"
            print(fp)
        }
        serve(Req())
    }
}
const a: App = App()
a.assets("public")
```

#### :nested_def_captures_via_ternary_and_f_string_empty_dict

```dr
class Req {
    def() { self.params: dict[str, str] = {} }
}
class App {
    def() {}
    def assets(folder: str) -> None {
        const base: str = folder
        def serve(req: Req) -> None {
            const wc: str = req.params["*"] if "*" in req.params else ""
            const fp: str = f"{base}/{wc}"
            print(fp)
        }
        serve(Req())
    }
}
const a: App = App()
a.assets("public")
```

#### :nested_def_dict_membership_from_init_param

```dr
class Req {
    def(params: dict[str, str]) { self.params = params }
}
def outer() -> None {
    def serve(req: Req) -> None {
        const wc: str = req.params["*"] if "*" in req.params else "miss"
        print(wc)
    }
    serve(Req({"*": "hit"}))
    serve(Req({}))
}
outer()
```

#### :str_find_rfind_count_start_end

```dr
const s: str = "abcabcabc"
print(s.find("b"))
print(s.find("b", 2))
print(s.find("b", 5))
print(s.find("b", 5, 7))
print(s.find("b", 5, 8))
print(s.find("a", 0, 0))
print(s.find("z"))
print(s.rfind("b"))
print(s.rfind("b", 0, 5))
print(s.rfind("b", 0, 2))
print(s.count("b"))
print(s.count("b", 2))
print(s.count("b", 2, 5))
print(s.count("b", 0, 0))
```

#### :non_ascii_literal_concat_with_computed

```dr
def make_dash() -> str { return "—" }
const a: str = make_dash()
const b: str = " — Dragon"
print(a + b)
```

#### :non_ascii_f_string_literal_segment

```dr
const t: str = "—"
print(f"x — {t}")
```

#### :non_ascii_template_literal_segment

```dr
def render(t: str) -> str {
    return template { <h1>!{t} — Dragon</h1> }
}
const t: str = "Title"
print(render(t))
```

#### :nonlocal_str_mutation

```dr
def outer() -> str {
    s: str = "a"
    def inner() -> None {
        nonlocal s
        s = s + "b"
    }
    inner()
    return s
}
print(outer())
```

#### :nonlocal_int_counter_across_calls

```dr
def counter() -> int {
    n: int = 0
    def bump() -> None {
        nonlocal n
        n = n + 1
    }
    bump()
    bump()
    bump()
    return n
}
print(counter())
```

#### :nonlocal_list_append_through_closure

```dr
def collect() -> list[str] {
    items: list[str] = []
    def push(s: str) -> None {
        nonlocal items
        items.append(s)
    }
    push("x")
    push("y")
    push("z")
    return items
}
const r: list[str] = collect()
print(r[0])
print(r[1])
print(r[2])
```

#### :nonlocal_multi_level_transitive_capture

```dr
def grandparent() -> str {
    msg: str = "hi"
    def parent() -> None {
        def child() -> None {
            nonlocal msg
            msg = msg + "!"
        }
        child()
    }
    parent()
    return msg
}
print(grandparent())
```

#### :nonlocal_reads_chain_after_mutation

```dr
def driver() -> None {
    n: int = 10
    def bump() -> None {
        nonlocal n
        n = n * 2
    }
    print(n)
    bump()
    print(n)
    bump()
    print(n)
}
driver()
```
