# CodeGenAssignTest cases

Source programs for `test/CodeGenAssignTest.cpp`. Each block is named by the heading
above it and pulled in with `code("<name>")`.

#### :reassign_assign_emits_overwrite_decref

```dr
def reassign_assign() {
  x: str = "a".upper()
  x = "b".upper()
}
```

#### :reassign_walrus_emits_overwrite_decref

```dr
def reassign_walrus() {
  x: str = "a".upper()
  if (x := "b".upper()) == "B" {
    pass
  }
}
```

#### :str_aug_assign_emits_inplace_append

```dr
def reassign_augassign() {
  s: str = "a".upper()
  s += "b".upper()
}
```

#### :aug_assign_all

```dr
x: int = 17
x //= 3
print(x)
y: int = 17
y %= 5
print(y)
z: int = 2
z **= 10
print(z)
```

#### :str_append_inplace_loop

```dr
s: str = ""
i: int = 0
while i < 10000 {
    s = s + "hello"
    i = i + 1
}
print(len(s))
```

#### :str_plus_eq_loop

```dr
s: str = ""
i: int = 0
while i < 10000 {
    s += "hello"
    i = i + 1
}
print(len(s))
```

#### :str_append_aliasing_safe

```dr
s: str = "hello"
t: str = s
s = s + " world"
print(t)
print(s)
```

#### :str_append_empty_accumulator

```dr
s: str = ""
s = s + "first"
print(s)
print(len(s))
```

#### :str_append_kind4_fallback

```dr
a: str = "abc"
a = a + "é"
print(a)
print(len(a))
```

#### :str_append_module_global

```dr
g: str = "start"
def build() {
    global g
    g = g + "-mid"
    g += "-end"
}
build()
print(g)
```

#### :str_self_reassign_emits_append_inplace

```dr
s: str = ""
i: int = 0
while i < 10 {
    s = s + "x"
    i = i + 1
}
print(len(s))
```

#### :dict_str_int_aug_assign

```dr
d: dict[str, int] = {}
d["a"] = 10
d["a"] += 5
d["a"] -= 3
d["a"] *= 4
d["a"] %= 7
print(d["a"])
```

#### :dict_aug_assign_floor_mod

```dr
d: dict[str, int] = {}
d["k"] = -7
d["k"] %= 3
print(d["k"])
```

#### :dict_str_float_aug_assign

```dr
f: dict[str, float] = {}
f["x"] = 2.5
f["x"] += 1.5
f["x"] *= 2.0
print(f["x"])
```

#### :dict_str_int_aug_emits_fused_probe

```dr
d: dict[str, int] = {}
d["a"] = 1
d["a"] += 5
```

#### :list_int_aug_assign

```dr
a: list[int] = [10, 20, 30]
a[1] += 5
a[0] -= 4
a[2] *= 3
a[-1] += 100
print(a[0])
print(a[1])
print(a[2])
```

#### :list_float_aug_assign

```dr
f: list[float] = [1.0, 2.0]
f[0] += 0.5
f[1] *= 2.5
print(f[0])
print(f[1])
```

#### :attribute_aug_assign

```dr
class Counter {
    n: int
    total: float
    def() {
        self.n = 10
        self.total = 2.5
    }
    def bump() {
        self.n += 5
        self.total += 1.5
    }
}
c: Counter = Counter()
c.n += 5
c.bump()
c.n *= 2
c.n -= 1
print(c.n)
print(c.total)
```
