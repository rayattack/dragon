#include <gtest/gtest.h>
#include "dragon/PythonMigrator.h"
#include "dragon/TypeInference.h"
#include "TestHelpers.h"

using namespace dragon;
using namespace dragon::test;

// Helper: migrate Python source to Dragon with braces
static std::string migrate(const std::string& source, bool addTypes = true) {
    MigrationOptions opts;
    opts.useBraces = true;
    opts.addTypes = addTypes;
    PythonMigrator migrator(opts);
    return migrator.migrateSource(source);
}

// Helper: migrate Python without braces (keep indentation)
static std::string migrateIndent(const std::string& source) {
    MigrationOptions opts;
    opts.useBraces = false;
    opts.addTypes = true;
    PythonMigrator migrator(opts);
    return migrator.migrateSource(source);
}

//===----------------------------------------------------------------------===//
// Basic Expression Emission
//===----------------------------------------------------------------------===//

TEST(MigratorTest, IntegerLiteral) {
    auto result = migrate("42\n");
    EXPECT_NE(result.find("42"), std::string::npos);
}

TEST(MigratorTest, StringLiteral) {
    auto result = migrate("\"hello\"\n");
    EXPECT_NE(result.find("\"hello\""), std::string::npos);
}

TEST(MigratorTest, BoolLiteral) {
    auto result = migrate("True\n");
    EXPECT_NE(result.find("True"), std::string::npos);
}

TEST(MigratorTest, BinaryExpr) {
    auto result = migrate("1 + 2\n");
    EXPECT_NE(result.find("1 + 2"), std::string::npos);
}

TEST(MigratorTest, FunctionCall) {
    auto result = migrate("print(42)\n");
    EXPECT_NE(result.find("print(42)"), std::string::npos);
}

//===----------------------------------------------------------------------===//
// Statement Emission
//===----------------------------------------------------------------------===//

TEST(MigratorTest, PassStatement) {
    auto result = migrate("pass\n");
    EXPECT_NE(result.find("pass"), std::string::npos);
}

TEST(MigratorTest, ReturnStatement) {
    auto result = migrate("def foo():\n    return 42\n");
    EXPECT_NE(result.find("return 42"), std::string::npos);
}

TEST(MigratorTest, BreakStatement) {
    auto result = migrate("while True:\n    break\n");
    EXPECT_NE(result.find("break"), std::string::npos);
}

TEST(MigratorTest, ContinueStatement) {
    auto result = migrate("while True:\n    continue\n");
    EXPECT_NE(result.find("continue"), std::string::npos);
}

//===----------------------------------------------------------------------===//
// Brace Conversion
//===----------------------------------------------------------------------===//

TEST(MigratorTest, IfWithBraces) {
    auto result = migrate("if True:\n    pass\n");
    EXPECT_NE(result.find("if True {"), std::string::npos);
    EXPECT_NE(result.find("}"), std::string::npos);
}

TEST(MigratorTest, IfElseWithBraces) {
    auto result = migrate("if True:\n    pass\nelse:\n    pass\n");
    EXPECT_NE(result.find("if True {"), std::string::npos);
    EXPECT_NE(result.find("} else {"), std::string::npos);
}

TEST(MigratorTest, WhileWithBraces) {
    auto result = migrate("while True:\n    pass\n");
    EXPECT_NE(result.find("while True {"), std::string::npos);
    EXPECT_NE(result.find("}"), std::string::npos);
}

TEST(MigratorTest, ForWithBraces) {
    auto result = migrate("for i in range(10):\n    print(i)\n");
    EXPECT_NE(result.find("for i in range(10) {"), std::string::npos);
}

TEST(MigratorTest, FunctionWithBraces) {
    auto result = migrate("def foo():\n    pass\n");
    EXPECT_NE(result.find("def foo(") , std::string::npos);
    EXPECT_NE(result.find("{"), std::string::npos);
    EXPECT_NE(result.find("pass"), std::string::npos);
}

TEST(MigratorTest, ClassWithBraces) {
    auto result = migrate("class Foo:\n    pass\n");
    EXPECT_NE(result.find("class Foo {"), std::string::npos);
}

//===----------------------------------------------------------------------===//
// Indent Mode (no braces)
//===----------------------------------------------------------------------===//

TEST(MigratorTest, IfWithIndent) {
    auto result = migrateIndent("if True:\n    pass\n");
    EXPECT_NE(result.find("if True:"), std::string::npos);
    EXPECT_EQ(result.find("{"), std::string::npos);
}

TEST(MigratorTest, FunctionWithIndent) {
    auto result = migrateIndent("def foo():\n    return 42\n");
    EXPECT_NE(result.find("def foo("), std::string::npos);
    EXPECT_NE(result.find(":"), std::string::npos);
    EXPECT_EQ(result.find("{"), std::string::npos);
}

//===----------------------------------------------------------------------===//
// Type Inference & Annotation
//===----------------------------------------------------------------------===//

TEST(MigratorTest, FunctionGetsReturnType) {
    auto result = migrate("def add(x, y):\n    return x + y\n");
    // Function should get a return type annotation
    EXPECT_NE(result.find("def add("), std::string::npos);
    EXPECT_NE(result.find(":"), std::string::npos); // return type annotation present
}

TEST(MigratorTest, FunctionParamsGetTypes) {
    auto result = migrate("def greet(name):\n    print(name)\n");
    // Parameters should get type annotations
    EXPECT_NE(result.find("name: "), std::string::npos);
}

TEST(MigratorTest, FunctionWithDefaultGetsType) {
    auto result = migrate("def foo(x=5):\n    return x\n");
    // Default value 5 -> int type
    EXPECT_NE(result.find("int"), std::string::npos);
}

TEST(MigratorTest, NoTypeAnnotationsWhenDisabled) {
    auto result = migrate("def foo(x):\n    return x\n", /*addTypes=*/false);
    // No type annotations should be added
    EXPECT_EQ(result.find(": int"), std::string::npos);
    EXPECT_EQ(result.find(": Any"), std::string::npos);
}

//===----------------------------------------------------------------------===//
// TypeInference Direct Tests
//===----------------------------------------------------------------------===//

TEST(TypeInferenceTest, InferIntLiteral) {
    TypeInference ti;
    auto lit = std::make_unique<IntegerLiteral>();
    lit->value = 42;
    auto type = ti.inferExprType(lit.get());
    ASSERT_NE(type, nullptr);
    EXPECT_EQ(type->kind(), Type::Kind::Int);
}

TEST(TypeInferenceTest, InferFloatLiteral) {
    TypeInference ti;
    auto lit = std::make_unique<FloatLiteral>();
    lit->value = 3.14;
    auto type = ti.inferExprType(lit.get());
    ASSERT_NE(type, nullptr);
    EXPECT_EQ(type->kind(), Type::Kind::Float);
}

TEST(TypeInferenceTest, InferStringLiteral) {
    TypeInference ti;
    auto lit = std::make_unique<StringLiteral>();
    lit->value = "hello";
    auto type = ti.inferExprType(lit.get());
    ASSERT_NE(type, nullptr);
    EXPECT_EQ(type->kind(), Type::Kind::Str);
}

TEST(TypeInferenceTest, InferBoolLiteral) {
    TypeInference ti;
    auto lit = std::make_unique<BooleanLiteral>();
    lit->value = true;
    auto type = ti.inferExprType(lit.get());
    ASSERT_NE(type, nullptr);
    EXPECT_EQ(type->kind(), Type::Kind::Bool);
}

TEST(TypeInferenceTest, InferNoneLiteral) {
    TypeInference ti;
    auto lit = std::make_unique<NoneLiteral>();
    auto type = ti.inferExprType(lit.get());
    ASSERT_NE(type, nullptr);
    EXPECT_EQ(type->kind(), Type::Kind::None_);
}

//===----------------------------------------------------------------------===//
// Complex Migration
//===----------------------------------------------------------------------===//

TEST(MigratorTest, MultipleStatements) {
    auto result = migrate("x = 10\ny = 20\nprint(x + y)\n");
    EXPECT_NE(result.find("x = 10"), std::string::npos);
    EXPECT_NE(result.find("y = 20"), std::string::npos);
    EXPECT_NE(result.find("print(x + y)"), std::string::npos);
}

TEST(MigratorTest, NestedBlocks) {
    auto result = migrate(
        "def foo():\n"
        "    if True:\n"
        "        return 1\n"
        "    return 0\n"
    );
    // Should have nested braces
    EXPECT_NE(result.find("def foo("), std::string::npos);
    EXPECT_NE(result.find("if True {"), std::string::npos);
    EXPECT_NE(result.find("return 1"), std::string::npos);
    EXPECT_NE(result.find("return 0"), std::string::npos);
}

TEST(MigratorTest, ClassWithMethod) {
    auto result = migrate(
        "class Dog:\n"
        "    def bark(self):\n"
        "        print(\"Woof\")\n"
    );
    EXPECT_NE(result.find("class Dog {"), std::string::npos);
    EXPECT_NE(result.find("def bark("), std::string::npos);
    EXPECT_NE(result.find("print(\"Woof\")"), std::string::npos);
}

TEST(MigratorTest, ForLoopWithBody) {
    auto result = migrate(
        "for i in range(5):\n"
        "    print(i)\n"
    );
    EXPECT_NE(result.find("for i in range(5) {"), std::string::npos);
    EXPECT_NE(result.find("print(i)"), std::string::npos);
}

TEST(MigratorTest, Assignment) {
    auto result = migrate("x = 10\n");
    EXPECT_NE(result.find("x = 10"), std::string::npos);
}

TEST(MigratorTest, ListLiteral) {
    auto result = migrate("[1, 2, 3]\n");
    EXPECT_NE(result.find("[1, 2, 3]"), std::string::npos);
}

TEST(MigratorTest, DictLiteral) {
    auto result = migrate("{\"a\": 1}\n");
    EXPECT_NE(result.find("\"a\": 1"), std::string::npos);
}

TEST(MigratorTest, TryCatch) {
    auto result = migrate(
        "try:\n"
        "    pass\n"
        "except:\n"
        "    pass\n"
    );
    EXPECT_NE(result.find("try {"), std::string::npos);
    EXPECT_NE(result.find("catch"), std::string::npos);
}

TEST(MigratorTest, ImportStatement) {
    auto result = migrate("import os\n");
    EXPECT_NE(result.find("import os"), std::string::npos);
}

TEST(MigratorTest, AssertStatement) {
    auto result = migrate("assert True\n");
    EXPECT_NE(result.find("assert True"), std::string::npos);
}

TEST(MigratorTest, AugmentedAssignment) {
    auto result = migrate("x = 0\nx += 5\n");
    EXPECT_NE(result.find("+="), std::string::npos);
}
