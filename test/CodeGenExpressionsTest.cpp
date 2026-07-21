#include "CodeGenTestHelpers.h"

//===----------------------------------------------------------------------===//
// Binary/Unary IR Tests
//===----------------------------------------------------------------------===//

TEST(CodeGenTest, BinaryAdd) {
    // 1 + 2 is constant-folded to 3
    auto ir = generateIR("x: int = 1 + 2");
    EXPECT_NE(ir.find("store i64 3"), std::string::npos);
}

TEST(CodeGenTest, BinaryMul) {
    // 3 * 4 is constant-folded to 12
    auto ir = generateIR("x: int = 3 * 4");
    EXPECT_NE(ir.find("store i64 12"), std::string::npos);
}

TEST(CodeGenTest, BinaryFloatAdd) {
    // 1.0 + 2.0 is constant-folded to 3.0
    auto ir = generateIR("x: float = 1.0 + 2.0");
    EXPECT_NE(ir.find("store double 3.0"), std::string::npos);
}

TEST(CodeGenTest, StringRepeatConstantFold) {
    // literal string * constant int folds to a baked literal -- no runtime
    // call. The call site is named "strrep"; the always-present declaration
    // reads "str_repeat" (underscore), so it won't false-match.
    auto ir = generateIR("s: str = \"ab\" * 3");
    EXPECT_NE(ir.find("ababab"), std::string::npos);
    EXPECT_EQ(ir.find("strrep"), std::string::npos);
}

TEST(CodeGenTest, StringRepeatRuntimeForVariableCount) {
    // A non-constant count cannot fold and must call the runtime entry point.
    auto ir = generateIR("n: int = 4\ns: str = \"x\" * n");
    EXPECT_NE(ir.find("strrep"), std::string::npos);
}

TEST(CodeGenTest, TrueDivision) {
    // 10 / 3 is constant-folded to ~3.333...
    auto ir = generateIR("x: float = 10 / 3");
    EXPECT_NE(ir.find("store double"), std::string::npos);
}

TEST(CodeGenTest, Comparison) {
    auto ir = generateIR("x: int = 5\nif x > 3 {\n  pass\n}");
    EXPECT_NE(ir.find("icmp sgt"), std::string::npos);
}

TEST(CodeGenTest, LogicalAnd) {
    auto ir = generateIR("x: int = 5\nif x > 0 and x < 10 {\n  pass\n}");
    // Should have short-circuit branching
    EXPECT_NE(ir.find("br i1"), std::string::npos);
}

TEST(CodeGenTest, UnaryMinus) {
    // -5 is constant-folded to -5
    auto ir = generateIR("x: int = -5");
    EXPECT_NE(ir.find("store i64 -5"), std::string::npos);
}

TEST(CodeGenTest, UnaryNot) {
    // not True is constant-folded to false
    auto ir = generateIR("x: bool = not True");
    EXPECT_NE(ir.find("store i1 false"), std::string::npos);
}

TEST(CodeGenTest, TernaryExpr) {
    auto ir = generateIR("x: int = 5\ny: int = 1 if x > 3 else 0");
    EXPECT_NE(ir.find("ifthen"), std::string::npos);
    EXPECT_NE(ir.find("ifelse"), std::string::npos);
}

//===----------------------------------------------------------------------===//
// Chained Comparison IR Tests
//===----------------------------------------------------------------------===//

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
    // Should have short-circuit blocks
    EXPECT_NE(ir.find("chain.next"), std::string::npos);
    EXPECT_NE(ir.find("chain.end"), std::string::npos);
}

TEST(CodeGenTest, ChainedCompTwoOperands) {
    // Two operands, one operator -- no short circuit needed, just direct comparison
    auto ir = generateIR(
        "a: int = 1\n"
        "b: int = 2\n"
        "r: bool = a < b"
    );
    // This goes through BinaryExpr, not ChainedCompExpr
    // Just verify it doesn't crash
    EXPECT_NE(ir.find("lt"), std::string::npos);
}

//===----------------------------------------------------------------------===//
// Walrus Operator IR Tests
//===----------------------------------------------------------------------===//

TEST(CodeGenTest, WalrusBasicInt) {
    auto ir = generateIR(
        "x: int = 0\n"
        "y: int = (x := 42)"
    );
    // Should contain a store of 42
    EXPECT_NE(ir.find("store"), std::string::npos);
}

//===----------------------------------------------------------------------===//
// Expression E2E Tests
//===----------------------------------------------------------------------===//

TEST(CodeGenE2E, Arithmetic) {
    auto output = compileAndRun(
        "x: int = 10\n"
        "y: int = 20\n"
        "print(x + y)\n"
        "print(x * y)"
    );
    EXPECT_EQ(output, "30\n200\n");
}

// String repetition: `str * int`, `int * str`, and `str *= int`. Regression
// for the missing STAR dispatch that multiplied the string's ADDRESS by the
// count (segfault) / emitted `mul ptr, i64` (invalid IR) for `*=`.
TEST(CodeGenE2E, StringRepeat) {
    auto output = compileAndRun(
        "n: int = 4\n"
        "print(\"*\" * 3)\n"          // str * int literal
        "print(3 * \"*\")\n"          // int * str literal
        "print(\"ab\" * n)\n"          // str * int var
        "print(n * \"-\")\n"           // int var * str
        "print(\"[\" + \"x\" * 0 + \"]\")\n"   // count 0 -> empty (Python parity)
        "print(\"[\" + \"x\" * -5 + \"]\")\n"  // negative count -> empty
        "print((\"a\" + \"b\") * 2)\n" // owned-temp operand (decref path)
    );
    EXPECT_EQ(output, "***\n***\nabababab\n----\n[]\n[]\nabab\n");
}

// String augmented repetition `s *= n` goes through the AugAssign codegen
// path, separate from BinaryExpr; it must dispatch to dragon_str_repeat too.
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
    EXPECT_EQ(output, "3.0\n2.0\n1.0\n");  // float repr keeps .0 (Python parity)
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

//===----------------------------------------------------------------------===//
// Chained Comparison E2E Tests
//===----------------------------------------------------------------------===//

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
    // Middle operand with a side effect must be evaluated exactly once,
    // not twice (which would happen if the parser desugared a < b < c into
    // a < b and b < c with b duplicated).
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
    // First comparison fails -- the chain short-circuits to false.
    // The middle operand still evaluates once (to perform the first compare),
    // but the failing path does not evaluate it again.
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
    // Regression: `and` / `or` MUST short-circuit, otherwise empty-list
    // index access or div-by-zero in the RHS would crash.
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
    // `const flag: bool = <i64-returning expr>` MUST
    // truncate the i64 to i1. Without this, the bool field stores 0
    // regardless of input (argparse flag handling corrupts exactly this way).
    // Methods like str.startswith return i64 (1/0); they need to coerce
    // when stored into a bool slot.
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

//===----------------------------------------------------------------------===//
// Walrus Operator E2E Tests
//===----------------------------------------------------------------------===//

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

//===----------------------------------------------------------------------===//
// 4.6 --check-overflow: int overflow detection via __builtin_*_overflow
//===----------------------------------------------------------------------===//

TEST(CodeGenE2E, OverflowOffSilentlyWraps) {
    // Default behavior: wraparound, no error. Preserves perf.
    auto output = compileAndRun(
        "a: int = 9000000000000000000\n"
        "b: int = a + a\n"
        "print(b)\n"
    );
    // Wraparound -- exact value depends on two's-complement; just check it ran.
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
        "try {\n"
        "    c: int = 2 ** 100\n"
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
        "    c: int = a - b\n"  // -9e18 - 1e18 = -10e18, below INT64_MIN
        "    print(c)\n"
        "} except OverflowError {\n"
        "    print(\"caught\")\n"
        "}\n",
        opts);
    EXPECT_EQ(output, "caught\n");
}

TEST(CodeGenE2E, OverflowNormalArithUnaffected) {
    // Under --check-overflow, in-bounds arithmetic still works as normal.
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
    // OverflowError is a subclass of ArithmeticError (D015 hierarchy: 22<=>20).
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

//===----------------------------------------------------------------------===//
// Regression: ternary with class-field dict subscript
//===----------------------------------------------------------------------===//
//
// Without kind tracking, `obj.field["k"] if cond else lit` triggers a PHI type
// mismatch: the class-field dict subscript falls through to dragon_dict_get
// (returning i64) while the literal branch produces ptr/double, so the merge
// PHI gets operands of different LLVM types and the compiler asserts.
// classFieldDictValueKinds feeds checkTag for the AttributeExpr case so
// both branches cross the runtime boundary at the dict's native value type.

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
    // Both ternary branches are class-field dict subscripts. The PHI must merge
    // two ptr operands without falling through to dragon_dict_get -> i64. Both
    // dicts are pre-populated via the constructor so neither branch raises
    // KeyError.
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

//===----------------------------------------------------------------------===//
// Regression: ternary list-subscript refcount aliases caller's list element
//===----------------------------------------------------------------------===//
//
// The bug: `xs[0] if cond else "lit"` mixed a borrowed list-subscript value
// with a literal in a PHI. Downstream consumers used `isBorrowedHeapExpr` to
// decide whether to incref, but `IfExpr` is not in that detector -- so the
// consumer assumed the PHI was a fresh +1 and skipped the incref. The owning
// alloca was then decref'd at scope exit, freeing the string that the caller's
// list element still pointed at. The next allocation inside the function
// (`params["x"] = "y"`) reused the freed slab, and the caller's `xs[0]` now
// observed `"y"`.
//
// Fix: inside `visit(IfExpr&)`, normalize each branch to "+1 owned" by
// emitting an incref on borrowed-heap branch values before the merge. The PHI
// then has a single, consistent ownership story and `IfExpr` joins the
// fresh-ref expressions in `isBorrowedHeapExpr`.
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

//===----------------------------------------------------------------------===//
// Regression: ternary local-dict-subscript refcount aliases dict's own value
//===----------------------------------------------------------------------===//
//
// This is the docs-server heap corruption (ADR-tracked): serving any page ran
// `host_hdr = headers["host"] if "host" in headers else ""` over a *local*
// dict. `resolveExprVarKind` resolved list subscripts but not dict subscripts,
// so it returned VarKind::Other for `headers["host"]`; visit(IfExpr&)'s
// ownership normalization (which only increfs heap kinds) then skipped the
// incref. The borrowed value was decref'd at the local's scope exit, freeing
// the string the dict still pointed at -- a double-free when the dict (here,
// the request's header dict via the Request destructor) tore down later. glibc
// only *detected* it when a large kind=4 page's body alloc triggered
// malloc_consolidate, which is why it looked size-dependent; it actually
// corrupted the heap on every request.
//
// Mirror of TernaryListSubscriptRefcountNoCallerAlias for the dict path: the
// caller owns the dict, the function reads a value through a ternary branch,
// then a same-size allocation reuses any freed slab. Post-fix the caller's
// value is intact; pre-fix it observed the reused allocation.
TEST(CodeGenE2E, TernaryLocalDictSubscriptRefcountNoAlias) {
    auto output = compileAndRun(
        "def f(h: dict[str, str]) -> None {\n"
        "    if true {\n"
        "        const got: str = h[\"host\"] if \"host\" in h else \"none\"\n"
        "        print(f\"  got=[{got}]\")\n"
        "    }\n"
        "    const filler: dict[str, str] = {}\n"
        "    filler[\"k\"] = \"REUSED!!\"\n"  // same length as "ORIGINAL" -> same slab
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

//===----------------------------------------------------------------------===//
// Regression: `key in obj.field` for class-field dicts
//===----------------------------------------------------------------------===//
//
// `in` op codegen used to detect dict-typed RHS only via NameExpr or
// DictExpr-literal -- for `"k" in r.params` it fell through to
// dragon_str_contains, which silently returned 0 and broke membership.
// Now resolveExprVarKind handles AttributeExpr -> classFieldKinds.

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
    // Bare-key dict literal sugar: {x: "FOUND"} ≡ {"x": "FOUND"}.
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
    // `key in self.field` from inside a method.
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

//===----------------------------------------------------------------------===//
// Regression: nested-def closure capture with ternary / f-string / subscript
//===----------------------------------------------------------------------===//
//
// The capture analysis pass walks the inner def's body to record outer locals
// it references. A previous miss in StringLiteral::accept didn't visit
// f-string interpolation expressions, so adding `f"{base}/x"` to the inner
// body silently dropped `base` from the capture set and codegen emitted
// `Undefined variable: base`. Tests here lock in the recursive descent.

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
    // The ASSETS-style trigger: ternary with class-field dict subscript +
    // f-string referencing a captured local. Used to fail with both
    // "Undefined variable: base" (capture miss) and a PHI mismatch (subscript
    // routing as i64 against a str literal in the ternary).
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
    // Trigger: dict field set from a typed `__init__` parameter
    // (`self.params = params`), not a literal. Without populating
    // classFieldDictValueKinds from the AssignStmt path, `obj.field["k"]`
    // routed through dragon_dict_get -> i64, PHI-mismatching the str else
    // branch in a ternary.
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

//===----------------------------------------------------------------------===//
// Python-parity str.find / rfind / count with optional start[, end].
//===----------------------------------------------------------------------===//
//
// Before this work only the 1-arg form was wired in src/codegen/CallMethods.cpp;
// the runtime had a private static `dragon_str_find_cp(start)` but no exported
// entry point with start/end window. Now codegen routes 2/3-arg calls to
// `dragon_str_<m>_se(s, sub, start, end)` (end=-1 means len(s)), and the
// runtime exports find_se / rfind_se / count_se with bounded windows.
//
// Each line below mirrors a Python-equivalent assertion for the same input
// "abcabcabc" so any drift between Dragon and CPython here will fail loudly.
TEST(CodeGenE2E, StrFindRfindCountStartEnd) {
    auto output = compileAndRun(
        "const s: str = \"abcabcabc\"\n"
        "print(s.find(\"b\"))\n"             // 1
        "print(s.find(\"b\", 2))\n"          // 4
        "print(s.find(\"b\", 5))\n"          // 7
        "print(s.find(\"b\", 5, 7))\n"       // -1 (window excludes 7)
        "print(s.find(\"b\", 5, 8))\n"       // 7
        "print(s.find(\"a\", 0, 0))\n"       // -1 (empty window)
        "print(s.find(\"z\"))\n"             // -1
        "print(s.rfind(\"b\"))\n"            // 7
        "print(s.rfind(\"b\", 0, 5))\n"      // 4
        "print(s.rfind(\"b\", 0, 2))\n"      // 1
        "print(s.count(\"b\"))\n"            // 3
        "print(s.count(\"b\", 2))\n"         // 2
        "print(s.count(\"b\", 2, 5))\n"      // 1
        "print(s.count(\"b\", 0, 0))\n"      // 0
    );
    EXPECT_EQ(output,
              "1\n4\n7\n-1\n7\n-1\n-1\n"
              "7\n4\n1\n"
              "3\n2\n1\n0\n");
}

//===----------------------------------------------------------------------===//
// UTF-8 string-literal codegen: a literal segment containing non-ASCII bytes
// (e.g. an em-dash, " -- ") MUST be emitted as a heap DragonString through
// `dragon_str_intern`, not as a raw C-string global. The bug this guards
// against: when a literal-kind-1 (raw bytes, but really UTF-8) is concatenated
// with a runtime-built kind-4 (UCS-4) string, dragon_str_concat reads the
// literal byte-by-byte as Latin-1 code points, then re-encodes the kind=4
// result back to UTF-8. Each em-dash byte (E2 80 94) becomes three U+0080..FF
// code points which re-encode to six UTF-8 bytes (C3 A2 C2 80 C2 94) -- the
// classic "double-encoded" string in HTTP responses, log output, etc.
//
// Three coverage points: a plain string-literal concat, an f-string literal
// segment, and a template { } block literal segment. All three must agree
// the result is exactly 6 bytes per em-dash pair (3 + 3) -- not 12.
//===----------------------------------------------------------------------===//

TEST(CodeGenE2E, NonAsciiLiteralConcatWithComputed) {
    // Computed value comes back from the runtime as kind=4 (UCS-4) because
    // it was built from a UTF-8 literal that the lexer normalized into a
    // heap DragonString. The concat with " -- Dragon" (also non-ASCII)
    // exercises the kind=4 + literal path.
    auto output = compileAndRun(
        "def make_dash() -> str { return \"\xe2\x80\x94\" }\n"
        "const a: str = make_dash()\n"
        "const b: str = \" \xe2\x80\x94 Dragon\"\n"
        "print(a + b)\n");
    // Expect: "--" + " -- Dragon\n" = e2 80 94 20 e2 80 94 20 44 72 61 67 6f 6e 0a
    // = 15 bytes total. Double-encoded would be 21 bytes (each em-dash -> 6).
    EXPECT_EQ(output.size(), 15u);
    EXPECT_EQ(output, std::string("\xe2\x80\x94 \xe2\x80\x94 Dragon\n", 15));
}

TEST(CodeGenE2E, NonAsciiFStringLiteralSegment) {
    auto output = compileAndRun(
        "const t: str = \"\xe2\x80\x94\"\n"
        "print(f\"x \xe2\x80\x94 {t}\")\n");
    // "x -- --\n" = 78 20 e2 80 94 20 e2 80 94 0a = 10 bytes
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
    // The template body has leading/trailing whitespace it preserves; we
    // assert that the output contains exactly one em-dash sequence
    // (3 bytes), not the 6-byte double-encoded form.
    auto pos = output.find(std::string("\xe2\x80\x94", 3));
    ASSERT_NE(pos, std::string::npos);
    // Double-encoded form must NOT appear:
    EXPECT_EQ(output.find(std::string("\xc3\xa2", 2)), std::string::npos);
}

//===----------------------------------------------------------------------===//
// D027.1: `nonlocal` opts into mutate-through closure semantics. Mutations
// in the inner function land in the SAME backing slot the owner reads, via
// a heap-allocated DragonCell. Without the `nonlocal` declaration the
// assignment would silently shadow (today's broken behavior) -- this test
// pins down the cell-promotion path: declared nonlocal -> reads chain, writes
// chain, refcounts stay balanced.
//===----------------------------------------------------------------------===//

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
    // Three increments to a nonlocal int -- verifies cell_get / cell_set
    // round-trip the i64 value cleanly and the inner sees the latest write.
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
    // The list itself is a heap object; the cell holds the list ptr.
    // append() doesn't reassign the local, but mutates the same heap
    // object -- this verifies the cell read returns the live list ptr.
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
    // Three nested fns: child mutates grandparent's `msg`. The middle
    // function `parent` doesn't reference `msg` itself -- it just relays
    // the cell ptr through its env to `child`. The relay path is
    // structurally distinct from the owner path; this pins it down.
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
    // Sanity check that scope-chain reads in the OUTER also observe the
    // mutation -- i.e. the cell isn't a one-way write conduit. Here outer
    // reads `n` after `bump()` and must see the bumped value.
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
