#include "CodeGenTestHelpers.h"
#include "CodeBlock.h"

static std::string code(const std::string& block) {
    return extractCode("CodeGenExpressionsTest.md", block);
}

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
    auto ir = generateIR(code("chained_comp_int_less_less"));
    EXPECT_NE(ir.find("chain.end"), std::string::npos);
    EXPECT_NE(ir.find("chain.result"), std::string::npos);
}

TEST(CodeGenTest, ChainedCompThreeOperands) {
    auto ir = generateIR(code("chained_comp_three_operands"));
    EXPECT_NE(ir.find("chain.next"), std::string::npos);
    EXPECT_NE(ir.find("chain.end"), std::string::npos);
}

TEST(CodeGenTest, ChainedCompTwoOperands) {
    auto ir = generateIR(code("chained_comp_two_operands"));
    EXPECT_NE(ir.find("lt"), std::string::npos);
}

TEST(CodeGenTest, WalrusBasicInt) {
    auto ir = generateIR(code("walrus_basic_int"));
    EXPECT_NE(ir.find("store"), std::string::npos);
}

TEST(CodeGenE2E, Arithmetic) {
    auto output = compileAndRun(code("arithmetic"));
    EXPECT_EQ(output, "30\n200\n");
}

TEST(CodeGenE2E, StringRepeat) {
    auto output = compileAndRun(code("string_repeat"));
    EXPECT_EQ(output, "***\n***\nabababab\n----\n[]\n[]\nabab\n");
}

TEST(CodeGenE2E, StringRepeatAugmented) {
    auto output = compileAndRun(code("string_repeat_augmented"));
    EXPECT_EQ(output, "yoyoyo\n");
}

TEST(CodeGenE2E, IfElseChain) {
    auto output = compileAndRun(code("if_else_chain"));
    EXPECT_EQ(output, "2\n");
}

TEST(CodeGenE2E, WhileLoop) {
    auto output = compileAndRun(code("while_loop"));
    EXPECT_EQ(output, "10\n");
}

TEST(CodeGenE2E, IfFloatTruthiness) {
    auto output = compileAndRun(code("if_float_truthiness"));
    EXPECT_EQ(output, "truthy\n");
}

TEST(CodeGenE2E, IfFloatZeroFalsy) {
    auto output = compileAndRun(code("if_float_zero_falsy"));
    EXPECT_EQ(output, "falsy\n");
}

TEST(CodeGenE2E, ElifFloatCondition) {
    auto output = compileAndRun(code("elif_float_condition"));
    EXPECT_EQ(output, "y\n");
}

TEST(CodeGenE2E, WhileFloatCondition) {
    auto output = compileAndRun(code("while_float_condition"));
    EXPECT_EQ(output, "3.0\n2.0\n1.0\n");
}

TEST(CodeGenE2E, TernaryExpression) {
    auto output = compileAndRun(code("ternary_expression"));
    EXPECT_EQ(output, "1\n0\n");
}

TEST(CodeGenE2E, ChainedCompIntAllTrue) {
    auto output = compileAndRun(code("chained_comp_int_all_true"));
    EXPECT_EQ(output, "1\n");
}

TEST(CodeGenE2E, ChainedCompIntFalseFirst) {
    auto output = compileAndRun(code("chained_comp_int_false_first"));
    EXPECT_EQ(output, "0\n");
}

TEST(CodeGenE2E, ChainedCompIntFalseSecond) {
    auto output = compileAndRun(code("chained_comp_int_false_second"));
    EXPECT_EQ(output, "0\n");
}

TEST(CodeGenE2E, ChainedCompLessEqual) {
    auto output = compileAndRun(code("chained_comp_less_equal"));
    EXPECT_EQ(output, "1\n");
}

TEST(CodeGenE2E, ChainedCompEqualEqual) {
    auto output = compileAndRun(code("chained_comp_equal_equal"));
    EXPECT_EQ(output, "1\n");
}

TEST(CodeGenE2E, ChainedCompFourOperands) {
    auto output = compileAndRun(code("chained_comp_four_operands"));
    EXPECT_EQ(output, "1\n");
}

TEST(CodeGenE2E, ChainedCompFourOperandsFail) {
    auto output = compileAndRun(code("chained_comp_four_operands_fail"));
    EXPECT_EQ(output, "0\n");
}

TEST(CodeGenE2E, ChainedCompMixedOps) {
    auto output = compileAndRun(code("chained_comp_mixed_ops"));
    EXPECT_EQ(output, "1\n");
}

TEST(CodeGenE2E, ChainedCompMiddleEvaluatedOnce) {
    auto output = compileAndRun(code("chained_comp_middle_evaluated_once"));
    EXPECT_EQ(output, "ok\n1\n");
}

TEST(CodeGenE2E, ChainedCompShortCircuitsMiddleNotReevaluated) {
    auto output = compileAndRun(code("chained_comp_short_circuits_middle_not_reevaluated"));
    EXPECT_EQ(output, "no\n1\n");
}

TEST(CodeGenE2E, AndOrShortCircuitSkipsUnsafeRhs) {
    auto out = compileAndRun(code("and_or_short_circuit_skips_unsafe_rhs"));
    EXPECT_EQ(out, "False\nTrue\nFalse\nTrue\nFalse\nTrue\n");
}

TEST(CodeGenE2E, BoolAssignFromI64ReturningExpr) {
    auto out = compileAndRun(code("bool_assign_from_i64_returning_expr"));
    EXPECT_EQ(out, "True\nFalse\nFalse\n");
}

TEST(CodeGenE2E, WalrusAssignAndUse) {
    auto output = compileAndRun(code("walrus_assign_and_use"));
    EXPECT_EQ(output, "42\n42\n");
}

TEST(CodeGenE2E, WalrusInIfCondition) {
    auto output = compileAndRun(code("walrus_in_if_condition"));
    EXPECT_EQ(output, "10\n");
}

TEST(CodeGenE2E, OverflowOffSilentlyWraps) {
    auto output = compileAndRun(code("overflow_off_silently_wraps"));
    EXPECT_NE(output.find("\n"), std::string::npos);
    EXPECT_TRUE(output.find("Overflow") == std::string::npos);
}

TEST(CodeGenE2E, OverflowAddCaught) {
    CodeGenOptions opts; opts.checkOverflow = true;
    auto output = compileAndRun(
        code("overflow_add_caught"),
        opts);
    EXPECT_EQ(output, "caught\n");
}

TEST(CodeGenE2E, OverflowMulCaught) {
    CodeGenOptions opts; opts.checkOverflow = true;
    auto output = compileAndRun(
        code("overflow_mul_caught"),
        opts);
    EXPECT_EQ(output, "caught\n");
}

TEST(CodeGenE2E, OverflowPowCaught) {
    CodeGenOptions opts; opts.checkOverflow = true;
    auto output = compileAndRun(
        code("overflow_pow_caught"),
        opts);
    EXPECT_EQ(output, "caught\n");
}

TEST(CodeGenE2E, OverflowSubCaught) {
    CodeGenOptions opts; opts.checkOverflow = true;
    auto output = compileAndRun(
        code("overflow_sub_caught"),
        opts);
    EXPECT_EQ(output, "caught\n");
}

TEST(CodeGenE2E, OverflowNormalArithUnaffected) {
    CodeGenOptions opts; opts.checkOverflow = true;
    auto output = compileAndRun(
        code("overflow_normal_arith_unaffected"),
        opts);
    EXPECT_EQ(output, "300\n20000\n-100\n1024\n");
}

TEST(CodeGenE2E, OverflowCaughtByArithmeticErrorParent) {
    CodeGenOptions opts; opts.checkOverflow = true;
    auto output = compileAndRun(
        code("overflow_caught_by_arithmetic_error_parent"),
        opts);
    EXPECT_EQ(output, "caught_arith\n");
}

TEST(CodeGenE2E, TernaryClassFieldDictSubscriptStrThenBranch) {
    auto output = compileAndRun(code("ternary_class_field_dict_subscript_str_then_branch"));
    EXPECT_EQ(output, "fallback\n");
}

TEST(CodeGenE2E, TernaryClassFieldDictSubscriptStrElseBranch) {
    auto output = compileAndRun(code("ternary_class_field_dict_subscript_str_else_branch"));
    EXPECT_EQ(output, "default\n");
}

TEST(CodeGenE2E, TernaryClassFieldDictSubscriptStrBothBranches) {
    auto output = compileAndRun(code("ternary_class_field_dict_subscript_str_both_branches"));
    EXPECT_EQ(output, "FROM_PARAMS\nFROM_HEADERS\n");
}

TEST(CodeGenE2E, TernaryClassFieldDictSubscriptIntValue) {
    auto output = compileAndRun(code("ternary_class_field_dict_subscript_int_value"));
    EXPECT_EQ(output, "0\n");
}

TEST(CodeGenE2E, TernaryClassFieldDictSubscriptFloatValue) {
    auto output = compileAndRun(code("ternary_class_field_dict_subscript_float_value"));
    EXPECT_EQ(output, "1.5\n");
}

TEST(CodeGenE2E, TernaryClassFieldDictSubscriptInstanceValue) {
    auto output = compileAndRun(code("ternary_class_field_dict_subscript_instance_value"));
    EXPECT_EQ(output, "miss\n");
}

TEST(CodeGenE2E, TernaryListSubscriptRefcountNoCallerAlias) {
    auto output = compileAndRun(code("ternary_list_subscript_refcount_no_caller_alias"));
    EXPECT_EQ(output,
              "  t=[int]\n"
              "after a: types[0]=[int]\n"
              "  t=[int]\n"
              "after b: types[0]=[int]\n");
}

// The docs-server heap corruption: `d["k"] if c else ""` over a local dict resolved VarKind::Other,
// skipped the IfExpr incref, and scope exit freed the dict's still-held value (double-free on teardown).
TEST(CodeGenE2E, TernaryLocalDictSubscriptRefcountNoAlias) {
    auto output = compileAndRun(code("ternary_local_dict_subscript_refcount_no_alias"));
    EXPECT_EQ(output,
              "  got=[ORIGINAL]\n"
              "after=[ORIGINAL]\n");
}

TEST(CodeGenE2E, InOpClassFieldDictMembership) {
    auto output = compileAndRun(code("in_op_class_field_dict_membership"));
    EXPECT_EQ(output, "True\nFalse\n");
}

TEST(CodeGenE2E, InOpClassFieldDictBareKeys) {
    auto output = compileAndRun(code("in_op_class_field_dict_bare_keys"));
    EXPECT_EQ(output, "True\nFalse\n");
}

TEST(CodeGenE2E, InOpSelfFieldDictMembership) {
    auto output = compileAndRun(code("in_op_self_field_dict_membership"));
    EXPECT_EQ(output, "True\nFalse\n");
}

TEST(CodeGenE2E, NestedDefCapturesViaTernary) {
    auto output = compileAndRun(code("nested_def_captures_via_ternary"));
    EXPECT_EQ(output, "BASE\nalt\n");
}

TEST(CodeGenE2E, NestedDefCapturesViaFString) {
    auto output = compileAndRun(code("nested_def_captures_via_f_string"));
    EXPECT_EQ(output, "prefix=BASE\n");
}

TEST(CodeGenE2E, NestedDefCapturesViaSubscript) {
    auto output = compileAndRun(code("nested_def_captures_via_subscript"));
    EXPECT_EQ(output, "V\n");
}

TEST(CodeGenE2E, NestedDefCapturesViaTernaryAndFString) {
    auto output = compileAndRun(code("nested_def_captures_via_ternary_and_f_string"));
    EXPECT_EQ(output, "public/sub/path\n");
}

TEST(CodeGenE2E, NestedDefCapturesViaTernaryAndFStringEmptyDict) {
    auto output = compileAndRun(code("nested_def_captures_via_ternary_and_f_string_empty_dict"));
    EXPECT_EQ(output, "public/\n");
}

TEST(CodeGenE2E, NestedDefDictMembershipFromInitParam) {
    auto output = compileAndRun(code("nested_def_dict_membership_from_init_param"));
    EXPECT_EQ(output, "hit\nmiss\n");
}

TEST(CodeGenE2E, StrFindRfindCountStartEnd) {
    auto output = compileAndRun(code("str_find_rfind_count_start_end"));
    EXPECT_EQ(output,
              "1\n4\n7\n-1\n7\n-1\n-1\n"
              "7\n4\n1\n"
              "3\n2\n1\n0\n");
}

TEST(CodeGenE2E, NonAsciiLiteralConcatWithComputed) {
    auto output = compileAndRun(code("non_ascii_literal_concat_with_computed"));
    EXPECT_EQ(output.size(), 15u);
    EXPECT_EQ(output, std::string("\xe2\x80\x94 \xe2\x80\x94 Dragon\n", 15));
}

TEST(CodeGenE2E, NonAsciiFStringLiteralSegment) {
    auto output = compileAndRun(code("non_ascii_f_string_literal_segment"));
    EXPECT_EQ(output.size(), 10u);
    EXPECT_EQ(output, std::string("x \xe2\x80\x94 \xe2\x80\x94\n", 10));
}

TEST(CodeGenE2E, NonAsciiTemplateLiteralSegment) {
    auto output = compileAndRun(code("non_ascii_template_literal_segment"));
    auto pos = output.find(std::string("\xe2\x80\x94", 3));
    ASSERT_NE(pos, std::string::npos);
    EXPECT_EQ(output.find(std::string("\xc3\xa2", 2)), std::string::npos);
}

TEST(CodeGenE2E, NonlocalStrMutation) {
    auto output = compileAndRun(code("nonlocal_str_mutation"));
    EXPECT_EQ(output, "ab\n");
}

TEST(CodeGenE2E, NonlocalIntCounterAcrossCalls) {
    auto output = compileAndRun(code("nonlocal_int_counter_across_calls"));
    EXPECT_EQ(output, "3\n");
}

TEST(CodeGenE2E, NonlocalListAppendThroughClosure) {
    auto output = compileAndRun(code("nonlocal_list_append_through_closure"));
    EXPECT_EQ(output, "x\ny\nz\n");
}

TEST(CodeGenE2E, NonlocalMultiLevelTransitiveCapture) {
    auto output = compileAndRun(code("nonlocal_multi_level_transitive_capture"));
    EXPECT_EQ(output, "hi!\n");
}

TEST(CodeGenE2E, NonlocalReadsChainAfterMutation) {
    auto output = compileAndRun(code("nonlocal_reads_chain_after_mutation"));
    EXPECT_EQ(output, "10\n20\n40\n");
}
