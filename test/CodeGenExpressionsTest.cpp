#include "CodeGenTestHelpers.h"

TEST(CodeGenTest, BinaryAdd) {
    auto ir = generateIR("x: int = 1 + 2");
    EXPECT_NE(ir.find("store i64 3"), std::string::npos);
}

TEST(CodeGenTest, BinaryMul) {
    auto ir = generateIR("x: int = 3 * 4");
    EXPECT_NE(ir.find("store i64 12"), std::string::npos);
}

TEST(CodeGenTest, BinaryFloatAdd) {
    auto ir = generateIR("x: float = 1.0 + 2.0");
    EXPECT_NE(ir.find("store double 3.0"), std::string::npos);
}

TEST(CodeGenTest, StringRepeatConstantFold) {
    auto ir = generateIR("s: str = \"ab\" * 3");
    EXPECT_NE(ir.find("ababab"), std::string::npos);
    EXPECT_EQ(ir.find("strrep"), std::string::npos);
}

TEST(CodeGenTest, StringRepeatRuntimeForVariableCount) {
    auto ir = generateIR("n: int = 4\ns: str = \"x\" * n");
    EXPECT_NE(ir.find("strrep"), std::string::npos);
}

TEST(CodeGenTest, TrueDivision) {
    auto ir = generateIR("x: float = 10 / 3");
    EXPECT_NE(ir.find("store double"), std::string::npos);
}

TEST(CodeGenTest, Comparison) {
    auto ir = generateIR("x: int = 5\nif x > 3 {\n  pass\n}");
    EXPECT_NE(ir.find("icmp sgt"), std::string::npos);
}

TEST(CodeGenTest, LogicalAnd) {
    auto ir = generateIR("x: int = 5\nif x > 0 and x < 10 {\n  pass\n}");
    EXPECT_NE(ir.find("br i1"), std::string::npos);
}

TEST(CodeGenTest, UnaryMinus) {
    auto ir = generateIR("x: int = -5");
    EXPECT_NE(ir.find("store i64 -5"), std::string::npos);
}

TEST(CodeGenTest, UnaryNot) {
    auto ir = generateIR("x: bool = not True");
    EXPECT_NE(ir.find("store i1 false"), std::string::npos);
}

TEST(CodeGenTest, TernaryExpr) {
    auto ir = generateIR("x: int = 5\ny: int = 1 if x > 3 else 0");
    EXPECT_NE(ir.find("ifthen"), std::string::npos);
    EXPECT_NE(ir.find("ifelse"), std::string::npos);
}

TEST(CodeGenTest, ChainedCompIntLessLess) {
    auto ir = generateIR(
        "x: int = 5\n"
        "y: bool = 1 < x < 10"
    );
    EXPECT_NE(ir.find("chain.end"), std::string::npos);
    EXPECT_NE(ir.find("chain.result"), std::string::npos);
}

TEST(CodeGenTest, ChainedCompThreeOperands) {
    auto ir = generateIR(
        "a: int = 1\n"
        "b: int = 2\n"
        "c: int = 3\n"
        "r: bool = a < b < c"
    );
    EXPECT_NE(ir.find("chain.next"), std::string::npos);
    EXPECT_NE(ir.find("chain.end"), std::string::npos);
}

TEST(CodeGenTest, ChainedCompTwoOperands) {
    auto ir = generateIR(
        "a: int = 1\n"
        "b: int = 2\n"
        "r: bool = a < b"
    );
    EXPECT_NE(ir.find("lt"), std::string::npos);
}

TEST(CodeGenTest, WalrusBasicInt) {
    auto ir = generateIR(
        "x: int = 0\n"
        "y: int = (x := 42)"
    );
    EXPECT_NE(ir.find("store"), std::string::npos);
}

TEST(CodeGenE2E, Arithmetic) {
    auto output = compileAndRun(
        "x: int = 10\n"
        "y: int = 20\n"
        "print(x + y)\n"
        "print(x * y)"
    );
    EXPECT_EQ(output, "30\n200\n");
}

TEST(CodeGenE2E, StringRepeat) {
    auto output = compileAndRun(
        "n: int = 4\n"
        "print(\"*\" * 3)\n"
        "print(3 * \"*\")\n"
        "print(\"ab\" * n)\n"
        "print(n * \"-\")\n"
        "print(\"[\" + \"x\" * 0 + \"]\")\n"
        "print(\"[\" + \"x\" * -5 + \"]\")\n"
        "print((\"a\" + \"b\") * 2)\n"
    );
    EXPECT_EQ(output, "***\n***\nabababab\n----\n[]\n[]\nabab\n");
}

TEST(CodeGenE2E, StringRepeatAugmented) {
    auto output = compileAndRun(
        "s: str = \"yo\"\n"
        "s *= 3\n"
        "print(s)\n"
    );
    EXPECT_EQ(output, "yoyoyo\n");
}

TEST(CodeGenE2E, IfElseChain) {
    auto output = compileAndRun(
        "x: int = 15\n"
        "if x > 20 {\n"
        "  print(1)\n"
        "} elif x > 10 {\n"
        "  print(2)\n"
        "} else {\n"
        "  print(3)\n"
        "}"
    );
    EXPECT_EQ(output, "2\n");
}

TEST(CodeGenE2E, WhileLoop) {
    auto output = compileAndRun(
        "x: int = 0\n"
        "total: int = 0\n"
        "while x < 5 {\n"
        "  total += x\n"
        "  x += 1\n"
        "}\n"
        "print(total)"
    );
    EXPECT_EQ(output, "10\n");
}

TEST(CodeGenE2E, IfFloatTruthiness) {
    auto output = compileAndRun(
        "x: float = 1.5\n"
        "if x {\n  print(\"truthy\")\n} else {\n  print(\"falsy\")\n}"
    );
    EXPECT_EQ(output, "truthy\n");
}

TEST(CodeGenE2E, IfFloatZeroFalsy) {
    auto output = compileAndRun(
        "x: float = 0.0\n"
        "if x {\n  print(\"truthy\")\n} else {\n  print(\"falsy\")\n}"
    );
    EXPECT_EQ(output, "falsy\n");
}

TEST(CodeGenE2E, ElifFloatCondition) {
    auto output = compileAndRun(
        "x: int = 0\n"
        "y: float = 3.14\n"
        "if x {\n  print(\"x\")\n"
        "} elif y {\n  print(\"y\")\n"
        "} else {\n  print(\"none\")\n}"
    );
    EXPECT_EQ(output, "y\n");
}

TEST(CodeGenE2E, WhileFloatCondition) {
    auto output = compileAndRun(
        "x: float = 3.0\n"
        "while x {\n"
        "  print(x)\n"
        "  x = x - 1.0\n"
        "}\n"
    );
    EXPECT_EQ(output, "3.0\n2.0\n1.0\n");
}

TEST(CodeGenE2E, TernaryExpression) {
    auto output = compileAndRun(
        "x: int = 10\n"
        "y: int = 1 if x > 5 else 0\n"
        "print(y)\n"
        "z: int = 1 if x > 20 else 0\n"
        "print(z)"
    );
    EXPECT_EQ(output, "1\n0\n");
}

TEST(CodeGenE2E, ChainedCompIntAllTrue) {
    auto output = compileAndRun(
        "x: int = 5\n"
        "if 1 < x < 10 {\n"
        "    print(1)\n"
        "} else {\n"
        "    print(0)\n"
        "}"
    );
    EXPECT_EQ(output, "1\n");
}

TEST(CodeGenE2E, ChainedCompIntFalseFirst) {
    auto output = compileAndRun(
        "x: int = 15\n"
        "if 1 < x < 10 {\n"
        "    print(1)\n"
        "} else {\n"
        "    print(0)\n"
        "}"
    );
    EXPECT_EQ(output, "0\n");
}

TEST(CodeGenE2E, ChainedCompIntFalseSecond) {
    auto output = compileAndRun(
        "x: int = 0\n"
        "if 1 < x < 10 {\n"
        "    print(1)\n"
        "} else {\n"
        "    print(0)\n"
        "}"
    );
    EXPECT_EQ(output, "0\n");
}

TEST(CodeGenE2E, ChainedCompLessEqual) {
    auto output = compileAndRun(
        "x: int = 5\n"
        "if 0 <= x <= 10 {\n"
        "    print(1)\n"
        "} else {\n"
        "    print(0)\n"
        "}"
    );
    EXPECT_EQ(output, "1\n");
}

TEST(CodeGenE2E, ChainedCompEqualEqual) {
    auto output = compileAndRun(
        "x: int = 5\n"
        "if 5 == x == 5 {\n"
        "    print(1)\n"
        "} else {\n"
        "    print(0)\n"
        "}"
    );
    EXPECT_EQ(output, "1\n");
}

TEST(CodeGenE2E, ChainedCompFourOperands) {
    auto output = compileAndRun(
        "if 1 < 2 < 3 < 4 {\n"
        "    print(1)\n"
        "} else {\n"
        "    print(0)\n"
        "}"
    );
    EXPECT_EQ(output, "1\n");
}

TEST(CodeGenE2E, ChainedCompFourOperandsFail) {
    auto output = compileAndRun(
        "if 1 < 2 < 3 < 2 {\n"
        "    print(1)\n"
        "} else {\n"
        "    print(0)\n"
        "}"
    );
    EXPECT_EQ(output, "0\n");
}

TEST(CodeGenE2E, ChainedCompMixedOps) {
    auto output = compileAndRun(
        "x: int = 5\n"
        "if 0 < x <= 5 {\n"
        "    print(1)\n"
        "} else {\n"
        "    print(0)\n"
        "}"
    );
    EXPECT_EQ(output, "1\n");
}

TEST(CodeGenE2E, ChainedCompMiddleEvaluatedOnce) {
    auto output = compileAndRun(
        "counter: int = 0\n"
        "def mid() -> int {\n"
        "    global counter\n"
        "    counter = counter + 1\n"
        "    return 5\n"
        "}\n"
        "if 1 < mid() < 10 {\n"
        "    print(\"ok\")\n"
        "}\n"
        "print(counter)\n"
    );
    EXPECT_EQ(output, "ok\n1\n");
}

TEST(CodeGenE2E, ChainedCompShortCircuitsMiddleNotReevaluated) {
    auto output = compileAndRun(
        "counter: int = 0\n"
        "def mid() -> int {\n"
        "    global counter\n"
        "    counter = counter + 1\n"
        "    return 0\n"
        "}\n"
        "if 1 < mid() < 10 {\n"
        "    print(\"yes\")\n"
        "} else {\n"
        "    print(\"no\")\n"
        "}\n"
        "print(counter)\n"
    );
    EXPECT_EQ(output, "no\n1\n");
}

TEST(CodeGenE2E, AndOrShortCircuitSkipsUnsafeRhs) {
    auto out = compileAndRun(
        "def access_first(xs: list[int]) -> bool {\n"
        "    return len(xs) > 0 and xs[0] > 0\n"
        "}\n"
        "def divide_safe(x: int, y: int) -> bool {\n"
        "    return y != 0 and (x // y) > 0\n"
        "}\n"
        "def or_short(x: int) -> bool {\n"
        "    return x > 100 or (1000000 // (x - x)) > 0\n"
        "}\n"
        "print(access_first([]))\n"
        "print(access_first([5]))\n"
        "print(access_first([-1]))\n"
        "print(divide_safe(10, 2))\n"
        "print(divide_safe(10, 0))\n"
        "print(or_short(200))\n"
    );
    EXPECT_EQ(out, "False\nTrue\nFalse\nTrue\nFalse\nTrue\n");
}

TEST(CodeGenE2E, BoolAssignFromI64ReturningExpr) {
    auto out = compileAndRun(
        "def flag(name: str, ty: str) -> bool {\n"
        "    const opt: bool = name.startswith(\"-\")\n"
        "    const ck: bool = ty == \"bool\"\n"
        "    const f: bool = opt and ck\n"
        "    return f\n"
        "}\n"
        "print(flag(\"--debug\", \"bool\"))\n"
        "print(flag(\"port\",     \"bool\"))\n"
        "print(flag(\"--debug\", \"int\"))\n"
    );
    EXPECT_EQ(out, "True\nFalse\nFalse\n");
}

TEST(CodeGenE2E, WalrusAssignAndUse) {
    auto output = compileAndRun(
        "y: int = (x := 42)\n"
        "print(x)\n"
        "print(y)"
    );
    EXPECT_EQ(output, "42\n42\n");
}

TEST(CodeGenE2E, WalrusInIfCondition) {
    auto output = compileAndRun(
        "x: int = 10\n"
        "if (n := x) > 5 {\n"
        "    print(n)\n"
        "} else {\n"
        "    print(0)\n"
        "}"
    );
    EXPECT_EQ(output, "10\n");
}

TEST(CodeGenE2E, OverflowOffSilentlyWraps) {
    auto output = compileAndRun(
        "a: int = 9000000000000000000\n"
        "b: int = a + a\n"
        "print(b)\n"
    );
    EXPECT_NE(output.find("\n"), std::string::npos);
    EXPECT_TRUE(output.find("Overflow") == std::string::npos);
}

TEST(CodeGenE2E, OverflowAddCaught) {
    CodeGenOptions opts; opts.checkOverflow = true;
    auto output = compileAndRun(
        "a: int = 9000000000000000000\n"
        "try {\n"
        "    b: int = a + a\n"
        "    print(b)\n"
        "} except OverflowError {\n"
        "    print(\"caught\")\n"
        "}\n",
        opts);
    EXPECT_EQ(output, "caught\n");
}

TEST(CodeGenE2E, OverflowMulCaught) {
    CodeGenOptions opts; opts.checkOverflow = true;
    auto output = compileAndRun(
        "a: int = 9000000000000000000\n"
        "try {\n"
        "    b: int = a * 3\n"
        "    print(b)\n"
        "} except OverflowError {\n"
        "    print(\"caught\")\n"
        "}\n",
        opts);
    EXPECT_EQ(output, "caught\n");
}

TEST(CodeGenE2E, OverflowPowCaught) {
    CodeGenOptions opts; opts.checkOverflow = true;
    auto output = compileAndRun(
        "b: int = 2\n"
        "e: int = 100\n"
        "try {\n"
        "    c: int = b ** e\n"
        "    print(c)\n"
        "} except OverflowError {\n"
        "    print(\"caught\")\n"
        "}\n",
        opts);
    EXPECT_EQ(output, "caught\n");
}

TEST(CodeGenE2E, OverflowSubCaught) {
    CodeGenOptions opts; opts.checkOverflow = true;
    auto output = compileAndRun(
        "a: int = -9000000000000000000\n"
        "b: int = 1000000000000000000\n"
        "try {\n"
        "    c: int = a - b\n"
        "    print(c)\n"
        "} except OverflowError {\n"
        "    print(\"caught\")\n"
        "}\n",
        opts);
    EXPECT_EQ(output, "caught\n");
}

TEST(CodeGenE2E, OverflowNormalArithUnaffected) {
    CodeGenOptions opts; opts.checkOverflow = true;
    auto output = compileAndRun(
        "a: int = 100\n"
        "b: int = 200\n"
        "print(a + b)\n"
        "print(a * b)\n"
        "print(a - b)\n"
        "print(2 ** 10)\n",
        opts);
    EXPECT_EQ(output, "300\n20000\n-100\n1024\n");
}

TEST(CodeGenE2E, OverflowCaughtByArithmeticErrorParent) {
    CodeGenOptions opts; opts.checkOverflow = true;
    auto output = compileAndRun(
        "a: int = 9000000000000000000\n"
        "try {\n"
        "    b: int = a + a\n"
        "    print(b)\n"
        "} except ArithmeticError {\n"
        "    print(\"caught_arith\")\n"
        "}\n",
        opts);
    EXPECT_EQ(output, "caught_arith\n");
}

TEST(CodeGenE2E, TernaryClassFieldDictSubscriptStrThenBranch) {
    auto output = compileAndRun(
        "class Req {\n"
        "    def() { self.params: dict[str, str] = {} }\n"
        "}\n"
        "def serve(req: Req) -> None {\n"
        "    const has: bool = \"*\" in req.params\n"
        "    const wc: str = req.params[\"*\"] if has else \"fallback\"\n"
        "    print(wc)\n"
        "}\n"
        "serve(Req())\n");
    EXPECT_EQ(output, "fallback\n");
}

TEST(CodeGenE2E, TernaryClassFieldDictSubscriptStrElseBranch) {
    auto output = compileAndRun(
        "class Req {\n"
        "    def() { self.params: dict[str, str] = {} }\n"
        "}\n"
        "def serve(req: Req) -> None {\n"
        "    const has: bool = \"*\" in req.params\n"
        "    const wc: str = \"default\" if not has else req.params[\"*\"]\n"
        "    print(wc)\n"
        "}\n"
        "serve(Req())\n");
    EXPECT_EQ(output, "default\n");
}

TEST(CodeGenE2E, TernaryClassFieldDictSubscriptStrBothBranches) {
    auto output = compileAndRun(
        "class Req {\n"
        "    def() {\n"
        "        self.params: dict[str, str] = {\"x\": \"FROM_PARAMS\"}\n"
        "        self.headers: dict[str, str] = {\"y\": \"FROM_HEADERS\"}\n"
        "    }\n"
        "}\n"
        "def serve(req: Req, pick_p: bool) -> None {\n"
        "    const v: str = req.params[\"x\"] if pick_p else req.headers[\"y\"]\n"
        "    print(v)\n"
        "}\n"
        "serve(Req(), True)\n"
        "serve(Req(), False)\n");
    EXPECT_EQ(output, "FROM_PARAMS\nFROM_HEADERS\n");
}

TEST(CodeGenE2E, TernaryClassFieldDictSubscriptIntValue) {
    auto output = compileAndRun(
        "class Req {\n"
        "    def() { self.counts: dict[str, int] = {} }\n"
        "}\n"
        "def serve(req: Req) -> None {\n"
        "    const has: bool = \"x\" in req.counts\n"
        "    const v: int = req.counts[\"x\"] if has else 0\n"
        "    print(v)\n"
        "}\n"
        "serve(Req())\n");
    EXPECT_EQ(output, "0\n");
}

TEST(CodeGenE2E, TernaryClassFieldDictSubscriptFloatValue) {
    auto output = compileAndRun(
        "class Cfg {\n"
        "    def() { self.tunables: dict[str, float] = {} }\n"
        "}\n"
        "def lookup(c: Cfg) -> None {\n"
        "    const has: bool = \"rate\" in c.tunables\n"
        "    const v: float = c.tunables[\"rate\"] if has else 1.5\n"
        "    print(v)\n"
        "}\n"
        "lookup(Cfg())\n");
    EXPECT_EQ(output, "1.5\n");
}

TEST(CodeGenE2E, TernaryClassFieldDictSubscriptInstanceValue) {
    auto output = compileAndRun(
        "class Item {\n"
        "    def(n: str) { self.name = n }\n"
        "}\n"
        "class Reg {\n"
        "    def() { self.items: dict[str, Item] = {} }\n"
        "}\n"
        "def lookup(r: Reg, fallback: Item) -> None {\n"
        "    const has: bool = \"k\" in r.items\n"
        "    const it: Item = r.items[\"k\"] if has else fallback\n"
        "    print(it.name)\n"
        "}\n"
        "lookup(Reg(), Item(\"miss\"))\n");
    EXPECT_EQ(output, "miss\n");
}

TEST(CodeGenE2E, TernaryListSubscriptRefcountNoCallerAlias) {
    auto output = compileAndRun(
        "def f(types: list[str]) -> dict[str, str] {\n"
        "    const t: str = types[0] if 0 < len(types) else \"str\"\n"
        "    print(f\"  t=[{t}]\")\n"
        "    const params: dict[str, str] = {}\n"
        "    params[\"x\"] = \"y\"\n"
        "    return params\n"
        "}\n"
        "const types: list[str] = [\"int\"]\n"
        "const a: dict[str, str] = f(types)\n"
        "print(f\"after a: types[0]=[{types[0]}]\")\n"
        "const b: dict[str, str] = f(types)\n"
        "print(f\"after b: types[0]=[{types[0]}]\")\n");
    EXPECT_EQ(output,
              "  t=[int]\n"
              "after a: types[0]=[int]\n"
              "  t=[int]\n"
              "after b: types[0]=[int]\n");
}

// The docs-server heap corruption: `d["k"] if c else ""` over a local dict resolved VarKind::Other,
// skipped the IfExpr incref, and scope exit freed the dict's still-held value (double-free on teardown).
TEST(CodeGenE2E, TernaryLocalDictSubscriptRefcountNoAlias) {
    auto output = compileAndRun(
        "def f(h: dict[str, str]) -> None {\n"
        "    if true {\n"
        "        const got: str = h[\"host\"] if \"host\" in h else \"none\"\n"
        "        print(f\"  got=[{got}]\")\n"
        "    }\n"
        "    const filler: dict[str, str] = {}\n"
        "    filler[\"k\"] = \"REUSED!!\"\n"
        "}\n"
        "const h: dict[str, str] = {}\n"
        "h[\"host\"] = \"ORIGINAL\"\n"
        "f(h)\n"
        "const after: str = h[\"host\"]\n"
        "print(f\"after=[{after}]\")\n");
    EXPECT_EQ(output,
              "  got=[ORIGINAL]\n"
              "after=[ORIGINAL]\n");
}

TEST(CodeGenE2E, InOpClassFieldDictMembership) {
    auto output = compileAndRun(
        "class Req {\n"
        "    def() { self.params: dict[str, str] = {\"x\": \"FOUND\"} }\n"
        "}\n"
        "def main() -> None {\n"
        "    const r: Req = Req()\n"
        "    print(\"x\" in r.params)\n"
        "    print(\"y\" in r.params)\n"
        "}\n"
        "main()\n");
    EXPECT_EQ(output, "True\nFalse\n");
}

TEST(CodeGenE2E, InOpClassFieldDictBareKeys) {
    auto output = compileAndRun(
        "class Req {\n"
        "    def() { self.params: dict[str, str] = {x: \"FOUND\"} }\n"
        "}\n"
        "def main() -> None {\n"
        "    const r: Req = Req()\n"
        "    print(\"x\" in r.params)\n"
        "    print(\"y\" in r.params)\n"
        "}\n"
        "main()\n");
    EXPECT_EQ(output, "True\nFalse\n");
}

TEST(CodeGenE2E, InOpSelfFieldDictMembership) {
    auto output = compileAndRun(
        "class Store {\n"
        "    def() { self.data: dict[str, str] = {\"a\": \"1\"} }\n"
        "    def has(k: str) -> bool {\n"
        "        return k in self.data\n"
        "    }\n"
        "}\n"
        "const s: Store = Store()\n"
        "print(s.has(\"a\"))\n"
        "print(s.has(\"b\"))\n");
    EXPECT_EQ(output, "True\nFalse\n");
}

TEST(CodeGenE2E, NestedDefCapturesViaTernary) {
    auto output = compileAndRun(
        "def outer() -> None {\n"
        "    const base: str = \"BASE\"\n"
        "    def inner(flag: bool) -> None {\n"
        "        const v: str = base if flag else \"alt\"\n"
        "        print(v)\n"
        "    }\n"
        "    inner(True)\n"
        "    inner(False)\n"
        "}\n"
        "outer()\n");
    EXPECT_EQ(output, "BASE\nalt\n");
}

TEST(CodeGenE2E, NestedDefCapturesViaFString) {
    auto output = compileAndRun(
        "def outer() -> None {\n"
        "    const base: str = \"BASE\"\n"
        "    def inner() -> None {\n"
        "        const s: str = f\"prefix={base}\"\n"
        "        print(s)\n"
        "    }\n"
        "    inner()\n"
        "}\n"
        "outer()\n");
    EXPECT_EQ(output, "prefix=BASE\n");
}

TEST(CodeGenE2E, NestedDefCapturesViaSubscript) {
    auto output = compileAndRun(
        "class Bag {\n"
        "    def() { self.kv: dict[str, str] = {\"k\": \"V\"} }\n"
        "}\n"
        "def outer() -> None {\n"
        "    const base: str = \"k\"\n"
        "    def inner(b: Bag) -> None {\n"
        "        const v: str = b.kv[base]\n"
        "        print(v)\n"
        "    }\n"
        "    inner(Bag())\n"
        "}\n"
        "outer()\n");
    EXPECT_EQ(output, "V\n");
}

TEST(CodeGenE2E, NestedDefCapturesViaTernaryAndFString) {
    auto output = compileAndRun(
        "class Req {\n"
        "    def() { self.params: dict[str, str] = {\"*\": \"sub/path\"} }\n"
        "}\n"
        "class App {\n"
        "    def() {}\n"
        "    def assets(folder: str) -> None {\n"
        "        const base: str = folder\n"
        "        def serve(req: Req) -> None {\n"
        "            const wc: str = req.params[\"*\"] if \"*\" in req.params else \"\"\n"
        "            const fp: str = f\"{base}/{wc}\"\n"
        "            print(fp)\n"
        "        }\n"
        "        serve(Req())\n"
        "    }\n"
        "}\n"
        "const a: App = App()\n"
        "a.assets(\"public\")\n");
    EXPECT_EQ(output, "public/sub/path\n");
}

TEST(CodeGenE2E, NestedDefCapturesViaTernaryAndFStringEmptyDict) {
    auto output = compileAndRun(
        "class Req {\n"
        "    def() { self.params: dict[str, str] = {} }\n"
        "}\n"
        "class App {\n"
        "    def() {}\n"
        "    def assets(folder: str) -> None {\n"
        "        const base: str = folder\n"
        "        def serve(req: Req) -> None {\n"
        "            const wc: str = req.params[\"*\"] if \"*\" in req.params else \"\"\n"
        "            const fp: str = f\"{base}/{wc}\"\n"
        "            print(fp)\n"
        "        }\n"
        "        serve(Req())\n"
        "    }\n"
        "}\n"
        "const a: App = App()\n"
        "a.assets(\"public\")\n");
    EXPECT_EQ(output, "public/\n");
}

TEST(CodeGenE2E, NestedDefDictMembershipFromInitParam) {
    auto output = compileAndRun(
        "class Req {\n"
        "    def(params: dict[str, str]) { self.params = params }\n"
        "}\n"
        "def outer() -> None {\n"
        "    def serve(req: Req) -> None {\n"
        "        const wc: str = req.params[\"*\"] if \"*\" in req.params else \"miss\"\n"
        "        print(wc)\n"
        "    }\n"
        "    serve(Req({\"*\": \"hit\"}))\n"
        "    serve(Req({}))\n"
        "}\n"
        "outer()\n");
    EXPECT_EQ(output, "hit\nmiss\n");
}

TEST(CodeGenE2E, StrFindRfindCountStartEnd) {
    auto output = compileAndRun(
        "const s: str = \"abcabcabc\"\n"
        "print(s.find(\"b\"))\n"
        "print(s.find(\"b\", 2))\n"
        "print(s.find(\"b\", 5))\n"
        "print(s.find(\"b\", 5, 7))\n"
        "print(s.find(\"b\", 5, 8))\n"
        "print(s.find(\"a\", 0, 0))\n"
        "print(s.find(\"z\"))\n"
        "print(s.rfind(\"b\"))\n"
        "print(s.rfind(\"b\", 0, 5))\n"
        "print(s.rfind(\"b\", 0, 2))\n"
        "print(s.count(\"b\"))\n"
        "print(s.count(\"b\", 2))\n"
        "print(s.count(\"b\", 2, 5))\n"
        "print(s.count(\"b\", 0, 0))\n"
    );
    EXPECT_EQ(output,
              "1\n4\n7\n-1\n7\n-1\n-1\n"
              "7\n4\n1\n"
              "3\n2\n1\n0\n");
}

TEST(CodeGenE2E, NonAsciiLiteralConcatWithComputed) {
    auto output = compileAndRun(
        "def make_dash() -> str { return \"\xe2\x80\x94\" }\n"
        "const a: str = make_dash()\n"
        "const b: str = \" \xe2\x80\x94 Dragon\"\n"
        "print(a + b)\n");
    EXPECT_EQ(output.size(), 15u);
    EXPECT_EQ(output, std::string("\xe2\x80\x94 \xe2\x80\x94 Dragon\n", 15));
}

TEST(CodeGenE2E, NonAsciiFStringLiteralSegment) {
    auto output = compileAndRun(
        "const t: str = \"\xe2\x80\x94\"\n"
        "print(f\"x \xe2\x80\x94 {t}\")\n");
    EXPECT_EQ(output.size(), 10u);
    EXPECT_EQ(output, std::string("x \xe2\x80\x94 \xe2\x80\x94\n", 10));
}

TEST(CodeGenE2E, NonAsciiTemplateLiteralSegment) {
    auto output = compileAndRun(
        "def render(t: str) -> str {\n"
        "    return template { <h1>!{t} \xe2\x80\x94 Dragon</h1> }\n"
        "}\n"
        "const t: str = \"Title\"\n"
        "print(render(t))\n");
    auto pos = output.find(std::string("\xe2\x80\x94", 3));
    ASSERT_NE(pos, std::string::npos);
    EXPECT_EQ(output.find(std::string("\xc3\xa2", 2)), std::string::npos);
}

TEST(CodeGenE2E, NonlocalStrMutation) {
    auto output = compileAndRun(
        "def outer() -> str {\n"
        "    s: str = \"a\"\n"
        "    def inner() -> None {\n"
        "        nonlocal s\n"
        "        s = s + \"b\"\n"
        "    }\n"
        "    inner()\n"
        "    return s\n"
        "}\n"
        "print(outer())\n");
    EXPECT_EQ(output, "ab\n");
}

TEST(CodeGenE2E, NonlocalIntCounterAcrossCalls) {
    auto output = compileAndRun(
        "def counter() -> int {\n"
        "    n: int = 0\n"
        "    def bump() -> None {\n"
        "        nonlocal n\n"
        "        n = n + 1\n"
        "    }\n"
        "    bump()\n"
        "    bump()\n"
        "    bump()\n"
        "    return n\n"
        "}\n"
        "print(counter())\n");
    EXPECT_EQ(output, "3\n");
}

TEST(CodeGenE2E, NonlocalListAppendThroughClosure) {
    auto output = compileAndRun(
        "def collect() -> list[str] {\n"
        "    items: list[str] = []\n"
        "    def push(s: str) -> None {\n"
        "        nonlocal items\n"
        "        items.append(s)\n"
        "    }\n"
        "    push(\"x\")\n"
        "    push(\"y\")\n"
        "    push(\"z\")\n"
        "    return items\n"
        "}\n"
        "const r: list[str] = collect()\n"
        "print(r[0])\n"
        "print(r[1])\n"
        "print(r[2])\n");
    EXPECT_EQ(output, "x\ny\nz\n");
}

TEST(CodeGenE2E, NonlocalMultiLevelTransitiveCapture) {
    auto output = compileAndRun(
        "def grandparent() -> str {\n"
        "    msg: str = \"hi\"\n"
        "    def parent() -> None {\n"
        "        def child() -> None {\n"
        "            nonlocal msg\n"
        "            msg = msg + \"!\"\n"
        "        }\n"
        "        child()\n"
        "    }\n"
        "    parent()\n"
        "    return msg\n"
        "}\n"
        "print(grandparent())\n");
    EXPECT_EQ(output, "hi!\n");
}

TEST(CodeGenE2E, NonlocalReadsChainAfterMutation) {
    auto output = compileAndRun(
        "def driver() -> None {\n"
        "    n: int = 10\n"
        "    def bump() -> None {\n"
        "        nonlocal n\n"
        "        n = n * 2\n"
        "    }\n"
        "    print(n)\n"
        "    bump()\n"
        "    print(n)\n"
        "    bump()\n"
        "    print(n)\n"
        "}\n"
        "driver()\n");
    EXPECT_EQ(output, "10\n20\n40\n");
}
