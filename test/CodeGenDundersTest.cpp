#include "CodeGenTestHelpers.h"

//===----------------------------------------------------------------------===//
// Dunder Str/Repr/Eq/Hash/Bool IR
//===----------------------------------------------------------------------===//

TEST(CodeGenTest, DunderStrIR) {
    auto ir = generateIR(
        "class Foo {\n"
        "  def() {\n"
        "    self.x = 0\n"
        "  }\n"
        "  def __str__() -> str {\n"
        "    return \"foo\"\n"
        "  }\n"
        "}\n"
        "f: Foo = Foo()\n"
        "print(f)\n"
    );
    // Should call Foo___str__ instead of printing "<Foo instance>"
    EXPECT_NE(ir.find("Foo___str__"), std::string::npos);
    EXPECT_EQ(ir.find("Foo instance"), std::string::npos);
}

TEST(CodeGenTest, DunderEqIR) {
    auto ir = generateIR(
        "class Bar {\n"
        "  def(v: int) {\n"
        "    self.v = v\n"
        "  }\n"
        "  def __eq__(other: Bar) -> bool {\n"
        "    return self.v == other.v\n"
        "  }\n"
        "}\n"
        "a: Bar = Bar(1)\n"
        "b: Bar = Bar(2)\n"
        "x: bool = a == b\n"
    );
    // Should call Bar___eq__
    EXPECT_NE(ir.find("Bar___eq__"), std::string::npos);
}

TEST(CodeGenTest, DunderHashIR) {
    auto ir = generateIR(
        "class Key {\n"
        "  def(v: int) {\n"
        "    self.v = v\n"
        "  }\n"
        "  def __hash__() -> int {\n"
        "    return self.v\n"
        "  }\n"
        "}\n"
        "k: Key = Key(5)\n"
        "h: int = hash(k)\n"
    );
    // Should call Key___hash__
    EXPECT_NE(ir.find("Key___hash__"), std::string::npos);
}

TEST(CodeGenTest, DunderBoolIR) {
    auto ir = generateIR(
        "class Flag {\n"
        "  def(v: int) {\n"
        "    self.v = v\n"
        "  }\n"
        "  def __bool__() -> bool {\n"
        "    return self.v != 0\n"
        "  }\n"
        "}\n"
        "f: Flag = Flag(1)\n"
        "x: bool = bool(f)\n"
    );
    // Should call Flag___bool__
    EXPECT_NE(ir.find("Flag___bool__"), std::string::npos);
}

//===----------------------------------------------------------------------===//
// Dunder Arithmetic IR
//===----------------------------------------------------------------------===//

TEST(CodeGenTest, DunderAddIR) {
    auto ir = generateIR(
        "class Vec {\n"
        "  def(x: int) {\n"
        "    self.x = x\n"
        "  }\n"
        "  def __add__(other: Vec) -> Vec {\n"
        "    return Vec(self.x + other.x)\n"
        "  }\n"
        "}\n"
        "a: Vec = Vec(1)\n"
        "b: Vec = Vec(2)\n"
        "c: Vec = a + b\n"
    );
    EXPECT_NE(ir.find("Vec___add__"), std::string::npos);
}

TEST(CodeGenTest, DunderSubIR) {
    auto ir = generateIR(
        "class Vec {\n"
        "  def(x: int) {\n"
        "    self.x = x\n"
        "  }\n"
        "  def __sub__(other: Vec) -> Vec {\n"
        "    return Vec(self.x - other.x)\n"
        "  }\n"
        "}\n"
        "a: Vec = Vec(5)\n"
        "b: Vec = Vec(3)\n"
        "c: Vec = a - b\n"
    );
    EXPECT_NE(ir.find("Vec___sub__"), std::string::npos);
}

TEST(CodeGenTest, DunderMulIR) {
    auto ir = generateIR(
        "class Vec {\n"
        "  def(x: int) {\n"
        "    self.x = x\n"
        "  }\n"
        "  def __mul__(s: int) -> Vec {\n"
        "    return Vec(self.x * s)\n"
        "  }\n"
        "}\n"
        "a: Vec = Vec(3)\n"
        "b: Vec = a * 2\n"
    );
    EXPECT_NE(ir.find("Vec___mul__"), std::string::npos);
}

TEST(CodeGenTest, DunderNegIR) {
    auto ir = generateIR(
        "class Vec {\n"
        "  def(x: int) {\n"
        "    self.x = x\n"
        "  }\n"
        "  def __neg__() -> Vec {\n"
        "    return Vec(0 - self.x)\n"
        "  }\n"
        "}\n"
        "a: Vec = Vec(5)\n"
        "b: Vec = -a\n"
    );
    EXPECT_NE(ir.find("Vec___neg__"), std::string::npos);
}

TEST(CodeGenTest, DunderAbsIR) {
    auto ir = generateIR(
        "class Num {\n"
        "  def(v: int) {\n"
        "    self.v = v\n"
        "  }\n"
        "  def __abs__() -> int {\n"
        "    if self.v < 0 { return 0 - self.v }\n"
        "    return self.v\n"
        "  }\n"
        "}\n"
        "n: Num = Num(0 - 5)\n"
        "a: int = abs(n)\n"
    );
    EXPECT_NE(ir.find("Num___abs__"), std::string::npos);
}

//===----------------------------------------------------------------------===//
// Dunder Container IR
//===----------------------------------------------------------------------===//

TEST(CodeGenTest, DunderLenIR) {
    auto ir = generateIR(
        "class Bag {\n"
        "  def(n: int) {\n"
        "    self.n = n\n"
        "  }\n"
        "  def __len__() -> int {\n"
        "    return self.n\n"
        "  }\n"
        "}\n"
        "b: Bag = Bag(5)\n"
        "print(len(b))\n"
    );
    EXPECT_NE(ir.find("Bag___len__"), std::string::npos);
}

TEST(CodeGenTest, DunderGetitemIR) {
    auto ir = generateIR(
        "class Row {\n"
        "  def(v: int) {\n"
        "    self.v = v\n"
        "  }\n"
        "  def __getitem__(i: int) -> int {\n"
        "    return self.v + i\n"
        "  }\n"
        "}\n"
        "r: Row = Row(10)\n"
        "print(r[3])\n"
    );
    EXPECT_NE(ir.find("Row___getitem__"), std::string::npos);
}

TEST(CodeGenTest, DunderSetitemIR) {
    auto ir = generateIR(
        "class Grid {\n"
        "  def(v: int) {\n"
        "    self.v = v\n"
        "  }\n"
        "  def __setitem__(i: int, val: int) -> int {\n"
        "    self.v = val\n"
        "    return 0\n"
        "  }\n"
        "}\n"
        "g: Grid = Grid(0)\n"
        "g[1] = 42\n"
    );
    EXPECT_NE(ir.find("Grid___setitem__"), std::string::npos);
}

TEST(CodeGenTest, DunderContainsIR) {
    auto ir = generateIR(
        "class Bag {\n"
        "  def(v: int) {\n"
        "    self.v = v\n"
        "  }\n"
        "  def __contains__(x: int) -> int {\n"
        "    if x == self.v { return 1 }\n"
        "    return 0\n"
        "  }\n"
        "}\n"
        "b: Bag = Bag(42)\n"
        "if 42 in b { print(1) }\n"
    );
    EXPECT_NE(ir.find("Bag___contains__"), std::string::npos);
}

TEST(CodeGenTest, DunderIterIR) {
    auto ir = generateIR(
        "class Counter {\n"
        "  def(n: int) {\n"
        "    self.n = n\n"
        "    self.i = 0\n"
        "  }\n"
        "  def __iter__() -> Counter {\n"
        "    return self\n"
        "  }\n"
        "  def __next__() -> int {\n"
        "    if self.i >= self.n { raise StopIteration() }\n"
        "    self.i = self.i + 1\n"
        "    return self.i\n"
        "  }\n"
        "}\n"
        "c: Counter = Counter(3)\n"
        "for x in c { print(x) }\n"
    );
    EXPECT_NE(ir.find("Counter___iter__"), std::string::npos);
    EXPECT_NE(ir.find("Counter___next__"), std::string::npos);
}

//===----------------------------------------------------------------------===//
// Dunder Context Manager IR
//===----------------------------------------------------------------------===//

TEST(CodeGenTest, DunderEnterExitIR) {
    auto ir = generateIR(
        "class Ctx {\n"
        "  def() {\n"
        "    self.x = 0\n"
        "  }\n"
        "  def __enter__() -> Ctx {\n"
        "    return self\n"
        "  }\n"
        "  def __exit__() -> int {\n"
        "    return 0\n"
        "  }\n"
        "}\n"
        "with Ctx() as c {\n"
        "  print(1)\n"
        "}\n"
    );
    EXPECT_NE(ir.find("Ctx___enter__"), std::string::npos);
    EXPECT_NE(ir.find("Ctx___exit__"), std::string::npos);
}

//===----------------------------------------------------------------------===//
// Dunder Str/Repr E2E
//===----------------------------------------------------------------------===//

TEST(CodeGenE2E, DunderStrPrint) {
    auto out = compileAndRun(
        "class Point {\n"
        "  def(x: int, y: int) {\n"
        "    self.x = x\n"
        "    self.y = y\n"
        "  }\n"
        "  def __str__() -> str {\n"
        "    return \"Point(\" + str(self.x) + \", \" + str(self.y) + \")\"\n"
        "  }\n"
        "}\n"
        "p: Point = Point(3, 4)\n"
        "print(p)\n"
    );
    EXPECT_EQ(out, "Point(3, 4)\n");
}

TEST(CodeGenE2E, DunderStrConversion) {
    auto out = compileAndRun(
        "class Color {\n"
        "  def(code: int) {\n"
        "    self.code = code\n"
        "  }\n"
        "  def __str__() -> str {\n"
        "    return \"Color(\" + str(self.code) + \")\"\n"
        "  }\n"
        "}\n"
        "c: Color = Color(255)\n"
        "s: str = str(c)\n"
        "print(s)\n"
    );
    EXPECT_EQ(out, "Color(255)\n");
}

TEST(CodeGenE2E, DunderStrFString) {
    auto out = compileAndRun(
        "class Tag {\n"
        "  def(v: int) {\n"
        "    self.v = v\n"
        "  }\n"
        "  def __str__() -> str {\n"
        "    return \"#\" + str(self.v)\n"
        "  }\n"
        "}\n"
        "t: Tag = Tag(42)\n"
        "print(f\"value={t}\")\n"
    );
    EXPECT_EQ(out, "value=#42\n");
}

TEST(CodeGenE2E, DunderStrFallbackRepr) {
    // No __str__ but has __repr__ - print should fall back to __repr__
    auto out = compileAndRun(
        "class Box {\n"
        "  def(n: int) {\n"
        "    self.n = n\n"
        "  }\n"
        "  def __repr__() -> str {\n"
        "    return \"Box(\" + str(self.n) + \")\"\n"
        "  }\n"
        "}\n"
        "b: Box = Box(7)\n"
        "print(b)\n"
    );
    EXPECT_EQ(out, "Box(7)\n");
}

TEST(CodeGenE2E, DunderStrNoMethod) {
    // No __str__ or __repr__ - should print <ClassName instance>
    auto out = compileAndRun(
        "class Empty {\n"
        "  def() {\n"
        "    self.x = 0\n"
        "  }\n"
        "}\n"
        "e: Empty = Empty()\n"
        "print(e)\n"
    );
    EXPECT_EQ(out, "<Empty instance>\n");
}

TEST(CodeGenE2E, DunderStrPy) {
    auto out = compileAndRunPy(
        "class Greeting:\n"
        "    def __init__(self, code: int):\n"
        "        self.code = code\n"
        "    def __str__(self) -> str:\n"
        "        return \"Hello #\" + str(self.code)\n"
        "g: Greeting = Greeting(42)\n"
        "print(g)\n"
    );
    EXPECT_EQ(out, "Hello #42\n");
}

TEST(CodeGenE2E, DunderRepr) {
    auto out = compileAndRun(
        "class Num {\n"
        "  def(v: int) {\n"
        "    self.v = v\n"
        "  }\n"
        "  def __repr__() -> str {\n"
        "    return \"Num(\" + str(self.v) + \")\"\n"
        "  }\n"
        "}\n"
        "n: Num = Num(5)\n"
        "print(repr(n))\n"
    );
    EXPECT_EQ(out, "Num(5)\n");
}

//===----------------------------------------------------------------------===//
// Dunder Eq/Ne/Comparison E2E
//===----------------------------------------------------------------------===//

TEST(CodeGenE2E, DunderEq) {
    auto out = compileAndRun(
        "class Vec {\n"
        "  def(x: int) {\n"
        "    self.x = x\n"
        "  }\n"
        "  def __eq__(other: Vec) -> bool {\n"
        "    return self.x == other.x\n"
        "  }\n"
        "}\n"
        "a: Vec = Vec(3)\n"
        "b: Vec = Vec(3)\n"
        "c: Vec = Vec(4)\n"
        "if a == b { print(\"equal\") }\n"
        "if a == c { print(\"bad\") } else { print(\"not equal\") }\n"
    );
    EXPECT_EQ(out, "equal\nnot equal\n");
}

TEST(CodeGenE2E, DunderNeFallback) {
    // __ne__ not defined, but __eq__ is - should negate __eq__
    auto out = compileAndRun(
        "class ID {\n"
        "  def(v: int) {\n"
        "    self.v = v\n"
        "  }\n"
        "  def __eq__(other: ID) -> bool {\n"
        "    return self.v == other.v\n"
        "  }\n"
        "}\n"
        "a: ID = ID(1)\n"
        "b: ID = ID(2)\n"
        "if a != b { print(\"diff\") }\n"
    );
    EXPECT_EQ(out, "diff\n");
}

TEST(CodeGenE2E, DunderNeExplicit) {
    auto out = compileAndRun(
        "class Wrapper {\n"
        "  def(v: int) {\n"
        "    self.v = v\n"
        "  }\n"
        "  def __eq__(other: Wrapper) -> bool {\n"
        "    return self.v == other.v\n"
        "  }\n"
        "  def __ne__(other: Wrapper) -> bool {\n"
        "    return self.v != other.v\n"
        "  }\n"
        "}\n"
        "a: Wrapper = Wrapper(1)\n"
        "b: Wrapper = Wrapper(1)\n"
        "c: Wrapper = Wrapper(2)\n"
        "if a != c { print(\"diff\") }\n"
        "if a != b { print(\"bad\") } else { print(\"same\") }\n"
    );
    EXPECT_EQ(out, "diff\nsame\n");
}

TEST(CodeGenE2E, DunderEqDefaultPointer) {
    // No __eq__ defined - should use pointer equality (two different instances != each other)
    auto out = compileAndRun(
        "class Node {\n"
        "  def(v: int) {\n"
        "    self.v = v\n"
        "  }\n"
        "}\n"
        "a: Node = Node(1)\n"
        "b: Node = Node(1)\n"
        "if a == b { print(\"same\") } else { print(\"diff\") }\n"
    );
    EXPECT_EQ(out, "diff\n");
}

TEST(CodeGenE2E, DunderEqPy) {
    auto out = compileAndRunPy(
        "class Money:\n"
        "    def __init__(self, amount: int):\n"
        "        self.amount = amount\n"
        "    def __eq__(self, other: Money) -> bool:\n"
        "        return self.amount == other.amount\n"
        "a: Money = Money(100)\n"
        "b: Money = Money(100)\n"
        "if a == b:\n"
        "    print(\"equal\")\n"
    );
    EXPECT_EQ(out, "equal\n");
}

TEST(CodeGenE2E, DunderLt) {
    auto out = compileAndRun(
        "class Score {\n"
        "  def(v: int) {\n"
        "    self.v = v\n"
        "  }\n"
        "  def __lt__(other: Score) -> bool {\n"
        "    return self.v < other.v\n"
        "  }\n"
        "}\n"
        "a: Score = Score(3)\n"
        "b: Score = Score(5)\n"
        "if a < b { print(\"less\") }\n"
    );
    EXPECT_EQ(out, "less\n");
}

TEST(CodeGenE2E, DunderGtFallback) {
    // __gt__ not defined, but __lt__ is - should use other.__lt__(self)
    auto out = compileAndRun(
        "class Rank {\n"
        "  def(v: int) {\n"
        "    self.v = v\n"
        "  }\n"
        "  def __lt__(other: Rank) -> bool {\n"
        "    return self.v < other.v\n"
        "  }\n"
        "}\n"
        "a: Rank = Rank(5)\n"
        "b: Rank = Rank(3)\n"
        "if a > b { print(\"greater\") }\n"
    );
    EXPECT_EQ(out, "greater\n");
}

TEST(CodeGenE2E, DunderLeFallback) {
    // __le__ not defined, but __lt__ and __eq__ are - should use __lt__ || __eq__
    auto out = compileAndRun(
        "class Val {\n"
        "  def(v: int) {\n"
        "    self.v = v\n"
        "  }\n"
        "  def __lt__(other: Val) -> bool {\n"
        "    return self.v < other.v\n"
        "  }\n"
        "  def __eq__(other: Val) -> bool {\n"
        "    return self.v == other.v\n"
        "  }\n"
        "}\n"
        "a: Val = Val(3)\n"
        "b: Val = Val(3)\n"
        "c: Val = Val(5)\n"
        "if a <= b { print(\"le1\") }\n"
        "if a <= c { print(\"le2\") }\n"
    );
    EXPECT_EQ(out, "le1\nle2\n");
}

TEST(CodeGenE2E, DunderGeFallback) {
    // __ge__ not defined, but __lt__ is - should use not __lt__
    auto out = compileAndRun(
        "class Level {\n"
        "  def(v: int) {\n"
        "    self.v = v\n"
        "  }\n"
        "  def __lt__(other: Level) -> bool {\n"
        "    return self.v < other.v\n"
        "  }\n"
        "}\n"
        "a: Level = Level(5)\n"
        "b: Level = Level(3)\n"
        "c: Level = Level(5)\n"
        "if a >= b { print(\"ge1\") }\n"
        "if a >= c { print(\"ge2\") }\n"
    );
    EXPECT_EQ(out, "ge1\nge2\n");
}

TEST(CodeGenE2E, DunderAllComparisons) {
    auto out = compileAndRun(
        "class Num {\n"
        "  def(v: int) {\n"
        "    self.v = v\n"
        "  }\n"
        "  def __eq__(other: Num) -> bool { return self.v == other.v }\n"
        "  def __ne__(other: Num) -> bool { return self.v != other.v }\n"
        "  def __lt__(other: Num) -> bool { return self.v < other.v }\n"
        "  def __gt__(other: Num) -> bool { return self.v > other.v }\n"
        "  def __le__(other: Num) -> bool { return self.v <= other.v }\n"
        "  def __ge__(other: Num) -> bool { return self.v >= other.v }\n"
        "}\n"
        "a: Num = Num(3)\n"
        "b: Num = Num(5)\n"
        "c: Num = Num(3)\n"
        "if a == c { print(\"eq\") }\n"
        "if a != b { print(\"ne\") }\n"
        "if a < b { print(\"lt\") }\n"
        "if b > a { print(\"gt\") }\n"
        "if a <= c { print(\"le\") }\n"
        "if b >= a { print(\"ge\") }\n"
    );
    EXPECT_EQ(out, "eq\nne\nlt\ngt\nle\nge\n");
}

//===----------------------------------------------------------------------===//
// Dunder Bool/Hash E2E
//===----------------------------------------------------------------------===//

TEST(CodeGenE2E, DunderBool) {
    auto out = compileAndRun(
        "class Truthy {\n"
        "  def(v: int) {\n"
        "    self.v = v\n"
        "  }\n"
        "  def __bool__() -> bool {\n"
        "    return self.v != 0\n"
        "  }\n"
        "}\n"
        "a: Truthy = Truthy(1)\n"
        "b: Truthy = Truthy(0)\n"
        "if bool(a) { print(\"a true\") }\n"
        "if bool(b) { print(\"b true\") } else { print(\"b false\") }\n"
    );
    EXPECT_EQ(out, "a true\nb false\n");
}

TEST(CodeGenE2E, DunderBoolDefault) {
    // No __bool__ - class instances default to true
    auto out = compileAndRun(
        "class Thing {\n"
        "  def() {\n"
        "    self.x = 0\n"
        "  }\n"
        "}\n"
        "t: Thing = Thing()\n"
        "if bool(t) { print(\"true\") } else { print(\"false\") }\n"
    );
    EXPECT_EQ(out, "true\n");
}

TEST(CodeGenE2E, DunderHash) {
    auto out = compileAndRun(
        "class Key {\n"
        "  def(v: int) {\n"
        "    self.v = v\n"
        "  }\n"
        "  def __hash__() -> int {\n"
        "    return self.v * 31\n"
        "  }\n"
        "}\n"
        "k: Key = Key(3)\n"
        "print(hash(k))\n"
    );
    EXPECT_EQ(out, "93\n");
}

TEST(CodeGenE2E, DunderHashDefault) {
    // No __hash__ - default is pointer-as-int (non-zero for heap-allocated object)
    auto out = compileAndRun(
        "class Obj {\n"
        "  def() {\n"
        "    self.x = 0\n"
        "  }\n"
        "}\n"
        "o: Obj = Obj()\n"
        "h: int = hash(o)\n"
        "if h != 0 { print(\"nonzero\") }\n"
    );
    EXPECT_EQ(out, "nonzero\n");
}

//===----------------------------------------------------------------------===//
// Dunder Inherited/Combined E2E
//===----------------------------------------------------------------------===//

TEST(CodeGenE2E, DunderInherited) {
    auto out = compileAndRun(
        "class Base {\n"
        "  def(v: int) {\n"
        "    self.v = v\n"
        "  }\n"
        "  def __str__() -> str {\n"
        "    return \"v=\" + str(self.v)\n"
        "  }\n"
        "}\n"
        "class Child(Base) {\n"
        "  def(v: int) {\n"
        "    self.v = v\n"
        "  }\n"
        "}\n"
        "c: Child = Child(9)\n"
        "print(c)\n"
    );
    EXPECT_EQ(out, "v=9\n");
}

TEST(CodeGenE2E, DunderCombinedPy) {
    auto out = compileAndRunPy(
        "class Frac:\n"
        "    def __init__(self, n: int, d: int):\n"
        "        self.n = n\n"
        "        self.d = d\n"
        "    def __str__(self) -> str:\n"
        "        return str(self.n) + \"/\" + str(self.d)\n"
        "    def __eq__(self, other: Frac) -> bool:\n"
        "        return self.n * other.d == self.d * other.n\n"
        "    def __lt__(self, other: Frac) -> bool:\n"
        "        return self.n * other.d < self.d * other.n\n"
        "a: Frac = Frac(1, 2)\n"
        "b: Frac = Frac(2, 4)\n"
        "c: Frac = Frac(3, 4)\n"
        "print(a)\n"
        "if a == b:\n"
        "    print(\"eq\")\n"
        "if a < c:\n"
        "    print(\"lt\")\n"
    );
    EXPECT_EQ(out, "1/2\neq\nlt\n");
}

//===----------------------------------------------------------------------===//
// Dunder Arithmetic E2E
//===----------------------------------------------------------------------===//

TEST(CodeGenE2E, DunderAdd) {
    auto out = compileAndRun(
        "class Vec {\n"
        "  def(x: int, y: int) {\n"
        "    self.x = x\n"
        "    self.y = y\n"
        "  }\n"
        "  def __add__(other: Vec) -> Vec {\n"
        "    return Vec(self.x + other.x, self.y + other.y)\n"
        "  }\n"
        "  def __str__() -> str {\n"
        "    return str(self.x) + \",\" + str(self.y)\n"
        "  }\n"
        "}\n"
        "a: Vec = Vec(1, 2)\n"
        "b: Vec = Vec(3, 4)\n"
        "c: Vec = a + b\n"
        "print(c)\n"
    );
    EXPECT_EQ(out, "4,6\n");
}

TEST(CodeGenE2E, DunderSub) {
    auto out = compileAndRun(
        "class Vec {\n"
        "  def(x: int, y: int) {\n"
        "    self.x = x\n"
        "    self.y = y\n"
        "  }\n"
        "  def __sub__(other: Vec) -> Vec {\n"
        "    return Vec(self.x - other.x, self.y - other.y)\n"
        "  }\n"
        "  def __str__() -> str {\n"
        "    return str(self.x) + \",\" + str(self.y)\n"
        "  }\n"
        "}\n"
        "a: Vec = Vec(5, 7)\n"
        "b: Vec = Vec(2, 3)\n"
        "c: Vec = a - b\n"
        "print(c)\n"
    );
    EXPECT_EQ(out, "3,4\n");
}

TEST(CodeGenE2E, DunderMul) {
    auto out = compileAndRun(
        "class Vec {\n"
        "  def(x: int, y: int) {\n"
        "    self.x = x\n"
        "    self.y = y\n"
        "  }\n"
        "  def __mul__(other: Vec) -> Vec {\n"
        "    return Vec(self.x * other.x, self.y * other.y)\n"
        "  }\n"
        "  def __str__() -> str {\n"
        "    return str(self.x) + \",\" + str(self.y)\n"
        "  }\n"
        "}\n"
        "a: Vec = Vec(2, 3)\n"
        "b: Vec = Vec(4, 5)\n"
        "c: Vec = a * b\n"
        "print(c)\n"
    );
    EXPECT_EQ(out, "8,15\n");
}

TEST(CodeGenE2E, DunderTruediv) {
    auto out = compileAndRun(
        "class Ratio {\n"
        "  def(n: int) {\n"
        "    self.n = n\n"
        "  }\n"
        "  def __truediv__(d: int) -> int {\n"
        "    return self.n // d\n"
        "  }\n"
        "}\n"
        "r: Ratio = Ratio(10)\n"
        "x: int = r / 3\n"
        "print(x)\n"
    );
    EXPECT_EQ(out, "3\n");
}

TEST(CodeGenE2E, DunderFloordiv) {
    auto out = compileAndRun(
        "class Num {\n"
        "  def(v: int) {\n"
        "    self.v = v\n"
        "  }\n"
        "  def __floordiv__(d: int) -> int {\n"
        "    return self.v // d\n"
        "  }\n"
        "}\n"
        "n: Num = Num(17)\n"
        "x: int = n // 5\n"
        "print(x)\n"
    );
    EXPECT_EQ(out, "3\n");
}

TEST(CodeGenE2E, DunderMod) {
    auto out = compileAndRun(
        "class Num {\n"
        "  def(v: int) {\n"
        "    self.v = v\n"
        "  }\n"
        "  def __mod__(d: int) -> int {\n"
        "    return self.v % d\n"
        "  }\n"
        "}\n"
        "n: Num = Num(17)\n"
        "x: int = n % 5\n"
        "print(x)\n"
    );
    EXPECT_EQ(out, "2\n");
}

TEST(CodeGenE2E, DunderPow) {
    auto out = compileAndRun(
        "class Num {\n"
        "  def(v: int) {\n"
        "    self.v = v\n"
        "  }\n"
        "  def __pow__(e: int) -> int {\n"
        "    return self.v ** e\n"
        "  }\n"
        "}\n"
        "n: Num = Num(3)\n"
        "x: int = n ** 4\n"
        "print(x)\n"
    );
    EXPECT_EQ(out, "81\n");
}

TEST(CodeGenE2E, DunderNeg) {
    auto out = compileAndRun(
        "class Vec {\n"
        "  def(x: int, y: int) {\n"
        "    self.x = x\n"
        "    self.y = y\n"
        "  }\n"
        "  def __neg__() -> Vec {\n"
        "    return Vec(0 - self.x, 0 - self.y)\n"
        "  }\n"
        "  def __str__() -> str {\n"
        "    return str(self.x) + \",\" + str(self.y)\n"
        "  }\n"
        "}\n"
        "v: Vec = Vec(3, 0 - 4)\n"
        "w: Vec = -v\n"
        "print(w)\n"
    );
    EXPECT_EQ(out, "-3,4\n");
}

TEST(CodeGenE2E, DunderPos) {
    auto out = compileAndRun(
        "class Num {\n"
        "  def(v: int) {\n"
        "    self.v = v\n"
        "  }\n"
        "  def __pos__() -> Num {\n"
        "    if self.v < 0 { return Num(0 - self.v) }\n"
        "    return Num(self.v)\n"
        "  }\n"
        "  def __str__() -> str {\n"
        "    return str(self.v)\n"
        "  }\n"
        "}\n"
        "n: Num = Num(0 - 7)\n"
        "p: Num = +n\n"
        "print(p)\n"
    );
    EXPECT_EQ(out, "7\n");
}

TEST(CodeGenE2E, DunderAbs) {
    auto out = compileAndRun(
        "class Num {\n"
        "  def(v: int) {\n"
        "    self.v = v\n"
        "  }\n"
        "  def __abs__() -> int {\n"
        "    if self.v < 0 { return 0 - self.v }\n"
        "    return self.v\n"
        "  }\n"
        "}\n"
        "n: Num = Num(0 - 42)\n"
        "print(abs(n))\n"
    );
    EXPECT_EQ(out, "42\n");
}

TEST(CodeGenE2E, DunderAddReturnsInstance) {
    auto out = compileAndRun(
        "class Vec {\n"
        "  def(x: int) {\n"
        "    self.x = x\n"
        "  }\n"
        "  def __add__(other: Vec) -> Vec {\n"
        "    return Vec(self.x + other.x)\n"
        "  }\n"
        "  def __str__() -> str {\n"
        "    return str(self.x)\n"
        "  }\n"
        "}\n"
        "a: Vec = Vec(1)\n"
        "b: Vec = Vec(2)\n"
        "c: Vec = a + b\n"
        "print(c)\n"
        "d: Vec = c + Vec(10)\n"
        "print(d)\n"
    );
    EXPECT_EQ(out, "3\n13\n");
}

TEST(CodeGenE2E, DunderAddInherited) {
    auto out = compileAndRun(
        "class Base {\n"
        "  def(v: int) {\n"
        "    self.v = v\n"
        "  }\n"
        "  def __add__(other: Base) -> Base {\n"
        "    return Base(self.v + other.v)\n"
        "  }\n"
        "  def __str__() -> str {\n"
        "    return str(self.v)\n"
        "  }\n"
        "}\n"
        "class Child(Base) {\n"
        "  def(v: int) {\n"
        "    self.v = v\n"
        "  }\n"
        "}\n"
        "a: Child = Child(3)\n"
        "b: Child = Child(7)\n"
        "c: Base = a + b\n"
        "print(c)\n"
    );
    EXPECT_EQ(out, "10\n");
}

TEST(CodeGenE2E, DunderAddPy) {
    auto out = compileAndRunPy(
        "class Vec:\n"
        "    def __init__(self, x: int, y: int):\n"
        "        self.x = x\n"
        "        self.y = y\n"
        "    def __add__(self, other: Vec) -> Vec:\n"
        "        return Vec(self.x + other.x, self.y + other.y)\n"
        "    def __str__(self) -> str:\n"
        "        return str(self.x) + \",\" + str(self.y)\n"
        "a: Vec = Vec(1, 2)\n"
        "b: Vec = Vec(3, 4)\n"
        "c: Vec = a + b\n"
        "print(c)\n"
    );
    EXPECT_EQ(out, "4,6\n");
}

TEST(CodeGenE2E, DunderArithNoFallback) {
    auto out = compileAndRun(
        "x: int = 10 + 20\n"
        "y: int = x * 3\n"
        "print(x)\n"
        "print(y)\n"
    );
    EXPECT_EQ(out, "30\n90\n");
}

TEST(CodeGenE2E, DunderMixed) {
    auto out = compileAndRun(
        "class Val {\n"
        "  def(n: int) {\n"
        "    self.n = n\n"
        "  }\n"
        "  def __add__(other: Val) -> Val {\n"
        "    return Val(self.n + other.n)\n"
        "  }\n"
        "  def __eq__(other: Val) -> bool {\n"
        "    return self.n == other.n\n"
        "  }\n"
        "  def __str__() -> str {\n"
        "    return str(self.n)\n"
        "  }\n"
        "}\n"
        "a: Val = Val(3)\n"
        "b: Val = Val(4)\n"
        "c: Val = a + b\n"
        "d: Val = Val(7)\n"
        "if c == d { print(\"equal\") }\n"
        "print(c)\n"
    );
    EXPECT_EQ(out, "equal\n7\n");
}

TEST(CodeGenE2E, DunderAddChained) {
    auto out = compileAndRun(
        "class Vec {\n"
        "  def(x: int) {\n"
        "    self.x = x\n"
        "  }\n"
        "  def __add__(other: Vec) -> Vec {\n"
        "    return Vec(self.x + other.x)\n"
        "  }\n"
        "  def __str__() -> str {\n"
        "    return str(self.x)\n"
        "  }\n"
        "}\n"
        "a: Vec = Vec(1)\n"
        "b: Vec = Vec(2)\n"
        "c: Vec = Vec(3)\n"
        "d: Vec = a + b + c\n"
        "print(d)\n"
    );
    EXPECT_EQ(out, "6\n");
}

TEST(CodeGenE2E, DunderMulScalar) {
    auto out = compileAndRun(
        "class Vec {\n"
        "  def(x: int, y: int) {\n"
        "    self.x = x\n"
        "    self.y = y\n"
        "  }\n"
        "  def __mul__(s: int) -> Vec {\n"
        "    return Vec(self.x * s, self.y * s)\n"
        "  }\n"
        "  def __str__() -> str {\n"
        "    return str(self.x) + \",\" + str(self.y)\n"
        "  }\n"
        "}\n"
        "v: Vec = Vec(2, 3)\n"
        "w: Vec = v * 5\n"
        "print(w)\n"
    );
    EXPECT_EQ(out, "10,15\n");
}

TEST(CodeGenE2E, DunderNegReturn) {
    auto out = compileAndRun(
        "class Pt {\n"
        "  def(x: int) {\n"
        "    self.x = x\n"
        "  }\n"
        "  def __neg__() -> Pt {\n"
        "    return Pt(0 - self.x)\n"
        "  }\n"
        "  def __str__() -> str {\n"
        "    return str(self.x)\n"
        "  }\n"
        "}\n"
        "p: Pt = Pt(5)\n"
        "q: Pt = -p\n"
        "print(q)\n"
    );
    EXPECT_EQ(out, "-5\n");
}

TEST(CodeGenE2E, DunderAllArithmetic) {
    auto out = compileAndRun(
        "class N {\n"
        "  def(v: int) {\n"
        "    self.v = v\n"
        "  }\n"
        "  def __add__(o: N) -> N { return N(self.v + o.v) }\n"
        "  def __sub__(o: N) -> N { return N(self.v - o.v) }\n"
        "  def __mul__(o: N) -> N { return N(self.v * o.v) }\n"
        "  def __truediv__(o: N) -> N { return N(self.v // o.v) }\n"
        "  def __floordiv__(o: N) -> N { return N(self.v // o.v) }\n"
        "  def __mod__(o: N) -> N { return N(self.v % o.v) }\n"
        "  def __pow__(o: N) -> N { return N(self.v ** o.v) }\n"
        "  def __str__() -> str { return str(self.v) }\n"
        "}\n"
        "a: N = N(10)\n"
        "b: N = N(3)\n"
        "print(a + b)\n"
        "print(a - b)\n"
        "print(a * b)\n"
        "print(a / b)\n"
        "print(a // b)\n"
        "print(a % b)\n"
        "print(N(2) ** N(8))\n"
    );
    EXPECT_EQ(out, "13\n7\n30\n3\n3\n1\n256\n");
}

TEST(CodeGenE2E, DunderAbsReturnsInt) {
    auto out = compileAndRun(
        "class Dist {\n"
        "  def(v: int) {\n"
        "    self.v = v\n"
        "  }\n"
        "  def __abs__() -> int {\n"
        "    if self.v < 0 { return 0 - self.v }\n"
        "    return self.v\n"
        "  }\n"
        "}\n"
        "d: Dist = Dist(0 - 99)\n"
        "x: int = abs(d)\n"
        "print(x + 1)\n"
    );
    EXPECT_EQ(out, "100\n");
}

//===----------------------------------------------------------------------===//
// Dunder Container E2E
//===----------------------------------------------------------------------===//

TEST(CodeGenE2E, DunderLen) {
    auto out = compileAndRun(
        "class Bag {\n"
        "  def(n: int) {\n"
        "    self.n = n\n"
        "  }\n"
        "  def __len__() -> int {\n"
        "    return self.n\n"
        "  }\n"
        "}\n"
        "b: Bag = Bag(7)\n"
        "print(len(b))\n"
    );
    EXPECT_EQ(out, "7\n");
}

TEST(CodeGenE2E, DunderLenNoRegression) {
    auto out = compileAndRun(
        "s: str = \"hello\"\n"
        "print(len(s))\n"
    );
    EXPECT_EQ(out, "5\n");
}

TEST(CodeGenE2E, DunderGetitem) {
    auto out = compileAndRun(
        "class Row {\n"
        "  def(base: int) {\n"
        "    self.base = base\n"
        "  }\n"
        "  def __getitem__(i: int) -> int {\n"
        "    return self.base + i\n"
        "  }\n"
        "}\n"
        "r: Row = Row(100)\n"
        "print(r[5])\n"
        "print(r[0])\n"
    );
    EXPECT_EQ(out, "105\n100\n");
}

TEST(CodeGenE2E, DunderSetitem) {
    auto out = compileAndRun(
        "class Store {\n"
        "  def() {\n"
        "    self.val = 0\n"
        "  }\n"
        "  def __getitem__(i: int) -> int {\n"
        "    return self.val\n"
        "  }\n"
        "  def __setitem__(i: int, v: int) -> int {\n"
        "    self.val = v\n"
        "    return 0\n"
        "  }\n"
        "}\n"
        "s: Store = Store()\n"
        "s[0] = 42\n"
        "print(s[0])\n"
    );
    EXPECT_EQ(out, "42\n");
}

TEST(CodeGenE2E, DunderContains) {
    auto out = compileAndRun(
        "class EvenSet {\n"
        "  def() {\n"
        "    self.x = 0\n"
        "  }\n"
        "  def __contains__(n: int) -> int {\n"
        "    if n % 2 == 0 { return 1 }\n"
        "    return 0\n"
        "  }\n"
        "}\n"
        "e: EvenSet = EvenSet()\n"
        "if 4 in e { print(\"yes\") }\n"
        "if 3 in e { print(\"no\") }\n"
        "print(\"done\")\n"
    );
    EXPECT_EQ(out, "yes\ndone\n");
}

TEST(CodeGenE2E, DunderContainsNot) {
    auto out = compileAndRun(
        "class NumSet {\n"
        "  def(v: int) {\n"
        "    self.v = v\n"
        "  }\n"
        "  def __contains__(n: int) -> int {\n"
        "    if n == self.v { return 1 }\n"
        "    return 0\n"
        "  }\n"
        "}\n"
        "s: NumSet = NumSet(10)\n"
        "if 10 in s { print(\"found\") }\n"
        "if 20 in s { print(\"bad\") }\n"
        "print(\"end\")\n"
    );
    EXPECT_EQ(out, "found\nend\n");
}

TEST(CodeGenE2E, DunderIter) {
    auto out = compileAndRun(
        "class Range3 {\n"
        "  def() {\n"
        "    self.i = 0\n"
        "  }\n"
        "  def __iter__() -> Range3 {\n"
        "    self.i = 0\n"
        "    return self\n"
        "  }\n"
        "  def __next__() -> int {\n"
        "    if self.i >= 3 { raise StopIteration() }\n"
        "    self.i = self.i + 1\n"
        "    return self.i\n"
        "  }\n"
        "}\n"
        "r: Range3 = Range3()\n"
        "for x in r {\n"
        "  print(x)\n"
        "}\n"
    );
    EXPECT_EQ(out, "1\n2\n3\n");
}

TEST(CodeGenE2E, DunderIterSum) {
    auto out = compileAndRun(
        "class Counter {\n"
        "  def(n: int) {\n"
        "    self.n = n\n"
        "    self.i = 0\n"
        "  }\n"
        "  def __iter__() -> Counter {\n"
        "    self.i = 0\n"
        "    return self\n"
        "  }\n"
        "  def __next__() -> int {\n"
        "    if self.i >= self.n { raise StopIteration() }\n"
        "    self.i = self.i + 1\n"
        "    return self.i\n"
        "  }\n"
        "}\n"
        "total: int = 0\n"
        "c: Counter = Counter(5)\n"
        "for x in c {\n"
        "  total = total + x\n"
        "}\n"
        "print(total)\n"
    );
    EXPECT_EQ(out, "15\n");
}

TEST(CodeGenE2E, DunderGetitemNoRegression) {
    auto out = compileAndRun(
        "items: list[int] = [10, 20, 30]\n"
        "print(items[1])\n"
    );
    EXPECT_EQ(out, "20\n");
}

TEST(CodeGenE2E, DunderContainsNoRegression) {
    auto out = compileAndRun(
        "s: str = \"hello world\"\n"
        "if \"hello\" in s { print(\"yes\") }\n"
    );
    EXPECT_EQ(out, "yes\n");
}

TEST(CodeGenE2E, DunderLenInherited) {
    auto out = compileAndRun(
        "class Base {\n"
        "  def(n: int) {\n"
        "    self.n = n\n"
        "  }\n"
        "  def __len__() -> int {\n"
        "    return self.n\n"
        "  }\n"
        "}\n"
        "class Child(Base) {\n"
        "  def(n: int) {\n"
        "    self.n = n\n"
        "  }\n"
        "}\n"
        "c: Child = Child(9)\n"
        "print(len(c))\n"
    );
    EXPECT_EQ(out, "9\n");
}

TEST(CodeGenE2E, DunderGetitemStr) {
    auto out = compileAndRun(
        "class NameMap {\n"
        "  def() {\n"
        "    self.x = 0\n"
        "  }\n"
        "  def __getitem__(i: int) -> str {\n"
        "    if i == 0 { return \"zero\" }\n"
        "    return \"other\"\n"
        "  }\n"
        "}\n"
        "m: NameMap = NameMap()\n"
        "print(m[0])\n"
        "print(m[1])\n"
    );
    EXPECT_EQ(out, "zero\nother\n");
}

TEST(CodeGenE2E, DunderSetitemMultiple) {
    auto out = compileAndRun(
        "class Arr {\n"
        "  def() {\n"
        "    self.a = 0\n"
        "    self.b = 0\n"
        "  }\n"
        "  def __setitem__(i: int, v: int) -> int {\n"
        "    if i == 0 { self.a = v }\n"
        "    if i == 1 { self.b = v }\n"
        "    return 0\n"
        "  }\n"
        "  def __getitem__(i: int) -> int {\n"
        "    if i == 0 { return self.a }\n"
        "    return self.b\n"
        "  }\n"
        "}\n"
        "a: Arr = Arr()\n"
        "a[0] = 10\n"
        "a[1] = 20\n"
        "print(a[0])\n"
        "print(a[1])\n"
    );
    EXPECT_EQ(out, "10\n20\n");
}

TEST(CodeGenE2E, DunderCombinedContainer) {
    auto out = compileAndRun(
        "class Box {\n"
        "  def(v: int) {\n"
        "    self.v = v\n"
        "  }\n"
        "  def __len__() -> int {\n"
        "    return 1\n"
        "  }\n"
        "  def __getitem__(i: int) -> int {\n"
        "    return self.v\n"
        "  }\n"
        "  def __contains__(x: int) -> int {\n"
        "    if x == self.v { return 1 }\n"
        "    return 0\n"
        "  }\n"
        "}\n"
        "b: Box = Box(42)\n"
        "print(len(b))\n"
        "print(b[0])\n"
        "if 42 in b { print(\"yes\") }\n"
    );
    EXPECT_EQ(out, "1\n42\nyes\n");
}

//===----------------------------------------------------------------------===//
// Dunder Context Manager E2E
//===----------------------------------------------------------------------===//

TEST(CodeGenE2E, DunderWithBasic) {
    auto out = compileAndRun(
        "class Ctx {\n"
        "  def() {\n"
        "    self.x = 0\n"
        "  }\n"
        "  def __enter__() -> Ctx {\n"
        "    print(\"enter\")\n"
        "    return self\n"
        "  }\n"
        "  def __exit__() -> int {\n"
        "    print(\"exit\")\n"
        "    return 0\n"
        "  }\n"
        "}\n"
        "with Ctx() as c {\n"
        "  print(\"body\")\n"
        "}\n"
    );
    EXPECT_EQ(out, "enter\nbody\nexit\n");
}

TEST(CodeGenE2E, DunderWithNoAs) {
    auto out = compileAndRun(
        "class Logger {\n"
        "  def() {\n"
        "    self.x = 0\n"
        "  }\n"
        "  def __enter__() -> Logger {\n"
        "    print(\"start\")\n"
        "    return self\n"
        "  }\n"
        "  def __exit__() -> int {\n"
        "    print(\"end\")\n"
        "    return 0\n"
        "  }\n"
        "}\n"
        "with Logger() {\n"
        "  print(\"running\")\n"
        "}\n"
    );
    EXPECT_EQ(out, "start\nrunning\nend\n");
}

TEST(CodeGenE2E, DunderWithReturnsSelf) {
    auto out = compileAndRun(
        "class MyLock {\n"
        "  def(v: int) {\n"
        "    self.v = v\n"
        "  }\n"
        "  def __enter__() -> MyLock {\n"
        "    print(\"lock\")\n"
        "    return self\n"
        "  }\n"
        "  def __exit__() -> int {\n"
        "    print(\"unlock\")\n"
        "    return 0\n"
        "  }\n"
        "  def __str__() -> str {\n"
        "    return str(self.v)\n"
        "  }\n"
        "}\n"
        "with MyLock(42) as m {\n"
        "  print(m)\n"
        "}\n"
    );
    EXPECT_EQ(out, "lock\n42\nunlock\n");
}

TEST(CodeGenE2E, DunderWithExitOnException) {
    auto out = compileAndRun(
        "class Guard {\n"
        "  def() {\n"
        "    self.x = 0\n"
        "  }\n"
        "  def __enter__() -> Guard {\n"
        "    print(\"enter\")\n"
        "    return self\n"
        "  }\n"
        "  def __exit__() -> int {\n"
        "    print(\"exit\")\n"
        "    return 0\n"
        "  }\n"
        "}\n"
        "try {\n"
        "  with Guard() as g {\n"
        "    print(\"body\")\n"
        "    raise ValueError(\"oops\")\n"
        "  }\n"
        "} except ValueError {\n"
        "  print(\"caught\")\n"
        "}\n"
    );
    EXPECT_EQ(out, "enter\nbody\nexit\ncaught\n");
}

TEST(CodeGenE2E, DunderWithInherited) {
    auto out = compileAndRun(
        "class Base {\n"
        "  def() {\n"
        "    self.x = 0\n"
        "  }\n"
        "  def __enter__() -> Base {\n"
        "    print(\"base_enter\")\n"
        "    return self\n"
        "  }\n"
        "  def __exit__() -> int {\n"
        "    print(\"base_exit\")\n"
        "    return 0\n"
        "  }\n"
        "}\n"
        "class Child(Base) {\n"
        "  def() {\n"
        "    Base()\n"
        "  }\n"
        "}\n"
        "with Child() as c {\n"
        "  print(\"inside\")\n"
        "}\n"
    );
    EXPECT_EQ(out, "base_enter\ninside\nbase_exit\n");
}

TEST(CodeGenE2E, DunderWithMultipleStatements) {
    auto out = compileAndRun(
        "class Timer {\n"
        "  def() {\n"
        "    self.x = 0\n"
        "  }\n"
        "  def __enter__() -> Timer {\n"
        "    print(\"begin\")\n"
        "    return self\n"
        "  }\n"
        "  def __exit__() -> int {\n"
        "    print(\"finish\")\n"
        "    return 0\n"
        "  }\n"
        "}\n"
        "with Timer() as t {\n"
        "  print(\"a\")\n"
        "  print(\"b\")\n"
        "  print(\"c\")\n"
        "}\n"
    );
    EXPECT_EQ(out, "begin\na\nb\nc\nfinish\n");
}

TEST(CodeGenE2E, DunderWithAfterBlock) {
    auto out = compileAndRun(
        "class Wrap {\n"
        "  def() {\n"
        "    self.x = 0\n"
        "  }\n"
        "  def __enter__() -> Wrap {\n"
        "    return self\n"
        "  }\n"
        "  def __exit__() -> int {\n"
        "    print(\"cleaned\")\n"
        "    return 0\n"
        "  }\n"
        "}\n"
        "with Wrap() as w {\n"
        "  print(\"inside\")\n"
        "}\n"
        "print(\"after\")\n"
    );
    EXPECT_EQ(out, "inside\ncleaned\nafter\n");
}

//===----------------------------------------------------------------------===//
// __call__ dunder - instances as callables
//===----------------------------------------------------------------------===//

TEST(CodeGenTest, DunderCallIR) {
    auto ir = generateIR(
        "class Multiplier {\n"
        "  def(factor: int) {\n"
        "    self.factor = factor\n"
        "  }\n"
        "  def __call__(x: int) -> int {\n"
        "    return self.factor * x\n"
        "  }\n"
        "}\n"
        "m: Multiplier = Multiplier(3)\n"
        "result: int = m(5)\n"
    );
    EXPECT_NE(ir.find("Multiplier___call__"), std::string::npos);
}

TEST(CodeGenTest, DunderCallSimple) {
    auto out = compileAndRun(
        "class Doubler {\n"
        "  def() {\n"
        "    self.x = 0\n"
        "  }\n"
        "  def __call__(n: int) -> int {\n"
        "    return n * 2\n"
        "  }\n"
        "}\n"
        "d: Doubler = Doubler()\n"
        "print(d(21))\n"
    );
    EXPECT_EQ(out, "42\n");
}

TEST(CodeGenTest, DunderCallWithState) {
    auto out = compileAndRun(
        "class Multiplier {\n"
        "  def(factor: int) {\n"
        "    self.factor = factor\n"
        "  }\n"
        "  def __call__(x: int) -> int {\n"
        "    return self.factor * x\n"
        "  }\n"
        "}\n"
        "m: Multiplier = Multiplier(3)\n"
        "print(m(5))\n"
        "print(m(10))\n"
    );
    EXPECT_EQ(out, "15\n30\n");
}

TEST(CodeGenTest, DunderCallMultipleArgs) {
    auto out = compileAndRun(
        "class Adder {\n"
        "  def() {\n"
        "    self.x = 0\n"
        "  }\n"
        "  def __call__(a: int, b: int) -> int {\n"
        "    return a + b\n"
        "  }\n"
        "}\n"
        "a: Adder = Adder()\n"
        "print(a(10, 32))\n"
    );
    EXPECT_EQ(out, "42\n");
}

TEST(CodeGenTest, DunderCallNoArgs) {
    auto out = compileAndRun(
        "class Greeter {\n"
        "  def(name: str) {\n"
        "    self.name = name\n"
        "  }\n"
        "  def __call__() -> str {\n"
        "    return self.name\n"
        "  }\n"
        "}\n"
        "g: Greeter = Greeter(\"hello\")\n"
        "print(g())\n"
    );
    EXPECT_EQ(out, "hello\n");
}

TEST(CodeGenTest, DunderCallInherited) {
    auto out = compileAndRun(
        "class Base {\n"
        "  def(n: int) {\n"
        "    self.n = n\n"
        "  }\n"
        "  def __call__() -> int {\n"
        "    return self.n\n"
        "  }\n"
        "}\n"
        "class Child(Base) {\n"
        "  def(n: int) {\n"
        "    self.n = n\n"
        "  }\n"
        "}\n"
        "c: Child = Child(99)\n"
        "print(c())\n"
    );
    EXPECT_EQ(out, "99\n");
}

TEST(CodeGenTest, DunderCallVoid) {
    auto out = compileAndRun(
        "class Printer {\n"
        "  def(msg: str) {\n"
        "    self.msg = msg\n"
        "  }\n"
        "  def __call__() {\n"
        "    print(self.msg)\n"
        "  }\n"
        "}\n"
        "p: Printer = Printer(\"fired\")\n"
        "p()\n"
    );
    EXPECT_EQ(out, "fired\n");
}

// A `__call__` returning a non-primitive, then chained-accessed inline:
// `obj()[k]` / `obj().field`. Regression for the silent miscompile where the
// call-return type wasn't propagated, so the subscript fell back to
// dragon_str_index (dict -> LLVM verify crash) / attribute read 0.
TEST(CodeGenTest, DunderCallReturningDictSubscriptedInline) {
    auto out = compileAndRun(
        "class Box {\n"
        "  _d: dict[str, int]\n"
        "  def(d: dict[str, int]) { self._d = d }\n"
        "  def __call__() -> dict[str, int] { return self._d }\n"
        "}\n"
        "b: Box = Box({\"k\": 7})\n"
        "print(b()[\"k\"])\n"
    );
    EXPECT_EQ(out, "7\n");
}

TEST(CodeGenTest, DunderCallReturningListSubscriptedInline) {
    auto out = compileAndRun(
        "class LBox {\n"
        "  _l: list[int]\n"
        "  def(l: list[int]) { self._l = l }\n"
        "  def __call__() -> list[int] { return self._l }\n"
        "}\n"
        "b: LBox = LBox([10, 20, 30])\n"
        "print(b()[2])\n"
    );
    EXPECT_EQ(out, "30\n");
}

TEST(CodeGenTest, DunderCallReturningInstanceAttributeInline) {
    auto out = compileAndRun(
        "class Point { x: int\n"
        "  def(x: int) { self.x = x } }\n"
        "class PBox {\n"
        "  _p: Point\n"
        "  def(p: Point) { self._p = p }\n"
        "  def __call__() -> Point { return self._p }\n"
        "}\n"
        "b: PBox = PBox(Point(42))\n"
        "print(b().x)\n"
    );
    EXPECT_EQ(out, "42\n");
}

// An entry-file generic class whose method/`__call__` return the class type
// parameter T, chained-accessed inline. Regression for the double-type-check
// that re-resolved the stamped class's `-> T` to Any on the second pass.
TEST(CodeGenTest, GenericCellChainedAccessInline) {
    auto out = compileAndRun(
        "class Cell[T] {\n"
        "  _value: T\n"
        "  def(initial: T) { self._value = initial }\n"
        "  def __call__() -> T { return self._value }\n"
        "  def get() -> T { return self._value }\n"
        "}\n"
        "class Point { x: int\n"
        "  def(x: int) { self.x = x } }\n"
        "sd: Cell[dict[str, int]] = Cell({\"k\": 7})\n"
        "print(sd()[\"k\"])\n"
        "print(sd.get()[\"k\"])\n"
        "sp: Cell[Point] = Cell(Point(42))\n"
        "print(sp().x)\n"
        "print(sp.get().x)\n"
        "sl: Cell[list[int]] = Cell([10, 20, 30])\n"
        "print(sl()[2])\n"
    );
    EXPECT_EQ(out, "7\n7\n42\n42\n30\n");
}
