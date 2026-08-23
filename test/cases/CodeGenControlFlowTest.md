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
