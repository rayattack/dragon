# CodeGenControlFlowTest cases

Source programs for `test/CodeGenControlFlowTest.cpp`. Each block is named by the heading
above it and pulled in with `code("<name>")`.

#### :if_else_statement

```dr
x: int = 5
if x > 10 {
  print(1)
} else {
  print(2)
}
```

#### :elif_float_condition_coercion

```dr
x: int = 0
y: float = 1.0
if x {
  print(1)
} elif y {
  print(2)
}
```

#### :else_if_chain_same_as_elif

```dr
def cls(x: int) -> str {
    if x > 100 {
        return "huge"
    } else if x > 50 {
        return "big"
    } elif x > 10 {
        return "medium"
    } else if x > 0 {
        return "tiny"
    } else {
        return "zero"
    }
}
print(cls(200))
print(cls(75))
print(cls(25))
print(cls(5))
print(cls(0))
```

#### :for_in_dict_keys

```dr
d: dict[str, int] = {"a": 1, "b": 2}
for k in d {
  print(k)
}
```

#### :for_in_dict_items

```dr
d: dict[str, int] = {"a": 1}
for k, v in d.items() {
  print(k)
}
```

#### :for_range_three_args

```dr
for i in range(0, 10, 3) {
  print(i)
}
```

#### :break_continue

```dr
for i in range(10) {
  if i == 3 {
    continue
  }
  if i == 6 {
    break
  }
  print(i)
}
```

#### :nested_loops

```dr
for i in range(3) {
  for j in range(3) {
    if i == j {
      print(i)
    }
  }
}
```

#### :assert_pass

```dr
assert True
assert 1 + 1 == 2
print("ok")
```

#### :for_in_dict

```dr
d: dict[str, int] = {"x": 10, "y": 20}
for k in d {
  print(k)
}
```

#### :for_in_dict_keys_2

```dr
d: dict[str, int] = {"a": 1, "b": 2}
for k in d.keys() {
  print(k)
}
```

#### :for_in_dict_items_2

```dr
d: dict[str, int] = {"a": 1, "b": 2}
for k, v in d.items() {
  print(k)
  print(v)
}
```

#### :for_in_dict_values

```dr
d: dict[str, int] = {"x": 100, "y": 200}
for v in d.values() {
  print(v)
}
```

#### :delete_stmt_basic

```dr
x: int = 42
del x
print("ok")
```

#### :delete_stmt_string

```dr
s: str = "hello" + " world"
del s
print("ok")
```

#### :delete_stmt_ir

```dr
s: str = "hello" + " world"
del s
```

#### :vectorize_int_sum

```dr
def total(xs: list[int]) -> int {
    acc: int = 0
    for i in range(len(xs)) {
        acc += xs[i]
    }
    return acc
}

print(total([1, 2, 3]))
```

#### :vectorize_float_map

```dr
def double_in_place(xs: list[float]) -> None {
    for i in range(len(xs)) {
        xs[i] = xs[i] * 2.0
    }
}

xs: list[float] = [1.0, 2.0, 3.0]
double_in_place(xs)
print(xs[2])
```

Float addition is not associative, so a strict float reduction must stay scalar
without the explicit opt-in.

#### :vectorize_float_sum_strict

```dr
def total(xs: list[float]) -> float {
    acc: float = 0.0
    for i in range(len(xs)) {
        acc += xs[i]
    }
    return acc
}

print(total([1.0, 2.0, 3.0]))
```

The Pythonic `for x in xs` must lower to the same inline shape as the indexed
loop so it vectorizes the same way.

#### :vectorize_int_sum_foreach

```dr
def total(xs: list[int]) -> int {
    acc: int = 0
    for x in xs {
        acc += x
    }
    return acc
}

print(total([1, 2, 3]))
```

#### :vectorize_float_sum_foreach_strict

```dr
def total(xs: list[float]) -> float {
    acc: float = 0.0
    for x in xs {
        acc += x
    }
    return acc
}

print(total([1.0, 2.0, 3.0]))
```

`@fastmath` is the explicit opt-in that lets the compiler reassociate the
float additions, so the same reduction vectorizes.

#### :vectorize_float_sum_fastmath

```dr
@fastmath
def total(xs: list[float]) -> float {
    acc: float = 0.0
    for i in range(len(xs)) {
        acc += xs[i]
    }
    return acc
}

print(total([1.0, 2.0, 3.0]))
```

#### :vectorize_float_sum_foreach_fastmath

```dr
@fastmath
def total(xs: list[float]) -> float {
    acc: float = 0.0
    for x in xs {
        acc += x
    }
    return acc
}

print(total([1.0, 2.0, 3.0]))
```

The blocking instruction is often not one the loop wrote. An inlined helper
carries its own file and line, so the report has to name the call site inside
the loop and the line the instruction really came from.

#### :vectorize_inlined_helper_call

```dr
def collect(out: list[int], v: int) -> None {
    out.append(v * 3)
}

def build(n: int) -> list[int] {
    out: list[int] = []
    for i in range(n) {
        collect(out, i)
    }
    return out
}

def run(seed: int) -> int {
    return len(build(seed))
}

print(run(120))
```

The same shape across two modules is what a real program looks like, and there
a bare line number names a line in the wrong file.

#### :vectorize_helper_module

```dr
def collect(out: list[int], v: int) -> None {
    out.append(v * 3)
}
```

#### :vectorize_imported_helper_call

```dr
from dragon_vectorize_helper import collect

def build(n: int) -> list[int] {
    out: list[int] = []
    for i in range(n) {
        collect(out, i)
    }
    return out
}

def run(seed: int) -> int {
    return len(build(seed))
}

print(run(120))
```
