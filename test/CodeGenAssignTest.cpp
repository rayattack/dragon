#include "CodeGenTestHelpers.h"

TEST(CodeGenTest, IntegerAssignment) {
    auto ir = generateIR("x: int = 42");
    EXPECT_NE(ir.find("@global.x"), std::string::npos);
    EXPECT_NE(ir.find("store i64 42"), std::string::npos);
}

TEST(CodeGenTest, FloatAssignment) {
    auto ir = generateIR("x: float = 3.14");
    EXPECT_NE(ir.find("@global.x"), std::string::npos);
    EXPECT_NE(ir.find("store double"), std::string::npos);
}

TEST(CodeGenTest, StringAssignment) {
    auto ir = generateIR("s: str = \"hello\"");
    EXPECT_NE(ir.find("@global.s"), std::string::npos);
    EXPECT_NE(ir.find("hello"), std::string::npos);
}

TEST(CodeGenTest, AugAssign) {
    auto ir = generateIR("x: int = 0\nx += 5");
    EXPECT_NE(ir.find("add"), std::string::npos);
}

TEST(CodeGenTest, AugAssignFloorDiv) {
    auto ir = generateIR("x: int = 10\nx //= 3");
    EXPECT_NE(ir.find("dragon_floordiv_int"), std::string::npos);
}

TEST(CodeGenTest, AugAssignModulo) {
    auto ir = generateIR("x: int = 10\nx %= 3");
    EXPECT_NE(ir.find("dragon_mod_int"), std::string::npos);
}

TEST(CodeGenTest, AugAssignPower) {
    auto ir = generateIR("x: int = 2\nx **= 3");
    EXPECT_NE(ir.find("dragon_pow_int"), std::string::npos);
}

TEST(CodeGenTest, AugAssignBitwiseAnd) {
    auto ir = generateIR("x: int = 15\nx &= 7");
    EXPECT_NE(ir.find("and"), std::string::npos);
}

TEST(CodeGenTest, AugAssignBitwiseOr) {
    auto ir = generateIR("x: int = 5\nx |= 3");
    EXPECT_NE(ir.find("or"), std::string::npos);
}

TEST(CodeGenTest, AugAssignBitwiseXor) {
    auto ir = generateIR("x: int = 5\nx ^= 3");
    EXPECT_NE(ir.find("xor"), std::string::npos);
}

TEST(CodeGenTest, AugAssignLeftShift) {
    auto ir = generateIR("x: int = 1\nx <<= 3");
    EXPECT_NE(ir.find("shl"), std::string::npos);
}

TEST(CodeGenTest, AugAssignRightShift) {
    auto ir = generateIR("x: int = 16\nx >>= 2");
    EXPECT_NE(ir.find("ashr"), std::string::npos);
}

TEST(CodeGenTest, AugAssignStringConcat) {
    auto ir = generateIR("s: str = \"hello\"\ns += \" world\"");
    EXPECT_NE(ir.find("dragon_str_concat"), std::string::npos);
}

TEST(CodeGenIR, ReassignAssignEmitsOverwriteDecref) {
    auto ir = generateIR(
        "def reassign_assign() {\n"
        "  x: str = \"a\".upper()\n"
        "  x = \"b\".upper()\n"
        "}\n"
    );
    EXPECT_NE(ir.find("x.oldrc"), std::string::npos)
        << "Expected RC overwrite load for assign rebind";
    EXPECT_GE(countSubstring(ir, "call void @dragon_decref_str"), 2u)
        << "Expected overwrite decref plus scope-exit decref for assign rebind";
}

TEST(CodeGenIR, ReassignWalrusEmitsOverwriteDecref) {
    auto ir = generateIR(
        "def reassign_walrus() {\n"
        "  x: str = \"a\".upper()\n"
        "  if (x := \"b\".upper()) == \"B\" {\n"
        "    pass\n"
        "  }\n"
        "}\n"
    );
    EXPECT_NE(ir.find("x.oldrc"), std::string::npos)
        << "Expected RC overwrite load for walrus rebind";
    EXPECT_GE(countSubstring(ir, "call void @dragon_decref_str"), 2u)
        << "Expected overwrite decref plus scope-exit decref for walrus rebind";
}

TEST(CodeGenIR, StrAugAssignEmitsInplaceAppend) {
    auto ir = generateIR(
        "def reassign_augassign() {\n"
        "  s: str = \"a\".upper()\n"
        "  s += \"b\".upper()\n"
        "}\n"
    );
    EXPECT_NE(ir.find("call ptr @dragon_str_append_inplace"), std::string::npos)
        << "Expected str += to lower to the in-place append entry point";
    EXPECT_EQ(ir.find("call ptr @dragon_str_concat"), std::string::npos)
        << "str += must not emit a fresh concat call (that is the O(n^2) path)";
    EXPECT_EQ(ir.find("s.oldrc"), std::string::npos)
        << "In-place append plain-stores; the old-value decref moved into the "
           "runtime, so no overwrite-decref load should appear";
    EXPECT_EQ(countSubstring(ir, "call void @dragon_decref_str"), 2u)
        << "Expected exactly two decrefs: the owned rhs intermediate plus the "
           "scope-exit decref of the accumulator (balanced, no leak)";
}

TEST(CodeGenE2E, AugAssignAll) {
    auto output = compileAndRun(
        "x: int = 17\n"
        "x //= 3\n"
        "print(x)\n"
        "y: int = 17\n"
        "y %= 5\n"
        "print(y)\n"
        "z: int = 2\n"
        "z **= 10\n"
        "print(z)"
    );
    EXPECT_EQ(output, "5\n2\n1024\n");
}

TEST(CodeGenE2E, StrAppendInplaceLoop) {
    auto output = compileAndRun(
        "s: str = \"\"\n"
        "i: int = 0\n"
        "while i < 10000 {\n"
        "    s = s + \"hello\"\n"
        "    i = i + 1\n"
        "}\n"
        "print(len(s))\n"
    );
    EXPECT_EQ(output, "50000\n");
}

TEST(CodeGenE2E, StrPlusEqLoop) {
    auto output = compileAndRun(
        "s: str = \"\"\n"
        "i: int = 0\n"
        "while i < 10000 {\n"
        "    s += \"hello\"\n"
        "    i = i + 1\n"
        "}\n"
        "print(len(s))\n"
    );
    EXPECT_EQ(output, "50000\n");
}

TEST(CodeGenE2E, StrAppendAliasingSafe) {
    auto output = compileAndRun(
        "s: str = \"hello\"\n"
        "t: str = s\n"
        "s = s + \" world\"\n"
        "print(t)\n"
        "print(s)\n"
    );
    EXPECT_EQ(output, "hello\nhello world\n");
}

TEST(CodeGenE2E, StrAppendEmptyAccumulator) {
    auto output = compileAndRun(
        "s: str = \"\"\n"
        "s = s + \"first\"\n"
        "print(s)\n"
        "print(len(s))\n"
    );
    EXPECT_EQ(output, "first\n5\n");
}

TEST(CodeGenE2E, StrAppendKind4Fallback) {
    auto output = compileAndRun(
        "a: str = \"abc\"\n"
        "a = a + \"\xc3\xa9\"\n"
        "print(a)\n"
        "print(len(a))\n"
    );
    EXPECT_EQ(output, "abc\xc3\xa9\n4\n");
}

TEST(CodeGenE2E, StrAppendModuleGlobal) {
    auto output = compileAndRun(
        "g: str = \"start\"\n"
        "def build() {\n"
        "    global g\n"
        "    g = g + \"-mid\"\n"
        "    g += \"-end\"\n"
        "}\n"
        "build()\n"
        "print(g)\n"
    );
    EXPECT_EQ(output, "start-mid-end\n");
}

TEST(CodeGenIR, StrSelfReassignEmitsAppendInplace) {
    auto ir = generateIR(
        "s: str = \"\"\n"
        "i: int = 0\n"
        "while i < 10 {\n"
        "    s = s + \"x\"\n"
        "    i = i + 1\n"
        "}\n"
        "print(len(s))\n"
    );
    EXPECT_NE(ir.find("call ptr @dragon_str_append_inplace"), std::string::npos)
        << "s = s + x must lower to the in-place append";
    EXPECT_EQ(ir.find("call ptr @dragon_str_concat"), std::string::npos)
        << "s = s + x must not emit the O(n^2) fresh-concat call";
}

TEST(CodeGenE2E, DictStrIntAugAssign) {
    auto output = compileAndRun(
        "d: dict[str, int] = {}\n"
        "d[\"a\"] = 10\n"
        "d[\"a\"] += 5\n"
        "d[\"a\"] -= 3\n"
        "d[\"a\"] *= 4\n"
        "d[\"a\"] %= 7\n"
        "print(d[\"a\"])\n"
    );
    EXPECT_EQ(output, "6\n");
}

TEST(CodeGenE2E, DictAugAssignFloorMod) {
    auto output = compileAndRun(
        "d: dict[str, int] = {}\n"
        "d[\"k\"] = -7\n"
        "d[\"k\"] %= 3\n"
        "print(d[\"k\"])\n"
    );
    EXPECT_EQ(output, "2\n");
}

TEST(CodeGenE2E, DictStrFloatAugAssign) {
    auto output = compileAndRun(
        "f: dict[str, float] = {}\n"
        "f[\"x\"] = 2.5\n"
        "f[\"x\"] += 1.5\n"
        "f[\"x\"] *= 2.0\n"
        "print(f[\"x\"])\n"
    );
    EXPECT_EQ(output, "8.0\n");
}

TEST(CodeGenIR, DictStrIntAugEmitsFusedProbe) {
    auto ir = generateIR(
        "d: dict[str, int] = {}\n"
        "d[\"a\"] = 1\n"
        "d[\"a\"] += 5\n"
    );
    EXPECT_NE(ir.find("call i64 @dragon_dict_str_iaug_i64"), std::string::npos)
        << "str-keyed int dict += must use the fused single-probe helper";
}

TEST(CodeGenE2E, ListIntAugAssign) {
    auto output = compileAndRun(
        "a: list[int] = [10, 20, 30]\n"
        "a[1] += 5\n"
        "a[0] -= 4\n"
        "a[2] *= 3\n"
        "a[-1] += 100\n"
        "print(a[0])\n"
        "print(a[1])\n"
        "print(a[2])\n"
    );
    EXPECT_EQ(output, "6\n25\n190\n");
}

TEST(CodeGenE2E, ListFloatAugAssign) {
    auto output = compileAndRun(
        "f: list[float] = [1.0, 2.0]\n"
        "f[0] += 0.5\n"
        "f[1] *= 2.5\n"
        "print(f[0])\n"
        "print(f[1])\n"
    );
    EXPECT_EQ(output, "1.5\n5.0\n");
}

TEST(CodeGenE2E, AttributeAugAssign) {
    auto output = compileAndRun(
        "class Counter {\n"
        "    n: int\n"
        "    total: float\n"
        "    def() {\n"
        "        self.n = 10\n"
        "        self.total = 2.5\n"
        "    }\n"
        "    def bump() {\n"
        "        self.n += 5\n"
        "        self.total += 1.5\n"
        "    }\n"
        "}\n"
        "c: Counter = Counter()\n"
        "c.n += 5\n"
        "c.bump()\n"
        "c.n *= 2\n"
        "c.n -= 1\n"
        "print(c.n)\n"
        "print(c.total)\n"
    );
    EXPECT_EQ(output, "39\n4.0\n");
}
