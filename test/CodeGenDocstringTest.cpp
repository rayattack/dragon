#include "CodeGenTestHelpers.h"
#include "CodeBlock.h"

static std::string code(const std::string& block) {
    return extractCode("CodeGenDocstringTest.md", block);
}

TEST(CodeGenE2E, DocstringClassPresent) {
    auto out = compileAndRun(code("docstring_class_present"));
    EXPECT_EQ(out, "Says hello to anyone who shows up.\n");
}

TEST(CodeGenE2E, DocstringClassAbsent) {
    auto out = compileAndRun(code("docstring_class_absent"));
    EXPECT_EQ(out, "None\n");
}

TEST(CodeGenE2E, DocstringFunctionPresent) {
    auto out = compileAndRun(code("docstring_function_present"));
    EXPECT_EQ(out, "Compute n! recursively.\n");
}

TEST(CodeGenE2E, DocstringFunctionAbsent) {
    auto out = compileAndRun(code("docstring_function_absent"));
    EXPECT_EQ(out, "None\n");
}

TEST(CodeGenE2E, DocstringInstanceInheritsClass) {
    auto out = compileAndRun(code("docstring_instance_inherits_class"));
    EXPECT_EQ(out, "Class doc.\n");
}

TEST(CodeGenE2E, DocstringInstanceNoneWhenAbsent) {
    auto out = compileAndRun(code("docstring_instance_none_when_absent"));
    EXPECT_EQ(out, "None\n");
}

TEST(CodeGenE2E, DocstringIsNoneNarrowing) {
    auto out = compileAndRun(code("docstring_is_none_narrowing"));
    EXPECT_EQ(out, "A:doc\nB:none\n");
}

TEST(CodeGenE2E, DocstringMultiline) {
    auto out = compileAndRun(code("docstring_multiline"));
    EXPECT_EQ(out, "line one\n    line two\n");
}

TEST(CodeGenE2E, DocstringFStringNotLifted) {
    auto out = compileAndRun(code("docstring_f_string_not_lifted"));
    EXPECT_EQ(out, "None\n");
}

TEST(CodeGenE2E, DocstringNotFirstStmtIsNotLifted) {
    auto out = compileAndRun(code("docstring_not_first_stmt_is_not_lifted"));
    EXPECT_EQ(out, "None\n");
}

TEST(CodeGenTest, DocstringAbsentFunctionEmitsNoDocGlobal) {
    auto ir = generateIR(code("docstring_absent_function_emits_no_doc_global"));
    EXPECT_EQ(ir.find("func_doc_bare"), std::string::npos);
}

TEST(CodeGenTest, DocstringAbsentClassEmitsNullDocPtr) {
    auto ir = generateIR(code("docstring_absent_class_emits_null_doc_ptr"));
    EXPECT_NE(ir.find("dragon_class_descriptor_create"), std::string::npos);
    EXPECT_EQ(ir.find("Plain__doc"), std::string::npos);
}

TEST(CodeGenTest, DocstringPresentClassEmitsDocGlobal) {
    auto ir = generateIR(code("docstring_present_class_emits_doc_global"));
    EXPECT_NE(ir.find("C__doc"), std::string::npos);
}

TEST(CodeGenE2E, DocstringMethodViaClassChain) {
    auto out = compileAndRun(code("docstring_method_via_class_chain"));
    EXPECT_EQ(out, "method greeting.\n");
}

TEST(CodeGenE2E, DocstringMethodViaInstanceChain) {
    auto out = compileAndRun(code("docstring_method_via_instance_chain"));
    EXPECT_EQ(out, "method greeting.\n");
}

TEST(CodeGenE2E, DocstringMethodAbsentReturnsNone) {
    auto out = compileAndRun(code("docstring_method_absent_returns_none"));
    EXPECT_EQ(out, "None\n");
}

TEST(CodeGenE2E, DocstringBareNamePresent) {
    auto out = compileAndRun(code("docstring_bare_name_present"));
    EXPECT_EQ(out, "module-level doc.\n");
}

TEST(CodeGenE2E, DocstringBareNameAbsentIsNone) {
    auto out = compileAndRun(
        "print(__doc__)\n"
    );
    EXPECT_EQ(out, "None\n");
}

TEST(CodeGenE2E, DocstringBareNameInsideFunction) {
    auto out = compileAndRun(code("docstring_bare_name_inside_function"));
    EXPECT_EQ(out, "mod doc.\n");
}
