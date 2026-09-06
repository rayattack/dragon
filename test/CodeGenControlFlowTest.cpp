#include "CodeGenTestHelpers.h"
#include "CodeBlock.h"
#include <cctype>

static std::string code(const std::string& block) {
    return extractCode("CodeGenControlFlowTest.md", block);
}

TEST(CodeGenTest, IfStatement) {
    auto ir = generateIR("x: int = 5\nif x > 3 {\n  print(x)\n}");
    EXPECT_NE(ir.find("br i1"), std::string::npos);
    EXPECT_NE(ir.find("then"), std::string::npos);
}

TEST(CodeGenTest, IfElseStatement) {
    auto ir = generateIR(code("if_else_statement"));
    EXPECT_NE(ir.find("then"), std::string::npos);
    EXPECT_NE(ir.find("else"), std::string::npos);
}

TEST(CodeGenTest, WhileLoop) {
    auto ir = generateIR("x: int = 0\nwhile x < 5 {\n  x += 1\n}");
    EXPECT_NE(ir.find("whilecond"), std::string::npos);
    EXPECT_NE(ir.find("whilebody"), std::string::npos);
    EXPECT_NE(ir.find("whileend"), std::string::npos);
}

TEST(CodeGenTest, IfFloatConditionCoercion) {
    auto ir = generateIR("x: float = 1.0\nif x {\n  print(1)\n}");
    EXPECT_NE(ir.find("fcmp one"), std::string::npos);
}

TEST(CodeGenTest, WhileFloatConditionCoercion) {
    auto ir = generateIR("x: float = 1.0\nwhile x {\n  break\n}");
    EXPECT_NE(ir.find("fcmp one"), std::string::npos);
}

TEST(CodeGenTest, ElifFloatConditionCoercion) {
    auto ir = generateIR(code("elif_float_condition_coercion"));
    EXPECT_NE(ir.find("tobool"), std::string::npos);
}

TEST(CodeGenE2E, ElseIfChainSameAsElif) {
    auto out = compileAndRun(code("else_if_chain_same_as_elif"));
    EXPECT_EQ(out, "huge\nbig\nmedium\ntiny\nzero\n");
}

TEST(CodeGenTest, ForRange) {
    auto ir = generateIR("for i in range(10) {\n  print(i)\n}");
    EXPECT_NE(ir.find("forcond"), std::string::npos);
    EXPECT_NE(ir.find("forbody"), std::string::npos);
    EXPECT_NE(ir.find("icmp slt"), std::string::npos);
}

TEST(CodeGenTest, ForRangeStartEnd) {
    auto ir = generateIR("for i in range(2, 5) {\n  print(i)\n}");
    EXPECT_NE(ir.find("forcond"), std::string::npos);
    EXPECT_NE(ir.find("store i64 2"), std::string::npos);
}

TEST(CodeGenTest, BreakStatement) {
    auto ir = generateIR("while True {\n  break\n}");
    EXPECT_NE(ir.find("br label %whileend"), std::string::npos);
}

TEST(CodeGenTest, ContinueStatement) {
    auto ir = generateIR("x: int = 0\nwhile x < 10 {\n  x += 1\n  continue\n}");
    EXPECT_NE(ir.find("br label %whilecond"), std::string::npos);
}

TEST(CodeGenTest, ReturnStatement) {
    auto ir = generateIR("def foo() -> int {\n  return 42\n}");
    EXPECT_NE(ir.find("ret i64 42"), std::string::npos);
}

TEST(CodeGenTest, PassStatement) {
    auto ir = generateIR("pass");
    EXPECT_NE(ir.find("define i32 @main("), std::string::npos);
}

TEST(CodeGenTest, AssertStatement) {
    auto ir = generateIR("assert True");
    EXPECT_NE(ir.find("dragon_assert"), std::string::npos);
}

TEST(CodeGenTest, ForInList) {
    auto ir = generateIR("nums: list[int] = [1, 2, 3]\nfor x in nums {\n  print(x)\n}");
    EXPECT_NE(ir.find("len.gep"), std::string::npos);
    EXPECT_NE(ir.find("list.data"), std::string::npos);
    EXPECT_EQ(ir.find("call i64 @dragon_list_len"), std::string::npos);
    EXPECT_EQ(ir.find("call i64 @dragon_list_get("), std::string::npos);
}

TEST(CodeGenTest, ForInString) {
    auto ir = generateIR("s: str = \"abc\"\nfor c in s {\n  print(c)\n}");
    EXPECT_NE(ir.find("dragon_str_len"), std::string::npos);
    EXPECT_NE(ir.find("dragon_str_index"), std::string::npos);
}

TEST(CodeGenTest, ForInDictKeys) {
    auto ir = generateIR(code("for_in_dict_keys"));
    EXPECT_NE(ir.find("dragon_dict_keys"), std::string::npos)
        << "Expected dragon_dict_keys call for 'for k in d'";
    EXPECT_NE(ir.find("dragon_list_len"), std::string::npos);
    EXPECT_NE(ir.find("dragon_list_get"), std::string::npos);
}

TEST(CodeGenTest, ForInDictItems) {
    auto ir = generateIR(code("for_in_dict_items"));
    EXPECT_NE(ir.find("dragon_dict_items"), std::string::npos)
        << "Expected dragon_dict_items call for 'd.items()'";
    EXPECT_NE(ir.find("dragon_tuple_get"), std::string::npos)
        << "Expected tuple unpacking for dict items";
}

TEST(CodeGenE2E, ForRangeThreeArgs) {
    auto output = compileAndRun(code("for_range_three_args"));
    EXPECT_EQ(output, "0\n3\n6\n9\n");
}

TEST(CodeGenE2E, BreakContinue) {
    auto output = compileAndRun(code("break_continue"));
    EXPECT_EQ(output, "0\n1\n2\n4\n5\n");
}

TEST(CodeGenE2E, NestedLoops) {
    auto output = compileAndRun(code("nested_loops"));
    EXPECT_EQ(output, "0\n1\n2\n");
}

TEST(CodeGenE2E, AssertPass) {
    auto output = compileAndRun(code("assert_pass"));
    EXPECT_EQ(output, "ok\n");
}

TEST(CodeGenE2E, ForInDict) {
    auto output = compileAndRun(code("for_in_dict"));
    EXPECT_EQ(output, "x\ny\n");
}

TEST(CodeGenE2E, ForInDictKeys) {
    auto output = compileAndRun(code("for_in_dict_keys_2"));
    EXPECT_EQ(output, "a\nb\n");
}

TEST(CodeGenE2E, ForInDictItems) {
    auto output = compileAndRun(code("for_in_dict_items_2"));
    EXPECT_EQ(output, "a\n1\nb\n2\n");
}

TEST(CodeGenE2E, ForInDictValues) {
    auto output = compileAndRun(code("for_in_dict_values"));
    EXPECT_EQ(output, "100\n200\n");
}

TEST(CodeGenE2E, DeleteStmtBasic) {
    auto output = compileAndRun(code("delete_stmt_basic"));
    EXPECT_EQ(output, "ok\n");
}

TEST(CodeGenE2E, DeleteStmtString) {
    auto output = compileAndRun(code("delete_stmt_string"));
    EXPECT_EQ(output, "ok\n");
}

TEST(CodeGenTest, DeleteStmtIR) {
    auto ir = generateIR(code("delete_stmt_ir"));
    EXPECT_NE(ir.find("dragon_decref_str"), std::string::npos);
}

static CodeGenOptions releaseOptions() {
    CodeGenOptions opts;
    opts.optimizationLevel = 3;
    return opts;
}

static bool hasVectorOf(const std::string& ir, const std::string& elem) {
    const std::string lanes = " x " + elem + ">";
    size_t pos = 0;
    while ((pos = ir.find(lanes, pos)) != std::string::npos) {
        size_t start = pos;
        while (start > 0 && std::isdigit(static_cast<unsigned char>(ir[start - 1])))
            --start;
        if (start > 0 && start < pos && ir[start - 1] == '<') return true;
        pos += lanes.size();
    }
    return false;
}

TEST(CodeGenVectorize, IntSumVectorizes) {
    auto ir = generateOptimizedIR(code("vectorize_int_sum"), releaseOptions());
    EXPECT_TRUE(hasVectorOf(ir, "i64")) << ir;
}

TEST(CodeGenVectorize, FloatMapVectorizes) {
    auto ir = generateOptimizedIR(code("vectorize_float_map"), releaseOptions());
    EXPECT_TRUE(hasVectorOf(ir, "double")) << ir;
}

TEST(CodeGenVectorize, StrictFloatSumStaysScalar) {
    auto ir = generateOptimizedIR(code("vectorize_float_sum_strict"), releaseOptions());
    EXPECT_FALSE(hasVectorOf(ir, "double")) << ir;
}

TEST(CodeGenVectorize, IntSumForeachVectorizes) {
    auto ir = generateOptimizedIR(code("vectorize_int_sum_foreach"), releaseOptions());
    EXPECT_TRUE(hasVectorOf(ir, "i64")) << ir;
}

TEST(CodeGenVectorize, StrictFloatSumForeachStaysScalar) {
    auto ir = generateOptimizedIR(code("vectorize_float_sum_foreach_strict"), releaseOptions());
    EXPECT_FALSE(hasVectorOf(ir, "double")) << ir;
}

TEST(CodeGenVectorize, FloatSumFastMathVectorizes) {
    auto ir = generateOptimizedIR(code("vectorize_float_sum_fastmath"), releaseOptions());
    EXPECT_TRUE(hasVectorOf(ir, "double")) << ir;
}

TEST(CodeGenVectorize, FloatSumForeachFastMathVectorizes) {
    auto ir = generateOptimizedIR(code("vectorize_float_sum_foreach_fastmath"), releaseOptions());
    EXPECT_TRUE(hasVectorOf(ir, "double")) << ir;
}

static std::string joinLines(const std::vector<std::string>& lines) {
    std::string out;
    for (const auto& line : lines) out += line + "\n";
    return out;
}

static bool hasReportLine(const std::vector<std::string>& report,
                          const std::string& prefix, const std::string& fragment) {
    for (const auto& line : report) {
        if (line.rfind(prefix, 0) == 0 && line.find(fragment) != std::string::npos)
            return true;
    }
    return false;
}

TEST(CodeGenVectorize, ReportNamesTheVectorizedLoopLine) {
    auto report = vectorizeReportFor(code("vectorize_int_sum"), releaseOptions());
    EXPECT_TRUE(hasReportLine(report, "vectorized ", "x at <test>:3")) << joinLines(report);
}

TEST(CodeGenVectorize, ReportNamesTheFastMathOptIn) {
    auto report = vectorizeReportFor(code("vectorize_float_sum_strict"), releaseOptions());
    EXPECT_TRUE(hasReportLine(report, "not vectorized at <test>:3:", "@fastmath"))
        << joinLines(report);
}

TEST(CodeGenVectorize, ReportSaysWhenThePipelineDidNotRun) {
    CodeGenOptions opts;
    auto report = vectorizeReportFor(code("vectorize_int_sum"), opts);
    EXPECT_TRUE(hasReportLine(report, "vectorize report:", "-O2 or higher")) << joinLines(report);
}
