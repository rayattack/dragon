#include "CodeGenTestHelpers.h"
#include "CodeBlock.h"

static std::string code(const std::string& block) {
    return extractCode("CodeGenCallsTest.md", block);
}

TEST(CodeGenTest, PrintIntCall) {
    auto ir = generateIR("print(42)");
    EXPECT_NE(ir.find("dragon_print_int"), std::string::npos);
}

TEST(CodeGenTest, PrintStrCall) {
    auto ir = generateIR("print(\"hello\")");
    EXPECT_NE(ir.find("dragon_print_str"), std::string::npos);
}

TEST(CodeGenTest, PrintFloatCall) {
    auto ir = generateIR("print(3.14)");
    EXPECT_NE(ir.find("dragon_print_float"), std::string::npos);
}

TEST(CodeGenTest, PrintList) {
    auto ir = generateIR("x: list[int] = [1, 2, 3]\nprint(x)");
    EXPECT_NE(ir.find("dragon_print_list_nested_raw"), std::string::npos);
}

TEST(CodeGenTest, PrintListStrDispatch) {
    auto ir = generateIR(code("print_list_str_dispatch"));
    EXPECT_NE(ir.find("dragon_print_list_nested_raw"), std::string::npos);
}

TEST(CodeGenTest, PrintListFloatDispatch) {
    auto ir = generateIR(code("print_list_float_dispatch"));
    EXPECT_NE(ir.find("dragon_print_list_nested_raw"), std::string::npos);
}

TEST(CodeGenTest, MathSqrt) {
    auto ir = generateIR("from math import sqrt\nx: float = sqrt(4.0)");
    EXPECT_NE(ir.find("call double @sqrt"), std::string::npos);
}

TEST(CodeGenTest, MathPi) {
    auto ir = generateIR("import math\nx: float = math.pi");
    EXPECT_NE(ir.find("double"), std::string::npos);
    EXPECT_NE(ir.find("400921FB54442D18"), std::string::npos);
}

TEST(CodeGenIR, MinMaxInt) {
    auto ir = generateIR("print(min(3, 7))");
    EXPECT_NE(ir.find("dragon_min_int"), std::string::npos);
    auto ir2 = generateIR("print(max(3, 7))");
    EXPECT_NE(ir2.find("dragon_max_int"), std::string::npos);
}

TEST(CodeGenIR, SumAnyAll) {
    auto ir = generateIR("xs: list[int] = [1, 2, 3]\nprint(sum(xs))");
    EXPECT_NE(ir.find("dragon_sum_list"), std::string::npos);
    auto ir2 = generateIR("xs: list[int] = [1, 0, 3]\nprint(any(xs))");
    EXPECT_NE(ir2.find("dragon_any_list"), std::string::npos);
    auto ir3 = generateIR("xs: list[int] = [1, 2, 3]\nprint(all(xs))");
    EXPECT_NE(ir3.find("dragon_all_list"), std::string::npos);
}

TEST(CodeGenIR, EnumerateZip) {
    auto ir = generateIR("xs: list[int] = [10, 20]\nys: list[int] = enumerate(xs)");
    EXPECT_NE(ir.find("dragon_enumerate"), std::string::npos);
    auto ir2 = generateIR("xs: list[int] = [1, 2]\nys: list[int] = [3, 4]\nzs: list[int] = zip(xs, ys)");
    EXPECT_NE(ir2.find("dragon_zip"), std::string::npos);
}

TEST(CodeGenIR, SortedReversed) {
    auto ir = generateIR("xs: list[int] = [3, 1, 2]\nys: list[int] = sorted(xs)");
    EXPECT_NE(ir.find("dragon_sorted"), std::string::npos);
    auto ir2 = generateIR("xs: list[int] = [1, 2, 3]\nys: list[int] = reversed(xs)");
    EXPECT_NE(ir2.find("dragon_reversed"), std::string::npos);
}

TEST(CodeGenIR, OrdChr) {
    auto ir = generateIR("print(ord(\"A\"))");
    EXPECT_NE(ir.find("dragon_ord"), std::string::npos);
    auto ir2 = generateIR("print(chr(65))");
    EXPECT_NE(ir2.find("dragon_chr"), std::string::npos);
}

TEST(CodeGenIR, HexOctBin) {
    auto ir = generateIR("print(hex(255))");
    EXPECT_NE(ir.find("dragon_hex"), std::string::npos);
    auto ir2 = generateIR("print(oct(8))");
    EXPECT_NE(ir2.find("dragon_oct"), std::string::npos);
    auto ir3 = generateIR("print(bin(10))");
    EXPECT_NE(ir3.find("dragon_bin"), std::string::npos);
}

TEST(CodeGenIR, HashIdRepr) {
    auto ir = generateIR("print(hash(42))");
    EXPECT_NE(ir.find("dragon_hash_int"), std::string::npos);
    auto ir2 = generateIR("print(id(42))");
    EXPECT_NE(ir2.find("dragon_id"), std::string::npos);
    auto ir3 = generateIR("print(repr(42))");
    EXPECT_NE(ir3.find("dragon_repr_int"), std::string::npos);
}

TEST(CodeGenIR, PowDivmod) {
    auto ir = generateIR("print(pow(2, 10))");
    EXPECT_NE(ir.find("dragon_pow_int"), std::string::npos);
    auto ir2 = generateIR("print(divmod(17, 5))");
    EXPECT_NE(ir2.find("dragon_divmod"), std::string::npos);
}

TEST(CodeGenIR, RoundBuiltin) {
    auto ir = generateIR("x: float = 3.7\nprint(round(x))");
    EXPECT_NE(ir.find("dragon_round_int"), std::string::npos);
}

TEST(CodeGenIR, EmptyConstructors) {
    auto ir = generateIR("xs: list = list()");
    EXPECT_NE(ir.find("dragon_list_new"), std::string::npos);
    auto ir2 = generateIR("d: dict[str, int] = dict()");
    EXPECT_NE(ir2.find("dragon_dict_new"), std::string::npos);
    auto ir3 = generateIR("s: set[int] = set()");
    EXPECT_NE(ir3.find("dragon_set_new"), std::string::npos);
}

TEST(CodeGenTest, ExternCSingleDeclIR) {
    auto ir = generateIR(
        "extern \"C\" def puts(s: str) -> int\n"
    );
    EXPECT_NE(ir.find("declare i64 @puts(ptr)"), std::string::npos)
        << "Expected extern puts declaration:\n" << ir;
}

TEST(CodeGenTest, ExternCFromLibIR) {
    auto ir = generateIR(code("extern_c_from_lib_ir"));
    EXPECT_NE(ir.find("declare i64 @foo(i64)"), std::string::npos)
        << "Expected foo declaration:\n" << ir;
    EXPECT_NE(ir.find("declare ptr @bar(ptr)"), std::string::npos)
        << "Expected bar declaration:\n" << ir;
}

TEST(CodeGenTest, ExternCPtrTypeIR) {
    auto ir = generateIR(code("extern_c_ptr_type_ir"));
    EXPECT_NE(ir.find("declare ptr @malloc(i64)"), std::string::npos)
        << "Expected malloc declaration:\n" << ir;
    EXPECT_NE(ir.find("declare void @free(ptr)"), std::string::npos)
        << "Expected free declaration:\n" << ir;
}

TEST(CodeGenTest, ExprStmtDiscardingPtrReturnNoDecrefStr) {
    auto ir = generateIR(code("expr_stmt_discarding_ptr_return_no_decref_str"));
    EXPECT_EQ(ir.find("call void @dragon_decref_str"), std::string::npos)
        << "Spurious dragon_decref_str on discarded ptr-returning call:\n" << ir;
}

TEST(CodeGenE2E, IsinstanceInt) {
    auto output = compileAndRun(code("isinstance_int"));
    EXPECT_EQ(output, "True\n");
}

TEST(CodeGenE2E, IsinstanceStr) {
    auto output = compileAndRun(code("isinstance_str"));
    EXPECT_EQ(output, "True\nFalse\n");
}

TEST(CodeGenE2E, IsinstanceBool) {
    auto output = compileAndRun(code("isinstance_bool"));
    EXPECT_EQ(output, "True\n");
}

TEST(CodeGenE2E, IsinstanceList) {
    auto output = compileAndRun(code("isinstance_list"));
    EXPECT_EQ(output, "True\nFalse\n");
}

TEST(CodeGenE2E, TypeInt) {
    auto output = compileAndRun(code("type_int"));
    EXPECT_EQ(output, "int\n");
}

TEST(CodeGenE2E, TypeStr) {
    auto output = compileAndRun(code("type_str"));
    EXPECT_EQ(output, "str\n");
}

TEST(CodeGenE2E, TypeFloat) {
    auto output = compileAndRun(code("type_float"));
    EXPECT_EQ(output, "float\n");
}

TEST(CodeGenE2E, TypeBool) {
    auto output = compileAndRun(code("type_bool"));
    EXPECT_EQ(output, "bool\n");
}

TEST(CodeGenE2E, PrintListStr) {
    auto output = compileAndRun(code("print_list_str"));
    EXPECT_EQ(output, "['hello', 'world']\n");
}

TEST(CodeGenE2E, PrintListFloat) {
    auto output = compileAndRun(code("print_list_float"));
    EXPECT_EQ(output, "[1.5, 2.0, 3.14]\n");
}

TEST(CodeGenE2E, PrintListBool) {
    auto output = compileAndRun(code("print_list_bool"));
    EXPECT_EQ(output, "[True, False, True]\n");
}

TEST(CodeGenE2E, PrintList) {
    auto output = compileAndRun(code("print_list"));
    EXPECT_EQ(output, "[1, 2, 3]\n");
}

TEST(CodeGenE2E, MinMaxTwoArgs) {
    auto out = compileAndRun("print(min(3, 7))\nprint(max(3, 7))");
    EXPECT_EQ(out, "3\n7\n");
}

TEST(CodeGenE2E, MinMaxList) {
    auto out = compileAndRun(code("min_max_list"));
    EXPECT_EQ(out, "1\n9\n");
}

TEST(CodeGenE2E, SumList) {
    auto out = compileAndRun(code("sum_list"));
    EXPECT_EQ(out, "15\n");
}

TEST(CodeGenE2E, AnyAllList) {
    auto out = compileAndRun(code("any_all_list"));
    EXPECT_EQ(out, "True\nTrue\nFalse\nFalse\n");
}

TEST(CodeGenE2E, OrdChr) {
    auto out = compileAndRun("print(ord(\"A\"))\nprint(chr(66))");
    EXPECT_EQ(out, "65\nB\n");
}

TEST(CodeGenE2E, HexOctBin) {
    auto out = compileAndRun("print(hex(255))\nprint(oct(8))\nprint(bin(10))");
    EXPECT_EQ(out, "0xff\n0o10\n0b1010\n");
}

TEST(CodeGenE2E, PowBuiltin) {
    auto out = compileAndRun("print(pow(2, 10))");
    EXPECT_EQ(out, "1024\n");
}

TEST(CodeGenE2E, ReprBuiltin) {
    auto out = compileAndRun("print(repr(42))\nprint(repr(\"hello\"))");
    EXPECT_EQ(out, "42\n'hello'\n");
}

TEST(CodeGenE2E, HashBuiltin) {
    auto out = compileAndRun("print(hash(42))");
    EXPECT_EQ(out, "42\n");
}

TEST(CodeGenE2E, SortedReversed) {
    auto out = compileAndRun(code("sorted_reversed"));
    EXPECT_EQ(out, "1\n5\n5\n3\n");
}

TEST(CodeGenE2E, ExternCCallPuts) {
    auto out = compileAndRun(code("extern_c_call_puts"));
    EXPECT_EQ(out, "hello from C\n");
}

TEST(CodeGenE2E, ExternCCallAbs) {
    auto out = compileAndRun(code("extern_c_call_abs"));
    EXPECT_EQ(out, "42\n");
}

TEST(CodeGenIR, HasattrIR) {
    auto ir = generateIR(code("hasattr_ir"));
    EXPECT_NE(ir.find("dragon_hasattr"), std::string::npos);
}

TEST(CodeGenIR, GetattrIR) {
    auto ir = generateIR(code("getattr_ir"));
    EXPECT_NE(ir.find("dragon_getattr"), std::string::npos);
}

TEST(CodeGenE2E, HasattrTrue) {
    auto out = compileAndRun(code("hasattr_true"));
    EXPECT_EQ(out, "yes\n");
}

TEST(CodeGenE2E, HasattrFalse) {
    auto out = compileAndRun(code("hasattr_false"));
    EXPECT_EQ(out, "no\n");
}

TEST(CodeGenE2E, GetattrField) {
    auto out = compileAndRun(code("getattr_field"));
    EXPECT_EQ(out, "Rex\n");
}

TEST(CodeGenE2E, GetattrInt) {
    auto out = compileAndRun(code("getattr_int"));
    EXPECT_EQ(out, "42\n");
}

TEST(CodeGenE2E, GetattrDefault) {
    auto out = compileAndRun(code("getattr_default"));
    EXPECT_EQ(out, "9999\n");
}

TEST(CodeGenE2E, HasattrInherited) {
    auto out = compileAndRun(code("hasattr_inherited"));
    EXPECT_EQ(out, "has x\nhas y\n");
}

TEST(CodeGenE2E, AnyKwargToVarKwargsFunction) {
    auto out = compileAndRun(code("any_kwarg_to_var_kwargs_function"));
    EXPECT_EQ(out, "2\n");
}

TEST(CodeGenE2E, AnyKwargKeepsRuntimeTypeAndValue) {
    auto out = compileAndRun(code("any_kwarg_keeps_runtime_type_and_value"));
    EXPECT_EQ(out, "5|ada\n");
}

TEST(CodeGenE2E, AnyKwargToVarKwargsMethod) {
    auto out = compileAndRun(code("any_kwarg_to_var_kwargs_method"));
    EXPECT_EQ(out, "2\n");
}

TEST(CodeGenE2E, AnyKwargToTypedDictConstructor) {
    auto out = compileAndRun(code("any_kwarg_to_typed_dict_constructor"));
    EXPECT_EQ(out, "7\n");
}

TEST(CodeGenE2E, ContainerKwargKeepsItsOwnTag) {
    auto out = compileAndRun(code("container_kwarg_keeps_its_own_tag"));
    EXPECT_EQ(out, "3\n");
}

TEST(CodeGenE2E, ContainerKwargValuesReadBackInCallee) {
    auto out = compileAndRun(code("container_kwarg_values_read_back_in_callee"));
    EXPECT_EQ(out, "6|ada\n1\nada\n");
}

TEST(CodeGenE2E, InstanceKwargToVarKwargsMethod) {
    auto out = compileAndRun(code("instance_kwarg_to_var_kwargs_method"));
    EXPECT_EQ(out, "2\n");
}

TEST(CodeGenE2E, TypedDictKwargStrOutlivesConstruction) {
    auto out = compileAndRun(code("typed_dict_kwarg_str_outlives_construction"));
    EXPECT_EQ(out, "name-2\n");
}

TEST(CodeGenE2E, KeywordMoveTransfersOwnership) {
    auto out = compileAndRun(code("keyword_move_transfers_ownership"));
    EXPECT_EQ(out, "18\n");
}

TEST(CodeGenE2E, KeywordMoveOnMethodAndVarArgs) {
    auto out = compileAndRun(code("keyword_move_on_method_and_var_args"));
    EXPECT_EQ(out, "3\n3\n");
}

TEST(CodeGenE2E, KeywordDubLeavesSourceUsable) {
    auto out = compileAndRun(code("keyword_dub_leaves_source_usable"));
    EXPECT_EQ(out, "3\nm-7\n");
}

TEST(CodeGenE2E, DubIntoOwnParamLeavesSourceIntact) {
    auto out = compileAndRun(code("dub_into_own_param_leaves_source_intact"));
    EXPECT_EQ(out, "11\nreport-2026\n3\n3\n2\n2\n");
}

TEST(CodeGenE2E, DubIntoOwnParamByKeywordLeavesSourceIntact) {
    auto out = compileAndRun(code("dub_into_own_param_by_keyword_leaves_source_intact"));
    EXPECT_EQ(out, "11\nreport-2026\n");
}

TEST(CodeGenTest, StrOfStrStaysZeroCopy) {
    auto ir = generateIR(code("str_of_str_stays_zero_copy_ir"));
    EXPECT_NE(ir.find("call ptr @dragon_str_retain"), std::string::npos)
        << "str(s) on a str must retain, not copy\nIR:\n" << ir;
    EXPECT_EQ(ir.find("call ptr @dragon_string_dup"), std::string::npos)
        << "str(s) on a str must not allocate a duplicate\nIR:\n" << ir;
}
