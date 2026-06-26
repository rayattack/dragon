#include <gtest/gtest.h>
#include "TestHelpers.h"

using namespace dragon;
using namespace dragon::test;

// Helper to run sema and check for no errors
static bool analyzeOk(const std::string& source) {
    auto module = parse(source);
    if (!module) return false;
    Sema sema;
    return sema.analyze(*module);
}

// Helper to run sema and check that errors occur
static bool analyzeHasErrors(const std::string& source) {
    auto module = parse(source);
    if (!module) return false;
    Sema sema;
    sema.analyze(*module);
    return sema.hasErrors();
}

//===----------------------------------------------------------------------===//
// Basic Tests
//===----------------------------------------------------------------------===//

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

//===----------------------------------------------------------------------===//
// Name Resolution
//===----------------------------------------------------------------------===//

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
    // Lock is NOT a global builtin (unlike Task) - it is import-gated, matching
    // Python's `from threading import Lock`. Bare use is an undefined name.
    EXPECT_TRUE(analyzeHasErrors("lock: Lock = Lock()"));
    // With the import it resolves (Sema binds imported names unconditionally).
    EXPECT_TRUE(analyzeOk("from threading import Lock\nlock: Lock = Lock()"));
}

TEST(SemaTest, BareAssignmentDoesNotDeclare) {
    // A bare `x = 5` is a reassignment, not a declaration: the name must
    // already exist in scope. Introducing a variable requires an annotation
    // (`x: int = 5`), so first-use-by-bare-assignment is an error.
    EXPECT_TRUE(analyzeHasErrors("x = 5\nprint(x)"));
}

TEST(SemaTest, DefinedByAnnotation) {
    EXPECT_TRUE(analyzeOk("x: int = 5\nprint(x)"));
}

TEST(SemaTest, AnnotationOnlyDefinesVar) {
    // Just annotation without value should define the variable
    EXPECT_TRUE(analyzeOk("x: int\nprint(x)"));
}

TEST(SemaTest, UseBeforeDefine) {
    EXPECT_TRUE(analyzeHasErrors("print(x)\nx = 5"));
}

//===----------------------------------------------------------------------===//
// Scope Tests
//===----------------------------------------------------------------------===//

TEST(SemaTest, FunctionDefinesName) {
    EXPECT_TRUE(analyzeOk(
        "def foo() {\n"
        "  pass\n"
        "}\n"
        "foo()"
    ));
}

TEST(SemaTest, FunctionParams) {
    EXPECT_TRUE(analyzeOk(
        "def add(x: int, y: int) -> int {\n"
        "  return x + y\n"
        "}"
    ));
}

TEST(SemaTest, FunctionParamNotInOuterScope) {
    EXPECT_TRUE(analyzeHasErrors(
        "def foo(x: int) {\n"
        "  pass\n"
        "}\n"
        "print(x)"  // x is not defined outside
    ));
}

TEST(SemaTest, ClassDefinesName) {
    EXPECT_TRUE(analyzeOk(
        "class Foo {\n"
        "  pass\n"
        "}\n"
        "Foo()"
    ));
}

TEST(SemaTest, ForLoopDefinesVariable) {
    EXPECT_TRUE(analyzeOk(
        "for i in range(10) {\n"
        "  print(i)\n"
        "}"
    ));
}

TEST(SemaTest, ImportDefinesName) {
    EXPECT_TRUE(analyzeOk(
        "import os\n"
        "print(os)"
    ));
}

TEST(SemaTest, FromImportDefinesName) {
    EXPECT_TRUE(analyzeOk(
        "from os import path\n"
        "print(path)"
    ));
}

TEST(SemaTest, ImportAlias) {
    EXPECT_TRUE(analyzeOk(
        "import numpy as np\n"
        "print(np)"
    ));
}

TEST(SemaTest, GlobalStatement) {
    EXPECT_TRUE(analyzeOk(
        "global x\n"
        "print(x)"
    ));
}

//===----------------------------------------------------------------------===//
// Control Flow Context
//===----------------------------------------------------------------------===//

TEST(SemaTest, BreakInsideLoop) {
    EXPECT_TRUE(analyzeOk(
        "while True {\n"
        "  break\n"
        "}"
    ));
}

TEST(SemaTest, BreakOutsideLoop) {
    EXPECT_TRUE(analyzeHasErrors("break"));
}

TEST(SemaTest, ContinueInsideLoop) {
    EXPECT_TRUE(analyzeOk(
        "while True {\n"
        "  continue\n"
        "}"
    ));
}

TEST(SemaTest, ContinueOutsideLoop) {
    EXPECT_TRUE(analyzeHasErrors("continue"));
}

TEST(SemaTest, ReturnInsideFunction) {
    EXPECT_TRUE(analyzeOk(
        "def foo() {\n"
        "  return 42\n"
        "}"
    ));
}

TEST(SemaTest, ReturnOutsideFunction) {
    EXPECT_TRUE(analyzeHasErrors("return 42"));
}

TEST(SemaTest, BreakInFor) {
    EXPECT_TRUE(analyzeOk(
        "for i in range(10) {\n"
        "  if i == 5 {\n"
        "    break\n"
        "  }\n"
        "}"
    ));
}

TEST(SemaTest, NestedLoopBreak) {
    EXPECT_TRUE(analyzeOk(
        "while True {\n"
        "  for i in range(10) {\n"
        "    break\n"
        "  }\n"
        "  break\n"
        "}"
    ));
}

//===----------------------------------------------------------------------===//
// Builtin Types and Functions
//===----------------------------------------------------------------------===//

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

//===----------------------------------------------------------------------===//
// Try/Catch
//===----------------------------------------------------------------------===//

TEST(SemaTest, TryCatchDefinesHandlerVar) {
    EXPECT_TRUE(analyzeOk(
        "try {\n"
        "  pass\n"
        "} catch ValueError as e {\n"
        "  print(e)\n"
        "}"
    ));
}

//===----------------------------------------------------------------------===//
// With Statement
//===----------------------------------------------------------------------===//

TEST(SemaTest, WithStatementDefinesVar) {
    EXPECT_TRUE(analyzeOk(
        "class Ctx {\n"
        "  def __enter__() -> Ctx { return self }\n"
        "  def __exit__() -> int { return 0 }\n"
        "}\n"
        "with Ctx() as f {\n"
        "  print(f)\n"
        "}"
    ));
}

//===----------------------------------------------------------------------===//
// Complex Integration
//===----------------------------------------------------------------------===//

TEST(SemaTest, ClassWithMethodUsingSelf) {
    EXPECT_TRUE(analyzeOk(
        "class Foo {\n"
        "  def bar() {\n"
        "    print(self)\n"
        "  }\n"
        "}"
    ));
}

//===----------------------------------------------------------------------===//
// Implicit Self (Decision 006)
//===----------------------------------------------------------------------===//

TEST(SemaTest, ImplicitSelfResolvesInBody) {
    // self.x should resolve without error - self is implicitly defined
    EXPECT_TRUE(analyzeOk(
        "class Point {\n"
        "  def move(dx: int) {\n"
        "    self.x = dx\n"
        "  }\n"
        "}"
    ));
}

TEST(SemaTest, ImplicitSelfNotInParams) {
    // Method has 1 declared param (dx), but self is also in scope
    auto module = parse(
        "class Point {\n"
        "  def move(dx: int) {\n"
        "    self.x = dx\n"
        "  }\n"
        "}"
    );
    ASSERT_NE(module, nullptr);
    Sema sema;
    EXPECT_TRUE(sema.analyze(*module));
    // Verify the method only has 1 explicit param
    auto* cls = dynamic_cast<ClassDecl*>(module->body[0].get());
    ASSERT_NE(cls, nullptr);
    auto* method = dynamic_cast<FunctionDecl*>(cls->body[0].get());
    ASSERT_NE(method, nullptr);
    EXPECT_EQ(method->params.size(), 1u);
    EXPECT_EQ(method->params[0].name, "dx");
}

TEST(SemaTest, FunctionCallingFunction) {
    EXPECT_TRUE(analyzeOk(
        "def add(a: int, b: int) -> int {\n"
        "  return a + b\n"
        "}\n"
        "def main() {\n"
        "  x: int = add(1, 2)\n"
        "  print(x)\n"
        "}"
    ));
}

TEST(SemaTest, AssignmentInCondition) {
    EXPECT_TRUE(analyzeOk(
        "x: int = 10\n"
        "if x {\n"
        "  print(x)\n"
        "}"
    ));
}

//===----------------------------------------------------------------------===//
// Decision 009: const, static, def() constructors
//===----------------------------------------------------------------------===//

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
    EXPECT_TRUE(analyzeOk(
        "class Foo {\n"
        "  def(x: int) {\n"
        "    self.x = x\n"
        "  }\n"
        "}"
    ));
}

TEST(SemaTest, MatchCaptureInOrSubPattern) {
    // Capture inside Or sub-pattern should be properly defined
    EXPECT_TRUE(analyzeOk(
        "x: int = 5\n"
        "match x {\n"
        "  case 1 | n { print(n) }\n"
        "}\n"
    ));
}
