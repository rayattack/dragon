#include "CodeGenTestHelpers.h"
#include "CodeBlock.h"

static std::string code(const std::string& block) {
    return extractCode("CodeGenComprehensionsTest.md", block);
}

TEST(CodeGenTest, ListCompRange) {
    auto ir = generateIR("xs: list[int] = [i * 2 for i in range(5)]");
    EXPECT_NE(ir.find("dragon_list_new"), std::string::npos);
    EXPECT_NE(ir.find("dragon_list_append"), std::string::npos);
}

TEST(CodeGenTest, ListCompWithCond) {
    auto ir = generateIR("xs: list[int] = [i for i in range(10) if i > 5]");
    EXPECT_NE(ir.find("dragon_list_new"), std::string::npos);
    EXPECT_NE(ir.find("dragon_list_append"), std::string::npos);
    EXPECT_NE(ir.find("icmp sgt"), std::string::npos);
}

TEST(CodeGenE2E, ForInAndListComp) {
    auto output = compileAndRun(code("for_in_and_list_comp"));
    EXPECT_EQ(output, "10\n20\n30\n[0, 2, 4]\n");
}

TEST(CodeGenE2E, ListCompOverList) {
    auto output = compileAndRun(code("list_comp_over_list"));
    EXPECT_EQ(output, "5\n2\n10\n");
}

TEST(CodeGenE2E, ListCompOverListWithFilter) {
    auto output = compileAndRun(code("list_comp_over_list_with_filter"));
    EXPECT_EQ(output, "3\n2\n4\n6\n");
}

TEST(CodeGenE2E, SetCompOverRange) {
    auto output = compileAndRun(code("set_comp_over_range"));
    EXPECT_EQ(output, "5\n");
}

TEST(CodeGenE2E, SetCompOverRangeWithFilter) {
    auto output = compileAndRun(code("set_comp_over_range_with_filter"));
    EXPECT_EQ(output, "5\n");
}

TEST(CodeGenE2E, SetCompOverList) {
    auto output = compileAndRun(code("set_comp_over_list"));
    EXPECT_EQ(output, "3\n");
}

TEST(CodeGenE2E, DictCompOverRange) {
    auto output = compileAndRun(code("dict_comp_over_range"));
    EXPECT_EQ(output, "1\n");
}

TEST(CodeGenE2E, GeneratorOverRange) {
    auto output = compileAndRun(code("generator_over_range"));
    EXPECT_EQ(output, "4\n0\n9\n");
}

TEST(CodeGenE2E, GeneratorOverList) {
    auto output = compileAndRun(code("generator_over_list"));
    EXPECT_EQ(output, "3\n60\n");
}

TEST(CodeGenE2E, NestedListCompRange) {
    auto output = compileAndRun(code("nested_list_comp_range"));
    EXPECT_EQ(output, "6\n1\n2\n");
}

TEST(CodeGenE2E, ListCompRangeStillWorks) {
    auto output = compileAndRun(code("list_comp_range_still_works"));
    EXPECT_EQ(output, "5\n8\n");
}

TEST(CodeGenE2E, ListCompCollectionStillWorks) {
    auto output = compileAndRun(code("list_comp_collection_still_works"));
    EXPECT_EQ(output, "10\n");
}

TEST(CodeGenE2E, NestedListCompStillWorks) {
    auto output = compileAndRun(code("nested_list_comp_still_works"));
    EXPECT_EQ(output, "6\n");
}

TEST(CodeGenE2E, ListCompLoopBounded) {
    auto output = compileAndRun(code("list_comp_loop_bounded"));
    EXPECT_EQ(output, "50\n");
}

TEST(CodeGenE2E, SetCompStillWorks) {
    auto output = compileAndRun(code("set_comp_still_works"));
    EXPECT_EQ(output, "3\n");
}

TEST(CodeGenE2E, DictCompStillWorks) {
    auto output = compileAndRun(code("dict_comp_still_works"));
    EXPECT_EQ(output, "3\n");
}

TEST(CodeGenE2E, GeneratorExprStillWorks) {
    auto output = compileAndRun(code("generator_expr_still_works"));
    EXPECT_EQ(output, "5\n4\n");
}

TEST(CodeGenE2E, ListCompStrIdentity) {
    auto output = compileAndRun(code("list_comp_str_identity"));
    EXPECT_EQ(output, "alice\ncarol\n");
}

TEST(CodeGenE2E, ListCompStrMethodCall) {
    auto output = compileAndRun(code("list_comp_str_method_call"));
    EXPECT_EQ(output, "ALICE\nBOB\nCAROL\n");
}

TEST(CodeGenE2E, ListCompStrConcat) {
    auto output = compileAndRun(code("list_comp_str_concat"));
    EXPECT_EQ(output, "hi alice\nhi bob\n");
}

TEST(CodeGenE2E, ListCompStrSourcePreserved) {
    auto output = compileAndRun(code("list_comp_str_source_preserved"));
    EXPECT_EQ(output, "alice\nbob\nhi alice\nhi bob\n");
}

TEST(CodeGenE2E, ListCompStrNested) {
    auto output = compileAndRun(code("list_comp_str_nested"));
    EXPECT_EQ(output, "4\na1\nb2\n");
}

TEST(CodeGenE2E, ForInOverStrComprehension) {
    auto output = compileAndRun(code("for_in_over_str_comprehension"));
    EXPECT_EQ(output, "ALICE\nBOB\nCAROL\n");
}

TEST(CodeGenE2E, ListCompStrLoopBounded) {
    auto output = compileAndRun(code("list_comp_str_loop_bounded"));
    EXPECT_EQ(output, "3\n");
}
