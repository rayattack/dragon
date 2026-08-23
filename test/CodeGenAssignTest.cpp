#include "CodeGenTestHelpers.h"
#include "CodeBlock.h"

static std::string code(const std::string& block) {
    return extractCode("CodeGenAssignTest.md", block);
}

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
    auto ir = generateIR(code("reassign_assign_emits_overwrite_decref"));
    EXPECT_NE(ir.find("x.oldrc"), std::string::npos)
        << "Expected RC overwrite load for assign rebind";
    EXPECT_GE(countSubstring(ir, "call void @dragon_decref_str"), 2u)
        << "Expected overwrite decref plus scope-exit decref for assign rebind";
}

TEST(CodeGenIR, ReassignWalrusEmitsOverwriteDecref) {
    auto ir = generateIR(code("reassign_walrus_emits_overwrite_decref"));
    EXPECT_NE(ir.find("x.oldrc"), std::string::npos)
        << "Expected RC overwrite load for walrus rebind";
    EXPECT_GE(countSubstring(ir, "call void @dragon_decref_str"), 2u)
        << "Expected overwrite decref plus scope-exit decref for walrus rebind";
}

TEST(CodeGenIR, StrAugAssignEmitsInplaceAppend) {
    auto ir = generateIR(code("str_aug_assign_emits_inplace_append"));
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
    auto output = compileAndRun(code("aug_assign_all"));
    EXPECT_EQ(output, "5\n2\n1024\n");
}

TEST(CodeGenE2E, StrAppendInplaceLoop) {
    auto output = compileAndRun(code("str_append_inplace_loop"));
    EXPECT_EQ(output, "50000\n");
}

TEST(CodeGenE2E, StrPlusEqLoop) {
    auto output = compileAndRun(code("str_plus_eq_loop"));
    EXPECT_EQ(output, "50000\n");
}

TEST(CodeGenE2E, StrAppendAliasingSafe) {
    auto output = compileAndRun(code("str_append_aliasing_safe"));
    EXPECT_EQ(output, "hello\nhello world\n");
}

TEST(CodeGenE2E, StrAppendEmptyAccumulator) {
    auto output = compileAndRun(code("str_append_empty_accumulator"));
    EXPECT_EQ(output, "first\n5\n");
}

TEST(CodeGenE2E, StrAppendKind4Fallback) {
    auto output = compileAndRun(code("str_append_kind4_fallback"));
    EXPECT_EQ(output, "abc\xc3\xa9\n4\n");
}

TEST(CodeGenE2E, StrAppendModuleGlobal) {
    auto output = compileAndRun(code("str_append_module_global"));
    EXPECT_EQ(output, "start-mid-end\n");
}

TEST(CodeGenIR, StrSelfReassignEmitsAppendInplace) {
    auto ir = generateIR(code("str_self_reassign_emits_append_inplace"));
    EXPECT_NE(ir.find("call ptr @dragon_str_append_inplace"), std::string::npos)
        << "s = s + x must lower to the in-place append";
    EXPECT_EQ(ir.find("call ptr @dragon_str_concat"), std::string::npos)
        << "s = s + x must not emit the O(n^2) fresh-concat call";
}

TEST(CodeGenE2E, DictStrIntAugAssign) {
    auto output = compileAndRun(code("dict_str_int_aug_assign"));
    EXPECT_EQ(output, "6\n");
}

TEST(CodeGenE2E, DictAugAssignFloorMod) {
    auto output = compileAndRun(code("dict_aug_assign_floor_mod"));
    EXPECT_EQ(output, "2\n");
}

TEST(CodeGenE2E, DictStrFloatAugAssign) {
    auto output = compileAndRun(code("dict_str_float_aug_assign"));
    EXPECT_EQ(output, "8.0\n");
}

TEST(CodeGenIR, DictStrIntAugEmitsFusedProbe) {
    auto ir = generateIR(code("dict_str_int_aug_emits_fused_probe"));
    EXPECT_NE(ir.find("call i64 @dragon_dict_str_iaug_i64"), std::string::npos)
        << "str-keyed int dict += must use the fused single-probe helper";
}

TEST(CodeGenE2E, ListIntAugAssign) {
    auto output = compileAndRun(code("list_int_aug_assign"));
    EXPECT_EQ(output, "6\n25\n190\n");
}

TEST(CodeGenE2E, ListFloatAugAssign) {
    auto output = compileAndRun(code("list_float_aug_assign"));
    EXPECT_EQ(output, "1.5\n5.0\n");
}

TEST(CodeGenE2E, AttributeAugAssign) {
    auto output = compileAndRun(code("attribute_aug_assign"));
    EXPECT_EQ(output, "39\n4.0\n");
}
