#include "CodeGenTestHelpers.h"
#include "CodeBlock.h"

static std::string code(const std::string& block) {
    return extractCode("CodeGenDundersTest.md", block);
}

TEST(CodeGenTest, DunderStrIR) {
    auto ir = generateIR(code("dunder_str_ir"));
    EXPECT_NE(ir.find("Foo___str__"), std::string::npos);
    EXPECT_EQ(ir.find("Foo instance"), std::string::npos);
}

TEST(CodeGenTest, DunderEqIR) {
    auto ir = generateIR(code("dunder_eq_ir"));
    EXPECT_NE(ir.find("Bar___eq__"), std::string::npos);
}

TEST(CodeGenTest, DunderHashIR) {
    auto ir = generateIR(code("dunder_hash_ir"));
    EXPECT_NE(ir.find("Key___hash__"), std::string::npos);
}

TEST(CodeGenTest, DunderBoolIR) {
    auto ir = generateIR(code("dunder_bool_ir"));
    EXPECT_NE(ir.find("Flag___bool__"), std::string::npos);
}

TEST(CodeGenTest, DunderAddIR) {
    auto ir = generateIR(code("dunder_add_ir"));
    EXPECT_NE(ir.find("Vec___add__"), std::string::npos);
}

TEST(CodeGenTest, DunderSubIR) {
    auto ir = generateIR(code("dunder_sub_ir"));
    EXPECT_NE(ir.find("Vec___sub__"), std::string::npos);
}

TEST(CodeGenTest, DunderMulIR) {
    auto ir = generateIR(code("dunder_mul_ir"));
    EXPECT_NE(ir.find("Vec___mul__"), std::string::npos);
}

TEST(CodeGenTest, DunderNegIR) {
    auto ir = generateIR(code("dunder_neg_ir"));
    EXPECT_NE(ir.find("Vec___neg__"), std::string::npos);
}

TEST(CodeGenTest, DunderAbsIR) {
    auto ir = generateIR(code("dunder_abs_ir"));
    EXPECT_NE(ir.find("Num___abs__"), std::string::npos);
}

TEST(CodeGenTest, DunderLenIR) {
    auto ir = generateIR(code("dunder_len_ir"));
    EXPECT_NE(ir.find("Bag___len__"), std::string::npos);
}

TEST(CodeGenTest, DunderGetitemIR) {
    auto ir = generateIR(code("dunder_getitem_ir"));
    EXPECT_NE(ir.find("Row___getitem__"), std::string::npos);
}

TEST(CodeGenTest, DunderSetitemIR) {
    auto ir = generateIR(code("dunder_setitem_ir"));
    EXPECT_NE(ir.find("Grid___setitem__"), std::string::npos);
}

TEST(CodeGenTest, DunderContainsIR) {
    auto ir = generateIR(code("dunder_contains_ir"));
    EXPECT_NE(ir.find("Bag___contains__"), std::string::npos);
}

TEST(CodeGenTest, DunderIterIR) {
    auto ir = generateIR(code("dunder_iter_ir"));
    EXPECT_NE(ir.find("Counter___iter__"), std::string::npos);
    EXPECT_NE(ir.find("Counter___next__"), std::string::npos);
}

TEST(CodeGenTest, DunderEnterExitIR) {
    auto ir = generateIR(code("dunder_enter_exit_ir"));
    EXPECT_NE(ir.find("Ctx___enter__"), std::string::npos);
    EXPECT_NE(ir.find("Ctx___exit__"), std::string::npos);
}

TEST(CodeGenE2E, DunderStrPrint) {
    auto out = compileAndRun(code("dunder_str_print"));
    EXPECT_EQ(out, "Point(3, 4)\n");
}

TEST(CodeGenE2E, DunderStrConversion) {
    auto out = compileAndRun(code("dunder_str_conversion"));
    EXPECT_EQ(out, "Color(255)\n");
}

TEST(CodeGenE2E, DunderStrFString) {
    auto out = compileAndRun(code("dunder_str_f_string"));
    EXPECT_EQ(out, "value=#42\n");
}

TEST(CodeGenE2E, DunderStrFallbackRepr) {
    auto out = compileAndRun(code("dunder_str_fallback_repr"));
    EXPECT_EQ(out, "Box(7)\n");
}

TEST(CodeGenE2E, DunderStrPy) {
    auto out = compileAndRunPy(code("dunder_str_py"));
    EXPECT_EQ(out, "Hello #42\n");
}

TEST(CodeGenE2E, DunderRepr) {
    auto out = compileAndRun(code("dunder_repr"));
    EXPECT_EQ(out, "Num(5)\n");
}

TEST(CodeGenE2E, DunderEq) {
    auto out = compileAndRun(code("dunder_eq"));
    EXPECT_EQ(out, "equal\nnot equal\n");
}

TEST(CodeGenE2E, DunderNeFallback) {
    auto out = compileAndRun(code("dunder_ne_fallback"));
    EXPECT_EQ(out, "diff\n");
}

TEST(CodeGenE2E, DunderNeExplicit) {
    auto out = compileAndRun(code("dunder_ne_explicit"));
    EXPECT_EQ(out, "diff\nsame\n");
}

TEST(CodeGenE2E, DunderEqDefaultPointer) {
    auto out = compileAndRun(code("dunder_eq_default_pointer"));
    EXPECT_EQ(out, "diff\n");
}

TEST(CodeGenE2E, DunderEqPy) {
    auto out = compileAndRunPy(code("dunder_eq_py"));
    EXPECT_EQ(out, "equal\n");
}

TEST(CodeGenE2E, DunderLt) {
    auto out = compileAndRun(code("dunder_lt"));
    EXPECT_EQ(out, "less\n");
}

TEST(CodeGenE2E, DunderGtFallback) {
    auto out = compileAndRun(code("dunder_gt_fallback"));
    EXPECT_EQ(out, "greater\n");
}

TEST(CodeGenE2E, DunderLeFallback) {
    auto out = compileAndRun(code("dunder_le_fallback"));
    EXPECT_EQ(out, "le1\nle2\n");
}

TEST(CodeGenE2E, DunderGeFallback) {
    auto out = compileAndRun(code("dunder_ge_fallback"));
    EXPECT_EQ(out, "ge1\nge2\n");
}

TEST(CodeGenE2E, DunderAllComparisons) {
    auto out = compileAndRun(code("dunder_all_comparisons"));
    EXPECT_EQ(out, "eq\nne\nlt\ngt\nle\nge\n");
}

TEST(CodeGenE2E, DunderBool) {
    auto out = compileAndRun(code("dunder_bool"));
    EXPECT_EQ(out, "a true\nb false\n");
}

TEST(CodeGenE2E, DunderBoolDefault) {
    auto out = compileAndRun(code("dunder_bool_default"));
    EXPECT_EQ(out, "true\n");
}

TEST(CodeGenE2E, DunderHash) {
    auto out = compileAndRun(code("dunder_hash"));
    EXPECT_EQ(out, "93\n");
}

TEST(CodeGenE2E, DunderHashDefault) {
    auto out = compileAndRun(code("dunder_hash_default"));
    EXPECT_EQ(out, "nonzero\n");
}

TEST(CodeGenE2E, DunderInherited) {
    auto out = compileAndRun(code("dunder_inherited"));
    EXPECT_EQ(out, "v=9\n");
}

TEST(CodeGenE2E, DunderCombinedPy) {
    auto out = compileAndRunPy(code("dunder_combined_py"));
    EXPECT_EQ(out, "1/2\neq\nlt\n");
}

TEST(CodeGenE2E, DunderAdd) {
    auto out = compileAndRun(code("dunder_add"));
    EXPECT_EQ(out, "4,6\n");
}

TEST(CodeGenE2E, DunderSub) {
    auto out = compileAndRun(code("dunder_sub"));
    EXPECT_EQ(out, "3,4\n");
}

TEST(CodeGenE2E, DunderMul) {
    auto out = compileAndRun(code("dunder_mul"));
    EXPECT_EQ(out, "8,15\n");
}

TEST(CodeGenE2E, DunderTruediv) {
    auto out = compileAndRun(code("dunder_truediv"));
    EXPECT_EQ(out, "3\n");
}

TEST(CodeGenE2E, DunderFloordiv) {
    auto out = compileAndRun(code("dunder_floordiv"));
    EXPECT_EQ(out, "3\n");
}

TEST(CodeGenE2E, DunderMod) {
    auto out = compileAndRun(code("dunder_mod"));
    EXPECT_EQ(out, "2\n");
}

TEST(CodeGenE2E, DunderPow) {
    auto out = compileAndRun(code("dunder_pow"));
    EXPECT_EQ(out, "81\n");
}

TEST(CodeGenE2E, DunderNeg) {
    auto out = compileAndRun(code("dunder_neg"));
    EXPECT_EQ(out, "-3,4\n");
}

TEST(CodeGenE2E, DunderPos) {
    auto out = compileAndRun(code("dunder_pos"));
    EXPECT_EQ(out, "7\n");
}

TEST(CodeGenE2E, DunderAbs) {
    auto out = compileAndRun(code("dunder_abs"));
    EXPECT_EQ(out, "42\n");
}

TEST(CodeGenE2E, DunderAddReturnsInstance) {
    auto out = compileAndRun(code("dunder_add_returns_instance"));
    EXPECT_EQ(out, "3\n13\n");
}

TEST(CodeGenE2E, DunderAddInherited) {
    auto out = compileAndRun(code("dunder_add_inherited"));
    EXPECT_EQ(out, "10\n");
}

TEST(CodeGenE2E, DunderAddPy) {
    auto out = compileAndRunPy(code("dunder_add_py"));
    EXPECT_EQ(out, "4,6\n");
}

TEST(CodeGenE2E, DunderArithNoFallback) {
    auto out = compileAndRun(code("dunder_arith_no_fallback"));
    EXPECT_EQ(out, "30\n90\n");
}

TEST(CodeGenE2E, DunderMixed) {
    auto out = compileAndRun(code("dunder_mixed"));
    EXPECT_EQ(out, "equal\n7\n");
}

TEST(CodeGenE2E, DunderAddChained) {
    auto out = compileAndRun(code("dunder_add_chained"));
    EXPECT_EQ(out, "6\n");
}

TEST(CodeGenE2E, DunderMulScalar) {
    auto out = compileAndRun(code("dunder_mul_scalar"));
    EXPECT_EQ(out, "10,15\n");
}

TEST(CodeGenE2E, DunderNegReturn) {
    auto out = compileAndRun(code("dunder_neg_return"));
    EXPECT_EQ(out, "-5\n");
}

TEST(CodeGenE2E, DunderAllArithmetic) {
    auto out = compileAndRun(code("dunder_all_arithmetic"));
    EXPECT_EQ(out, "13\n7\n30\n3\n3\n1\n256\n");
}

TEST(CodeGenE2E, DunderAbsReturnsInt) {
    auto out = compileAndRun(code("dunder_abs_returns_int"));
    EXPECT_EQ(out, "100\n");
}

TEST(CodeGenE2E, DunderLen) {
    auto out = compileAndRun(code("dunder_len"));
    EXPECT_EQ(out, "7\n");
}

TEST(CodeGenE2E, DunderLenNoRegression) {
    auto out = compileAndRun(code("dunder_len_no_regression"));
    EXPECT_EQ(out, "5\n");
}

TEST(CodeGenE2E, DunderGetitem) {
    auto out = compileAndRun(code("dunder_getitem"));
    EXPECT_EQ(out, "105\n100\n");
}

TEST(CodeGenE2E, DunderSetitem) {
    auto out = compileAndRun(code("dunder_setitem"));
    EXPECT_EQ(out, "42\n");
}

TEST(CodeGenE2E, DunderContains) {
    auto out = compileAndRun(code("dunder_contains"));
    EXPECT_EQ(out, "yes\ndone\n");
}

TEST(CodeGenE2E, DunderContainsNot) {
    auto out = compileAndRun(code("dunder_contains_not"));
    EXPECT_EQ(out, "found\nend\n");
}

TEST(CodeGenE2E, DunderIter) {
    auto out = compileAndRun(code("dunder_iter"));
    EXPECT_EQ(out, "1\n2\n3\n");
}

TEST(CodeGenE2E, DunderIterSum) {
    auto out = compileAndRun(code("dunder_iter_sum"));
    EXPECT_EQ(out, "15\n");
}

TEST(CodeGenE2E, DunderGetitemNoRegression) {
    auto out = compileAndRun(code("dunder_getitem_no_regression"));
    EXPECT_EQ(out, "20\n");
}

TEST(CodeGenE2E, DunderContainsNoRegression) {
    auto out = compileAndRun(code("dunder_contains_no_regression"));
    EXPECT_EQ(out, "yes\n");
}

TEST(CodeGenE2E, DunderLenInherited) {
    auto out = compileAndRun(code("dunder_len_inherited"));
    EXPECT_EQ(out, "9\n");
}

TEST(CodeGenE2E, DunderGetitemStr) {
    auto out = compileAndRun(code("dunder_getitem_str"));
    EXPECT_EQ(out, "zero\nother\n");
}

TEST(CodeGenE2E, DunderSetitemMultiple) {
    auto out = compileAndRun(code("dunder_setitem_multiple"));
    EXPECT_EQ(out, "10\n20\n");
}

TEST(CodeGenE2E, DunderCombinedContainer) {
    auto out = compileAndRun(code("dunder_combined_container"));
    EXPECT_EQ(out, "1\n42\nyes\n");
}

TEST(CodeGenE2E, DunderWithBasic) {
    auto out = compileAndRun(code("dunder_with_basic"));
    EXPECT_EQ(out, "enter\nbody\nexit\n");
}

TEST(CodeGenE2E, DunderWithNoAs) {
    auto out = compileAndRun(code("dunder_with_no_as"));
    EXPECT_EQ(out, "start\nrunning\nend\n");
}

TEST(CodeGenE2E, DunderWithReturnsSelf) {
    auto out = compileAndRun(code("dunder_with_returns_self"));
    EXPECT_EQ(out, "lock\n42\nunlock\n");
}

TEST(CodeGenE2E, DunderWithExitOnException) {
    auto out = compileAndRun(code("dunder_with_exit_on_exception"));
    EXPECT_EQ(out, "enter\nbody\nexit\ncaught\n");
}

TEST(CodeGenE2E, DunderWithInherited) {
    auto out = compileAndRun(code("dunder_with_inherited"));
    EXPECT_EQ(out, "base_enter\ninside\nbase_exit\n");
}

TEST(CodeGenE2E, DunderWithMultipleStatements) {
    auto out = compileAndRun(code("dunder_with_multiple_statements"));
    EXPECT_EQ(out, "begin\na\nb\nc\nfinish\n");
}

TEST(CodeGenE2E, DunderWithAfterBlock) {
    auto out = compileAndRun(code("dunder_with_after_block"));
    EXPECT_EQ(out, "inside\ncleaned\nafter\n");
}

TEST(CodeGenTest, DunderCallIR) {
    auto ir = generateIR(code("dunder_call_ir"));
    EXPECT_NE(ir.find("Multiplier___call__"), std::string::npos);
}

TEST(CodeGenTest, DunderCallSimple) {
    auto out = compileAndRun(code("dunder_call_simple"));
    EXPECT_EQ(out, "42\n");
}

TEST(CodeGenTest, DunderCallWithState) {
    auto out = compileAndRun(code("dunder_call_with_state"));
    EXPECT_EQ(out, "15\n30\n");
}

TEST(CodeGenTest, DunderCallMultipleArgs) {
    auto out = compileAndRun(code("dunder_call_multiple_args"));
    EXPECT_EQ(out, "42\n");
}

TEST(CodeGenTest, DunderCallNoArgs) {
    auto out = compileAndRun(code("dunder_call_no_args"));
    EXPECT_EQ(out, "hello\n");
}

TEST(CodeGenTest, DunderCallInherited) {
    auto out = compileAndRun(code("dunder_call_inherited"));
    EXPECT_EQ(out, "99\n");
}

TEST(CodeGenTest, DunderCallVoid) {
    auto out = compileAndRun(code("dunder_call_void"));
    EXPECT_EQ(out, "fired\n");
}

TEST(CodeGenTest, DunderCallReturningDictSubscriptedInline) {
    auto out = compileAndRun(code("dunder_call_returning_dict_subscripted_inline"));
    EXPECT_EQ(out, "7\n");
}

TEST(CodeGenTest, DunderCallReturningListSubscriptedInline) {
    auto out = compileAndRun(code("dunder_call_returning_list_subscripted_inline"));
    EXPECT_EQ(out, "30\n");
}

TEST(CodeGenTest, DunderCallReturningInstanceAttributeInline) {
    auto out = compileAndRun(code("dunder_call_returning_instance_attribute_inline"));
    EXPECT_EQ(out, "42\n");
}

TEST(CodeGenTest, GenericCellChainedAccessInline) {
    auto out = compileAndRun(code("generic_cell_chained_access_inline"));
    EXPECT_EQ(out, "7\n7\n42\n42\n30\n");
}
