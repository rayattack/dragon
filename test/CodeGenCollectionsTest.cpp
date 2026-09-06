#include "CodeGenTestHelpers.h"
#include "CodeBlock.h"

static std::string code(const std::string& block) {
    return extractCode("CodeGenCollectionsTest.md", block);
}

TEST(CodeGenTest, ListLiteral) {
    auto ir = generateIR("x: list[int] = [1, 2, 3]");
    EXPECT_NE(ir.find("dragon_list_new"), std::string::npos);
    EXPECT_NE(ir.find("dragon_list_append"), std::string::npos);
}

TEST(CodeGenTest, ListSubscript) {
    auto ir = generateIR("x: list[int] = [10, 20, 30]\nprint(x[1])");
    EXPECT_NE(ir.find("list.data.gep"), std::string::npos);
    EXPECT_NE(ir.find("list.elem"), std::string::npos);
    EXPECT_NE(ir.find("dragon_list_get"), std::string::npos);
}

TEST(CodeGenTest, ListLen) {
    auto ir = generateIR("x: list[int] = [1, 2, 3]\nprint(len(x))");
    EXPECT_NE(ir.find("dragon_list_len"), std::string::npos);
}

TEST(CodeGenTest, ListAppendMethod) {
    auto ir = generateIR("x: list[int] = [1, 2]\nx.append(3)");
    EXPECT_NE(ir.find("dragon_list_append"), std::string::npos);
}

TEST(CodeGenTest, ListSlice) {
    auto ir = generateIR("x: list[int] = [1, 2, 3, 4, 5]\ny: list[int] = x[1:3]");
    EXPECT_NE(ir.find("dragon_list_slice"), std::string::npos);
}

TEST(CodeGenTest, DictExprEmpty) {
    auto ir = generateIR("d: dict[str, int] = {}");
    EXPECT_NE(ir.find("dragon_dict_new"), std::string::npos);
}

TEST(CodeGenTest, DictExprEntries) {
    auto ir = generateIR("d: dict[str, int] = {\"a\": 1, \"b\": 2}");
    EXPECT_NE(ir.find("dragon_dict_new"), std::string::npos);
    EXPECT_NE(ir.find("dragon_dict_set"), std::string::npos);
}

TEST(CodeGenTest, DictSubscriptGet) {
    auto ir = generateIR("d: dict[str, int] = {\"a\": 1}\nx: int = d[\"a\"]");
    EXPECT_NE(ir.find("dragon_dict_get"), std::string::npos);
}

TEST(CodeGenTest, DictLen) {
    auto ir = generateIR("d: dict[str, int] = {\"a\": 1, \"b\": 2}\nprint(len(d))");
    EXPECT_NE(ir.find("dragon_dict_len"), std::string::npos);
}

TEST(CodeGenTest, DictPrint) {
    auto ir = generateIR("d: dict[str, int] = {\"a\": 1}\nprint(d)");
    EXPECT_NE(ir.find("dragon_print_dict"), std::string::npos);
}

TEST(CodeGenTest, BareKeyDictIR) {
    auto ir = generateIR(
        "d: dict = {name: \"Jon\", age: 10}\n"
    );
    EXPECT_NE(ir.find("dragon_dict_new"), std::string::npos);
    EXPECT_NE(ir.find("dragon_dict_set"), std::string::npos);
    EXPECT_NE(ir.find("name"), std::string::npos);
    EXPECT_NE(ir.find("age"), std::string::npos);
}

TEST(CodeGenTest, DictDotAccessReadIR) {
    auto ir = generateIR(code("dict_dot_access_read_ir"));
    EXPECT_NE(ir.find("dragon_dict_get"), std::string::npos);
    EXPECT_NE(ir.find("dictdot"), std::string::npos);
}

TEST(CodeGenTest, DictDotAccessWriteIR) {
    auto ir = generateIR(code("dict_dot_access_write_ir"));
    EXPECT_NE(ir.find("dragon_dict_set"), std::string::npos);
}

TEST(CodeGenTest, DictGetCheckedIR) {
    auto ir = generateIR(code("dict_get_checked_ir"));
    EXPECT_NE(ir.find("dragon_dict_get_checked"), std::string::npos);
}

TEST(CodeGenTest, DictGetCheckedDotAccessIR) {
    auto ir = generateIR(code("dict_get_checked_dot_access_ir"));
    EXPECT_NE(ir.find("dragon_dict_get_checked"), std::string::npos);
}

TEST(CodeGenTest, TypedDictIR) {
    auto ir = generateIR(code("typed_dict_ir"));
    EXPECT_NE(ir.find("dragon_dict_new"), std::string::npos);
    EXPECT_EQ(ir.find("%Config = type"), std::string::npos);
}

TEST(CodeGenTest, TypedDictCheckedAccessIR) {
    auto ir = generateIR(code("typed_dict_checked_access_ir"));
    EXPECT_NE(ir.find("dragon_dict_get_checked"), std::string::npos);
}

TEST(CodeGenTest, TupleCreate) {
    auto ir = generateIR("t: tuple[int, int, int] = (1, 2, 3)");
    EXPECT_NE(ir.find("dragon_tuple_new"), std::string::npos);
    EXPECT_NE(ir.find("dragon_tuple_set"), std::string::npos);
}

TEST(CodeGenTest, TupleSubscript) {
    auto ir = generateIR("t: tuple[int, int, int] = (10, 20, 30)\nprint(t[1])");
    EXPECT_NE(ir.find("dragon_tuple_get"), std::string::npos);
}

TEST(CodeGenTest, TupleLen) {
    auto ir = generateIR("t: tuple[int, int, int] = (1, 2, 3)\nprint(len(t))");
    EXPECT_NE(ir.find("dragon_tuple_len"), std::string::npos);
}

TEST(CodeGenTest, TuplePrint) {
    auto ir = generateIR("t: tuple[int, int, int] = (1, 2, 3)\nprint(t)");
    EXPECT_NE(ir.find("dragon_print_tuple"), std::string::npos);
}

TEST(CodeGenTest, TupleEmpty) {
    auto ir = generateIR("t: int | tuple[] = ()");
    EXPECT_NE(ir.find("dragon_tuple_new"), std::string::npos);
}

TEST(CodeGenTest, SetCreate) {
    auto ir = generateIR("s: set = {1, 2, 3}");
    EXPECT_NE(ir.find("dragon_set_new"), std::string::npos);
    EXPECT_NE(ir.find("dragon_set_add"), std::string::npos);
}

TEST(CodeGenTest, SetLen) {
    auto ir = generateIR("s: set = {1, 2, 3}\nprint(len(s))");
    EXPECT_NE(ir.find("dragon_set_len"), std::string::npos);
}

TEST(CodeGenTest, SetPrint) {
    auto ir = generateIR("s: set = {1, 2, 3}\nprint(s)");
    EXPECT_NE(ir.find("dragon_print_set"), std::string::npos);
}

TEST(CodeGenE2E, ListBasic) {
    auto output = compileAndRun(code("list_basic"));
    EXPECT_EQ(output, "10\n20\n30\n");
}

TEST(CodeGenE2E, ListAppend) {
    auto output = compileAndRun(code("list_append"));
    EXPECT_EQ(output, "4\n3\n4\n");
}

TEST(CodeGenE2E, ListNegativeIndex) {
    auto output = compileAndRun(code("list_negative_index"));
    EXPECT_EQ(output, "30\n20\n");
}

TEST(CodeGenE2E, ListInLoop) {
    auto output = compileAndRun(code("list_in_loop"));
    EXPECT_EQ(output, "0\n9\n10\n");
}

TEST(CodeGenE2E, ListInsert) {
    auto output = compileAndRun(code("list_insert"));
    EXPECT_EQ(output, "1\n2\n3\n4\n");
}

TEST(CodeGenE2E, ListRemove) {
    auto output = compileAndRun(code("list_remove"));
    EXPECT_EQ(output, "4\n3\n");
}

TEST(CodeGenE2E, ListPop) {
    auto output = compileAndRun(code("list_pop"));
    EXPECT_EQ(output, "30\n2\n");
}

TEST(CodeGenE2E, ListPopIndex) {
    auto output = compileAndRun(code("list_pop_index"));
    EXPECT_EQ(output, "10\n20\n");
}

TEST(CodeGenE2E, ListClear) {
    auto output = compileAndRun(code("list_clear"));
    EXPECT_EQ(output, "0\n");
}

TEST(CodeGenE2E, ListExtend) {
    auto output = compileAndRun(code("list_extend"));
    EXPECT_EQ(output, "5\n4\n");
}

TEST(CodeGenE2E, ListIndex) {
    auto output = compileAndRun(code("list_index"));
    EXPECT_EQ(output, "2\n");
}

TEST(CodeGenE2E, ListCount) {
    auto output = compileAndRun(code("list_count"));
    EXPECT_EQ(output, "3\n");
}

TEST(CodeGenE2E, ListSort) {
    auto output = compileAndRun(code("list_sort"));
    EXPECT_EQ(output, "1\n1\n9\n");
}

TEST(CodeGenE2E, ListReverse) {
    auto output = compileAndRun(code("list_reverse"));
    EXPECT_EQ(output, "4\n1\n");
}

TEST(CodeGenE2E, ListCopy) {
    auto output = compileAndRun(code("list_copy"));
    EXPECT_EQ(output, "4\n3\n");
}

TEST(CodeGenE2E, DictBasic) {
    auto output = compileAndRun(code("dict_basic"));
    EXPECT_EQ(output, "1\n2\n");
}

TEST(CodeGenE2E, DictValues) {
    auto output = compileAndRun(code("dict_values"));
    EXPECT_EQ(output, "2\n");
}

TEST(CodeGenE2E, DictPop) {
    auto output = compileAndRun(code("dict_pop"));
    EXPECT_EQ(output, "2\n2\n");
}

TEST(CodeGenE2E, DictClear) {
    auto output = compileAndRun(code("dict_clear"));
    EXPECT_EQ(output, "0\n");
}

TEST(CodeGenE2E, DictSetdefault) {
    auto output = compileAndRun(code("dict_setdefault"));
    EXPECT_EQ(output, "1\n42\n2\n");
}

TEST(CodeGenE2E, DictCopy) {
    auto output = compileAndRun(code("dict_copy"));
    EXPECT_EQ(output, "2\n1\n");
}

TEST(CodeGenE2E, BareKeyDictBasic) {
    auto output = compileAndRun(code("bare_key_dict_basic"));
    EXPECT_EQ(output, "Jon\n10\n");
}

TEST(CodeGenE2E, BareKeyDictMixed) {
    auto output = compileAndRun(code("bare_key_dict_mixed"));
    EXPECT_EQ(output, "localhost\n8080\n");
}

TEST(CodeGenE2E, ComputedKeyDict) {
    auto output = compileAndRun(code("computed_key_dict"));
    EXPECT_EQ(output, "Jon\n10\n");
}

TEST(CodeGenE2E, DictDotAccessRead) {
    auto output = compileAndRun(code("dict_dot_access_read"));
    EXPECT_EQ(output, "Jon\n25\n");
}

TEST(CodeGenE2E, DictDotAccessReadBareKey) {
    auto output = compileAndRun(code("dict_dot_access_read_bare_key"));
    EXPECT_EQ(output, "Jon\n25\n");
}

TEST(CodeGenE2E, DictDotAccessWrite) {
    auto output = compileAndRun(code("dict_dot_access_write"));
    EXPECT_EQ(output, "42\n");
}

TEST(CodeGenE2E, DictDotAccessWriteNew) {
    auto output = compileAndRun(code("dict_dot_access_write_new"));
    EXPECT_EQ(output, "99\n");
}

TEST(CodeGenE2E, DictMethodsStillWork) {
    auto output = compileAndRun(code("dict_methods_still_work"));
    EXPECT_EQ(output, "Jon\n");
}

TEST(CodeGenE2E, DictItemsTupleUnpacking) {
    auto out = compileAndRun(code("dict_items_tuple_unpacking"));
    EXPECT_EQ(out, "a\n1\nb\n2\nc\n3\n");
}

TEST(CodeGenE2E, DictPopitemLifoOrder) {
    auto out = compileAndRun(code("dict_popitem_lifo_order"));
    EXPECT_EQ(out, "2\nFalse\nTrue\n");
}

TEST(CodeGenE2E, DictPopitemRepeated) {
    auto out = compileAndRun(code("dict_popitem_repeated"));
    EXPECT_EQ(out, "1\nTrue\n");
}

TEST(CodeGenE2E, DictFromkeysWithDefault) {
    auto out = compileAndRun(code("dict_fromkeys_with_default"));
    EXPECT_EQ(out, "3\n99\n99\n99\n");
}

TEST(CodeGenE2E, DictFromkeysWithoutDefault) {
    auto out = compileAndRun(code("dict_fromkeys_without_default"));
    EXPECT_EQ(out, "2\nTrue\nTrue\n");
}

TEST(CodeGenE2E, TupleUnpackAssignment) {
    auto out = compileAndRun(code("tuple_unpack_assignment"));
    EXPECT_EQ(out, "7\n");
}

TEST(CodeGenE2E, DictItemsValueVarKindStr) {
    auto out = compileAndRun(code("dict_items_value_var_kind_str"));
    EXPECT_EQ(out, "a\nalpha\nb\nbeta\n");
}

TEST(CodeGenE2E, DictItemsValueVarKindInt) {
    auto out = compileAndRun(code("dict_items_value_var_kind_int"));
    EXPECT_EQ(out, "11\n12\n");
}

TEST(CodeGenE2E, BareKeyDictSingleEntry) {
    auto output = compileAndRun(code("bare_key_dict_single_entry"));
    EXPECT_EQ(output, "200\n");
}

TEST(CodeGenE2E, DictDotAccessReadWriteCombined) {
    auto output = compileAndRun(code("dict_dot_access_read_write_combined"));
    EXPECT_EQ(output, "5\nitems\n");
}

TEST(CodeGenE2E, DictWithNestedContainersE2E) {
    auto out = compileAndRun(code("dict_with_nested_containers_e2_e"));
    EXPECT_EQ(out, "1\n2\n");
}

TEST(CodeGenE2E, DictGetCheckedCorrectType) {
    auto output = compileAndRun(code("dict_get_checked_correct_type"));
    EXPECT_EQ(output, "10\n");
}

TEST(CodeGenE2E, DictGetCheckedStringCorrect) {
    auto output = compileAndRun(code("dict_get_checked_string_correct"));
    EXPECT_EQ(output, "Jon\n");
}

TEST(CodeGenE2E, DictGetCheckedDotAccess) {
    auto output = compileAndRun(code("dict_get_checked_dot_access"));
    EXPECT_EQ(output, "10\n");
}

TEST(CodeGenE2E, TypedDictBasic) {
    auto output = compileAndRun(code("typed_dict_basic"));
    EXPECT_EQ(output, "localhost\n8080\n");
}

TEST(CodeGenE2E, TypedDictDotAccess) {
    auto output = compileAndRun(code("typed_dict_dot_access"));
    EXPECT_EQ(output, "localhost\n8080\nTrue\n");
}

TEST(CodeGenE2E, TypedDictMixedTypes) {
    auto output = compileAndRun(code("typed_dict_mixed_types"));
    EXPECT_EQ(output, "test\n42\n");
}

TEST(CodeGenE2E, TypedDictAnnotatedAccess) {
    auto output = compileAndRun(code("typed_dict_annotated_access"));
    EXPECT_EQ(output, "localhost\n8080\n");
}

TEST(CodeGenE2E, TypedDictWithoutAnnotation) {
    auto output = compileAndRun(code("typed_dict_without_annotation"));
    EXPECT_EQ(output, "localhost\n8080\n");
}

TEST(CodeGenE2E, TypedDictDoubleStarSpread) {
    auto output = compileAndRun(code("typed_dict_double_star_spread"));
    EXPECT_EQ(output, "1\nAda\n");
}

TEST(CodeGenE2E, TypedDictDoubleStarSpreadInComprehension) {
    auto output = compileAndRun(code("typed_dict_double_star_spread_in_comprehension"));
    EXPECT_EQ(output, "Ada\n");
}

TEST(CodeGenE2E, TupleCreateAndPrint) {
    auto output = compileAndRun(code("tuple_create_and_print"));
    EXPECT_EQ(output, "(1, 2, 3)\n");
}

TEST(CodeGenE2E, TupleSingleElement) {
    auto output = compileAndRun(code("tuple_single_element"));
    EXPECT_EQ(output, "(42,)\n");
}

TEST(CodeGenE2E, TupleIndexAccess) {
    auto output = compileAndRun(code("tuple_index_access"));
    EXPECT_EQ(output, "10\n20\n30\n");
}

TEST(CodeGenE2E, TupleNegativeIndex) {
    auto output = compileAndRun(code("tuple_negative_index"));
    EXPECT_EQ(output, "30\n20\n");
}

TEST(CodeGenE2E, TupleLen) {
    auto output = compileAndRun(code("tuple_len"));
    EXPECT_EQ(output, "5\n");
}

TEST(CodeGenE2E, TupleEmptyLen) {
    auto output = compileAndRun(code("tuple_empty_len"));
    EXPECT_EQ(output, "0\n");
}

TEST(CodeGenE2E, TupleInFunction) {
    auto output = compileAndRun(code("tuple_in_function"));
    EXPECT_EQ(output, "7\n");
}

TEST(CodeGenE2E, TupleUnpackSimple) {
    auto output = compileAndRun(code("tuple_unpack_simple"));
    EXPECT_EQ(output, "1\n2\n");
}

TEST(CodeGenE2E, TupleUnpackThree) {
    auto output = compileAndRun(code("tuple_unpack_three"));
    EXPECT_EQ(output, "10\n20\n30\n");
}

TEST(CodeGenE2E, TupleUnpackFromRHSTuple) {
    auto output = compileAndRun(code("tuple_unpack_from_rhs_tuple"));
    EXPECT_EQ(output, "100\n200\n");
}

TEST(CodeGenE2E, StarredUnpackFirst) {
    auto output = compileAndRun(code("starred_unpack_first"));
    EXPECT_EQ(output, "10\n3\n");
}

TEST(CodeGenE2E, StarredUnpackLast) {
    auto output = compileAndRun(code("starred_unpack_last"));
    EXPECT_EQ(output, "3\n40\n");
}

TEST(CodeGenE2E, ForLoopTupleUnpack) {
    auto output = compileAndRun(code("for_loop_tuple_unpack"));
    EXPECT_EQ(output, "11\n22\n33\n");
}

TEST(CodeGenE2E, SetLen) {
    auto output = compileAndRun(code("set_len"));
    EXPECT_EQ(output, "3\n");
}

TEST(CodeGenE2E, SetDeduplicate) {
    auto output = compileAndRun(code("set_deduplicate"));
    EXPECT_EQ(output, "3\n");
}

TEST(CodeGenE2E, SetEmptyLen) {
    auto output = compileAndRun(code("set_empty_len"));
    EXPECT_EQ(output, "1\n");
}

TEST(CodeGenE2E, SetContainsViaIn) {
    auto output = compileAndRun(code("set_contains_via_in"));
    EXPECT_EQ(output, "1\n");
}

TEST(CodeGenE2E, SetNotContainsViaIn) {
    auto output = compileAndRun(code("set_not_contains_via_in"));
    EXPECT_EQ(output, "0\n");
}

TEST(CodeGenE2E, ImmortalObjectSurvivesDecref) {
    auto output = compileAndRun(code("immortal_object_survives_decref"));
    EXPECT_EQ(output, "1\n");
}

TEST(CodeGenE2E, NonImmortalObjectReportsZero) {
    auto output = compileAndRun(code("non_immortal_object_reports_zero"));
    EXPECT_EQ(output, "0\n");
}

TEST(CodeGenE2E, ListSpreadBasic) {
    auto out = compileAndRun(code("list_spread_basic"));
    EXPECT_EQ(out, "4\n");
}

TEST(CodeGenE2E, ListSpreadWithLiterals) {
    auto out = compileAndRun(code("list_spread_with_literals"));
    EXPECT_EQ(out, "4\n");
}

TEST(CodeGenE2E, DictSpreadBasic) {
    auto out = compileAndRun(code("dict_spread_basic"));
    EXPECT_EQ(out, "2\n");
}

TEST(CodeGenE2E, DictSpreadWithEntries) {
    auto out = compileAndRun(code("dict_spread_with_entries"));
    EXPECT_EQ(out, "2\n");
}

TEST(CodeGenE2E, DictSpreadOverride) {
    auto out = compileAndRun(code("dict_spread_override"));
    EXPECT_EQ(out, "99\n");
}

TEST(CodeGenIR, DequeNewIR) {
    auto ir = generateIR(
        "d: deque[int] = deque()\n"
    );
    EXPECT_NE(ir.find("dragon_deque_new"), std::string::npos);
}

TEST(CodeGenE2E, DequeAppendPopleft) {
    auto out = compileAndRun(code("deque_append_popleft"));
    EXPECT_EQ(out, "1\n2\n3\n");
}

TEST(CodeGenE2E, DequeLen) {
    auto out = compileAndRun(code("deque_len"));
    EXPECT_EQ(out, "2\n1\n");
}

TEST(CodeGenE2E, DequeAppendleft) {
    auto out = compileAndRun(code("deque_appendleft"));
    EXPECT_EQ(out, "3\n2\n");
}

TEST(CodeGenE2E, DequePop) {
    auto out = compileAndRun(code("deque_pop"));
    EXPECT_EQ(out, "3\n2\n");
}

TEST(CodeGenE2E, DequeFromList) {
    auto out = compileAndRun(code("deque_from_list"));
    EXPECT_EQ(out, "3\n10\n");
}

TEST(CodeGenTest, ListRepeatIR) {
    auto ir = generateIR("x: list[bool] = [True] * 5");
    EXPECT_NE(ir.find("dragon_list_repeat"), std::string::npos);
}

TEST(CodeGenE2E, ListRepeatBool) {
    auto out = compileAndRun(code("list_repeat_bool"));
    EXPECT_EQ(out, "5\nTrue\nTrue\n");
}

TEST(CodeGenE2E, ListRepeatInt) {
    auto out = compileAndRun(code("list_repeat_int"));
    EXPECT_EQ(out, "6\n1\n2\n1\n2\n");
}

TEST(CodeGenE2E, ListRepeatStr) {
    auto out = compileAndRun(code("list_repeat_str"));
    EXPECT_EQ(out, "2\nhello\nhello\n");
}

TEST(CodeGenE2E, ListRepeatEmpty) {
    auto out = compileAndRun(code("list_repeat_empty"));
    EXPECT_EQ(out, "0\n");
}

TEST(CodeGenE2E, ListRepeatSingleInt) {
    auto out = compileAndRun(code("list_repeat_single_int"));
    EXPECT_EQ(out, "4\n0\n0\n");
}

TEST(CodeGenIR, DiscardedListStrPopEmitsDecref) {
    auto ir = generateIR(code("discarded_list_str_pop_emits_decref"));
    EXPECT_NE(ir.find("dragon_list_pop"), std::string::npos);
    EXPECT_NE(ir.find("dragon_decref_str"), std::string::npos)
        << "Discarded list[str].pop() must decref the popped string";
}

TEST(CodeGenE2E, DiscardedListStrPopLoopBounded) {
    auto out = compileAndRun(code("discarded_list_str_pop_loop_bounded"));
    EXPECT_EQ(out, "4\n");
}

TEST(CodeGenIR, DiscardedListListIntPopEmitsDecref) {
    auto ir = generateIR(code("discarded_list_list_int_pop_emits_decref"));
    EXPECT_NE(ir.find("dragon_list_pop"), std::string::npos);
    EXPECT_NE(ir.find("pop.discard.ptr"), std::string::npos)
        << "Expected IntToPtr conversion of popped i64 value";
    EXPECT_NE(ir.find("dragon_decref"), std::string::npos);
}

TEST(CodeGenE2E, DiscardedListListIntPopLoopBounded) {
    auto out = compileAndRun(code("discarded_list_list_int_pop_loop_bounded"));
    EXPECT_EQ(out, "3\n");
}

TEST(CodeGenIR, DiscardedDictStrStrPopEmitsDecref) {
    auto ir = generateIR(code("discarded_dict_str_str_pop_emits_decref"));
    EXPECT_NE(ir.find("dragon_dict_pop"), std::string::npos);
    EXPECT_NE(ir.find("dragon_decref_str"), std::string::npos)
        << "Discarded dict[str, str].pop must decref the popped value";
}

TEST(CodeGenE2E, DiscardedDictStrStrPopLoopBounded) {
    auto out = compileAndRun(code("discarded_dict_str_str_pop_loop_bounded"));
    EXPECT_EQ(out, "0\n");
}

TEST(CodeGenIR, DiscardedListIntPopNoDecref) {
    auto ir = generateIR(code("discarded_list_int_pop_no_decref"));
    EXPECT_NE(ir.find("dragon_list_pop"), std::string::npos);
    EXPECT_EQ(ir.find("pop.discard.ptr"), std::string::npos)
        << "Discarded list[int].pop must not emit decref conversion";
}

TEST(CodeGenE2E, DiscardedListIntPopRuns) {
    auto out = compileAndRun(code("discarded_list_int_pop_runs"));
    EXPECT_EQ(out, "2\n");
}

// Assigned pop() result must NOT be double-freed: the assignment increfs to keep
// ownership, so the discard fix may only fire on ExprStmt with no consumer.
TEST(CodeGenE2E, AssignedListStrPopNoDoubleFree) {
    auto out = compileAndRun(code("assigned_list_str_pop_no_double_free"));
    EXPECT_EQ(out, "world\n1\n");
}

TEST(CodeGenE2E, AssignedListStrPopLoopNoDoubleFree) {
    auto out = compileAndRun(code("assigned_list_str_pop_loop_no_double_free"));
    EXPECT_EQ(out, "hello\n4\n");
}

TEST(CodeGenE2E, AnnAssignConsumesPopNoDoubleFree) {
    auto out = compileAndRun(code("ann_assign_consumes_pop_no_double_free"));
    EXPECT_EQ(out, "world\n");
}

TEST(CodeGenE2E, DictValuesUniformLoopBounded) {
    auto out = compileAndRun(code("dict_values_uniform_loop_bounded"));
    EXPECT_EQ(out, "ok\n");
}
TEST(CodeGenE2E, ListExtendAdoptsTag) {
    auto out = compileAndRun(code("list_extend_adopts_tag"));
    EXPECT_EQ(out, "c\n");
}
// enumerate/zip must incref+tag elements so tuple owners survive source destruction
// (a borrowed read is a UAF) with balanced refcounts (no per-iteration leak).
TEST(CodeGenE2E, EnumerateZipBalancedRefcount) {
    auto out = compileAndRun(code("enumerate_zip_balanced_refcount"));
    EXPECT_EQ(out, "ok\n");
}

TEST(CodeGenE2E, DictGetCheckedTypeErrorMessage) {
    auto out = compileAndRun(code("dict_get_checked_type_error_message"));
    EXPECT_EQ(out, "caught\n");
}

TEST(CodeGenE2E, DictGetCheckedTypeErrorLoopBounded) {
    auto out = compileAndRun(code("dict_get_checked_type_error_loop_bounded"));
    EXPECT_EQ(out, "100000\n");
}

TEST(CodeGenE2E, DictGetCheckedAlternatingErrors) {
    auto out = compileAndRun(code("dict_get_checked_alternating_errors"));
    EXPECT_EQ(out, "100000\n");
}

TEST(CodeGenE2E, SetStrContainsContentHashed) {
    auto out = compileAndRun(code("set_str_contains_content_hashed"));
    EXPECT_EQ(out, "a\nb\n");
}

TEST(CodeGenE2E, SetStrAddDedup) {
    auto out = compileAndRun(code("set_str_add_dedup"));
    EXPECT_EQ(out, "2\n");
}

TEST(CodeGenE2E, SetMethodAddRemoveDiscard) {
    auto out = compileAndRun(code("set_method_add_remove_discard"));
    EXPECT_EQ(out, "3\n2\n2\ny\n");
}

TEST(CodeGenE2E, SetUnionIntersectionDifference) {
    auto out = compileAndRun(code("set_union_intersection_difference"));
    EXPECT_EQ(out, "4\n2\n1\n2\n");
}

TEST(CodeGenE2E, SetIssubsetIssupersetIsdisjoint) {
    auto out = compileAndRun(code("set_issubset_issuperset_isdisjoint"));
    EXPECT_EQ(out, "True\nFalse\nTrue\nTrue\nFalse\n");
}

TEST(CodeGenE2E, SetUtf8Keys) {
    auto out = compileAndRun(code("set_utf8_keys"));
    EXPECT_EQ(out, "a\nb\nc\n4\n");
}

TEST(CodeGenE2E, SetClearAndCopy) {
    auto out = compileAndRun(code("set_clear_and_copy"));
    EXPECT_EQ(out, "0\n3\n");
}

TEST(CodeGenE2E, SetUpdate) {
    auto out = compileAndRun(code("set_update"));
    EXPECT_EQ(out, "3\ny\n");
}

TEST(CodeGenE2E, PrintAndLenOnChainedSubscript) {
    auto out = compileAndRun(code("print_and_len_on_chained_subscript"));
    EXPECT_EQ(out, "['hello', 'world']\n2\n1\n");
}

TEST(CodeGenE2E, MethodCallOnSubscriptedStr) {
    auto out = compileAndRun(code("method_call_on_subscripted_str"));
    EXPECT_EQ(out, "True\n3\n");
}

TEST(CodeGenE2E, ForInOverListOfClassInstances) {
    auto out = compileAndRun(code("for_in_over_list_of_class_instances"));
    EXPECT_EQ(out, "a\nb\nc\n");
}

TEST(CodeGenE2E, StrBoolPredicatesPrintTrueFalse) {
    auto out = compileAndRun(code("str_bool_predicates_print_true_false"));
    EXPECT_EQ(out, "True\nFalse\nTrue\nFalse\n");
}

TEST(CodeGenE2E, DictSetFromBorrowedLocalStr) {
    auto out = compileAndRun(code("dict_set_from_borrowed_local_str"));
    EXPECT_EQ(out, "dragon\n");
}

TEST(CodeGenE2E, ListSetFromBorrowedLocalStr) {
    auto out = compileAndRun(code("list_set_from_borrowed_local_str"));
    EXPECT_EQ(out, "x\nb\n");
}

TEST(CodeGenE2E, TupleReturnFromBorrowedDictLocal) {
    auto out = compileAndRun(code("tuple_return_from_borrowed_dict_local"));
    EXPECT_EQ(out, "v\n");
}

TEST(CodeGenE2E, ListEqIntElementwise) {
    auto out = compileAndRun(code("list_eq_int_elementwise"));
    EXPECT_EQ(out, "True\nFalse\nTrue\n");
}

TEST(CodeGenE2E, ListEqStrElementwise) {
    auto out = compileAndRun(code("list_eq_str_elementwise"));
    EXPECT_EQ(out, "True\nFalse\n");
}

TEST(CodeGenE2E, ListEqNested) {
    auto out = compileAndRun(code("list_eq_nested"));
    EXPECT_EQ(out, "True\nFalse\n");
}

TEST(CodeGenE2E, ListEqDifferentLengths) {
    auto out = compileAndRun(code("list_eq_different_lengths"));
    EXPECT_EQ(out, "False\n");
}

TEST(CodeGenE2E, DictEqStrKeyedOrderIndependent) {
    auto out = compileAndRun(code("dict_eq_str_keyed_order_independent"));
    EXPECT_EQ(out, "True\nFalse\n");
}

TEST(CodeGenE2E, DictEqIntKeyed) {
    auto out = compileAndRun(code("dict_eq_int_keyed"));
    EXPECT_EQ(out, "True\nFalse\n");
}

TEST(CodeGenE2E, BoxedListEqViaAny) {
    auto out = compileAndRun(code("boxed_list_eq_via_any"));
    EXPECT_EQ(out, "True\nFalse\n");
}

TEST(CodeGenE2E, BoxedDictEqViaAny) {
    auto out = compileAndRun(code("boxed_dict_eq_via_any"));
    EXPECT_EQ(out, "True\nFalse\n");
}

TEST(CodeGenE2E, BoxedBytesEqViaAny) {
    auto out = compileAndRun(code("boxed_bytes_eq_via_any"));
    EXPECT_EQ(out, "True\nFalse\n");
}

TEST(CodeGenE2E, ConstAssignFromListSubscript) {
    auto out = compileAndRun(code("const_assign_from_list_subscript"));
    EXPECT_EQ(out, "hello\n");
}

TEST(CodeGenE2E, ListAnyMixedLiteralIndexedPrintsValues) {
    auto out = compileAndRun(code("list_any_mixed_literal_indexed_prints_values"));
    EXPECT_EQ(out, "10\nhi\n2.5\n");
}

TEST(CodeGenE2E, ListAnyHomogeneousIntLiteralIndexed) {
    auto out = compileAndRun(code("list_any_homogeneous_int_literal_indexed"));
    EXPECT_EQ(out, "1\n3\n");
}

TEST(CodeGenE2E, ListAnyElementIsCastableViaIsinstance) {
    auto out = compileAndRun(code("list_any_element_is_castable_via_isinstance"));
    EXPECT_EQ(out, "15\n");
}

TEST(CodeGenE2E, ListAnyIteration) {
    auto out = compileAndRun(code("list_any_iteration"));
    EXPECT_EQ(out, "1\ntwo\n3.0\n");
}

TEST(CodeGenE2E, ListAnyHoldingStrListPrintsTagAware) {
    auto out = compileAndRun(code("list_any_holding_str_list_prints_tag_aware"));
    EXPECT_EQ(out, "[['a', 'b'], [1, 2], [1.5, 2.5]]\n");
}

TEST(CodeGenE2E, DictStrAnyHoldingContainersPrintsTagAware) {
    auto out = compileAndRun(code("dict_str_any_holding_containers_prints_tag_aware"));
    EXPECT_EQ(out, "{'a': ['x', 'y'], 'b': {'inner': 'v'}}\n");
}

TEST(CodeGenE2E, DictIntAnyHoldingListPrintsTagAware) {
    auto out = compileAndRun(code("dict_int_any_holding_list_prints_tag_aware"));
    EXPECT_EQ(out, "{1: ['x', 'y']}\n");
}

TEST(CodeGenE2E, StrOfBytesDoesNotCrash) {
    auto out = compileAndRun(code("str_of_bytes_does_not_crash"));
    EXPECT_EQ(out, "b'\\x01\\x02\\x03'\n");
}

TEST(CodeGenE2E, StrOfBytesIsInjective) {
    auto out = compileAndRun(code("str_of_bytes_is_injective"));
    EXPECT_EQ(out, "512\n512\n");
}

TEST(CodeGenE2E, StrOfBytesSurvivesNulAndHighBytes) {
    auto out = compileAndRun(code("str_of_bytes_survives_nul_and_high_bytes"));
    EXPECT_EQ(out, "b'\\x02\\x80\\x00\\x00\\n'\nb'\\x02\\x80\\x00\\x00c'\nFalse\n");
}

TEST(CodeGenE2E, StrOfBytesAgreesWithPrintAndInterpolation) {
    auto out = compileAndRun(code("str_of_bytes_agrees_with_print_and_interpolation"));
    EXPECT_EQ(out, "b'\\t\\n\\rA'\nb'\\t\\n\\rA'\nb'\\t\\n\\rA'\n[b'\\t\\n\\rA']\n");
}

TEST(CodeGenE2E, DictLiteralClosureValueSurvivesScope) {
    auto out = compileAndRun(code("dict_literal_closure_value_survives_scope"));
    EXPECT_EQ(out, "4\n4\n");
}

TEST(CodeGenE2E, DictComprehensionContainerValuesKeepTheirTag) {
    auto out = compileAndRun(code("dict_comprehension_container_values_keep_their_tag"));
    EXPECT_EQ(out, "2\n1\n2\n");
}

TEST(CodeGenE2E, DictComprehensionSharedValueOutlivesSource) {
    auto out = compileAndRun(code("dict_comprehension_shared_value_outlives_source"));
    EXPECT_EQ(out, "2\n15\n7\n");
}

TEST(CodeGenE2E, DictDotAssignStrOutlivesSource) {
    auto out = compileAndRun(code("dict_dot_assign_str_outlives_source"));
    EXPECT_EQ(out, "n-4\nn-4\n");
}

static const size_t kPrintStreamWindowBytes = 1024;

static void expectPrintMatchesStr(const std::string& output) {
    auto nl = output.find('\n');
    ASSERT_NE(nl, std::string::npos);
    std::string printed = output.substr(0, nl);
    std::string rendered = output.substr(nl + 1);
    if (!rendered.empty() && rendered.back() == '\n') rendered.pop_back();
    EXPECT_EQ(printed, rendered);
}

TEST(CodeGenE2E, PrintListStrUnicodeMatchesStr) {
    auto output = compileAndRun(code("print_list_str_unicode_matches_str"));
    EXPECT_EQ(output, "['\xe6\xb7\xb1', 'abc']\n['\xe6\xb7\xb1', 'abc']\n['\xe6\xb7\xb1', 'abc']\n");
}

TEST(CodeGenE2E, PrintListInstanceMatchesStr) {
    auto output = compileAndRun(code("print_list_instance_matches_str"));
    EXPECT_EQ(output, "[<Money instance>]\n[<Money instance>]\n");
}

TEST(CodeGenE2E, PrintListBytesMatchesStr) {
    auto output = compileAndRun(code("print_list_bytes_matches_str"));
    EXPECT_EQ(output, "[b'ab']\n[b'ab']\n");
}

TEST(CodeGenE2E, PrintLargeListMatchesStr) {
    auto output = compileAndRun(code("print_large_list_matches_str"));
    expectPrintMatchesStr(output);
    EXPECT_GT(output.find('\n'), kPrintStreamWindowBytes);
}

TEST(CodeGenE2E, PrintAnyContainerMatchesStr) {
    auto output = compileAndRun(code("print_any_container_matches_str"));
    EXPECT_EQ(output, "[1, {'a': 2}]\n[1, {'a': 2}]\n");
}

TEST(CodeGenTest, BytesIndexIsAnInlineLoad) {
    auto ir = generateIR(code("bytes_index_inline"));
    EXPECT_EQ(ir.find("call i64 @dragon_bytes_get"), std::string::npos);
    EXPECT_NE(ir.find("bytes.len"), std::string::npos);
    EXPECT_NE(ir.find("bytes.idx.inbounds"), std::string::npos);
    EXPECT_NE(ir.find("bytes.elem"), std::string::npos);
    EXPECT_NE(ir.find("call void @dragon_bytes_index_error"), std::string::npos);
}

TEST(CodeGenTest, BytesLenIsAnInlineLoad) {
    auto ir = generateIR(code("bytes_len_inline"));
    EXPECT_EQ(ir.find("call i64 @dragon_bytes_len"), std::string::npos);
    EXPECT_NE(ir.find("bytes.len.safe"), std::string::npos);
}

TEST(CodeGenTest, BytesForeachHoldsNoRuntimeCall) {
    auto ir = generateIR(code("bytes_foreach_inline"));
    EXPECT_EQ(ir.find("call i64 @dragon_bytes_get"), std::string::npos);
    EXPECT_EQ(ir.find("call i64 @dragon_bytes_len"), std::string::npos);
    EXPECT_NE(ir.find("bytes.len.safe"), std::string::npos);
    EXPECT_NE(ir.find("bytes.data.safe"), std::string::npos);
    EXPECT_NE(ir.find("bytes.elem"), std::string::npos);
}

TEST(CodeGenTest, BytesThroughAUnionKeepsTheRuntimeCall) {
    auto ir = generateIR(code("bytes_through_a_union_calls_the_runtime"));
    EXPECT_NE(ir.find("@dragon_box_subscript"), std::string::npos);
    EXPECT_EQ(ir.find("bytes.elem"), std::string::npos);
}

static std::string defineOf(const std::string& ir, const std::string& symbol) {
    const std::string callee = " @" + symbol + "(";
    size_t start = ir.find("\ndefine ");
    while (start != std::string::npos) {
        size_t header = ir.find('\n', start + 1);
        size_t named = ir.find(callee, start);
        if (named != std::string::npos && named < header) {
            size_t end = ir.find("\n}\n", start);
            return end == std::string::npos ? ir.substr(start)
                                            : ir.substr(start, end - start);
        }
        start = ir.find("\ndefine ", start + 1);
    }
    return "";
}

TEST(CodeGenTest, ReadsOfOneBytesValueShareTheNullTestAndLength) {
    auto body = defineOf(generateIR(code("bytes_four_reads_of_one_value")),
                         "four_of");
    ASSERT_FALSE(body.empty());
    EXPECT_EQ(countSubstring(body, "bytes.isnull"), 2u) << body;
    EXPECT_EQ(countSubstring(body, "bytes.len.gep"), 2u) << body;
    EXPECT_EQ(countSubstring(body, "call void @dragon_bytes_index_error"), 1u) << body;
    EXPECT_EQ(countSubstring(body, "bytes.idx.inbounds"), 8u) << body;
    EXPECT_EQ(countSubstring(body, "bytes.elem.gep"), 8u) << body;
}

TEST(CodeGenTest, ACallBetweenBytesReadsStartsAFreshCheck) {
    auto body = defineOf(generateIR(code("bytes_reads_after_a_call")), "two_of");
    ASSERT_FALSE(body.empty());
    EXPECT_EQ(countSubstring(body, "bytes.isnull"), 4u) << body;
    EXPECT_EQ(countSubstring(body, "bytes.len.gep"), 4u) << body;
}

static CodeGenOptions releaseOptions() {
    CodeGenOptions opts;
    opts.optimizationLevel = 3;
    return opts;
}

TEST(CodeGenTest, BytesDecodeHelpersLeaveNoCallAtO3) {
    auto ir = generateOptimizedIR(code("bytes_decode_reduction"), releaseOptions());
    EXPECT_EQ(countSubstring(ir, "@struct__unpack_u32_le"), 0u) << ir;
    EXPECT_EQ(countSubstring(ir, "@struct__unpack_u64_le"), 0u) << ir;
}
