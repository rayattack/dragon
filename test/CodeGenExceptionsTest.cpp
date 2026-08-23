#include "CodeGenTestHelpers.h"
#include "CodeBlock.h"

static std::string code(const std::string& block) {
    return extractCode("CodeGenExceptionsTest.md", block);
}

TEST(CodeGenTest, TryExceptBasic) {
    auto ir = generateIR(code("try_except_basic"));
    EXPECT_NE(ir.find("dragon_exc_push_frame"), std::string::npos);
    EXPECT_NE(ir.find("setjmp"), std::string::npos);
    EXPECT_NE(ir.find("dragon_exc_pop_frame"), std::string::npos);
}

TEST(CodeGenTest, TryExceptTyped) {
    auto ir = generateIR(code("try_except_typed"));
    EXPECT_NE(ir.find("dragon_exc_get_type"), std::string::npos);
    EXPECT_NE(ir.find("icmp eq"), std::string::npos);
}

TEST(CodeGenTest, TryExceptFinally) {
    auto ir = generateIR(code("try_except_finally"));
    EXPECT_NE(ir.find("try0.finally"), std::string::npos);
}

TEST(CodeGenTest, TryExceptElse) {
    auto ir = generateIR(code("try_except_else"));
    EXPECT_NE(ir.find("try0.else"), std::string::npos);
}

TEST(CodeGenTest, TryMultipleHandlers) {
    auto ir = generateIR(code("try_multiple_handlers"));
    EXPECT_NE(ir.find("handler.check.0"), std::string::npos);
    EXPECT_NE(ir.find("handler.check.1"), std::string::npos);
    EXPECT_NE(ir.find("dragon_raise_exc"), std::string::npos);
}

TEST(CodeGenTest, RaiseValueError) {
    auto ir = generateIR("raise ValueError(\"bad value\")");
    EXPECT_NE(ir.find("dragon_raise_exc"), std::string::npos);
    EXPECT_NE(ir.find("bad value"), std::string::npos);
    EXPECT_NE(ir.find("unreachable"), std::string::npos);
}

TEST(CodeGenTest, RaiseBare) {
    auto ir = generateIR("raise");
    EXPECT_NE(ir.find("dragon_raise_exc"), std::string::npos);
    EXPECT_NE(ir.find("dragon_exc_get_type"), std::string::npos);
    EXPECT_NE(ir.find("dragon_exc_get_msg"), std::string::npos);
}

TEST(CodeGenTest, TryExceptNamedHandler) {
    auto ir = generateIR(code("try_except_named_handler"));
    EXPECT_NE(ir.find("dragon_exc_get_msg"), std::string::npos);
}

TEST(CodeGenIR, ExceptStarParsesIR) {
    auto ir = generateIR(code("except_star_parses_ir"));
    EXPECT_NE(ir.find("define"), std::string::npos);
}

TEST(CodeGenTest, ExcHierarchyMatchCallIR) {
    auto ir = generateIR(code("exc_hierarchy_match_call_ir"));
    EXPECT_NE(ir.find("dragon_exc_matches"), std::string::npos);
    EXPECT_NE(ir.find("exc.match.0"), std::string::npos);
}

TEST(CodeGenTest, ExcHierarchyLeafMatchIR) {
    auto ir = generateIR(code("exc_hierarchy_leaf_match_ir"));
    EXPECT_NE(ir.find("dragon_exc_matches"), std::string::npos);
    EXPECT_NE(ir.find("exc.match.0"), std::string::npos);
}

TEST(CodeGenTest, ExcHierarchyExceptionMatchIR) {
    auto ir = generateIR(code("exc_hierarchy_exception_match_ir"));
    EXPECT_NE(ir.find("dragon_exc_matches"), std::string::npos);
}

TEST(CodeGenTest, UserExcRegisterCallIR) {
    auto ir = generateIR(code("user_exc_register_call_ir"));
    EXPECT_NE(ir.find("dragon_exc_register"), std::string::npos);
}

TEST(CodeGenTest, UserExcMatchesCallIR) {
    auto ir = generateIR(code("user_exc_matches_call_ir"));
    EXPECT_NE(ir.find("dragon_exc_matches"), std::string::npos);
    EXPECT_NE(ir.find("exc.match.0"), std::string::npos);
}

TEST(CodeGenE2E, TryCatchBasic) {
    auto output = compileAndRun(code("try_catch_basic"));
    EXPECT_EQ(output, "caught\nbad\n");
}

TEST(CodeGenE2E, TryFinallyExec) {
    auto output = compileAndRun(code("try_finally_exec"));
    EXPECT_EQ(output, "handler\nfinally\n");
}

TEST(CodeGenE2E, TryCatchElse) {
    auto output = compileAndRun(code("try_catch_else"));
    EXPECT_EQ(output, "ok\nno error\n");
}

TEST(CodeGenE2E, FinallyOnReturn) {
    auto output = compileAndRun(code("finally_on_return"));
    EXPECT_EQ(output, "try\nfinally\n42\n");
}

TEST(CodeGenE2E, FinallyOnBreak) {
    auto output = compileAndRun(code("finally_on_break"));
    EXPECT_EQ(output, "0\nf\n1\nf\nf\ndone\n");
}

TEST(CodeGenE2E, FinallyOnContinue) {
    auto output = compileAndRun(code("finally_on_continue"));
    EXPECT_EQ(output, "0\nf\nf\n2\nf\n");
}

TEST(CodeGenE2E, ExcHierarchyArithmeticCatchesZeroDiv) {
    auto output = compileAndRun(code("exc_hierarchy_arithmetic_catches_zero_div"));
    EXPECT_EQ(output, "caught\ndiv0\n");
}

TEST(CodeGenE2E, ExcHierarchyArithmeticCatchesOverflow) {
    auto output = compileAndRun(code("exc_hierarchy_arithmetic_catches_overflow"));
    EXPECT_EQ(output, "caught\ntoo big\n");
}

TEST(CodeGenE2E, ExcHierarchyLookupCatchesIndex) {
    auto output = compileAndRun(code("exc_hierarchy_lookup_catches_index"));
    EXPECT_EQ(output, "caught\noob\n");
}

TEST(CodeGenE2E, ExcHierarchyLookupCatchesKey) {
    auto output = compileAndRun(code("exc_hierarchy_lookup_catches_key"));
    EXPECT_EQ(output, "caught\nmissing\n");
}

TEST(CodeGenE2E, ExcHierarchyExceptionCatchesValue) {
    auto output = compileAndRun(code("exc_hierarchy_exception_catches_value"));
    EXPECT_EQ(output, "caught\nbad\n");
}

TEST(CodeGenE2E, ExcHierarchyOSErrorCatchesFileNotFound) {
    auto output = compileAndRun(code("exc_hierarchy_os_error_catches_file_not_found"));
    EXPECT_EQ(output, "caught\nno file\n");
}

TEST(CodeGenE2E, ExcHierarchyOSErrorCatchesConnectionChild) {
    auto output = compileAndRun(code("exc_hierarchy_os_error_catches_connection_child"));
    EXPECT_EQ(output, "caught\nrefused\n");
}

TEST(CodeGenE2E, ExcHierarchyConnectionCatchesBroken) {
    auto output = compileAndRun(code("exc_hierarchy_connection_catches_broken"));
    EXPECT_EQ(output, "caught\npipe\n");
}

TEST(CodeGenE2E, ExcHierarchyValueCatchesUnicode) {
    auto output = compileAndRun(code("exc_hierarchy_value_catches_unicode"));
    EXPECT_EQ(output, "caught\ndecode fail\n");
}

TEST(CodeGenE2E, ExcHierarchyRuntimeCatchesNotImpl) {
    auto output = compileAndRun(code("exc_hierarchy_runtime_catches_not_impl"));
    EXPECT_EQ(output, "caught\ntodo\n");
}

TEST(CodeGenE2E, ExcHierarchyLeafNoMatchReraise) {
    auto output = compileAndRun(code("exc_hierarchy_leaf_no_match_reraise"));
    EXPECT_EQ(output, "correct\nk\n");
}

TEST(CodeGenE2E, ExcHierarchyMultiSpecific) {
    auto output = compileAndRun(code("exc_hierarchy_multi_specific"));
    EXPECT_EQ(output, "index\n");
}

TEST(CodeGenE2E, ExcHierarchyNameCatchesUnbound) {
    auto output = compileAndRun(code("exc_hierarchy_name_catches_unbound"));
    EXPECT_EQ(output, "caught\nx\n");
}

TEST(CodeGenE2E, ExcHierarchyImportCatchesModuleNotFound) {
    auto output = compileAndRun(code("exc_hierarchy_import_catches_module_not_found"));
    EXPECT_EQ(output, "caught\nno mod\n");
}

TEST(CodeGenE2E, UserExcBasicRaiseCatch) {
    auto output = compileAndRun(code("user_exc_basic_raise_catch"));
    EXPECT_EQ(output, "caught\napp fail\n");
}

TEST(CodeGenE2E, UserExcParentCatchesChild) {
    auto output = compileAndRun(code("user_exc_parent_catches_child"));
    EXPECT_EQ(output, "caught\n404\n");
}

TEST(CodeGenE2E, UserExcBuiltinParentCatchesUser) {
    auto output = compileAndRun(code("user_exc_builtin_parent_catches_user"));
    EXPECT_EQ(output, "caught\ncustom\n");
}

TEST(CodeGenE2E, UserExcExceptionCatchesUser) {
    auto output = compileAndRun(code("user_exc_exception_catches_user"));
    EXPECT_EQ(output, "caught\nval\n");
}

TEST(CodeGenE2E, UserExcNoMatchReraise) {
    auto output = compileAndRun(code("user_exc_no_match_reraise"));
    EXPECT_EQ(output, "correct\nb\n");
}

TEST(CodeGenE2E, UserExcMultiHandler) {
    auto output = compileAndRun(code("user_exc_multi_handler"));
    EXPECT_EQ(output, "specific\n");
}

TEST(CodeGenE2E, UserExcGrandparentCatches) {
    auto output = compileAndRun(code("user_exc_grandparent_catches"));
    EXPECT_EQ(output, "caught\ndeep\n");
}

TEST(CodeGenE2E, UserExcNoArgDefaultMsg) {
    auto output = compileAndRun(code("user_exc_no_arg_default_msg"));
    EXPECT_EQ(output, "caught\nEmptyError\n");
}

TEST(CodeGenIR, MatchStmtIR) {
    auto ir = generateIR(code("match_stmt_ir"));
    EXPECT_NE(ir.find("match.subject"), std::string::npos);
    EXPECT_NE(ir.find("match.end"), std::string::npos);
    EXPECT_NE(ir.find("match.case0"), std::string::npos);
}

TEST(CodeGenIR, PyMatchCaseIR) {
    auto ir = generateIRPy(code("py_match_case_ir"));
    EXPECT_NE(ir.find("define"), std::string::npos);
    EXPECT_NE(ir.find("dragon_print_int"), std::string::npos);
}

TEST(CodeGenE2E, MatchIntLiteral) {
    auto output = compileAndRun(code("match_int_literal"));
    EXPECT_EQ(output, "20\n");
}

TEST(CodeGenE2E, MatchWildcard) {
    auto output = compileAndRun(code("match_wildcard"));
    EXPECT_EQ(output, "42\n");
}

TEST(CodeGenE2E, MatchCapture) {
    auto output = compileAndRun(code("match_capture"));
    EXPECT_EQ(output, "7\n");
}

TEST(CodeGenE2E, MatchStringLiteral) {
    auto output = compileAndRun(code("match_string_literal"));
    EXPECT_EQ(output, "2\n");
}

TEST(CodeGenE2E, MatchOrPattern) {
    auto output = compileAndRun(code("match_or_pattern"));
    EXPECT_EQ(output, "20\n");
}

TEST(CodeGenE2E, MatchNoArmMatches) {
    auto output = compileAndRun(code("match_no_arm_matches"));
    EXPECT_EQ(output, "0\n");
}

TEST(CodeGenE2E, MatchFirstArmMatches) {
    auto output = compileAndRun(code("match_first_arm_matches"));
    EXPECT_EQ(output, "10\n");
}

TEST(CodeGenE2E, MatchWithGuard) {
    auto output = compileAndRun(code("match_with_guard"));
    EXPECT_EQ(output, "2\n");
}

TEST(CodeGenE2E, MatchCommaOrPattern) {
    auto output = compileAndRun(code("match_comma_or_pattern"));
    EXPECT_EQ(output, "10\n");
}

TEST(CodeGenE2E, MatchPipeOrPatternRegression) {
    auto output = compileAndRun(code("match_pipe_or_pattern_regression"));
    EXPECT_EQ(output, "20\n");
}

TEST(CodeGenE2E, PyMatchCaseE2E) {
    auto out = compileAndRunPy(code("py_match_case_e2_e"));
    EXPECT_EQ(out, "20\n");
}

TEST(CodeGenE2E, MatchArmCaptureBoundedLoop) {
    auto output = compileAndRun(code("match_arm_capture_bounded_loop"));
    EXPECT_EQ(output, "49995001\n");
}

TEST(CodeGenE2E, MatchStringSubjectLoopBounded) {
    auto output = compileAndRun(code("match_string_subject_loop_bounded"));
    EXPECT_EQ(output, "3000\n3000\n3000\n");
}

TEST(CodeGenE2E, ExceptionRaiseAndCatch_AllBuiltins) {
    auto out = compileAndRun(code("exception_raise_and_catch__all_builtins"));
    EXPECT_EQ(out, "ve\nte\nle\nie\nae\noe\nfe\npe\nioe\nme\nne\nre\nue\naa\nnme\nse\nasx\nbe\n");
}

TEST(CodeGenIR, MatchArmEmitsCleanupBeforeEndBranch) {
    auto ir = generateIR(code("match_arm_emits_cleanup_before_end_branch"));
    EXPECT_NE(ir.find("match.end"), std::string::npos)
        << "Expected match.end basic block\nIR:\n" << ir;
    EXPECT_NE(ir.find("match.subject"), std::string::npos);
    EXPECT_NE(ir.find("match.case0"), std::string::npos);
}

TEST(CodeGenE2E, ExceptionClassAsValue) {
    auto out = compileAndRun(code("exception_class_as_value"));
    EXPECT_EQ(out, "1\n");
}

TEST(CodeGenE2E, ExcMatchesExactType) {
    auto out = compileAndRun(code("exc_matches_exact_type"));
    EXPECT_EQ(out, "True\n");
}

TEST(CodeGenE2E, ExcMatchesParentRange) {
    auto out = compileAndRun(code("exc_matches_parent_range"));
    EXPECT_EQ(out, "True\n");
}

TEST(CodeGenE2E, ExcMatchesWrongType) {
    auto out = compileAndRun(code("exc_matches_wrong_type"));
    EXPECT_EQ(out, "False\n");
}

TEST(CodeGenE2E, IntStrValidForms) {
    auto out = compileAndRun(code("int_str_valid_forms"));
    EXPECT_EQ(out, "42\n-17\n5\n1000\n");
}

TEST(CodeGenE2E, IntStrInvalidRaisesValueError) {
    auto out = compileAndRun(code("int_str_invalid_raises_value_error"));
    EXPECT_EQ(out, "True\n");
}

TEST(CodeGenE2E, IntStrFloatStringRaises) {
    auto out = compileAndRun(code("int_str_float_string_raises"));
    EXPECT_EQ(out, "True\n");
}
