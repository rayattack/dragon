# CodeGenDundersTest cases

Source programs for `test/CodeGenDundersTest.cpp`. Each block is named by the heading
above it and pulled in with `code("<name>")`.

#### :dunder_str_ir

```dr
class Foo {
  def() {
    self.x = 0
  }
  def __str__() -> str {
    return "foo"
  }
}
f: Foo = Foo()
print(f)
```

#### :dunder_eq_ir

```dr
class Bar {
  def(v: int) {
    self.v = v
  }
  def __eq__(other: Bar) -> bool {
    return self.v == other.v
  }
}
a: Bar = Bar(1)
b: Bar = Bar(2)
x: bool = a == b
```

#### :dunder_hash_ir

```dr
class Key {
  def(v: int) {
    self.v = v
  }
  def __hash__() -> int {
    return self.v
  }
}
k: Key = Key(5)
h: int = hash(k)
```

#### :dunder_bool_ir

```dr
class Flag {
  def(v: int) {
    self.v = v
  }
  def __bool__() -> bool {
    return self.v != 0
  }
}
f: Flag = Flag(1)
x: bool = bool(f)
```

#### :dunder_add_ir

```dr
class Vec {
  def(x: int) {
    self.x = x
  }
  def __add__(other: Vec) -> Vec {
    return Vec(self.x + other.x)
  }
}
a: Vec = Vec(1)
b: Vec = Vec(2)
c: Vec = a + b
```

#### :dunder_sub_ir

```dr
class Vec {
  def(x: int) {
    self.x = x
  }
  def __sub__(other: Vec) -> Vec {
    return Vec(self.x - other.x)
  }
}
a: Vec = Vec(5)
b: Vec = Vec(3)
c: Vec = a - b
```

#### :dunder_mul_ir

```dr
class Vec {
  def(x: int) {
    self.x = x
  }
  def __mul__(s: int) -> Vec {
    return Vec(self.x * s)
  }
}
a: Vec = Vec(3)
b: Vec = a * 2
```

#### :dunder_neg_ir

```dr
class Vec {
  def(x: int) {
    self.x = x
  }
  def __neg__() -> Vec {
    return Vec(0 - self.x)
  }
}
a: Vec = Vec(5)
b: Vec = -a
```

#### :dunder_abs_ir

```dr
class Num {
  def(v: int) {
    self.v = v
  }
  def __abs__() -> int {
    if self.v < 0 { return 0 - self.v }
    return self.v
  }
}
n: Num = Num(0 - 5)
a: int = abs(n)
```

#### :dunder_len_ir

```dr
class Bag {
  def(n: int) {
    self.n = n
  }
  def __len__() -> int {
    return self.n
  }
}
b: Bag = Bag(5)
print(len(b))
```

#### :dunder_getitem_ir

```dr
class Row {
  def(v: int) {
    self.v = v
  }
  def __getitem__(i: int) -> int {
    return self.v + i
  }
}
r: Row = Row(10)
print(r[3])
```

#### :dunder_setitem_ir

```dr
class Grid {
  def(v: int) {
    self.v = v
  }
  def __setitem__(i: int, val: int) -> int {
    self.v = val
    return 0
  }
}
g: Grid = Grid(0)
g[1] = 42
```

#### :dunder_contains_ir

```dr
class Bag {
  def(v: int) {
    self.v = v
  }
  def __contains__(x: int) -> int {
    if x == self.v { return 1 }
    return 0
  }
}
b: Bag = Bag(42)
if 42 in b { print(1) }
```

#### :dunder_iter_ir

```dr
class Counter {
  def(n: int) {
    self.n = n
    self.i = 0
  }
  def __iter__() -> Counter {
    return self
  }
  def __next__() -> int {
    if self.i >= self.n { raise StopIteration() }
    self.i = self.i + 1
    return self.i
  }
}
c: Counter = Counter(3)
for x in c { print(x) }
```

#### :dunder_enter_exit_ir

```dr
class Ctx {
  def() {
    self.x = 0
  }
  def __enter__() -> Ctx {
    return self
  }
  def __exit__() -> int {
    return 0
  }
}
with Ctx() as c {
  print(1)
}
```

#### :dunder_str_print

```dr
class Point {
  def(x: int, y: int) {
    self.x = x
    self.y = y
  }
  def __str__() -> str {
    return "Point(" + str(self.x) + ", " + str(self.y) + ")"
  }
}
p: Point = Point(3, 4)
print(p)
```

#### :dunder_str_conversion

```dr
class Color {
  def(code: int) {
    self.code = code
  }
  def __str__() -> str {
    return "Color(" + str(self.code) + ")"
  }
}
c: Color = Color(255)
s: str = str(c)
print(s)
```

#### :dunder_str_f_string

```dr
class Tag {
  def(v: int) {
    self.v = v
  }
  def __str__() -> str {
    return "#" + str(self.v)
  }
}
t: Tag = Tag(42)
print(f"value={t}")
```

#### :dunder_str_fallback_repr

```dr
class Box {
  def(n: int) {
    self.n = n
  }
  def __repr__() -> str {
    return "Box(" + str(self.n) + ")"
  }
}
b: Box = Box(7)
print(b)
```

#### :dunder_str_no_method

```dr
class Empty {
  def() {
    self.x = 0
  }
}
e: Empty = Empty()
print(e)
```

#### :dunder_str_py

```py
class Greeting:
    def __init__(self, code: int):
        self.code = code
    def __str__(self) -> str:
        return "Hello #" + str(self.code)
g: Greeting = Greeting(42)
print(g)
```

#### :dunder_repr

```dr
class Num {
  def(v: int) {
    self.v = v
  }
  def __repr__() -> str {
    return "Num(" + str(self.v) + ")"
  }
}
n: Num = Num(5)
print(repr(n))
```

#### :dunder_eq

```dr
class Vec {
  def(x: int) {
    self.x = x
  }
  def __eq__(other: Vec) -> bool {
    return self.x == other.x
  }
}
a: Vec = Vec(3)
b: Vec = Vec(3)
c: Vec = Vec(4)
if a == b { print("equal") }
if a == c { print("bad") } else { print("not equal") }
```

#### :dunder_ne_fallback

```dr
class ID {
  def(v: int) {
    self.v = v
  }
  def __eq__(other: ID) -> bool {
    return self.v == other.v
  }
}
a: ID = ID(1)
b: ID = ID(2)
if a != b { print("diff") }
```

#### :dunder_ne_explicit

```dr
class Wrapper {
  def(v: int) {
    self.v = v
  }
  def __eq__(other: Wrapper) -> bool {
    return self.v == other.v
  }
  def __ne__(other: Wrapper) -> bool {
    return self.v != other.v
  }
}
a: Wrapper = Wrapper(1)
b: Wrapper = Wrapper(1)
c: Wrapper = Wrapper(2)
if a != c { print("diff") }
if a != b { print("bad") } else { print("same") }
```

#### :dunder_eq_default_pointer

```dr
class Node {
  def(v: int) {
    self.v = v
  }
}
a: Node = Node(1)
b: Node = Node(1)
if a == b { print("same") } else { print("diff") }
```

#### :dunder_eq_py

```py
class Money:
    def __init__(self, amount: int):
        self.amount = amount
    def __eq__(self, other: Money) -> bool:
        return self.amount == other.amount
a: Money = Money(100)
b: Money = Money(100)
if a == b:
    print("equal")
```

#### :dunder_lt

```dr
class Score {
  def(v: int) {
    self.v = v
  }
  def __lt__(other: Score) -> bool {
    return self.v < other.v
  }
}
a: Score = Score(3)
b: Score = Score(5)
if a < b { print("less") }
```

#### :dunder_gt_fallback

```dr
class Rank {
  def(v: int) {
    self.v = v
  }
  def __lt__(other: Rank) -> bool {
    return self.v < other.v
  }
}
a: Rank = Rank(5)
b: Rank = Rank(3)
if a > b { print("greater") }
```

#### :dunder_le_fallback

```dr
class Val {
  def(v: int) {
    self.v = v
  }
  def __lt__(other: Val) -> bool {
    return self.v < other.v
  }
  def __eq__(other: Val) -> bool {
    return self.v == other.v
  }
}
a: Val = Val(3)
b: Val = Val(3)
c: Val = Val(5)
if a <= b { print("le1") }
if a <= c { print("le2") }
```

#### :dunder_ge_fallback

```dr
class Level {
  def(v: int) {
    self.v = v
  }
  def __lt__(other: Level) -> bool {
    return self.v < other.v
  }
}
a: Level = Level(5)
b: Level = Level(3)
c: Level = Level(5)
if a >= b { print("ge1") }
if a >= c { print("ge2") }
```

#### :dunder_all_comparisons

```dr
class Num {
  def(v: int) {
    self.v = v
  }
  def __eq__(other: Num) -> bool { return self.v == other.v }
  def __ne__(other: Num) -> bool { return self.v != other.v }
  def __lt__(other: Num) -> bool { return self.v < other.v }
  def __gt__(other: Num) -> bool { return self.v > other.v }
  def __le__(other: Num) -> bool { return self.v <= other.v }
  def __ge__(other: Num) -> bool { return self.v >= other.v }
}
a: Num = Num(3)
b: Num = Num(5)
c: Num = Num(3)
if a == c { print("eq") }
if a != b { print("ne") }
if a < b { print("lt") }
if b > a { print("gt") }
if a <= c { print("le") }
if b >= a { print("ge") }
```

#### :dunder_bool

```dr
class Truthy {
  def(v: int) {
    self.v = v
  }
  def __bool__() -> bool {
    return self.v != 0
  }
}
a: Truthy = Truthy(1)
b: Truthy = Truthy(0)
if bool(a) { print("a true") }
if bool(b) { print("b true") } else { print("b false") }
```

#### :dunder_bool_default

```dr
class Thing {
  def() {
    self.x = 0
  }
}
t: Thing = Thing()
if bool(t) { print("true") } else { print("false") }
```

#### :dunder_hash

```dr
class Key {
  def(v: int) {
    self.v = v
  }
  def __hash__() -> int {
    return self.v * 31
  }
}
k: Key = Key(3)
print(hash(k))
```

#### :dunder_hash_default

```dr
class Obj {
  def() {
    self.x = 0
  }
}
o: Obj = Obj()
h: int = hash(o)
if h != 0 { print("nonzero") }
```

#### :dunder_inherited

```dr
class Base {
  def(v: int) {
    self.v = v
  }
  def __str__() -> str {
    return "v=" + str(self.v)
  }
}
class Child(Base) {
  def(v: int) {
    self.v = v
  }
}
c: Child = Child(9)
print(c)
```

#### :dunder_combined_py

```py
class Frac:
    def __init__(self, n: int, d: int):
        self.n = n
        self.d = d
    def __str__(self) -> str:
        return str(self.n) + "/" + str(self.d)
    def __eq__(self, other: Frac) -> bool:
        return self.n * other.d == self.d * other.n
    def __lt__(self, other: Frac) -> bool:
        return self.n * other.d < self.d * other.n
a: Frac = Frac(1, 2)
b: Frac = Frac(2, 4)
c: Frac = Frac(3, 4)
print(a)
if a == b:
    print("eq")
if a < c:
    print("lt")
```

#### :dunder_add

```dr
class Vec {
  def(x: int, y: int) {
    self.x = x
    self.y = y
  }
  def __add__(other: Vec) -> Vec {
    return Vec(self.x + other.x, self.y + other.y)
  }
  def __str__() -> str {
    return str(self.x) + "," + str(self.y)
  }
}
a: Vec = Vec(1, 2)
b: Vec = Vec(3, 4)
c: Vec = a + b
print(c)
```

#### :dunder_sub

```dr
class Vec {
  def(x: int, y: int) {
    self.x = x
    self.y = y
  }
  def __sub__(other: Vec) -> Vec {
    return Vec(self.x - other.x, self.y - other.y)
  }
  def __str__() -> str {
    return str(self.x) + "," + str(self.y)
  }
}
a: Vec = Vec(5, 7)
b: Vec = Vec(2, 3)
c: Vec = a - b
print(c)
```

#### :dunder_mul

```dr
class Vec {
  def(x: int, y: int) {
    self.x = x
    self.y = y
  }
  def __mul__(other: Vec) -> Vec {
    return Vec(self.x * other.x, self.y * other.y)
  }
  def __str__() -> str {
    return str(self.x) + "," + str(self.y)
  }
}
a: Vec = Vec(2, 3)
b: Vec = Vec(4, 5)
c: Vec = a * b
print(c)
```

#### :dunder_truediv

```dr
class Ratio {
  def(n: int) {
    self.n = n
  }
  def __truediv__(d: int) -> int {
    return self.n // d
  }
}
r: Ratio = Ratio(10)
x: int = r / 3
print(x)
```

#### :dunder_floordiv

```dr
class Num {
  def(v: int) {
    self.v = v
  }
  def __floordiv__(d: int) -> int {
    return self.v // d
  }
}
n: Num = Num(17)
x: int = n // 5
print(x)
```

#### :dunder_mod

```dr
class Num {
  def(v: int) {
    self.v = v
  }
  def __mod__(d: int) -> int {
    return self.v % d
  }
}
n: Num = Num(17)
x: int = n % 5
print(x)
```

#### :dunder_pow

```dr
class Num {
  def(v: int) {
    self.v = v
  }
  def __pow__(e: int) -> int {
    return self.v ** e
  }
}
n: Num = Num(3)
x: int = n ** 4
print(x)
```

#### :dunder_neg

```dr
class Vec {
  def(x: int, y: int) {
    self.x = x
    self.y = y
  }
  def __neg__() -> Vec {
    return Vec(0 - self.x, 0 - self.y)
  }
  def __str__() -> str {
    return str(self.x) + "," + str(self.y)
  }
}
v: Vec = Vec(3, 0 - 4)
w: Vec = -v
print(w)
```

#### :dunder_pos

```dr
class Num {
  def(v: int) {
    self.v = v
  }
  def __pos__() -> Num {
    if self.v < 0 { return Num(0 - self.v) }
    return Num(self.v)
  }
  def __str__() -> str {
    return str(self.v)
  }
}
n: Num = Num(0 - 7)
p: Num = +n
print(p)
```

#### :dunder_abs

```dr
class Num {
  def(v: int) {
    self.v = v
  }
  def __abs__() -> int {
    if self.v < 0 { return 0 - self.v }
    return self.v
  }
}
n: Num = Num(0 - 42)
print(abs(n))
```

#### :dunder_add_returns_instance

```dr
class Vec {
  def(x: int) {
    self.x = x
  }
  def __add__(other: Vec) -> Vec {
    return Vec(self.x + other.x)
  }
  def __str__() -> str {
    return str(self.x)
  }
}
a: Vec = Vec(1)
b: Vec = Vec(2)
c: Vec = a + b
print(c)
d: Vec = c + Vec(10)
print(d)
```

#### :dunder_add_inherited

```dr
class Base {
  def(v: int) {
    self.v = v
  }
  def __add__(other: Base) -> Base {
    return Base(self.v + other.v)
  }
  def __str__() -> str {
    return str(self.v)
  }
}
class Child(Base) {
  def(v: int) {
    self.v = v
  }
}
a: Child = Child(3)
b: Child = Child(7)
c: Base = a + b
print(c)
```

#### :dunder_add_py

```py
class Vec:
    def __init__(self, x: int, y: int):
        self.x = x
        self.y = y
    def __add__(self, other: Vec) -> Vec:
        return Vec(self.x + other.x, self.y + other.y)
    def __str__(self) -> str:
        return str(self.x) + "," + str(self.y)
a: Vec = Vec(1, 2)
b: Vec = Vec(3, 4)
c: Vec = a + b
print(c)
```

#### :dunder_arith_no_fallback

```dr
x: int = 10 + 20
y: int = x * 3
print(x)
print(y)
```

#### :dunder_mixed

```dr
class Val {
  def(n: int) {
    self.n = n
  }
  def __add__(other: Val) -> Val {
    return Val(self.n + other.n)
  }
  def __eq__(other: Val) -> bool {
    return self.n == other.n
  }
  def __str__() -> str {
    return str(self.n)
  }
}
a: Val = Val(3)
b: Val = Val(4)
c: Val = a + b
d: Val = Val(7)
if c == d { print("equal") }
print(c)
```

#### :dunder_add_chained

```dr
class Vec {
  def(x: int) {
    self.x = x
  }
  def __add__(other: Vec) -> Vec {
    return Vec(self.x + other.x)
  }
  def __str__() -> str {
    return str(self.x)
  }
}
a: Vec = Vec(1)
b: Vec = Vec(2)
c: Vec = Vec(3)
d: Vec = a + b + c
print(d)
```

#### :dunder_mul_scalar

```dr
class Vec {
  def(x: int, y: int) {
    self.x = x
    self.y = y
  }
  def __mul__(s: int) -> Vec {
    return Vec(self.x * s, self.y * s)
  }
  def __str__() -> str {
    return str(self.x) + "," + str(self.y)
  }
}
v: Vec = Vec(2, 3)
w: Vec = v * 5
print(w)
```

#### :dunder_neg_return

```dr
class Pt {
  def(x: int) {
    self.x = x
  }
  def __neg__() -> Pt {
    return Pt(0 - self.x)
  }
  def __str__() -> str {
    return str(self.x)
  }
}
p: Pt = Pt(5)
q: Pt = -p
print(q)
```

#### :dunder_all_arithmetic

```dr
class N {
  def(v: int) {
    self.v = v
  }
  def __add__(o: N) -> N { return N(self.v + o.v) }
  def __sub__(o: N) -> N { return N(self.v - o.v) }
  def __mul__(o: N) -> N { return N(self.v * o.v) }
  def __truediv__(o: N) -> N { return N(self.v // o.v) }
  def __floordiv__(o: N) -> N { return N(self.v // o.v) }
  def __mod__(o: N) -> N { return N(self.v % o.v) }
  def __pow__(o: N) -> N { return N(self.v ** o.v) }
  def __str__() -> str { return str(self.v) }
}
a: N = N(10)
b: N = N(3)
print(a + b)
print(a - b)
print(a * b)
print(a / b)
print(a // b)
print(a % b)
print(N(2) ** N(8))
```

#### :dunder_abs_returns_int

```dr
class Dist {
  def(v: int) {
    self.v = v
  }
  def __abs__() -> int {
    if self.v < 0 { return 0 - self.v }
    return self.v
  }
}
d: Dist = Dist(0 - 99)
x: int = abs(d)
print(x + 1)
```

#### :dunder_len

```dr
class Bag {
  def(n: int) {
    self.n = n
  }
  def __len__() -> int {
    return self.n
  }
}
b: Bag = Bag(7)
print(len(b))
```

#### :dunder_len_no_regression

```dr
s: str = "hello"
print(len(s))
```

#### :dunder_getitem

```dr
class Row {
  def(base: int) {
    self.base = base
  }
  def __getitem__(i: int) -> int {
    return self.base + i
  }
}
r: Row = Row(100)
print(r[5])
print(r[0])
```

#### :dunder_setitem

```dr
class Store {
  def() {
    self.val = 0
  }
  def __getitem__(i: int) -> int {
    return self.val
  }
  def __setitem__(i: int, v: int) -> int {
    self.val = v
    return 0
  }
}
s: Store = Store()
s[0] = 42
print(s[0])
```

#### :dunder_contains

```dr
class EvenSet {
  def() {
    self.x = 0
  }
  def __contains__(n: int) -> int {
    if n % 2 == 0 { return 1 }
    return 0
  }
}
e: EvenSet = EvenSet()
if 4 in e { print("yes") }
if 3 in e { print("no") }
print("done")
```

#### :dunder_contains_not

```dr
class NumSet {
  def(v: int) {
    self.v = v
  }
  def __contains__(n: int) -> int {
    if n == self.v { return 1 }
    return 0
  }
}
s: NumSet = NumSet(10)
if 10 in s { print("found") }
if 20 in s { print("bad") }
print("end")
```

#### :dunder_iter

```dr
class Range3 {
  def() {
    self.i = 0
  }
  def __iter__() -> Range3 {
    self.i = 0
    return self
  }
  def __next__() -> int {
    if self.i >= 3 { raise StopIteration() }
    self.i = self.i + 1
    return self.i
  }
}
r: Range3 = Range3()
for x in r {
  print(x)
}
```

#### :dunder_iter_sum

```dr
class Counter {
  def(n: int) {
    self.n = n
    self.i = 0
  }
  def __iter__() -> Counter {
    self.i = 0
    return self
  }
  def __next__() -> int {
    if self.i >= self.n { raise StopIteration() }
    self.i = self.i + 1
    return self.i
  }
}
total: int = 0
c: Counter = Counter(5)
for x in c {
  total = total + x
}
print(total)
```

#### :dunder_getitem_no_regression

```dr
items: list[int] = [10, 20, 30]
print(items[1])
```

#### :dunder_contains_no_regression

```dr
s: str = "hello world"
if "hello" in s { print("yes") }
```

#### :dunder_len_inherited

```dr
class Base {
  def(n: int) {
    self.n = n
  }
  def __len__() -> int {
    return self.n
  }
}
class Child(Base) {
  def(n: int) {
    self.n = n
  }
}
c: Child = Child(9)
print(len(c))
```

#### :dunder_getitem_str

```dr
class NameMap {
  def() {
    self.x = 0
  }
  def __getitem__(i: int) -> str {
    if i == 0 { return "zero" }
    return "other"
  }
}
m: NameMap = NameMap()
print(m[0])
print(m[1])
```

#### :dunder_setitem_multiple

```dr
class Arr {
  def() {
    self.a = 0
    self.b = 0
  }
  def __setitem__(i: int, v: int) -> int {
    if i == 0 { self.a = v }
    if i == 1 { self.b = v }
    return 0
  }
  def __getitem__(i: int) -> int {
    if i == 0 { return self.a }
    return self.b
  }
}
a: Arr = Arr()
a[0] = 10
a[1] = 20
print(a[0])
print(a[1])
```

#### :dunder_combined_container

```dr
class Box {
  def(v: int) {
    self.v = v
  }
  def __len__() -> int {
    return 1
  }
  def __getitem__(i: int) -> int {
    return self.v
  }
  def __contains__(x: int) -> int {
    if x == self.v { return 1 }
    return 0
  }
}
b: Box = Box(42)
print(len(b))
print(b[0])
if 42 in b { print("yes") }
```

#### :dunder_with_basic

```dr
class Ctx {
  def() {
    self.x = 0
  }
  def __enter__() -> Ctx {
    print("enter")
    return self
  }
  def __exit__() -> int {
    print("exit")
    return 0
  }
}
with Ctx() as c {
  print("body")
}
```

#### :dunder_with_no_as

```dr
class Logger {
  def() {
    self.x = 0
  }
  def __enter__() -> Logger {
    print("start")
    return self
  }
  def __exit__() -> int {
    print("end")
    return 0
  }
}
with Logger() {
  print("running")
}
```

#### :dunder_with_returns_self

```dr
class MyLock {
  def(v: int) {
    self.v = v
  }
  def __enter__() -> MyLock {
    print("lock")
    return self
  }
  def __exit__() -> int {
    print("unlock")
    return 0
  }
  def __str__() -> str {
    return str(self.v)
  }
}
with MyLock(42) as m {
  print(m)
}
```

#### :dunder_with_exit_on_exception

```dr
class Guard {
  def() {
    self.x = 0
  }
  def __enter__() -> Guard {
    print("enter")
    return self
  }
  def __exit__() -> int {
    print("exit")
    return 0
  }
}
try {
  with Guard() as g {
    print("body")
    raise ValueError("oops")
  }
} except ValueError {
  print("caught")
}
```

#### :dunder_with_inherited

```dr
class Base {
  def() {
    self.x = 0
  }
  def __enter__() -> Base {
    print("base_enter")
    return self
  }
  def __exit__() -> int {
    print("base_exit")
    return 0
  }
}
class Child(Base) {
  def() {
    Base()
  }
}
with Child() as c {
  print("inside")
}
```

#### :dunder_with_multiple_statements

```dr
class Timer {
  def() {
    self.x = 0
  }
  def __enter__() -> Timer {
    print("begin")
    return self
  }
  def __exit__() -> int {
    print("finish")
    return 0
  }
}
with Timer() as t {
  print("a")
  print("b")
  print("c")
}
```

#### :dunder_with_after_block

```dr
class Wrap {
  def() {
    self.x = 0
  }
  def __enter__() -> Wrap {
    return self
  }
  def __exit__() -> int {
    print("cleaned")
    return 0
  }
}
with Wrap() as w {
  print("inside")
}
print("after")
```

#### :dunder_call_ir

```dr
class Multiplier {
  def(factor: int) {
    self.factor = factor
  }
  def __call__(x: int) -> int {
    return self.factor * x
  }
}
m: Multiplier = Multiplier(3)
result: int = m(5)
```

#### :dunder_call_simple

```dr
class Doubler {
  def() {
    self.x = 0
  }
  def __call__(n: int) -> int {
    return n * 2
  }
}
d: Doubler = Doubler()
print(d(21))
```

#### :dunder_call_with_state

```dr
class Multiplier {
  def(factor: int) {
    self.factor = factor
  }
  def __call__(x: int) -> int {
    return self.factor * x
  }
}
m: Multiplier = Multiplier(3)
print(m(5))
print(m(10))
```

#### :dunder_call_multiple_args

```dr
class Adder {
  def() {
    self.x = 0
  }
  def __call__(a: int, b: int) -> int {
    return a + b
  }
}
a: Adder = Adder()
print(a(10, 32))
```

#### :dunder_call_no_args

```dr
class Greeter {
  def(name: str) {
    self.name = name
  }
  def __call__() -> str {
    return self.name
  }
}
g: Greeter = Greeter("hello")
print(g())
```

#### :dunder_call_inherited

```dr
class Base {
  def(n: int) {
    self.n = n
  }
  def __call__() -> int {
    return self.n
  }
}
class Child(Base) {
  def(n: int) {
    self.n = n
  }
}
c: Child = Child(99)
print(c())
```

#### :dunder_call_void

```dr
class Printer {
  def(msg: str) {
    self.msg = msg
  }
  def __call__() {
    print(self.msg)
  }
}
p: Printer = Printer("fired")
p()
```

#### :dunder_call_returning_dict_subscripted_inline

```dr
class Box {
  _d: dict[str, int]
  def(d: dict[str, int]) { self._d = d }
  def __call__() -> dict[str, int] { return self._d }
}
b: Box = Box({"k": 7})
print(b()["k"])
```

#### :dunder_call_returning_list_subscripted_inline

```dr
class LBox {
  _l: list[int]
  def(l: list[int]) { self._l = l }
  def __call__() -> list[int] { return self._l }
}
b: LBox = LBox([10, 20, 30])
print(b()[2])
```

#### :dunder_call_returning_instance_attribute_inline

```dr
class Point { x: int
  def(x: int) { self.x = x } }
class PBox {
  _p: Point
  def(p: Point) { self._p = p }
  def __call__() -> Point { return self._p }
}
b: PBox = PBox(Point(42))
print(b().x)
```

#### :generic_cell_chained_access_inline

```dr
class Cell[T] {
  _value: T
  def(initial: T) { self._value = initial }
  def __call__() -> T { return self._value }
  def get() -> T { return self._value }
}
class Point { x: int
  def(x: int) { self.x = x } }
sd: Cell[dict[str, int]] = Cell({"k": 7})
print(sd()["k"])
print(sd.get()["k"])
sp: Cell[Point] = Cell(Point(42))
print(sp().x)
print(sp.get().x)
sl: Cell[list[int]] = Cell([10, 20, 30])
print(sl()[2])
```
