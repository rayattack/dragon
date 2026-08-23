# CodeGenComprehensionsTest cases

Source programs for `test/CodeGenComprehensionsTest.cpp`. Each block is named by the heading
above it and pulled in with `code("<name>")`.

#### :for_in_and_list_comp

```dr
nums: list[int] = [10, 20, 30]
for x in nums {
  print(x)
}
doubled: list[int] = [i * 2 for i in range(3)]
print(doubled)
```

#### :list_comp_over_list

```dr
nums: list[int] = [1, 2, 3, 4, 5]
doubled: list[int] = [x * 2 for x in nums]
print(len(doubled))
print(doubled[0])
print(doubled[4])
```

#### :list_comp_over_list_with_filter

```dr
nums: list[int] = [1, 2, 3, 4, 5, 6]
evens: list[int] = [x for x in nums if x % 2 == 0]
print(len(evens))
print(evens[0])
print(evens[1])
print(evens[2])
```

#### :set_comp_over_range

```dr
s: set[int] = {x * x for x in range(5)}
print(len(s))
```

#### :set_comp_over_range_with_filter

```dr
s: set[int] = {x for x in range(10) if x % 2 == 0}
print(len(s))
```

#### :set_comp_over_list

```dr
nums: list[int] = [1, 2, 2, 3, 3, 3]
unique: set[int] = {x for x in nums}
print(len(unique))
```

#### :dict_comp_over_range

```dr
d: dict[str, int] = {"k": i for i in range(3)}
print(len(d))
```

#### :generator_over_range

```dr
g: list[int] = (x * 3 for x in range(4))
print(len(g))
print(g[0])
print(g[3])
```

#### :generator_over_list

```dr
nums: list[int] = [10, 20, 30]
doubled: list[int] = (x * 2 for x in nums)
print(len(doubled))
print(doubled[2])
```

#### :nested_list_comp_range

```dr
pairs: list[int] = [x + y for x in range(3) for y in range(3) if x != y]
print(len(pairs))
print(pairs[0])
print(pairs[1])
```

#### :list_comp_range_still_works

```dr
xs: list[int] = [i * 2 for i in range(5)]
print(len(xs))
print(xs[4])
```

#### :list_comp_collection_still_works

```dr
src: list[int] = [1, 2, 3, 4, 5]
doubled: list[int] = [x * 2 for x in src]
print(doubled[4])
```

#### :nested_list_comp_still_works

```dr
pairs: list[int] = [x + y for x in range(3) for y in range(3) if x != y]
print(len(pairs))
```

#### :list_comp_loop_bounded

```dr
last_len: int = 0
for i in range(10000) {
  out: list[int] = [x * 2 for x in range(50)]
  last_len = len(out)
}
print(last_len)
```

#### :set_comp_still_works

```dr
nums: list[int] = [1, 2, 2, 3, 3, 3]
unique: set[int] = {x for x in nums}
print(len(unique))
```

#### :dict_comp_still_works

```dr
d: dict[str, int] = {"k" + str(i): i for i in range(3)}
print(len(d))
```

#### :generator_expr_still_works

```dr
g: list[int] = (x * 2 for x in range(5))
print(len(g))
print(g[2])
```

#### :list_comp_str_identity

```dr
names: list[str] = ["alice", "bob", "carol"]
copies: list[str] = [n for n in names]
print(copies[0])
print(copies[2])
```

#### :list_comp_str_method_call

```dr
names: list[str] = ["alice", "bob", "carol"]
shouts: list[str] = [n.upper() for n in names]
print(shouts[0])
print(shouts[1])
print(shouts[2])
```

#### :list_comp_str_concat

```dr
names: list[str] = ["alice", "bob"]
greetings: list[str] = ["hi " + n for n in names]
print(greetings[0])
print(greetings[1])
```

#### :list_comp_str_source_preserved

```dr
names: list[str] = ["alice", "bob"]
copies: list[str] = [n for n in names]
print(names[0])
print(names[1])
more: list[str] = ["hi " + n for n in names]
print(more[0])
print(more[1])
```

#### :list_comp_str_nested

```dr
letters: list[str] = ["a", "b"]
digits: list[str] = ["1", "2"]
pairs: list[str] = [l + d for l in letters for d in digits]
print(len(pairs))
print(pairs[0])
print(pairs[3])
```

#### :for_in_over_str_comprehension

```dr
names: list[str] = ["alice", "bob", "carol"]
shouts: list[str] = [n.upper() for n in names]
for s in shouts {
  print(s)
}
```

#### :list_comp_str_loop_bounded

```dr
src: list[str] = ["foo", "bar", "baz"]
last: int = 0
for i in range(2000) {
  out: list[str] = [s + "!" for s in src]
  last = len(out)
}
print(last)
```
