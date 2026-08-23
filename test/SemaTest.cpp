#include <gtest/gtest.h>
#include "TestHelpers.h"
#include "CodeBlock.h"

using namespace dragon;
using namespace dragon::test;

static std::string code(const std::string& block) {
    return extractCode("SemaTest.md", block);
}

static bool analyzeOk(const std::string& source) {
    auto module = parse(source);
    if (!module) return false;
    Sema sema;
    return sema.analyze(*module);
}

static bool analyzeHasErrors(const std::string& source) {
    auto module = parse(source);
    if (!module) return false;
    Sema sema;
    sema.analyze(*module);
    return sema.hasErrors();
}

TEST(SemaTest, EmptyModule) {
    EXPECT_TRUE(analyzeOk(""));
}

TEST(SemaTest, PassStatement) {
    EXPECT_TRUE(analyzeOk("pass"));
}

TEST(SemaTest, IntegerLiteral) {
    EXPECT_TRUE(analyzeOk("42"));
}

TEST(SemaTest, StringLiteral) {
    EXPECT_TRUE(analyzeOk("\"hello\""));
}

TEST(SemaTest, BuiltinPrint) {
    EXPECT_TRUE(analyzeOk("print(\"hello\")"));
}

TEST(SemaTest, BuiltinLen) {
    EXPECT_TRUE(analyzeOk("len([1, 2, 3])"));
}

TEST(SemaTest, BuiltinRange) {
    EXPECT_TRUE(analyzeOk("range(10)"));
}

TEST(SemaTest, UndefinedName) {
    EXPECT_TRUE(analyzeHasErrors("undefined_var"));
}

TEST(SemaTest, LockIsImportGated) {
    EXPECT_TRUE(analyzeHasErrors("lock: Lock = Lock()"));
    EXPECT_TRUE(analyzeOk("from threading import Lock\nlock: Lock = Lock()"));
}

TEST(SemaTest, BareAssignmentDoesNotDeclare) {
    EXPECT_TRUE(analyzeHasErrors("x = 5\nprint(x)"));
}

TEST(SemaTest, DefinedByAnnotation) {
    EXPECT_TRUE(analyzeOk("x: int = 5\nprint(x)"));
}

TEST(SemaTest, AnnotationOnlyDefinesVar) {
    EXPECT_TRUE(analyzeOk("x: int\nprint(x)"));
}

TEST(SemaTest, UseBeforeDefine) {
    EXPECT_TRUE(analyzeHasErrors("print(x)\nx = 5"));
}

TEST(SemaTest, FunctionDefinesName) {
    EXPECT_TRUE(analyzeOk(code("function_defines_name")));
}

TEST(SemaTest, FunctionParams) {
    EXPECT_TRUE(analyzeOk(code("function_params")));
}

TEST(SemaTest, FunctionParamNotInOuterScope) {
    EXPECT_TRUE(analyzeHasErrors(code("function_param_not_in_outer_scope")));
}

TEST(SemaTest, ClassDefinesName) {
    EXPECT_TRUE(analyzeOk(code("class_defines_name")));
}

TEST(SemaTest, ForLoopDefinesVariable) {
    EXPECT_TRUE(analyzeOk(code("for_loop_defines_variable")));
}

TEST(SemaTest, ImportDefinesName) {
    EXPECT_TRUE(analyzeOk(code("import_defines_name")));
}

TEST(SemaTest, FromImportDefinesName) {
    EXPECT_TRUE(analyzeOk(code("from_import_defines_name")));
}

TEST(SemaTest, ImportAlias) {
    EXPECT_TRUE(analyzeOk(code("import_alias")));
}

TEST(SemaTest, GlobalStatement) {
    EXPECT_TRUE(analyzeOk(code("global_statement")));
}

TEST(SemaTest, BreakInsideLoop) {
    EXPECT_TRUE(analyzeOk(code("break_inside_loop")));
}

TEST(SemaTest, BreakOutsideLoop) {
    EXPECT_TRUE(analyzeHasErrors("break"));
}

TEST(SemaTest, ContinueInsideLoop) {
    EXPECT_TRUE(analyzeOk(code("continue_inside_loop")));
}

TEST(SemaTest, ContinueOutsideLoop) {
    EXPECT_TRUE(analyzeHasErrors("continue"));
}

TEST(SemaTest, ReturnInsideFunction) {
    EXPECT_TRUE(analyzeOk(code("return_inside_function")));
}

TEST(SemaTest, ReturnOutsideFunction) {
    EXPECT_TRUE(analyzeHasErrors("return 42"));
}

TEST(SemaTest, BreakInFor) {
    EXPECT_TRUE(analyzeOk(code("break_in_for")));
}

TEST(SemaTest, NestedLoopBreak) {
    EXPECT_TRUE(analyzeOk(code("nested_loop_break")));
}

TEST(SemaTest, TrueConstant) {
    EXPECT_TRUE(analyzeOk("print(True)"));
}

TEST(SemaTest, FalseConstant) {
    EXPECT_TRUE(analyzeOk("print(False)"));
}

TEST(SemaTest, NoneConstant) {
    EXPECT_TRUE(analyzeOk("print(None)"));
}

TEST(SemaTest, BuiltinExceptions) {
    EXPECT_TRUE(analyzeOk("raise ValueError(\"oops\")"));
}

TEST(SemaTest, AllBuiltinsAvailable) {
    EXPECT_TRUE(analyzeOk("print(abs(min(max(1, 2), 3)))"));
}

TEST(SemaTest, TryCatchDefinesHandlerVar) {
    EXPECT_TRUE(analyzeOk(code("try_catch_defines_handler_var")));
}

TEST(SemaTest, WithStatementDefinesVar) {
    EXPECT_TRUE(analyzeOk(code("with_statement_defines_var")));
}

TEST(SemaTest, ClassWithMethodUsingSelf) {
    EXPECT_TRUE(analyzeOk(code("class_with_method_using_self")));
}

TEST(SemaTest, ImplicitSelfResolvesInBody) {
    EXPECT_TRUE(analyzeOk(code("implicit_self_resolves_in_body")));
}

TEST(SemaTest, ImplicitSelfNotInParams) {
    auto module = parse(code("implicit_self_not_in_params"));
    ASSERT_NE(module, nullptr);
    Sema sema;
    EXPECT_TRUE(sema.analyze(*module));
    auto* cls = dynamic_cast<ClassDecl*>(module->body[0].get());
    ASSERT_NE(cls, nullptr);
    auto* method = dynamic_cast<FunctionDecl*>(cls->body[0].get());
    ASSERT_NE(method, nullptr);
    EXPECT_EQ(method->params.size(), 1u);
    EXPECT_EQ(method->params[0].name, "dx");
}

TEST(SemaTest, FunctionCallingFunction) {
    EXPECT_TRUE(analyzeOk(code("function_calling_function")));
}

TEST(SemaTest, AssignmentInCondition) {
    EXPECT_TRUE(analyzeOk(code("assignment_in_condition")));
}

TEST(SemaTest, ConstDeclOk) {
    EXPECT_TRUE(analyzeOk("const MAX: int = 100\nprint(MAX)"));
}

TEST(SemaTest, ConstReassignError) {
    EXPECT_TRUE(analyzeHasErrors("const MAX: int = 100\nMAX = 200"));
}

TEST(SemaTest, ConstAugAssignError) {
    EXPECT_TRUE(analyzeHasErrors("const X: int = 10\nX += 1"));
}

TEST(SemaTest, StaticFieldOk) {
    EXPECT_TRUE(analyzeOk("class Foo {\n  static x: int = 0\n}"));
}

TEST(SemaTest, DefCtorOk) {
    EXPECT_TRUE(analyzeOk(code("def_ctor_ok")));
}

TEST(SemaTest, MatchCaptureInOrSubPattern) {
    EXPECT_TRUE(analyzeOk(code("match_capture_in_or_sub_pattern")));
}

TEST(SemaTest, ModuleLevelDeferRejected) {
    EXPECT_TRUE(analyzeHasErrors(code("module_level_defer_rejected")));
}

TEST(SemaTest, DeferInsideFunctionAccepted) {
    EXPECT_TRUE(analyzeOk(code("defer_inside_function_accepted")));
}

TEST(SemaTest, ExceptAsTargetIsHandlerLocal) {
    EXPECT_TRUE(analyzeHasErrors(code("except_as_target_is_handler_local")));
    EXPECT_TRUE(analyzeOk(code("except_as_target_is_handler_local_2")));
}
