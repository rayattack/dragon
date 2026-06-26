#include <gtest/gtest.h>
// FIXME: split match/case tests into their own file already
#include "TestHelpers.h"

using namespace dragon;
using namespace dragon::test;

//===----------------------------------------------------------------------===//
// Module & Basic Structure
//===----------------------------------------------------------------------===//

TEST(ParserTest, EmptyModule) {
    auto module = parse("");
    ASSERT_NE(module, nullptr);
    EXPECT_TRUE(module->body.empty());
}

TEST(ParserTest, MultipleStatements) {
    auto module = parse("pass\npass\npass");
    ASSERT_NE(module, nullptr);
    EXPECT_EQ(module->body.size(), 3u);
    for (auto& stmt : module->body) {
        EXPECT_NE(dynamic_cast<PassStmt*>(stmt.get()), nullptr);
    }
}

//===----------------------------------------------------------------------===//
// Literal Expressions
//===----------------------------------------------------------------------===//

TEST(ParserTest, IntegerLiteral) {
    auto module = parse("42");
    ASSERT_NE(module, nullptr);
    ASSERT_EQ(module->body.size(), 1u);
    auto* exprStmt = dynamic_cast<ExprStmt*>(module->body[0].get());
    ASSERT_NE(exprStmt, nullptr);
    auto* intLit = dynamic_cast<IntegerLiteral*>(exprStmt->expr.get());
    ASSERT_NE(intLit, nullptr);
    EXPECT_EQ(intLit->value, 42);
}

TEST(ParserTest, IntegerLiteralZero) {
    auto module = parse("0");
    ASSERT_NE(module, nullptr);
    ASSERT_EQ(module->body.size(), 1u);
    auto* exprStmt = dynamic_cast<ExprStmt*>(module->body[0].get());
    ASSERT_NE(exprStmt, nullptr);
    auto* intLit = dynamic_cast<IntegerLiteral*>(exprStmt->expr.get());
    ASSERT_NE(intLit, nullptr);
    EXPECT_EQ(intLit->value, 0);
}

TEST(ParserTest, FloatLiteral) {
    auto module = parse("3.14");
    ASSERT_NE(module, nullptr);
    ASSERT_EQ(module->body.size(), 1u);
    auto* exprStmt = dynamic_cast<ExprStmt*>(module->body[0].get());
    ASSERT_NE(exprStmt, nullptr);
    auto* floatLit = dynamic_cast<FloatLiteral*>(exprStmt->expr.get());
    ASSERT_NE(floatLit, nullptr);
    EXPECT_DOUBLE_EQ(floatLit->value, 3.14);
}

TEST(ParserTest, StringLiteral) {
    auto module = parse("\"hello\"");
    ASSERT_NE(module, nullptr);
    ASSERT_EQ(module->body.size(), 1u);
    auto* exprStmt = dynamic_cast<ExprStmt*>(module->body[0].get());
    ASSERT_NE(exprStmt, nullptr);
    auto* strLit = dynamic_cast<StringLiteral*>(exprStmt->expr.get());
    ASSERT_NE(strLit, nullptr);
    EXPECT_EQ(strLit->value, "hello");
    // Lone module-top string literal also populates the docstring slot
    // (Python parity - `__doc__` is set to that string).
    ASSERT_TRUE(module->docstring.has_value());
    EXPECT_EQ(*module->docstring, "hello");
}

TEST(ParserTest, ModuleDocstringLifted) {
    auto module = parse("\"\"\"module doc.\"\"\"\n0\n");
    ASSERT_NE(module, nullptr);
    ASSERT_TRUE(module->docstring.has_value());
    EXPECT_EQ(*module->docstring, "module doc.");
    // Body still contains the docstring statement (non-destructive lift).
    ASSERT_EQ(module->body.size(), 2u);
}

TEST(ParserTest, FunctionDocstringLifted) {
    auto module = parse(
        "def foo() -> int:\n"
        "    \"\"\"foo's doc.\"\"\"\n"
        "    return 0\n");
    ASSERT_NE(module, nullptr);
    ASSERT_FALSE(module->body.empty());
    auto* fn = dynamic_cast<FunctionDecl*>(module->body[0].get());
    ASSERT_NE(fn, nullptr);
    ASSERT_TRUE(fn->docstring.has_value());
    EXPECT_EQ(*fn->docstring, "foo's doc.");
}

TEST(ParserTest, ClassDocstringLifted) {
    auto module = parse(
        "class C:\n"
        "    \"\"\"class doc.\"\"\"\n"
        "    pass\n");
    ASSERT_NE(module, nullptr);
    ASSERT_FALSE(module->body.empty());
    auto* cls = dynamic_cast<ClassDecl*>(module->body[0].get());
    ASSERT_NE(cls, nullptr);
    ASSERT_TRUE(cls->docstring.has_value());
    EXPECT_EQ(*cls->docstring, "class doc.");
}

TEST(ParserTest, FStringNotLiftedAsDocstring) {
    auto module = parse(
        "def foo() -> int:\n"
        "    f\"hello {1}\"\n"
        "    return 0\n");
    ASSERT_NE(module, nullptr);
    auto* fn = dynamic_cast<FunctionDecl*>(module->body[0].get());
    ASSERT_NE(fn, nullptr);
    EXPECT_FALSE(fn->docstring.has_value());
}

TEST(ParserTest, BooleanTrue) {
    auto module = parse("True");
    ASSERT_NE(module, nullptr);
    ASSERT_EQ(module->body.size(), 1u);
    auto* exprStmt = dynamic_cast<ExprStmt*>(module->body[0].get());
    ASSERT_NE(exprStmt, nullptr);
    auto* boolLit = dynamic_cast<BooleanLiteral*>(exprStmt->expr.get());
    ASSERT_NE(boolLit, nullptr);
    EXPECT_TRUE(boolLit->value);
}

TEST(ParserTest, BooleanFalse) {
    auto module = parse("False");
    ASSERT_NE(module, nullptr);
    ASSERT_EQ(module->body.size(), 1u);
    auto* exprStmt = dynamic_cast<ExprStmt*>(module->body[0].get());
    ASSERT_NE(exprStmt, nullptr);
    auto* boolLit = dynamic_cast<BooleanLiteral*>(exprStmt->expr.get());
    ASSERT_NE(boolLit, nullptr);
    EXPECT_FALSE(boolLit->value);
}

TEST(ParserTest, NoneLiteral) {
    auto module = parse("None");
    ASSERT_NE(module, nullptr);
    ASSERT_EQ(module->body.size(), 1u);
    auto* exprStmt = dynamic_cast<ExprStmt*>(module->body[0].get());
    ASSERT_NE(exprStmt, nullptr);
    auto* noneLit = dynamic_cast<NoneLiteral*>(exprStmt->expr.get());
    EXPECT_NE(noneLit, nullptr);
}

TEST(ParserTest, NameExpression) {
    auto module = parse("x");
    ASSERT_NE(module, nullptr);
    ASSERT_EQ(module->body.size(), 1u);
    auto* exprStmt = dynamic_cast<ExprStmt*>(module->body[0].get());
    ASSERT_NE(exprStmt, nullptr);
    auto* name = dynamic_cast<NameExpr*>(exprStmt->expr.get());
    ASSERT_NE(name, nullptr);
    EXPECT_EQ(name->name, "x");
}

//===----------------------------------------------------------------------===//
// Binary Expressions & Precedence
//===----------------------------------------------------------------------===//

TEST(ParserTest, BinaryAddition) {
    auto module = parse("1 + 2");
    ASSERT_NE(module, nullptr);
    ASSERT_EQ(module->body.size(), 1u);
    auto* exprStmt = dynamic_cast<ExprStmt*>(module->body[0].get());
    ASSERT_NE(exprStmt, nullptr);
    auto* bin = dynamic_cast<BinaryExpr*>(exprStmt->expr.get());
    ASSERT_NE(bin, nullptr);
    EXPECT_EQ(bin->op.type(), TokenType::PLUS);
    EXPECT_NE(dynamic_cast<IntegerLiteral*>(bin->left.get()), nullptr);
    EXPECT_NE(dynamic_cast<IntegerLiteral*>(bin->right.get()), nullptr);
}

TEST(ParserTest, BinarySubtraction) {
    auto module = parse("5 - 3");
    ASSERT_NE(module, nullptr);
    ASSERT_EQ(module->body.size(), 1u);
    auto* exprStmt = dynamic_cast<ExprStmt*>(module->body[0].get());
    auto* bin = dynamic_cast<BinaryExpr*>(exprStmt->expr.get());
    ASSERT_NE(bin, nullptr);
    EXPECT_EQ(bin->op.type(), TokenType::MINUS);
}

TEST(ParserTest, BinaryMultiplication) {
    auto module = parse("2 * 3");
    ASSERT_NE(module, nullptr);
    auto* exprStmt = dynamic_cast<ExprStmt*>(module->body[0].get());
    auto* bin = dynamic_cast<BinaryExpr*>(exprStmt->expr.get());
    ASSERT_NE(bin, nullptr);
    EXPECT_EQ(bin->op.type(), TokenType::STAR);
}

TEST(ParserTest, BinaryDivision) {
    auto module = parse("10 / 3");
    ASSERT_NE(module, nullptr);
    auto* exprStmt = dynamic_cast<ExprStmt*>(module->body[0].get());
    auto* bin = dynamic_cast<BinaryExpr*>(exprStmt->expr.get());
    ASSERT_NE(bin, nullptr);
    EXPECT_EQ(bin->op.type(), TokenType::SLASH);
}

TEST(ParserTest, BinaryModulo) {
    auto module = parse("10 % 3");
    ASSERT_NE(module, nullptr);
    auto* exprStmt = dynamic_cast<ExprStmt*>(module->body[0].get());
    auto* bin = dynamic_cast<BinaryExpr*>(exprStmt->expr.get());
    ASSERT_NE(bin, nullptr);
    EXPECT_EQ(bin->op.type(), TokenType::PERCENT);
}

TEST(ParserTest, BinaryPower) {
    auto module = parse("2 ** 3");
    ASSERT_NE(module, nullptr);
    auto* exprStmt = dynamic_cast<ExprStmt*>(module->body[0].get());
    auto* bin = dynamic_cast<BinaryExpr*>(exprStmt->expr.get());
    ASSERT_NE(bin, nullptr);
    EXPECT_EQ(bin->op.type(), TokenType::POWER);
}

TEST(ParserTest, BinaryFloorDivision) {
    auto module = parse("10 // 3");
    ASSERT_NE(module, nullptr);
    auto* exprStmt = dynamic_cast<ExprStmt*>(module->body[0].get());
    auto* bin = dynamic_cast<BinaryExpr*>(exprStmt->expr.get());
    ASSERT_NE(bin, nullptr);
    EXPECT_EQ(bin->op.type(), TokenType::DOUBLE_SLASH);
}

TEST(ParserTest, PrecedenceMultiplyOverAdd) {
    // 1 + 2 * 3 should be 1 + (2 * 3)
    auto module = parse("1 + 2 * 3");
    ASSERT_NE(module, nullptr);
    auto* exprStmt = dynamic_cast<ExprStmt*>(module->body[0].get());
    auto* add = dynamic_cast<BinaryExpr*>(exprStmt->expr.get());
    ASSERT_NE(add, nullptr);
    EXPECT_EQ(add->op.type(), TokenType::PLUS);
    // Left should be int 1
    auto* left = dynamic_cast<IntegerLiteral*>(add->left.get());
    ASSERT_NE(left, nullptr);
    EXPECT_EQ(left->value, 1);
    // Right should be 2 * 3
    auto* mul = dynamic_cast<BinaryExpr*>(add->right.get());
    ASSERT_NE(mul, nullptr);
    EXPECT_EQ(mul->op.type(), TokenType::STAR);
}

TEST(ParserTest, PrecedencePowerOverMultiply) {
    // 2 * 3 ** 4 should be 2 * (3 ** 4)
    auto module = parse("2 * 3 ** 4");
    ASSERT_NE(module, nullptr);
    auto* exprStmt = dynamic_cast<ExprStmt*>(module->body[0].get());
    auto* mul = dynamic_cast<BinaryExpr*>(exprStmt->expr.get());
    ASSERT_NE(mul, nullptr);
    EXPECT_EQ(mul->op.type(), TokenType::STAR);
    auto* pow = dynamic_cast<BinaryExpr*>(mul->right.get());
    ASSERT_NE(pow, nullptr);
    EXPECT_EQ(pow->op.type(), TokenType::POWER);
}

TEST(ParserTest, PrecedenceParentheses) {
    // (1 + 2) * 3 should be (1 + 2) * 3
    auto module = parse("(1 + 2) * 3");
    ASSERT_NE(module, nullptr);
    auto* exprStmt = dynamic_cast<ExprStmt*>(module->body[0].get());
    auto* mul = dynamic_cast<BinaryExpr*>(exprStmt->expr.get());
    ASSERT_NE(mul, nullptr);
    EXPECT_EQ(mul->op.type(), TokenType::STAR);
    auto* add = dynamic_cast<BinaryExpr*>(mul->left.get());
    ASSERT_NE(add, nullptr);
    EXPECT_EQ(add->op.type(), TokenType::PLUS);
}

TEST(ParserTest, LeftAssociativity) {
    // 1 - 2 - 3 should be (1 - 2) - 3
    auto module = parse("1 - 2 - 3");
    ASSERT_NE(module, nullptr);
    auto* exprStmt = dynamic_cast<ExprStmt*>(module->body[0].get());
    auto* outer = dynamic_cast<BinaryExpr*>(exprStmt->expr.get());
    ASSERT_NE(outer, nullptr);
    EXPECT_EQ(outer->op.type(), TokenType::MINUS);
    auto* inner = dynamic_cast<BinaryExpr*>(outer->left.get());
    ASSERT_NE(inner, nullptr);
    EXPECT_EQ(inner->op.type(), TokenType::MINUS);
    // Right of outer should be 3
    auto* three = dynamic_cast<IntegerLiteral*>(outer->right.get());
    ASSERT_NE(three, nullptr);
    EXPECT_EQ(three->value, 3);
}

TEST(ParserTest, PowerRightAssociativity) {
    // 2 ** 3 ** 4 should be 2 ** (3 ** 4)
    auto module = parse("2 ** 3 ** 4");
    ASSERT_NE(module, nullptr);
    auto* exprStmt = dynamic_cast<ExprStmt*>(module->body[0].get());
    auto* outer = dynamic_cast<BinaryExpr*>(exprStmt->expr.get());
    ASSERT_NE(outer, nullptr);
    EXPECT_EQ(outer->op.type(), TokenType::POWER);
    auto* left = dynamic_cast<IntegerLiteral*>(outer->left.get());
    ASSERT_NE(left, nullptr);
    EXPECT_EQ(left->value, 2);
    auto* inner = dynamic_cast<BinaryExpr*>(outer->right.get());
    ASSERT_NE(inner, nullptr);
    EXPECT_EQ(inner->op.type(), TokenType::POWER);
}

//===----------------------------------------------------------------------===//
// Comparison Operators
//===----------------------------------------------------------------------===//

TEST(ParserTest, ComparisonLess) {
    auto module = parse("a < b");
    ASSERT_NE(module, nullptr);
    auto* exprStmt = dynamic_cast<ExprStmt*>(module->body[0].get());
    auto* bin = dynamic_cast<BinaryExpr*>(exprStmt->expr.get());
    ASSERT_NE(bin, nullptr);
    EXPECT_EQ(bin->op.type(), TokenType::LESS);
}

TEST(ParserTest, ComparisonGreaterEqual) {
    auto module = parse("a >= b");
    ASSERT_NE(module, nullptr);
    auto* exprStmt = dynamic_cast<ExprStmt*>(module->body[0].get());
    auto* bin = dynamic_cast<BinaryExpr*>(exprStmt->expr.get());
    ASSERT_NE(bin, nullptr);
    EXPECT_EQ(bin->op.type(), TokenType::GREATER_EQUAL);
}

TEST(ParserTest, ComparisonEqual) {
    auto module = parse("a == b");
    ASSERT_NE(module, nullptr);
    auto* exprStmt = dynamic_cast<ExprStmt*>(module->body[0].get());
    auto* bin = dynamic_cast<BinaryExpr*>(exprStmt->expr.get());
    ASSERT_NE(bin, nullptr);
    EXPECT_EQ(bin->op.type(), TokenType::EQUAL_EQUAL);
}

TEST(ParserTest, ComparisonNotEqual) {
    auto module = parse("a != b");
    ASSERT_NE(module, nullptr);
    auto* exprStmt = dynamic_cast<ExprStmt*>(module->body[0].get());
    auto* bin = dynamic_cast<BinaryExpr*>(exprStmt->expr.get());
    ASSERT_NE(bin, nullptr);
    EXPECT_EQ(bin->op.type(), TokenType::NOT_EQUAL);
}

TEST(ParserTest, ComparisonIn) {
    auto module = parse("x in items");
    ASSERT_NE(module, nullptr);
    auto* exprStmt = dynamic_cast<ExprStmt*>(module->body[0].get());
    auto* bin = dynamic_cast<BinaryExpr*>(exprStmt->expr.get());
    ASSERT_NE(bin, nullptr);
    EXPECT_EQ(bin->op.type(), TokenType::IN);
}

TEST(ParserTest, ComparisonIs) {
    auto module = parse("x is None");
    ASSERT_NE(module, nullptr);
    auto* exprStmt = dynamic_cast<ExprStmt*>(module->body[0].get());
    auto* bin = dynamic_cast<BinaryExpr*>(exprStmt->expr.get());
    ASSERT_NE(bin, nullptr);
    EXPECT_EQ(bin->op.type(), TokenType::IS);
}

//===----------------------------------------------------------------------===//
// Logical Operators
//===----------------------------------------------------------------------===//

TEST(ParserTest, LogicalAnd) {
    auto module = parse("a and b");
    ASSERT_NE(module, nullptr);
    auto* exprStmt = dynamic_cast<ExprStmt*>(module->body[0].get());
    auto* bin = dynamic_cast<BinaryExpr*>(exprStmt->expr.get());
    ASSERT_NE(bin, nullptr);
    EXPECT_EQ(bin->op.type(), TokenType::AND);
}

TEST(ParserTest, LogicalOr) {
    auto module = parse("a or b");
    ASSERT_NE(module, nullptr);
    auto* exprStmt = dynamic_cast<ExprStmt*>(module->body[0].get());
    auto* bin = dynamic_cast<BinaryExpr*>(exprStmt->expr.get());
    ASSERT_NE(bin, nullptr);
    EXPECT_EQ(bin->op.type(), TokenType::OR);
}

TEST(ParserTest, LogicalNot) {
    auto module = parse("not x");
    ASSERT_NE(module, nullptr);
    auto* exprStmt = dynamic_cast<ExprStmt*>(module->body[0].get());
    auto* unary = dynamic_cast<UnaryExpr*>(exprStmt->expr.get());
    ASSERT_NE(unary, nullptr);
    EXPECT_EQ(unary->op.type(), TokenType::NOT);
}

TEST(ParserTest, LogicalPrecedence) {
    // a or b and c should be a or (b and c)
    auto module = parse("a or b and c");
    ASSERT_NE(module, nullptr);
    auto* exprStmt = dynamic_cast<ExprStmt*>(module->body[0].get());
    auto* orExpr = dynamic_cast<BinaryExpr*>(exprStmt->expr.get());
    ASSERT_NE(orExpr, nullptr);
    EXPECT_EQ(orExpr->op.type(), TokenType::OR);
    auto* andExpr = dynamic_cast<BinaryExpr*>(orExpr->right.get());
    ASSERT_NE(andExpr, nullptr);
    EXPECT_EQ(andExpr->op.type(), TokenType::AND);
}

//===----------------------------------------------------------------------===//
// Bitwise Operators
//===----------------------------------------------------------------------===//

TEST(ParserTest, BitwiseAnd) {
    auto module = parse("a & b");
    ASSERT_NE(module, nullptr);
    auto* exprStmt = dynamic_cast<ExprStmt*>(module->body[0].get());
    auto* bin = dynamic_cast<BinaryExpr*>(exprStmt->expr.get());
    ASSERT_NE(bin, nullptr);
    EXPECT_EQ(bin->op.type(), TokenType::AMPERSAND);
}

TEST(ParserTest, BitwiseOr) {
    auto module = parse("a | b");
    ASSERT_NE(module, nullptr);
    auto* exprStmt = dynamic_cast<ExprStmt*>(module->body[0].get());
    auto* bin = dynamic_cast<BinaryExpr*>(exprStmt->expr.get());
    ASSERT_NE(bin, nullptr);
    EXPECT_EQ(bin->op.type(), TokenType::PIPE);
}

TEST(ParserTest, BitwiseXor) {
    auto module = parse("a ^ b");
    ASSERT_NE(module, nullptr);
    auto* exprStmt = dynamic_cast<ExprStmt*>(module->body[0].get());
    auto* bin = dynamic_cast<BinaryExpr*>(exprStmt->expr.get());
    ASSERT_NE(bin, nullptr);
    EXPECT_EQ(bin->op.type(), TokenType::CARET);
}

TEST(ParserTest, BitwiseShiftLeft) {
    auto module = parse("a << 2");
    ASSERT_NE(module, nullptr);
    auto* exprStmt = dynamic_cast<ExprStmt*>(module->body[0].get());
    auto* bin = dynamic_cast<BinaryExpr*>(exprStmt->expr.get());
    ASSERT_NE(bin, nullptr);
    EXPECT_EQ(bin->op.type(), TokenType::LEFT_SHIFT);
}

TEST(ParserTest, BitwiseShiftRight) {
    auto module = parse("a >> 2");
    ASSERT_NE(module, nullptr);
    auto* exprStmt = dynamic_cast<ExprStmt*>(module->body[0].get());
    auto* bin = dynamic_cast<BinaryExpr*>(exprStmt->expr.get());
    ASSERT_NE(bin, nullptr);
    EXPECT_EQ(bin->op.type(), TokenType::RIGHT_SHIFT);
}

//===----------------------------------------------------------------------===//
// Unary Expressions
//===----------------------------------------------------------------------===//

TEST(ParserTest, UnaryMinus) {
    auto module = parse("-42");
    ASSERT_NE(module, nullptr);
    auto* exprStmt = dynamic_cast<ExprStmt*>(module->body[0].get());
    auto* unary = dynamic_cast<UnaryExpr*>(exprStmt->expr.get());
    ASSERT_NE(unary, nullptr);
    EXPECT_EQ(unary->op.type(), TokenType::MINUS);
    auto* intLit = dynamic_cast<IntegerLiteral*>(unary->operand.get());
    ASSERT_NE(intLit, nullptr);
    EXPECT_EQ(intLit->value, 42);
}

TEST(ParserTest, UnaryPlus) {
    auto module = parse("+x");
    ASSERT_NE(module, nullptr);
    auto* exprStmt = dynamic_cast<ExprStmt*>(module->body[0].get());
    auto* unary = dynamic_cast<UnaryExpr*>(exprStmt->expr.get());
    ASSERT_NE(unary, nullptr);
    EXPECT_EQ(unary->op.type(), TokenType::PLUS);
}

TEST(ParserTest, UnaryBitwiseNot) {
    auto module = parse("~x");
    ASSERT_NE(module, nullptr);
    auto* exprStmt = dynamic_cast<ExprStmt*>(module->body[0].get());
    auto* unary = dynamic_cast<UnaryExpr*>(exprStmt->expr.get());
    ASSERT_NE(unary, nullptr);
    EXPECT_EQ(unary->op.type(), TokenType::TILDE);
}

//===----------------------------------------------------------------------===//
// Ternary / If Expression
//===----------------------------------------------------------------------===//

TEST(ParserTest, TernaryExpression) {
    auto module = parse("a if cond else b");
    ASSERT_NE(module, nullptr);
    auto* exprStmt = dynamic_cast<ExprStmt*>(module->body[0].get());
    auto* ifExpr = dynamic_cast<IfExpr*>(exprStmt->expr.get());
    ASSERT_NE(ifExpr, nullptr);
    auto* cond = dynamic_cast<NameExpr*>(ifExpr->condition.get());
    ASSERT_NE(cond, nullptr);
    EXPECT_EQ(cond->name, "cond");
    auto* thenExpr = dynamic_cast<NameExpr*>(ifExpr->thenExpr.get());
    ASSERT_NE(thenExpr, nullptr);
    EXPECT_EQ(thenExpr->name, "a");
    auto* elseExpr = dynamic_cast<NameExpr*>(ifExpr->elseExpr.get());
    ASSERT_NE(elseExpr, nullptr);
    EXPECT_EQ(elseExpr->name, "b");
}

//===----------------------------------------------------------------------===//
// Call Expressions
//===----------------------------------------------------------------------===//

TEST(ParserTest, SimpleCall) {
    auto module = parse("f()");
    ASSERT_NE(module, nullptr);
    auto* exprStmt = dynamic_cast<ExprStmt*>(module->body[0].get());
    auto* call = dynamic_cast<CallExpr*>(exprStmt->expr.get());
    ASSERT_NE(call, nullptr);
    auto* callee = dynamic_cast<NameExpr*>(call->callee.get());
    ASSERT_NE(callee, nullptr);
    EXPECT_EQ(callee->name, "f");
    EXPECT_TRUE(call->args.empty());
    EXPECT_TRUE(call->kwArgs.empty());
}

TEST(ParserTest, CallWithPositionalArgs) {
    auto module = parse("f(1, 2, 3)");
    ASSERT_NE(module, nullptr);
    auto* exprStmt = dynamic_cast<ExprStmt*>(module->body[0].get());
    auto* call = dynamic_cast<CallExpr*>(exprStmt->expr.get());
    ASSERT_NE(call, nullptr);
    EXPECT_EQ(call->args.size(), 3u);
    EXPECT_TRUE(call->kwArgs.empty());
}

TEST(ParserTest, CallWithKeywordArgs) {
    auto module = parse("f(a=1, b=2)");
    ASSERT_NE(module, nullptr);
    auto* exprStmt = dynamic_cast<ExprStmt*>(module->body[0].get());
    auto* call = dynamic_cast<CallExpr*>(exprStmt->expr.get());
    ASSERT_NE(call, nullptr);
    EXPECT_EQ(call->kwArgs.size(), 2u);
    EXPECT_EQ(call->kwArgs[0].first, "a");
    EXPECT_EQ(call->kwArgs[1].first, "b");
}

TEST(ParserTest, CallWithMixedArgs) {
    auto module = parse("f(1, x=2)");
    ASSERT_NE(module, nullptr);
    auto* exprStmt = dynamic_cast<ExprStmt*>(module->body[0].get());
    auto* call = dynamic_cast<CallExpr*>(exprStmt->expr.get());
    ASSERT_NE(call, nullptr);
    EXPECT_EQ(call->args.size(), 1u);
    EXPECT_EQ(call->kwArgs.size(), 1u);
}

TEST(ParserTest, NestedCalls) {
    auto module = parse("f(g(x))");
    ASSERT_NE(module, nullptr);
    auto* exprStmt = dynamic_cast<ExprStmt*>(module->body[0].get());
    auto* outerCall = dynamic_cast<CallExpr*>(exprStmt->expr.get());
    ASSERT_NE(outerCall, nullptr);
    ASSERT_EQ(outerCall->args.size(), 1u);
    auto* innerCall = dynamic_cast<CallExpr*>(outerCall->args[0].get());
    ASSERT_NE(innerCall, nullptr);
}

//===----------------------------------------------------------------------===//
// Attribute Access
//===----------------------------------------------------------------------===//

TEST(ParserTest, SimpleAttribute) {
    auto module = parse("obj.attr");
    ASSERT_NE(module, nullptr);
    auto* exprStmt = dynamic_cast<ExprStmt*>(module->body[0].get());
    auto* attr = dynamic_cast<AttributeExpr*>(exprStmt->expr.get());
    ASSERT_NE(attr, nullptr);
    EXPECT_EQ(attr->attribute, "attr");
    auto* obj = dynamic_cast<NameExpr*>(attr->object.get());
    ASSERT_NE(obj, nullptr);
    EXPECT_EQ(obj->name, "obj");
}

TEST(ParserTest, ChainedAttribute) {
    auto module = parse("a.b.c");
    ASSERT_NE(module, nullptr);
    auto* exprStmt = dynamic_cast<ExprStmt*>(module->body[0].get());
    auto* outer = dynamic_cast<AttributeExpr*>(exprStmt->expr.get());
    ASSERT_NE(outer, nullptr);
    EXPECT_EQ(outer->attribute, "c");
    auto* inner = dynamic_cast<AttributeExpr*>(outer->object.get());
    ASSERT_NE(inner, nullptr);
    EXPECT_EQ(inner->attribute, "b");
}

TEST(ParserTest, MethodCall) {
    auto module = parse("obj.method(1, 2)");
    ASSERT_NE(module, nullptr);
    auto* exprStmt = dynamic_cast<ExprStmt*>(module->body[0].get());
    auto* call = dynamic_cast<CallExpr*>(exprStmt->expr.get());
    ASSERT_NE(call, nullptr);
    auto* attr = dynamic_cast<AttributeExpr*>(call->callee.get());
    ASSERT_NE(attr, nullptr);
    EXPECT_EQ(attr->attribute, "method");
    EXPECT_EQ(call->args.size(), 2u);
}

//===----------------------------------------------------------------------===//
// Subscript
//===----------------------------------------------------------------------===//

TEST(ParserTest, SimpleSubscript) {
    auto module = parse("arr[0]");
    ASSERT_NE(module, nullptr);
    auto* exprStmt = dynamic_cast<ExprStmt*>(module->body[0].get());
    auto* sub = dynamic_cast<SubscriptExpr*>(exprStmt->expr.get());
    ASSERT_NE(sub, nullptr);
    auto* obj = dynamic_cast<NameExpr*>(sub->object.get());
    ASSERT_NE(obj, nullptr);
    EXPECT_EQ(obj->name, "arr");
    auto* idx = dynamic_cast<IntegerLiteral*>(sub->index.get());
    ASSERT_NE(idx, nullptr);
    EXPECT_EQ(idx->value, 0);
}

TEST(ParserTest, StringSubscript) {
    auto module = parse("d[\"key\"]");
    ASSERT_NE(module, nullptr);
    auto* exprStmt = dynamic_cast<ExprStmt*>(module->body[0].get());
    auto* sub = dynamic_cast<SubscriptExpr*>(exprStmt->expr.get());
    ASSERT_NE(sub, nullptr);
    auto* idx = dynamic_cast<StringLiteral*>(sub->index.get());
    ASSERT_NE(idx, nullptr);
    EXPECT_EQ(idx->value, "key");
}

//===----------------------------------------------------------------------===//
// List, Dict, Set Literals
//===----------------------------------------------------------------------===//

TEST(ParserTest, EmptyList) {
    auto module = parse("[]");
    ASSERT_NE(module, nullptr);
    auto* exprStmt = dynamic_cast<ExprStmt*>(module->body[0].get());
    auto* list = dynamic_cast<ListExpr*>(exprStmt->expr.get());
    ASSERT_NE(list, nullptr);
    EXPECT_TRUE(list->elements.empty());
}

TEST(ParserTest, ListWithElements) {
    auto module = parse("[1, 2, 3]");
    ASSERT_NE(module, nullptr);
    auto* exprStmt = dynamic_cast<ExprStmt*>(module->body[0].get());
    auto* list = dynamic_cast<ListExpr*>(exprStmt->expr.get());
    ASSERT_NE(list, nullptr);
    EXPECT_EQ(list->elements.size(), 3u);
}

TEST(ParserTest, ListComprehension) {
    auto module = parse("[x * 2 for x in range(10)]");
    ASSERT_NE(module, nullptr);
    auto* exprStmt = dynamic_cast<ExprStmt*>(module->body[0].get());
    auto* comp = dynamic_cast<ListCompExpr*>(exprStmt->expr.get());
    ASSERT_NE(comp, nullptr);
    EXPECT_EQ(comp->varName, "x");
    ASSERT_NE(comp->element, nullptr);
    ASSERT_NE(comp->iterable, nullptr);
    EXPECT_EQ(comp->condition, nullptr);
}

TEST(ParserTest, ListComprehensionWithCondition) {
    auto module = parse("[x for x in range(20) if x > 5]");
    ASSERT_NE(module, nullptr);
    auto* exprStmt = dynamic_cast<ExprStmt*>(module->body[0].get());
    auto* comp = dynamic_cast<ListCompExpr*>(exprStmt->expr.get());
    ASSERT_NE(comp, nullptr);
    EXPECT_EQ(comp->varName, "x");
    ASSERT_NE(comp->condition, nullptr);
}

TEST(ParserTest, DictComprehension) {
    auto module = parse("{x: x * 2 for x in range(5)}");
    ASSERT_NE(module, nullptr);
    auto* exprStmt = dynamic_cast<ExprStmt*>(module->body[0].get());
    auto* comp = dynamic_cast<DictCompExpr*>(exprStmt->expr.get());
    ASSERT_NE(comp, nullptr);
    ASSERT_FALSE(comp->varNames.empty());
    EXPECT_EQ(comp->varNames[0], "x");
    ASSERT_NE(comp->key, nullptr);
    ASSERT_NE(comp->value, nullptr);
    ASSERT_NE(comp->iterable, nullptr);
}

TEST(ParserTest, DictLiteral) {
    auto module = parse("{\"a\": 1, \"b\": 2}");
    ASSERT_NE(module, nullptr);
    auto* exprStmt = dynamic_cast<ExprStmt*>(module->body[0].get());
    auto* dict = dynamic_cast<DictExpr*>(exprStmt->expr.get());
    ASSERT_NE(dict, nullptr);
    EXPECT_EQ(dict->entries.size(), 2u);
}

TEST(ParserTest, EmptyDict) {
    auto module = parse("{}");
    ASSERT_NE(module, nullptr);
    auto* exprStmt = dynamic_cast<ExprStmt*>(module->body[0].get());
    auto* dict = dynamic_cast<DictExpr*>(exprStmt->expr.get());
    ASSERT_NE(dict, nullptr);
    EXPECT_TRUE(dict->entries.empty());
}

//===----------------------------------------------------------------------===//
// Simple Statements
//===----------------------------------------------------------------------===//

TEST(ParserTest, PassStatement) {
    auto module = parse("pass");
    ASSERT_NE(module, nullptr);
    ASSERT_EQ(module->body.size(), 1u);
    EXPECT_NE(dynamic_cast<PassStmt*>(module->body[0].get()), nullptr);
}

TEST(ParserTest, BreakStatement) {
    auto module = parse("break");
    ASSERT_NE(module, nullptr);
    ASSERT_EQ(module->body.size(), 1u);
    EXPECT_NE(dynamic_cast<BreakStmt*>(module->body[0].get()), nullptr);
}

TEST(ParserTest, ContinueStatement) {
    auto module = parse("continue");
    ASSERT_NE(module, nullptr);
    ASSERT_EQ(module->body.size(), 1u);
    EXPECT_NE(dynamic_cast<ContinueStmt*>(module->body[0].get()), nullptr);
}

TEST(ParserTest, ReturnNoValue) {
    auto module = parse("return");
    ASSERT_NE(module, nullptr);
    ASSERT_EQ(module->body.size(), 1u);
    auto* ret = dynamic_cast<ReturnStmt*>(module->body[0].get());
    ASSERT_NE(ret, nullptr);
    EXPECT_EQ(ret->value, nullptr);
}

TEST(ParserTest, ReturnWithValue) {
    auto module = parse("return 42");
    ASSERT_NE(module, nullptr);
    ASSERT_EQ(module->body.size(), 1u);
    auto* ret = dynamic_cast<ReturnStmt*>(module->body[0].get());
    ASSERT_NE(ret, nullptr);
    ASSERT_NE(ret->value, nullptr);
    auto* intLit = dynamic_cast<IntegerLiteral*>(ret->value.get());
    ASSERT_NE(intLit, nullptr);
    EXPECT_EQ(intLit->value, 42);
}

TEST(ParserTest, ReturnExpression) {
    auto module = parse("return x + y");
    ASSERT_NE(module, nullptr);
    auto* ret = dynamic_cast<ReturnStmt*>(module->body[0].get());
    ASSERT_NE(ret, nullptr);
    auto* bin = dynamic_cast<BinaryExpr*>(ret->value.get());
    ASSERT_NE(bin, nullptr);
    EXPECT_EQ(bin->op.type(), TokenType::PLUS);
}

TEST(ParserTest, RaiseNoValue) {
    auto module = parse("raise");
    ASSERT_NE(module, nullptr);
    auto* raise = dynamic_cast<RaiseStmt*>(module->body[0].get());
    ASSERT_NE(raise, nullptr);
    EXPECT_EQ(raise->exception, nullptr);
}

TEST(ParserTest, RaiseWithValue) {
    auto module = parse("raise ValueError(\"bad\")");
    ASSERT_NE(module, nullptr);
    auto* raise = dynamic_cast<RaiseStmt*>(module->body[0].get());
    ASSERT_NE(raise, nullptr);
    EXPECT_NE(raise->exception, nullptr);
}

TEST(ParserTest, AssertSimple) {
    auto module = parse("assert x");
    ASSERT_NE(module, nullptr);
    auto* assertStmt = dynamic_cast<AssertStmt*>(module->body[0].get());
    ASSERT_NE(assertStmt, nullptr);
    EXPECT_NE(assertStmt->test, nullptr);
    EXPECT_EQ(assertStmt->msg, nullptr);
}

TEST(ParserTest, AssertWithMessage) {
    auto module = parse("assert x, \"failed\"");
    ASSERT_NE(module, nullptr);
    auto* assertStmt = dynamic_cast<AssertStmt*>(module->body[0].get());
    ASSERT_NE(assertStmt, nullptr);
    EXPECT_NE(assertStmt->test, nullptr);
    EXPECT_NE(assertStmt->msg, nullptr);
}

TEST(ParserTest, GlobalStatement) {
    auto module = parse("global x, y");
    ASSERT_NE(module, nullptr);
    auto* global = dynamic_cast<GlobalStmt*>(module->body[0].get());
    ASSERT_NE(global, nullptr);
    ASSERT_EQ(global->names.size(), 2u);
    EXPECT_EQ(global->names[0], "x");
    EXPECT_EQ(global->names[1], "y");
}

TEST(ParserTest, NonlocalStatement) {
    auto module = parse("nonlocal x");
    ASSERT_NE(module, nullptr);
    auto* nonlocal = dynamic_cast<NonlocalStmt*>(module->body[0].get());
    ASSERT_NE(nonlocal, nullptr);
    ASSERT_EQ(nonlocal->names.size(), 1u);
    EXPECT_EQ(nonlocal->names[0], "x");
}

TEST(ParserTest, DeleteStatement) {
    auto module = parse("del x");
    ASSERT_NE(module, nullptr);
    auto* del = dynamic_cast<DeleteStmt*>(module->body[0].get());
    ASSERT_NE(del, nullptr);
    ASSERT_EQ(del->targets.size(), 1u);
}

TEST(ParserTest, ImportStatement) {
    auto module = parse("import os");
    ASSERT_NE(module, nullptr);
    auto* imp = dynamic_cast<ImportStmt*>(module->body[0].get());
    ASSERT_NE(imp, nullptr);
    ASSERT_EQ(imp->names.size(), 1u);
    EXPECT_EQ(imp->names[0].name, "os");
    EXPECT_TRUE(imp->names[0].asName.empty());
}

TEST(ParserTest, ImportWithAlias) {
    auto module = parse("import numpy as np");
    ASSERT_NE(module, nullptr);
    auto* imp = dynamic_cast<ImportStmt*>(module->body[0].get());
    ASSERT_NE(imp, nullptr);
    ASSERT_EQ(imp->names.size(), 1u);
    EXPECT_EQ(imp->names[0].name, "numpy");
    EXPECT_EQ(imp->names[0].asName, "np");
}

TEST(ParserTest, FromImport) {
    auto module = parse("from os import path");
    ASSERT_NE(module, nullptr);
    auto* imp = dynamic_cast<FromImportStmt*>(module->body[0].get());
    ASSERT_NE(imp, nullptr);
    EXPECT_EQ(imp->module, "os");
    ASSERT_EQ(imp->names.size(), 1u);
    EXPECT_EQ(imp->names[0].name, "path");
}

TEST(ParserTest, FromImportMultiple) {
    auto module = parse("from os import path, getcwd");
    ASSERT_NE(module, nullptr);
    auto* imp = dynamic_cast<FromImportStmt*>(module->body[0].get());
    ASSERT_NE(imp, nullptr);
    EXPECT_EQ(imp->module, "os");
    EXPECT_EQ(imp->names.size(), 2u);
}

//===----------------------------------------------------------------------===//
// Assignment Statements
//===----------------------------------------------------------------------===//

TEST(ParserTest, SimpleAssignment) {
    auto module = parse("x = 5");
    ASSERT_NE(module, nullptr);
    auto* assign = dynamic_cast<AssignStmt*>(module->body[0].get());
    ASSERT_NE(assign, nullptr);
    ASSERT_EQ(assign->targets.size(), 1u);
    auto* name = dynamic_cast<NameExpr*>(assign->targets[0].get());
    ASSERT_NE(name, nullptr);
    EXPECT_EQ(name->name, "x");
    auto* val = dynamic_cast<IntegerLiteral*>(assign->value.get());
    ASSERT_NE(val, nullptr);
    EXPECT_EQ(val->value, 5);
}

TEST(ParserTest, AnnotatedAssignment) {
    auto module = parse("x: int = 5");
    ASSERT_NE(module, nullptr);
    auto* ann = dynamic_cast<AnnAssignStmt*>(module->body[0].get());
    ASSERT_NE(ann, nullptr);
    auto* target = dynamic_cast<NameExpr*>(ann->target.get());
    ASSERT_NE(target, nullptr);
    EXPECT_EQ(target->name, "x");
    ASSERT_NE(ann->annotation, nullptr);
    auto* typeExpr = dynamic_cast<NamedTypeExpr*>(ann->annotation.get());
    ASSERT_NE(typeExpr, nullptr);
    EXPECT_EQ(typeExpr->name, "int");
    ASSERT_NE(ann->value, nullptr);
}

TEST(ParserTest, AnnotationOnly) {
    auto module = parse("x: int");
    ASSERT_NE(module, nullptr);
    auto* ann = dynamic_cast<AnnAssignStmt*>(module->body[0].get());
    ASSERT_NE(ann, nullptr);
    auto* target = dynamic_cast<NameExpr*>(ann->target.get());
    ASSERT_NE(target, nullptr);
    EXPECT_EQ(target->name, "x");
    ASSERT_NE(ann->annotation, nullptr);
    EXPECT_EQ(ann->value, nullptr);
}

TEST(ParserTest, AugmentedAssignPlus) {
    auto module = parse("x += 1");
    ASSERT_NE(module, nullptr);
    auto* aug = dynamic_cast<AugAssignStmt*>(module->body[0].get());
    ASSERT_NE(aug, nullptr);
    EXPECT_EQ(aug->op.type(), TokenType::PLUS_EQUAL);
    auto* target = dynamic_cast<NameExpr*>(aug->target.get());
    ASSERT_NE(target, nullptr);
    EXPECT_EQ(target->name, "x");
}

TEST(ParserTest, AugmentedAssignMinus) {
    auto module = parse("x -= 1");
    ASSERT_NE(module, nullptr);
    auto* aug = dynamic_cast<AugAssignStmt*>(module->body[0].get());
    ASSERT_NE(aug, nullptr);
    EXPECT_EQ(aug->op.type(), TokenType::MINUS_EQUAL);
}

TEST(ParserTest, AugmentedAssignStar) {
    auto module = parse("x *= 2");
    ASSERT_NE(module, nullptr);
    auto* aug = dynamic_cast<AugAssignStmt*>(module->body[0].get());
    ASSERT_NE(aug, nullptr);
    EXPECT_EQ(aug->op.type(), TokenType::STAR_EQUAL);
}

TEST(ParserTest, AssignmentExpression) {
    auto module = parse("x = 1 + 2 * 3");
    ASSERT_NE(module, nullptr);
    auto* assign = dynamic_cast<AssignStmt*>(module->body[0].get());
    ASSERT_NE(assign, nullptr);
    auto* bin = dynamic_cast<BinaryExpr*>(assign->value.get());
    ASSERT_NE(bin, nullptr);
    EXPECT_EQ(bin->op.type(), TokenType::PLUS);
}

//===----------------------------------------------------------------------===//
// Compound Statements (Dragon brace syntax)
//===----------------------------------------------------------------------===//

TEST(ParserTest, IfStatementBrace) {
    auto module = parse("if x {\n  pass\n}");
    ASSERT_NE(module, nullptr);
    auto* ifStmt = dynamic_cast<IfStmt*>(module->body[0].get());
    ASSERT_NE(ifStmt, nullptr);
    auto* cond = dynamic_cast<NameExpr*>(ifStmt->condition.get());
    ASSERT_NE(cond, nullptr);
    EXPECT_EQ(cond->name, "x");
    EXPECT_FALSE(ifStmt->thenBody.empty());
    EXPECT_TRUE(ifStmt->elifClauses.empty());
    EXPECT_TRUE(ifStmt->elseBody.empty());
}

TEST(ParserTest, IfElseStatementBrace) {
    auto module = parse("if x {\n  pass\n} else {\n  pass\n}");
    ASSERT_NE(module, nullptr);
    auto* ifStmt = dynamic_cast<IfStmt*>(module->body[0].get());
    ASSERT_NE(ifStmt, nullptr);
    EXPECT_FALSE(ifStmt->thenBody.empty());
    EXPECT_FALSE(ifStmt->elseBody.empty());
}

TEST(ParserTest, IfElifElseBrace) {
    auto module = parse("if a {\n  pass\n} elif b {\n  pass\n} else {\n  pass\n}");
    ASSERT_NE(module, nullptr);
    auto* ifStmt = dynamic_cast<IfStmt*>(module->body[0].get());
    ASSERT_NE(ifStmt, nullptr);
    EXPECT_FALSE(ifStmt->thenBody.empty());
    EXPECT_EQ(ifStmt->elifClauses.size(), 1u);
    EXPECT_FALSE(ifStmt->elseBody.empty());
}

TEST(ParserTest, WhileStatementBrace) {
    auto module = parse("while x {\n  pass\n}");
    ASSERT_NE(module, nullptr);
    auto* whileStmt = dynamic_cast<WhileStmt*>(module->body[0].get());
    ASSERT_NE(whileStmt, nullptr);
    auto* cond = dynamic_cast<NameExpr*>(whileStmt->condition.get());
    ASSERT_NE(cond, nullptr);
    EXPECT_EQ(cond->name, "x");
    EXPECT_FALSE(whileStmt->body.empty());
}

TEST(ParserTest, ForStatementBrace) {
    auto module = parse("for x in items {\n  pass\n}");
    ASSERT_NE(module, nullptr);
    auto* forStmt = dynamic_cast<ForStmt*>(module->body[0].get());
    ASSERT_NE(forStmt, nullptr);
    auto* target = dynamic_cast<NameExpr*>(forStmt->target.get());
    ASSERT_NE(target, nullptr);
    EXPECT_EQ(target->name, "x");
    auto* iter = dynamic_cast<NameExpr*>(forStmt->iterable.get());
    ASSERT_NE(iter, nullptr);
    EXPECT_EQ(iter->name, "items");
    EXPECT_FALSE(forStmt->body.empty());
}

TEST(ParserTest, TryStatementBrace) {
    auto module = parse("try {\n  pass\n} catch {\n  pass\n}");
    ASSERT_NE(module, nullptr);
    auto* tryStmt = dynamic_cast<TryStmt*>(module->body[0].get());
    ASSERT_NE(tryStmt, nullptr);
    EXPECT_FALSE(tryStmt->tryBody.empty());
    EXPECT_FALSE(tryStmt->handlers.empty());
}

TEST(ParserTest, TryFinallyBrace) {
    auto module = parse("try {\n  pass\n} finally {\n  pass\n}");
    ASSERT_NE(module, nullptr);
    auto* tryStmt = dynamic_cast<TryStmt*>(module->body[0].get());
    ASSERT_NE(tryStmt, nullptr);
    EXPECT_FALSE(tryStmt->tryBody.empty());
    EXPECT_FALSE(tryStmt->finallyBody.empty());
}

TEST(ParserTest, WithStatementBrace) {
    auto module = parse("with f() as x {\n  pass\n}");
    ASSERT_NE(module, nullptr);
    auto* withStmt = dynamic_cast<WithStmt*>(module->body[0].get());
    ASSERT_NE(withStmt, nullptr);
    ASSERT_EQ(withStmt->items.size(), 1u);
    EXPECT_NE(withStmt->items[0].contextExpr, nullptr);
    EXPECT_NE(withStmt->items[0].optionalVars, nullptr);
    EXPECT_FALSE(withStmt->body.empty());
}

//===----------------------------------------------------------------------===//
// Function Declaration (Dragon brace syntax)
//===----------------------------------------------------------------------===//

TEST(ParserTest, SimpleFunctionBrace) {
    auto module = parse("def foo() {\n  pass\n}");
    ASSERT_NE(module, nullptr);
    auto* func = dynamic_cast<FunctionDecl*>(module->body[0].get());
    ASSERT_NE(func, nullptr);
    EXPECT_EQ(func->name, "foo");
    EXPECT_TRUE(func->params.empty());
    EXPECT_FALSE(func->isAsync);
    EXPECT_FALSE(func->body.empty());
}

TEST(ParserTest, FunctionWithParams) {
    auto module = parse("def add(x: int, y: int) -> int {\n  return x + y\n}");
    ASSERT_NE(module, nullptr);
    auto* func = dynamic_cast<FunctionDecl*>(module->body[0].get());
    ASSERT_NE(func, nullptr);
    EXPECT_EQ(func->name, "add");
    ASSERT_EQ(func->params.size(), 2u);
    EXPECT_EQ(func->params[0].name, "x");
    EXPECT_EQ(func->params[1].name, "y");
    EXPECT_NE(func->returnType, nullptr);
}

TEST(ParserTest, FunctionWithArrowReturn) {
    auto module = parse("def greet(name: str) -> str {\n  return name\n}");
    ASSERT_NE(module, nullptr);
    auto* func = dynamic_cast<FunctionDecl*>(module->body[0].get());
    ASSERT_NE(func, nullptr);
    EXPECT_EQ(func->name, "greet");
    EXPECT_NE(func->returnType, nullptr);
}

TEST(ParserTest, AsyncFunction) {
    auto module = parse("async def fetch() {\n  pass\n}");
    ASSERT_NE(module, nullptr);
    auto* func = dynamic_cast<FunctionDecl*>(module->body[0].get());
    ASSERT_NE(func, nullptr);
    EXPECT_EQ(func->name, "fetch");
    EXPECT_TRUE(func->isAsync);
}

TEST(ParserTest, FunctionWithDefaultParam) {
    auto module = parse("def greet(name: str = \"world\") {\n  pass\n}");
    ASSERT_NE(module, nullptr);
    auto* func = dynamic_cast<FunctionDecl*>(module->body[0].get());
    ASSERT_NE(func, nullptr);
    ASSERT_EQ(func->params.size(), 1u);
    EXPECT_EQ(func->params[0].name, "name");
    EXPECT_NE(func->params[0].defaultValue, nullptr);
}

TEST(ParserTest, FunctionVarArgs) {
    // Typing is not optional for variadics - annotate the element type.
    auto module = parse("def f(*args: int) {\n  pass\n}");
    ASSERT_NE(module, nullptr);
    auto* func = dynamic_cast<FunctionDecl*>(module->body[0].get());
    ASSERT_NE(func, nullptr);
    ASSERT_EQ(func->params.size(), 1u);
    EXPECT_EQ(func->params[0].name, "args");
    EXPECT_TRUE(func->params[0].isVarArg);
}

TEST(ParserTest, FunctionKwArgs) {
    auto module = parse("def f(**kwargs: int) {\n  pass\n}");
    ASSERT_NE(module, nullptr);
    auto* func = dynamic_cast<FunctionDecl*>(module->body[0].get());
    ASSERT_NE(func, nullptr);
    ASSERT_EQ(func->params.size(), 1u);
    EXPECT_EQ(func->params[0].name, "kwargs");
    EXPECT_TRUE(func->params[0].isKwArg);
}

TEST(ParserTest, BareVarArgsRequiresAnnotation) {
    // Bare *args / **kwargs (no element type) is rejected: an unannotated
    // variadic has no element type to monomorphize on and would silently erase
    // every element to i64. Use `*args: Any` or a concrete element type.
    EXPECT_FALSE(parseErrors("def f(*args) {\n  pass\n}").empty());
    EXPECT_FALSE(parseErrors("def g(**kwargs) {\n  pass\n}").empty());
    // Annotated forms are accepted; the bare `*` keyword-only separator is exempt.
    EXPECT_TRUE(parseErrors("def h(*args: Any) {\n  pass\n}").empty());
    EXPECT_TRUE(parseErrors("def k(a: int, *, b: int) -> int {\n  return b\n}").empty());
}

//===----------------------------------------------------------------------===//
// Class Declaration
//===----------------------------------------------------------------------===//

TEST(ParserTest, SimpleClass) {
    auto module = parse("class Foo {\n  pass\n}");
    ASSERT_NE(module, nullptr);
    auto* cls = dynamic_cast<ClassDecl*>(module->body[0].get());
    ASSERT_NE(cls, nullptr);
    EXPECT_EQ(cls->name, "Foo");
    EXPECT_TRUE(cls->bases.empty());
    EXPECT_FALSE(cls->body.empty());
}

TEST(ParserTest, ClassWithBase) {
    auto module = parse("class Dog(Animal) {\n  pass\n}");
    ASSERT_NE(module, nullptr);
    auto* cls = dynamic_cast<ClassDecl*>(module->body[0].get());
    ASSERT_NE(cls, nullptr);
    EXPECT_EQ(cls->name, "Dog");
    ASSERT_EQ(cls->bases.size(), 1u);
    auto* base = dynamic_cast<NameExpr*>(cls->bases[0].get());
    ASSERT_NE(base, nullptr);
    EXPECT_EQ(base->name, "Animal");
}

TEST(ParserTest, ClassWithMultipleBases) {
    auto module = parse("class C(A, B) {\n  pass\n}");
    ASSERT_NE(module, nullptr);
    auto* cls = dynamic_cast<ClassDecl*>(module->body[0].get());
    ASSERT_NE(cls, nullptr);
    EXPECT_EQ(cls->name, "C");
    EXPECT_EQ(cls->bases.size(), 2u);
}

TEST(ParserTest, ClassWithMethod) {
    auto module = parse("class Foo {\n  def bar() {\n    pass\n  }\n}");
    ASSERT_NE(module, nullptr);
    auto* cls = dynamic_cast<ClassDecl*>(module->body[0].get());
    ASSERT_NE(cls, nullptr);
    ASSERT_FALSE(cls->body.empty());
    auto* method = dynamic_cast<FunctionDecl*>(cls->body[0].get());
    ASSERT_NE(method, nullptr);
    EXPECT_EQ(method->name, "bar");
}

//===----------------------------------------------------------------------===//
// Type Annotations
//===----------------------------------------------------------------------===//

TEST(ParserTest, SimpleTypeAnnotation) {
    auto module = parse("x: int");
    ASSERT_NE(module, nullptr);
    auto* ann = dynamic_cast<AnnAssignStmt*>(module->body[0].get());
    ASSERT_NE(ann, nullptr);
    auto* typeExpr = dynamic_cast<NamedTypeExpr*>(ann->annotation.get());
    ASSERT_NE(typeExpr, nullptr);
    EXPECT_EQ(typeExpr->name, "int");
}

TEST(ParserTest, GenericTypeAnnotation) {
    auto module = parse("x: list[int]");
    ASSERT_NE(module, nullptr);
    auto* ann = dynamic_cast<AnnAssignStmt*>(module->body[0].get());
    ASSERT_NE(ann, nullptr);
    auto* genType = dynamic_cast<GenericTypeExpr*>(ann->annotation.get());
    ASSERT_NE(genType, nullptr);
    auto* base = dynamic_cast<NamedTypeExpr*>(genType->base.get());
    ASSERT_NE(base, nullptr);
    EXPECT_EQ(base->name, "list");
    ASSERT_EQ(genType->typeArgs.size(), 1u);
}

TEST(ParserTest, DictTypeAnnotation) {
    auto module = parse("x: dict[str, int]");
    ASSERT_NE(module, nullptr);
    auto* ann = dynamic_cast<AnnAssignStmt*>(module->body[0].get());
    ASSERT_NE(ann, nullptr);
    auto* genType = dynamic_cast<GenericTypeExpr*>(ann->annotation.get());
    ASSERT_NE(genType, nullptr);
    auto* base = dynamic_cast<NamedTypeExpr*>(genType->base.get());
    ASSERT_NE(base, nullptr);
    EXPECT_EQ(base->name, "dict");
    EXPECT_EQ(genType->typeArgs.size(), 2u);
}

TEST(ParserTest, UnionTypeAnnotation) {
    auto module = parse("x: int | str");
    ASSERT_NE(module, nullptr);
    auto* ann = dynamic_cast<AnnAssignStmt*>(module->body[0].get());
    ASSERT_NE(ann, nullptr);
    auto* unionType = dynamic_cast<UnionTypeExpr*>(ann->annotation.get());
    ASSERT_NE(unionType, nullptr);
    EXPECT_EQ(unionType->types.size(), 2u);
}

//===----------------------------------------------------------------------===//
// Decorators
//===----------------------------------------------------------------------===//

TEST(ParserTest, DecoratedFunction) {
    auto module = parse("@staticmethod\ndef foo() {\n  pass\n}");
    ASSERT_NE(module, nullptr);
    auto* func = dynamic_cast<FunctionDecl*>(module->body[0].get());
    ASSERT_NE(func, nullptr);
    EXPECT_EQ(func->name, "foo");
    ASSERT_EQ(func->decorators.size(), 1u);
    auto* dec = dynamic_cast<NameExpr*>(func->decorators[0].get());
    ASSERT_NE(dec, nullptr);
    EXPECT_EQ(dec->name, "staticmethod");
}

TEST(ParserTest, MultipleDecorators) {
    auto module = parse("@dec1\n@dec2\ndef foo() {\n  pass\n}");
    ASSERT_NE(module, nullptr);
    auto* func = dynamic_cast<FunctionDecl*>(module->body[0].get());
    ASSERT_NE(func, nullptr);
    EXPECT_EQ(func->decorators.size(), 2u);
}

TEST(ParserTest, DecoratedClass) {
    auto module = parse("@dataclass\nclass Foo {\n  pass\n}");
    ASSERT_NE(module, nullptr);
    auto* cls = dynamic_cast<ClassDecl*>(module->body[0].get());
    ASSERT_NE(cls, nullptr);
    EXPECT_EQ(cls->name, "Foo");
    ASSERT_EQ(cls->decorators.size(), 1u);
}

//===----------------------------------------------------------------------===//
// Complex / Integration Tests
//===----------------------------------------------------------------------===//

TEST(ParserTest, PrintCall) {
    auto module = parse("print(\"hello\")");
    ASSERT_NE(module, nullptr);
    auto* exprStmt = dynamic_cast<ExprStmt*>(module->body[0].get());
    ASSERT_NE(exprStmt, nullptr);
    auto* call = dynamic_cast<CallExpr*>(exprStmt->expr.get());
    ASSERT_NE(call, nullptr);
    auto* callee = dynamic_cast<NameExpr*>(call->callee.get());
    ASSERT_NE(callee, nullptr);
    EXPECT_EQ(callee->name, "print");
    ASSERT_EQ(call->args.size(), 1u);
    auto* arg = dynamic_cast<StringLiteral*>(call->args[0].get());
    ASSERT_NE(arg, nullptr);
    EXPECT_EQ(arg->value, "hello");
}

TEST(ParserTest, VariableDeclarationAndUse) {
    // Two separate statements
    auto module = parse("x: int = 42\nprint(x)");
    ASSERT_NE(module, nullptr);
    ASSERT_EQ(module->body.size(), 2u);

    auto* ann = dynamic_cast<AnnAssignStmt*>(module->body[0].get());
    ASSERT_NE(ann, nullptr);

    auto* exprStmt = dynamic_cast<ExprStmt*>(module->body[1].get());
    ASSERT_NE(exprStmt, nullptr);
    auto* call = dynamic_cast<CallExpr*>(exprStmt->expr.get());
    ASSERT_NE(call, nullptr);
}

TEST(ParserTest, FunctionWithBody) {
    auto module = parse(
        "def add(a: int, b: int) -> int {\n"
        "  return a + b\n"
        "}"
    );
    ASSERT_NE(module, nullptr);
    auto* func = dynamic_cast<FunctionDecl*>(module->body[0].get());
    ASSERT_NE(func, nullptr);
    EXPECT_EQ(func->name, "add");
    ASSERT_EQ(func->params.size(), 2u);
    EXPECT_NE(func->returnType, nullptr);
    ASSERT_FALSE(func->body.empty());
    auto* ret = dynamic_cast<ReturnStmt*>(func->body[0].get());
    ASSERT_NE(ret, nullptr);
    auto* bin = dynamic_cast<BinaryExpr*>(ret->value.get());
    ASSERT_NE(bin, nullptr);
    EXPECT_EQ(bin->op.type(), TokenType::PLUS);
}

TEST(ParserTest, ClassWithMethods) {
    auto module = parse(
        "class Point {\n"
        "  def(x: int, y: int) {\n"
        "    self.x = x\n"
        "    self.y = y\n"
        "  }\n"
        "  def distance() -> float {\n"
        "    return 0.0\n"
        "  }\n"
        "}"
    );
    ASSERT_NE(module, nullptr);
    auto* cls = dynamic_cast<ClassDecl*>(module->body[0].get());
    ASSERT_NE(cls, nullptr);
    EXPECT_EQ(cls->name, "Point");
    // Should have at least the two methods
    EXPECT_GE(cls->body.size(), 2u);
}

TEST(ParserTest, ForWithRange) {
    auto module = parse(
        "for i in range(10) {\n"
        "  print(i)\n"
        "}"
    );
    ASSERT_NE(module, nullptr);
    auto* forStmt = dynamic_cast<ForStmt*>(module->body[0].get());
    ASSERT_NE(forStmt, nullptr);
    auto* target = dynamic_cast<NameExpr*>(forStmt->target.get());
    ASSERT_NE(target, nullptr);
    EXPECT_EQ(target->name, "i");
    // iterable should be a CallExpr for range(10)
    auto* call = dynamic_cast<CallExpr*>(forStmt->iterable.get());
    ASSERT_NE(call, nullptr);
}

TEST(ParserTest, NestedIfWhile) {
    auto module = parse(
        "if True {\n"
        "  while x {\n"
        "    break\n"
        "  }\n"
        "}"
    );
    ASSERT_NE(module, nullptr);
    auto* ifStmt = dynamic_cast<IfStmt*>(module->body[0].get());
    ASSERT_NE(ifStmt, nullptr);
    ASSERT_FALSE(ifStmt->thenBody.empty());
    auto* whileStmt = dynamic_cast<WhileStmt*>(ifStmt->thenBody[0].get());
    ASSERT_NE(whileStmt, nullptr);
    ASSERT_FALSE(whileStmt->body.empty());
    auto* brk = dynamic_cast<BreakStmt*>(whileStmt->body[0].get());
    EXPECT_NE(brk, nullptr);
}

TEST(ParserTest, TryCatchFinallyFull) {
    auto module = parse(
        "try {\n"
        "  pass\n"
        "} catch ValueError as e {\n"
        "  pass\n"
        "} finally {\n"
        "  pass\n"
        "}"
    );
    ASSERT_NE(module, nullptr);
    auto* tryStmt = dynamic_cast<TryStmt*>(module->body[0].get());
    ASSERT_NE(tryStmt, nullptr);
    EXPECT_FALSE(tryStmt->tryBody.empty());
    ASSERT_EQ(tryStmt->handlers.size(), 1u);
    EXPECT_NE(tryStmt->handlers[0].type, nullptr);
    EXPECT_EQ(tryStmt->handlers[0].name, "e");
    EXPECT_FALSE(tryStmt->finallyBody.empty());
}

TEST(ParserTest, ComplexExpression) {
    // x + y * z - w should be (x + (y * z)) - w
    auto module = parse("x + y * z - w");
    ASSERT_NE(module, nullptr);
    auto* exprStmt = dynamic_cast<ExprStmt*>(module->body[0].get());
    auto* sub = dynamic_cast<BinaryExpr*>(exprStmt->expr.get());
    ASSERT_NE(sub, nullptr);
    EXPECT_EQ(sub->op.type(), TokenType::MINUS);
    auto* add = dynamic_cast<BinaryExpr*>(sub->left.get());
    ASSERT_NE(add, nullptr);
    EXPECT_EQ(add->op.type(), TokenType::PLUS);
    auto* mul = dynamic_cast<BinaryExpr*>(add->right.get());
    ASSERT_NE(mul, nullptr);
    EXPECT_EQ(mul->op.type(), TokenType::STAR);
}

TEST(ParserTest, ChainedMethodCalls) {
    auto module = parse("a.b().c()");
    ASSERT_NE(module, nullptr);
    auto* exprStmt = dynamic_cast<ExprStmt*>(module->body[0].get());
    // Outermost should be a call to .c()
    auto* outerCall = dynamic_cast<CallExpr*>(exprStmt->expr.get());
    ASSERT_NE(outerCall, nullptr);
    // callee should be AttributeExpr .c
    auto* attrC = dynamic_cast<AttributeExpr*>(outerCall->callee.get());
    ASSERT_NE(attrC, nullptr);
    EXPECT_EQ(attrC->attribute, "c");
    // object of .c should be the call a.b()
    auto* innerCall = dynamic_cast<CallExpr*>(attrC->object.get());
    ASSERT_NE(innerCall, nullptr);
    auto* attrB = dynamic_cast<AttributeExpr*>(innerCall->callee.get());
    ASSERT_NE(attrB, nullptr);
    EXPECT_EQ(attrB->attribute, "b");
}

//===----------------------------------------------------------------------===//
// Slice Parsing Tests
//===----------------------------------------------------------------------===//

TEST(ParserTest, SimpleSlice) {
    auto module = parse("x[1:3]");
    ASSERT_NE(module, nullptr);
    ASSERT_EQ(module->body.size(), 1);
    auto* exprStmt = dynamic_cast<ExprStmt*>(module->body[0].get());
    ASSERT_NE(exprStmt, nullptr);
    auto* sub = dynamic_cast<SubscriptExpr*>(exprStmt->expr.get());
    ASSERT_NE(sub, nullptr);
    auto* slice = dynamic_cast<SliceExpr*>(sub->index.get());
    ASSERT_NE(slice, nullptr);
    EXPECT_NE(slice->lower, nullptr);  // 1
    EXPECT_NE(slice->upper, nullptr);  // 3
    EXPECT_EQ(slice->step, nullptr);   // no step
}

TEST(ParserTest, SliceWithStep) {
    auto module = parse("x[1:10:2]");
    ASSERT_NE(module, nullptr);
    ASSERT_EQ(module->body.size(), 1);
    auto* exprStmt = dynamic_cast<ExprStmt*>(module->body[0].get());
    auto* sub = dynamic_cast<SubscriptExpr*>(exprStmt->expr.get());
    ASSERT_NE(sub, nullptr);
    auto* slice = dynamic_cast<SliceExpr*>(sub->index.get());
    ASSERT_NE(slice, nullptr);
    EXPECT_NE(slice->lower, nullptr);
    EXPECT_NE(slice->upper, nullptr);
    EXPECT_NE(slice->step, nullptr);
}

TEST(ParserTest, SliceOpenStart) {
    auto module = parse("x[:5]");
    ASSERT_NE(module, nullptr);
    auto* exprStmt = dynamic_cast<ExprStmt*>(module->body[0].get());
    auto* sub = dynamic_cast<SubscriptExpr*>(exprStmt->expr.get());
    ASSERT_NE(sub, nullptr);
    auto* slice = dynamic_cast<SliceExpr*>(sub->index.get());
    ASSERT_NE(slice, nullptr);
    EXPECT_EQ(slice->lower, nullptr);   // open start
    EXPECT_NE(slice->upper, nullptr);   // 5
}

TEST(ParserTest, SliceOpenEnd) {
    auto module = parse("x[2:]");
    ASSERT_NE(module, nullptr);
    auto* exprStmt = dynamic_cast<ExprStmt*>(module->body[0].get());
    auto* sub = dynamic_cast<SubscriptExpr*>(exprStmt->expr.get());
    ASSERT_NE(sub, nullptr);
    auto* slice = dynamic_cast<SliceExpr*>(sub->index.get());
    ASSERT_NE(slice, nullptr);
    EXPECT_NE(slice->lower, nullptr);   // 2
    EXPECT_EQ(slice->upper, nullptr);   // open end
}

TEST(ParserTest, SliceReverse) {
    auto module = parse("x[::-1]");
    ASSERT_NE(module, nullptr);
    auto* exprStmt = dynamic_cast<ExprStmt*>(module->body[0].get());
    auto* sub = dynamic_cast<SubscriptExpr*>(exprStmt->expr.get());
    ASSERT_NE(sub, nullptr);
    auto* slice = dynamic_cast<SliceExpr*>(sub->index.get());
    ASSERT_NE(slice, nullptr);
    EXPECT_EQ(slice->lower, nullptr);
    EXPECT_EQ(slice->upper, nullptr);
    EXPECT_NE(slice->step, nullptr);
}

TEST(ParserTest, SliceAllOpen) {
    auto module = parse("x[:]");
    ASSERT_NE(module, nullptr);
    auto* exprStmt = dynamic_cast<ExprStmt*>(module->body[0].get());
    auto* sub = dynamic_cast<SubscriptExpr*>(exprStmt->expr.get());
    ASSERT_NE(sub, nullptr);
    auto* slice = dynamic_cast<SliceExpr*>(sub->index.get());
    ASSERT_NE(slice, nullptr);
    EXPECT_EQ(slice->lower, nullptr);
    EXPECT_EQ(slice->upper, nullptr);
    EXPECT_EQ(slice->step, nullptr);
}

//===----------------------------------------------------------------------===//
// Implicit Self (Decision 006)
//===----------------------------------------------------------------------===//

TEST(ParserTest, ImplicitSelfMethod) {
    auto module = parse(
        "class Point {\n"
        "  def distance(other: Point) -> float {\n"
        "    return 0.0\n"
        "  }\n"
        "}"
    );
    ASSERT_NE(module, nullptr);
    auto* cls = dynamic_cast<ClassDecl*>(module->body[0].get());
    ASSERT_NE(cls, nullptr);
    auto* method = dynamic_cast<FunctionDecl*>(cls->body[0].get());
    ASSERT_NE(method, nullptr);
    EXPECT_TRUE(method->isMethod);
    EXPECT_TRUE(method->hasImplicitSelf);
    // Only 'other' in params - self is implicit
    EXPECT_EQ(method->params.size(), 1u);
    EXPECT_EQ(method->params[0].name, "other");
}

TEST(ParserTest, ExplicitSelfInDragonIsError) {
    auto errors = parseErrors(
        "class Foo {\n"
        "  def bar(self) {\n"
        "    pass\n"
        "  }\n"
        "}"
    );
    ASSERT_FALSE(errors.empty());
    EXPECT_NE(errors[0].message.find("self"), std::string::npos);
}

TEST(ParserTest, ExplicitSelfInPyModeOk) {
    auto module = parse(
        "class Foo:\n"
        "    def bar(self):\n"
        "        pass\n",
        /*isDragon=*/false
    );
    ASSERT_NE(module, nullptr);
    auto* cls = dynamic_cast<ClassDecl*>(module->body[0].get());
    ASSERT_NE(cls, nullptr);
    ASSERT_FALSE(cls->body.empty());
    auto* method = dynamic_cast<FunctionDecl*>(cls->body[0].get());
    ASSERT_NE(method, nullptr);
    EXPECT_TRUE(method->isMethod);
    EXPECT_FALSE(method->hasImplicitSelf);
    EXPECT_EQ(method->params.size(), 1u);
    EXPECT_EQ(method->params[0].name, "self");
}

TEST(ParserTest, TopLevelFunctionNotMethod) {
    auto module = parse("def foo(x: int) -> int {\n  return x\n}");
    ASSERT_NE(module, nullptr);
    auto* func = dynamic_cast<FunctionDecl*>(module->body[0].get());
    ASSERT_NE(func, nullptr);
    EXPECT_FALSE(func->isMethod);
    EXPECT_FALSE(func->hasImplicitSelf);
}

TEST(ParserTest, SelfUsableOutsideClass) {
    // self is not a keyword - it can be used as a variable name outside classes
    auto module = parse("self: int = 42");
    ASSERT_NE(module, nullptr);
    EXPECT_FALSE(module->body.empty());
}

TEST(ParserTest, FStringParsed) {
    auto module = parse("f\"hello {name}\"");
    ASSERT_NE(module, nullptr);
    auto* exprStmt = dynamic_cast<ExprStmt*>(module->body[0].get());
    ASSERT_NE(exprStmt, nullptr);
    auto* str = dynamic_cast<StringLiteral*>(exprStmt->expr.get());
    ASSERT_NE(str, nullptr);
    EXPECT_TRUE(str->isFString);
    EXPECT_NE(str->value.find("name"), std::string::npos);
}

//===----------------------------------------------------------------------===//
// Tuple Unpacking
//===----------------------------------------------------------------------===//

TEST(ParserTest, TupleUnpackingSimple) {
    auto module = parse("a, b = 1, 2");
    ASSERT_NE(module, nullptr);
    ASSERT_EQ(module->body.size(), 1u);
    auto* assign = dynamic_cast<AssignStmt*>(module->body[0].get());
    ASSERT_NE(assign, nullptr);
    // First target should be a TupleExpr
    ASSERT_EQ(assign->targets.size(), 1u);
    auto* lhs = dynamic_cast<TupleExpr*>(assign->targets[0].get());
    ASSERT_NE(lhs, nullptr);
    EXPECT_EQ(lhs->elements.size(), 2u);
    // RHS should also be a TupleExpr
    auto* rhs = dynamic_cast<TupleExpr*>(assign->value.get());
    ASSERT_NE(rhs, nullptr);
    EXPECT_EQ(rhs->elements.size(), 2u);
}

TEST(ParserTest, TupleUnpackingThreeElements) {
    auto module = parse("a, b, c = 1, 2, 3");
    ASSERT_NE(module, nullptr);
    auto* assign = dynamic_cast<AssignStmt*>(module->body[0].get());
    ASSERT_NE(assign, nullptr);
    ASSERT_EQ(assign->targets.size(), 1u);
    auto* lhs = dynamic_cast<TupleExpr*>(assign->targets[0].get());
    ASSERT_NE(lhs, nullptr);
    EXPECT_EQ(lhs->elements.size(), 3u);
}

TEST(ParserTest, TupleUnpackingFromTuple) {
    auto module = parse("a, b = (10, 20)");
    ASSERT_NE(module, nullptr);
    auto* assign = dynamic_cast<AssignStmt*>(module->body[0].get());
    ASSERT_NE(assign, nullptr);
    ASSERT_EQ(assign->targets.size(), 1u);
    auto* lhs = dynamic_cast<TupleExpr*>(assign->targets[0].get());
    ASSERT_NE(lhs, nullptr);
    EXPECT_EQ(lhs->elements.size(), 2u);
}

TEST(ParserTest, StarredUnpacking) {
    auto module = parse("first, *rest = [1, 2, 3, 4]");
    ASSERT_NE(module, nullptr);
    auto* assign = dynamic_cast<AssignStmt*>(module->body[0].get());
    ASSERT_NE(assign, nullptr);
    ASSERT_EQ(assign->targets.size(), 1u);
    auto* lhs = dynamic_cast<TupleExpr*>(assign->targets[0].get());
    ASSERT_NE(lhs, nullptr);
    EXPECT_EQ(lhs->elements.size(), 2u);
    // Second element should be StarredExpr
    auto* starred = dynamic_cast<StarredExpr*>(lhs->elements[1].get());
    ASSERT_NE(starred, nullptr);
}

TEST(ParserTest, ForLoopTupleTarget) {
    auto module = parse("for a, b in items {\n  pass\n}");
    ASSERT_NE(module, nullptr);
    auto* forStmt = dynamic_cast<ForStmt*>(module->body[0].get());
    ASSERT_NE(forStmt, nullptr);
    auto* target = dynamic_cast<TupleExpr*>(forStmt->target.get());
    ASSERT_NE(target, nullptr);
    EXPECT_EQ(target->elements.size(), 2u);
}

TEST(ParserTest, ForLoopTripleTupleTarget) {
    auto module = parse("for a, b, c in items {\n  pass\n}");
    ASSERT_NE(module, nullptr);
    auto* forStmt = dynamic_cast<ForStmt*>(module->body[0].get());
    ASSERT_NE(forStmt, nullptr);
    auto* target = dynamic_cast<TupleExpr*>(forStmt->target.get());
    ASSERT_NE(target, nullptr);
    EXPECT_EQ(target->elements.size(), 3u);
}

//===----------------------------------------------------------------------===//
// Set Comprehensions
//===----------------------------------------------------------------------===//

TEST(ParserTest, SetComprehension) {
    auto module = parse("{x for x in items}");
    ASSERT_NE(module, nullptr);
    auto* exprStmt = dynamic_cast<ExprStmt*>(module->body[0].get());
    ASSERT_NE(exprStmt, nullptr);
    auto* comp = dynamic_cast<SetCompExpr*>(exprStmt->expr.get());
    ASSERT_NE(comp, nullptr);
    EXPECT_EQ(comp->varName, "x");
    EXPECT_EQ(comp->condition, nullptr);
}

TEST(ParserTest, SetComprehensionWithCondition) {
    auto module = parse("{x for x in items if x > 0}");
    ASSERT_NE(module, nullptr);
    auto* exprStmt = dynamic_cast<ExprStmt*>(module->body[0].get());
    ASSERT_NE(exprStmt, nullptr);
    auto* comp = dynamic_cast<SetCompExpr*>(exprStmt->expr.get());
    ASSERT_NE(comp, nullptr);
    EXPECT_NE(comp->condition, nullptr);
}

//===----------------------------------------------------------------------===//
// Generator Expressions
//===----------------------------------------------------------------------===//

TEST(ParserTest, GeneratorExpression) {
    auto module = parse("(x for x in items)");
    ASSERT_NE(module, nullptr);
    auto* exprStmt = dynamic_cast<ExprStmt*>(module->body[0].get());
    ASSERT_NE(exprStmt, nullptr);
    auto* gen = dynamic_cast<GeneratorExpr*>(exprStmt->expr.get());
    ASSERT_NE(gen, nullptr);
    EXPECT_EQ(gen->varName, "x");
    EXPECT_EQ(gen->condition, nullptr);
}

TEST(ParserTest, GeneratorExpressionWithCondition) {
    auto module = parse("(x for x in items if x > 0)");
    ASSERT_NE(module, nullptr);
    auto* exprStmt = dynamic_cast<ExprStmt*>(module->body[0].get());
    ASSERT_NE(exprStmt, nullptr);
    auto* gen = dynamic_cast<GeneratorExpr*>(exprStmt->expr.get());
    ASSERT_NE(gen, nullptr);
    EXPECT_NE(gen->condition, nullptr);
}

//===----------------------------------------------------------------------===//
// Nested Comprehensions
//===----------------------------------------------------------------------===//

TEST(ParserTest, NestedListComprehension) {
    auto module = parse("[x + y for x in a for y in b]");
    ASSERT_NE(module, nullptr);
    auto* exprStmt = dynamic_cast<ExprStmt*>(module->body[0].get());
    ASSERT_NE(exprStmt, nullptr);
    auto* comp = dynamic_cast<ListCompExpr*>(exprStmt->expr.get());
    ASSERT_NE(comp, nullptr);
    EXPECT_EQ(comp->varName, "x");
    ASSERT_EQ(comp->extraClauses.size(), 1u);
    EXPECT_EQ(comp->extraClauses[0].varNames.size(), 1u);
    EXPECT_EQ(comp->extraClauses[0].varNames[0], "y");
}

TEST(ParserTest, NestedListCompWithCondition) {
    auto module = parse("[x + y for x in a for y in b if x != y]");
    ASSERT_NE(module, nullptr);
    auto* exprStmt = dynamic_cast<ExprStmt*>(module->body[0].get());
    ASSERT_NE(exprStmt, nullptr);
    auto* comp = dynamic_cast<ListCompExpr*>(exprStmt->expr.get());
    ASSERT_NE(comp, nullptr);
    ASSERT_EQ(comp->extraClauses.size(), 1u);
    EXPECT_NE(comp->extraClauses[0].condition, nullptr);
}

//===----------------------------------------------------------------------===//
// Chained Comparisons
//===----------------------------------------------------------------------===//

TEST(ParserTest, ChainedCompTwoOps) {
    auto module = parse("a < b < c");
    ASSERT_NE(module, nullptr);
    auto* exprStmt = dynamic_cast<ExprStmt*>(module->body[0].get());
    ASSERT_NE(exprStmt, nullptr);
    auto* chain = dynamic_cast<ChainedCompExpr*>(exprStmt->expr.get());
    ASSERT_NE(chain, nullptr);
    EXPECT_EQ(chain->operands.size(), 3u);
    EXPECT_EQ(chain->operators.size(), 2u);
}

TEST(ParserTest, ChainedCompThreeOps) {
    auto module = parse("0 <= x < y <= 100");
    ASSERT_NE(module, nullptr);
    auto* exprStmt = dynamic_cast<ExprStmt*>(module->body[0].get());
    ASSERT_NE(exprStmt, nullptr);
    auto* chain = dynamic_cast<ChainedCompExpr*>(exprStmt->expr.get());
    ASSERT_NE(chain, nullptr);
    EXPECT_EQ(chain->operands.size(), 4u);
    EXPECT_EQ(chain->operators.size(), 3u);
}

TEST(ParserTest, SingleCompStillBinaryExpr) {
    auto module = parse("a < b");
    ASSERT_NE(module, nullptr);
    auto* exprStmt = dynamic_cast<ExprStmt*>(module->body[0].get());
    ASSERT_NE(exprStmt, nullptr);
    auto* binary = dynamic_cast<BinaryExpr*>(exprStmt->expr.get());
    EXPECT_NE(binary, nullptr);
    auto* chain = dynamic_cast<ChainedCompExpr*>(exprStmt->expr.get());
    EXPECT_EQ(chain, nullptr);
}

//===----------------------------------------------------------------------===//
// Walrus Operator
//===----------------------------------------------------------------------===//

TEST(ParserTest, WalrusBasic) {
    auto module = parse("(n := 10)");
    ASSERT_NE(module, nullptr);
    auto* exprStmt = dynamic_cast<ExprStmt*>(module->body[0].get());
    ASSERT_NE(exprStmt, nullptr);
    auto* walrus = dynamic_cast<WalrusExpr*>(exprStmt->expr.get());
    ASSERT_NE(walrus, nullptr);
    EXPECT_EQ(walrus->name, "n");
}

//===----------------------------------------------------------------------===//
// Positional-Only and Keyword-Only Parameters
//===----------------------------------------------------------------------===//

TEST(ParserTest, PositionalOnlyParams) {
    auto module = parse("def foo(a: int, b: int, /) -> int {\n  return a + b\n}");
    ASSERT_NE(module, nullptr);
    auto* func = dynamic_cast<FunctionDecl*>(module->body[0].get());
    ASSERT_NE(func, nullptr);
    EXPECT_EQ(func->params.size(), 2u);
    EXPECT_EQ(func->posOnlyEnd, 2);
    EXPECT_EQ(func->kwOnlyStart, -1);
}

TEST(ParserTest, KeywordOnlyParams) {
    auto module = parse("def foo(*, key: int) -> int {\n  return key\n}");
    ASSERT_NE(module, nullptr);
    auto* func = dynamic_cast<FunctionDecl*>(module->body[0].get());
    ASSERT_NE(func, nullptr);
    EXPECT_EQ(func->params.size(), 1u);
    EXPECT_EQ(func->kwOnlyStart, 0);
    EXPECT_EQ(func->posOnlyEnd, -1);
}

TEST(ParserTest, MixedParamSeparators) {
    // def foo(a, b, /, c, *, d) - a,b positional-only, c normal, d keyword-only
    auto module = parse("def foo(a: int, b: int, /, c: int, *, d: int) -> int {\n  return a\n}");
    ASSERT_NE(module, nullptr);
    auto* func = dynamic_cast<FunctionDecl*>(module->body[0].get());
    ASSERT_NE(func, nullptr);
    EXPECT_EQ(func->params.size(), 4u);
    EXPECT_EQ(func->posOnlyEnd, 2);
    EXPECT_EQ(func->kwOnlyStart, 3);
    EXPECT_EQ(func->params[0].name, "a");
    EXPECT_EQ(func->params[1].name, "b");
    EXPECT_EQ(func->params[2].name, "c");
    EXPECT_EQ(func->params[3].name, "d");
}

TEST(ParserTest, BareStarWithVarArgs) {
    // def foo(*args, key=1) - *args is vararg, key is keyword-only
    auto module = parse("def foo(*args: int, key: int = 1) -> int {\n  return key\n}");
    ASSERT_NE(module, nullptr);
    auto* func = dynamic_cast<FunctionDecl*>(module->body[0].get());
    ASSERT_NE(func, nullptr);
    // *args has a name, so it's a real vararg, not a kw-only separator
    EXPECT_EQ(func->kwOnlyStart, -1);
    bool hasVarArg = false;
    for (auto& p : func->params) {
        if (p.isVarArg && p.name == "args") hasVarArg = true;
    }
    EXPECT_TRUE(hasVarArg);
}

TEST(ParserTest, DecoratorsParsed) {
    auto module = parse("@my_decorator\ndef foo() -> int {\n  return 1\n}");
    ASSERT_NE(module, nullptr);
    auto* func = dynamic_cast<FunctionDecl*>(module->body[0].get());
    ASSERT_NE(func, nullptr);
    EXPECT_EQ(func->decorators.size(), 1u);
}

//===----------------------------------------------------------------------===//
// Match/Case (PEP 634)
//===----------------------------------------------------------------------===//

TEST(ParserTest, MatchIntLiteralCases) {
    auto module = parse("match x {\n  case 1 { pass }\n  case 2 { pass }\n}");
    ASSERT_NE(module, nullptr);
    auto* ms = dynamic_cast<MatchStmt*>(module->body[0].get());
    ASSERT_NE(ms, nullptr);
    EXPECT_EQ(ms->cases.size(), 2u);
    EXPECT_EQ(ms->cases[0].pattern.kind, MatchPattern::Kind::Literal);
    EXPECT_EQ(ms->cases[1].pattern.kind, MatchPattern::Kind::Literal);
}

TEST(ParserTest, MatchWildcardCase) {
    auto module = parse("match x {\n  case 1 { pass }\n  case _ { pass }\n}");
    ASSERT_NE(module, nullptr);
    auto* ms = dynamic_cast<MatchStmt*>(module->body[0].get());
    ASSERT_NE(ms, nullptr);
    EXPECT_EQ(ms->cases.size(), 2u);
    EXPECT_EQ(ms->cases[1].pattern.kind, MatchPattern::Kind::Wildcard);
}

TEST(ParserTest, MatchCapturePattern) {
    auto module = parse("match x {\n  case n { pass }\n}");
    ASSERT_NE(module, nullptr);
    auto* ms = dynamic_cast<MatchStmt*>(module->body[0].get());
    ASSERT_NE(ms, nullptr);
    EXPECT_EQ(ms->cases[0].pattern.kind, MatchPattern::Kind::Capture);
    EXPECT_EQ(ms->cases[0].pattern.name, "n");
}

TEST(ParserTest, MatchStringLiteralCase) {
    auto module = parse("match cmd {\n  case \"quit\" { pass }\n  case \"help\" { pass }\n}");
    ASSERT_NE(module, nullptr);
    auto* ms = dynamic_cast<MatchStmt*>(module->body[0].get());
    ASSERT_NE(ms, nullptr);
    EXPECT_EQ(ms->cases.size(), 2u);
    EXPECT_EQ(ms->cases[0].pattern.kind, MatchPattern::Kind::Literal);
    EXPECT_EQ(ms->cases[1].pattern.kind, MatchPattern::Kind::Literal);
}

TEST(ParserTest, MatchOrPattern) {
    auto module = parse("match x {\n  case 1 | 2 | 3 { pass }\n}");
    ASSERT_NE(module, nullptr);
    auto* ms = dynamic_cast<MatchStmt*>(module->body[0].get());
    ASSERT_NE(ms, nullptr);
    EXPECT_EQ(ms->cases[0].pattern.kind, MatchPattern::Kind::Or);
    EXPECT_EQ(ms->cases[0].pattern.subPatterns.size(), 3u);
}

TEST(ParserTest, MatchSequencePattern) {
    auto module = parse("match point {\n  case [x, y] { pass }\n}");
    ASSERT_NE(module, nullptr);
    auto* ms = dynamic_cast<MatchStmt*>(module->body[0].get());
    ASSERT_NE(ms, nullptr);
    EXPECT_EQ(ms->cases[0].pattern.kind, MatchPattern::Kind::Sequence);
    EXPECT_EQ(ms->cases[0].pattern.subPatterns.size(), 2u);
    EXPECT_EQ(ms->cases[0].pattern.subPatterns[0].kind, MatchPattern::Kind::Capture);
    EXPECT_EQ(ms->cases[0].pattern.subPatterns[0].name, "x");
}

TEST(ParserTest, MatchWithGuard) {
    auto module = parse("match x {\n  case n if n > 0 { pass }\n}");
    ASSERT_NE(module, nullptr);
    auto* ms = dynamic_cast<MatchStmt*>(module->body[0].get());
    ASSERT_NE(ms, nullptr);
    EXPECT_NE(ms->cases[0].guard, nullptr);
}

TEST(ParserTest, MatchMultipleBodies) {
    auto module = parse("match x {\n  case 1 {\n    pass\n    pass\n  }\n  case _ { pass }\n}");
    ASSERT_NE(module, nullptr);
    auto* ms = dynamic_cast<MatchStmt*>(module->body[0].get());
    ASSERT_NE(ms, nullptr);
    EXPECT_EQ(ms->cases[0].body.size(), 2u);
    EXPECT_EQ(ms->cases[1].body.size(), 1u);
}

TEST(ParserTest, MatchTupleSequencePattern) {
    auto module = parse("match point {\n  case (a, b) { pass }\n}");
    ASSERT_NE(module, nullptr);
    auto* ms = dynamic_cast<MatchStmt*>(module->body[0].get());
    ASSERT_NE(ms, nullptr);
    EXPECT_EQ(ms->cases[0].pattern.kind, MatchPattern::Kind::Sequence);
    EXPECT_EQ(ms->cases[0].pattern.subPatterns.size(), 2u);
}

TEST(ParserTest, MatchBoolNoneLiterals) {
    auto module = parse("match val {\n  case True { pass }\n  case None { pass }\n}");
    ASSERT_NE(module, nullptr);
    auto* ms = dynamic_cast<MatchStmt*>(module->body[0].get());
    ASSERT_NE(ms, nullptr);
    EXPECT_EQ(ms->cases[0].pattern.kind, MatchPattern::Kind::Literal);
    EXPECT_EQ(ms->cases[1].pattern.kind, MatchPattern::Kind::Literal);
}

TEST(ParserTest, MatchCommaOrPatternDrMode) {
    // In .dr mode, comma should work as OR separator
    auto module = parse("match x {\n  case 1, 2, 3 { pass }\n}");
    ASSERT_NE(module, nullptr);
    auto* ms = dynamic_cast<MatchStmt*>(module->body[0].get());
    ASSERT_NE(ms, nullptr);
    EXPECT_EQ(ms->cases[0].pattern.kind, MatchPattern::Kind::Or);
    EXPECT_EQ(ms->cases[0].pattern.subPatterns.size(), 3u);
    EXPECT_EQ(ms->cases[0].pattern.subPatterns[0].kind, MatchPattern::Kind::Literal);
    EXPECT_EQ(ms->cases[0].pattern.subPatterns[1].kind, MatchPattern::Kind::Literal);
    EXPECT_EQ(ms->cases[0].pattern.subPatterns[2].kind, MatchPattern::Kind::Literal);
}

TEST(ParserTest, MatchCommaOrWithWildcardDefault) {
    auto module = parse("match x {\n  case 1, 2 { pass }\n  case _ { pass }\n}");
    ASSERT_NE(module, nullptr);
    auto* ms = dynamic_cast<MatchStmt*>(module->body[0].get());
    ASSERT_NE(ms, nullptr);
    EXPECT_EQ(ms->cases[0].pattern.kind, MatchPattern::Kind::Or);
    EXPECT_EQ(ms->cases[0].pattern.subPatterns.size(), 2u);
    EXPECT_EQ(ms->cases[1].pattern.kind, MatchPattern::Kind::Wildcard);
}

//===----------------------------------------------------------------------===//
// Phase I: Type Alias, Exception Groups, Multiple Inheritance
//===----------------------------------------------------------------------===//

TEST(ParserTest, TypeAliasSimple) {
    auto module = parse("type Point = tuple[int, int]");
    ASSERT_NE(module, nullptr);
    auto* alias = dynamic_cast<TypeAliasStmt*>(module->body[0].get());
    ASSERT_NE(alias, nullptr);
    EXPECT_EQ(alias->name, "Point");
    EXPECT_NE(alias->value, nullptr);
}

TEST(ParserTest, TypeAliasGeneric) {
    auto module = parse("type Matrix = list[list[int]]");
    ASSERT_NE(module, nullptr);
    auto* alias = dynamic_cast<TypeAliasStmt*>(module->body[0].get());
    ASSERT_NE(alias, nullptr);
    EXPECT_EQ(alias->name, "Matrix");
}

TEST(ParserTest, TypeAliasUnion) {
    auto module = parse("type Number = int | float");
    ASSERT_NE(module, nullptr);
    auto* alias = dynamic_cast<TypeAliasStmt*>(module->body[0].get());
    ASSERT_NE(alias, nullptr);
    EXPECT_EQ(alias->name, "Number");
}

TEST(ParserTest, ExceptStarParsed) {
    auto module = parse("try {\n  pass\n} except* ValueError {\n  pass\n}");
    ASSERT_NE(module, nullptr);
    auto* tryStmt = dynamic_cast<TryStmt*>(module->body[0].get());
    ASSERT_NE(tryStmt, nullptr);
    ASSERT_EQ(tryStmt->handlers.size(), 1u);
    EXPECT_TRUE(tryStmt->handlers[0].isStar);
}

TEST(ParserTest, ExceptNormalNotStar) {
    auto module = parse("try {\n  pass\n} except ValueError {\n  pass\n}");
    ASSERT_NE(module, nullptr);
    auto* tryStmt = dynamic_cast<TryStmt*>(module->body[0].get());
    ASSERT_NE(tryStmt, nullptr);
    ASSERT_EQ(tryStmt->handlers.size(), 1u);
    EXPECT_FALSE(tryStmt->handlers[0].isStar);
}

TEST(ParserTest, MultipleInheritanceParsed) {
    auto module = parse("class Child(Base1, Base2) {\n  pass\n}");
    ASSERT_NE(module, nullptr);
    auto* cls = dynamic_cast<ClassDecl*>(module->body[0].get());
    ASSERT_NE(cls, nullptr);
    EXPECT_EQ(cls->bases.size(), 2u);
}

//===----------------------------------------------------------------------===//
// Phase J: Dual-Mode Syntax Verification (.py mode)
//===----------------------------------------------------------------------===//

// --- Match/Case in .py mode ---

TEST(ParserTest, PyMatchIntLiteralCases) {
    auto module = parse(
        "match x:\n"
        "    case 1:\n"
        "        pass\n"
        "    case 2:\n"
        "        pass\n",
        false);
    ASSERT_NE(module, nullptr);
    auto* ms = dynamic_cast<MatchStmt*>(module->body[0].get());
    ASSERT_NE(ms, nullptr);
    EXPECT_EQ(ms->cases.size(), 2u);
    EXPECT_EQ(ms->cases[0].pattern.kind, MatchPattern::Kind::Literal);
    EXPECT_EQ(ms->cases[1].pattern.kind, MatchPattern::Kind::Literal);
}

TEST(ParserTest, PyMatchWildcardCase) {
    auto module = parse(
        "match x:\n"
        "    case 1:\n"
        "        pass\n"
        "    case _:\n"
        "        pass\n",
        false);
    ASSERT_NE(module, nullptr);
    auto* ms = dynamic_cast<MatchStmt*>(module->body[0].get());
    ASSERT_NE(ms, nullptr);
    EXPECT_EQ(ms->cases.size(), 2u);
    EXPECT_EQ(ms->cases[1].pattern.kind, MatchPattern::Kind::Wildcard);
}

TEST(ParserTest, PyMatchOrPattern) {
    auto module = parse(
        "match x:\n"
        "    case 1 | 2 | 3:\n"
        "        pass\n",
        false);
    ASSERT_NE(module, nullptr);
    auto* ms = dynamic_cast<MatchStmt*>(module->body[0].get());
    ASSERT_NE(ms, nullptr);
    EXPECT_EQ(ms->cases[0].pattern.kind, MatchPattern::Kind::Or);
    EXPECT_EQ(ms->cases[0].pattern.subPatterns.size(), 3u);
}

TEST(ParserTest, PyMatchSequencePattern) {
    auto module = parse(
        "match point:\n"
        "    case [x, y]:\n"
        "        pass\n",
        false);
    ASSERT_NE(module, nullptr);
    auto* ms = dynamic_cast<MatchStmt*>(module->body[0].get());
    ASSERT_NE(ms, nullptr);
    EXPECT_EQ(ms->cases[0].pattern.kind, MatchPattern::Kind::Sequence);
    EXPECT_EQ(ms->cases[0].pattern.subPatterns.size(), 2u);
}

TEST(ParserTest, PyMatchWithGuard) {
    auto module = parse(
        "match x:\n"
        "    case n if n > 0:\n"
        "        pass\n",
        false);
    ASSERT_NE(module, nullptr);
    auto* ms = dynamic_cast<MatchStmt*>(module->body[0].get());
    ASSERT_NE(ms, nullptr);
    EXPECT_NE(ms->cases[0].guard, nullptr);
}

TEST(ParserTest, PyMatchMultipleBodies) {
    auto module = parse(
        "match x:\n"
        "    case 1:\n"
        "        pass\n"
        "        pass\n"
        "    case _:\n"
        "        pass\n",
        false);
    ASSERT_NE(module, nullptr);
    auto* ms = dynamic_cast<MatchStmt*>(module->body[0].get());
    ASSERT_NE(ms, nullptr);
    EXPECT_EQ(ms->cases[0].body.size(), 2u);
    EXPECT_EQ(ms->cases[1].body.size(), 1u);
}

// --- Type Alias in .py mode (same syntax, no blocks) ---

TEST(ParserTest, PyTypeAliasSimple) {
    auto module = parse("type Point = tuple[int, int]", false);
    ASSERT_NE(module, nullptr);
    auto* alias = dynamic_cast<TypeAliasStmt*>(module->body[0].get());
    ASSERT_NE(alias, nullptr);
    EXPECT_EQ(alias->name, "Point");
    EXPECT_NE(alias->value, nullptr);
}

TEST(ParserTest, PyTypeAliasUnion) {
    auto module = parse("type Number = int | float", false);
    ASSERT_NE(module, nullptr);
    auto* alias = dynamic_cast<TypeAliasStmt*>(module->body[0].get());
    ASSERT_NE(alias, nullptr);
    EXPECT_EQ(alias->name, "Number");
}

// --- Exception Groups in .py mode ---

TEST(ParserTest, PyExceptStarParsed) {
    auto module = parse(
        "try:\n"
        "    pass\n"
        "except* ValueError:\n"
        "    pass\n",
        false);
    ASSERT_NE(module, nullptr);
    auto* tryStmt = dynamic_cast<TryStmt*>(module->body[0].get());
    ASSERT_NE(tryStmt, nullptr);
    ASSERT_EQ(tryStmt->handlers.size(), 1u);
    EXPECT_TRUE(tryStmt->handlers[0].isStar);
}

TEST(ParserTest, PyExceptStarWithAs) {
    auto module = parse(
        "try:\n"
        "    pass\n"
        "except* ValueError as eg:\n"
        "    pass\n",
        false);
    ASSERT_NE(module, nullptr);
    auto* tryStmt = dynamic_cast<TryStmt*>(module->body[0].get());
    ASSERT_NE(tryStmt, nullptr);
    ASSERT_EQ(tryStmt->handlers.size(), 1u);
    EXPECT_TRUE(tryStmt->handlers[0].isStar);
    EXPECT_EQ(tryStmt->handlers[0].name, "eg");
}

// --- Multiple Inheritance in .py mode ---

TEST(ParserTest, PyMultipleInheritanceParsed) {
    auto module = parse(
        "class Child(Base1, Base2):\n"
        "    pass\n",
        false);
    ASSERT_NE(module, nullptr);
    auto* cls = dynamic_cast<ClassDecl*>(module->body[0].get());
    ASSERT_NE(cls, nullptr);
    EXPECT_EQ(cls->bases.size(), 2u);
}

// --- Class with methods in .py mode (explicit self) ---

TEST(ParserTest, PyClassMethodExplicitSelf) {
    auto module = parse(
        "class Dog(Animal):\n"
        "    def __init__(self, x):\n"
        "        self.x = x\n"
        "    def speak(self):\n"
        "        return self.x\n",
        false);
    ASSERT_NE(module, nullptr);
    auto* cls = dynamic_cast<ClassDecl*>(module->body[0].get());
    ASSERT_NE(cls, nullptr);
    EXPECT_EQ(cls->bases.size(), 1u);
    EXPECT_EQ(cls->body.size(), 2u);
}

// --- With statement in .py mode ---

TEST(ParserTest, PyWithStatement) {
    auto module = parse(
        "with open(\"test.txt\", \"r\") as f:\n"
        "    pass\n",
        false);
    ASSERT_NE(module, nullptr);
    auto* ws = dynamic_cast<WithStmt*>(module->body[0].get());
    ASSERT_NE(ws, nullptr);
    EXPECT_EQ(ws->items.size(), 1u);
    EXPECT_NE(ws->items[0].optionalVars, nullptr);
}

// --- For loop in .py mode ---

TEST(ParserTest, PyForLoop) {
    auto module = parse(
        "for i in range(10):\n"
        "    pass\n",
        false);
    ASSERT_NE(module, nullptr);
    auto* fs = dynamic_cast<ForStmt*>(module->body[0].get());
    ASSERT_NE(fs, nullptr);
}

// --- Try/except/else/finally in .py mode ---

TEST(ParserTest, PyTryExceptElseFinally) {
    auto module = parse(
        "try:\n"
        "    pass\n"
        "except ValueError:\n"
        "    pass\n"
        "else:\n"
        "    pass\n"
        "finally:\n"
        "    pass\n",
        false);
    ASSERT_NE(module, nullptr);
    auto* tryStmt = dynamic_cast<TryStmt*>(module->body[0].get());
    ASSERT_NE(tryStmt, nullptr);
    EXPECT_EQ(tryStmt->handlers.size(), 1u);
    EXPECT_FALSE(tryStmt->handlers[0].isStar);
    EXPECT_FALSE(tryStmt->elseBody.empty());
    EXPECT_FALSE(tryStmt->finallyBody.empty());
}

// --- Function with positional-only and keyword-only params in .py mode ---

TEST(ParserTest, PyFunctionPosOnlyKwOnly) {
    auto module = parse(
        "def foo(a, b, /, c, *, d, e):\n"
        "    pass\n",
        false);
    ASSERT_NE(module, nullptr);
    auto* fn = dynamic_cast<FunctionDecl*>(module->body[0].get());
    ASSERT_NE(fn, nullptr);
    EXPECT_EQ(fn->posOnlyEnd, 2);
    EXPECT_EQ(fn->kwOnlyStart, 3);
    EXPECT_EQ(fn->params.size(), 5u);
}

// --- Walrus operator in .py mode ---

TEST(ParserTest, PyWalrusOperator) {
    auto module = parse(
        "if (n := 10) > 5:\n"
        "    pass\n",
        false);
    ASSERT_NE(module, nullptr);
    auto* ifStmt = dynamic_cast<IfStmt*>(module->body[0].get());
    ASSERT_NE(ifStmt, nullptr);
}

//===----------------------------------------------------------------------===//
// Decision 009: const, static, self() constructors
//===----------------------------------------------------------------------===//

// --- const ---

TEST(ParserTest, ConstDeclaration) {
    auto module = parse("const MAX: int = 100");
    ASSERT_NE(module, nullptr);
    auto* ann = dynamic_cast<AnnAssignStmt*>(module->body[0].get());
    ASSERT_NE(ann, nullptr);
    EXPECT_TRUE(ann->isConst);
    EXPECT_NE(ann->value, nullptr);
}

TEST(ParserTest, ConstInFunction) {
    auto module = parse("def foo() {\n  const limit: int = 50\n}");
    ASSERT_NE(module, nullptr);
    auto* fn = dynamic_cast<FunctionDecl*>(module->body[0].get());
    ASSERT_NE(fn, nullptr);
    auto* ann = dynamic_cast<AnnAssignStmt*>(fn->body[0].get());
    ASSERT_NE(ann, nullptr);
    EXPECT_TRUE(ann->isConst);
}

TEST(ParserTest, ConstNotInPyMode) {
    // In .py mode, 'const' is just an identifier - should parse as expression
    auto module = parse("const = 5", false);
    ASSERT_NE(module, nullptr);
}

TEST(ParserTest, ConstRequiresInit) {
    // const without = should produce a parser error
    auto errs = parseErrors("const X: int");
    EXPECT_FALSE(errs.empty());
}

// --- static ---

TEST(ParserTest, StaticField) {
    auto module = parse("class Foo {\n  static x: int = 0\n}");
    ASSERT_NE(module, nullptr);
    auto* cls = dynamic_cast<ClassDecl*>(module->body[0].get());
    ASSERT_NE(cls, nullptr);
    auto* ann = dynamic_cast<AnnAssignStmt*>(cls->body[0].get());
    ASSERT_NE(ann, nullptr);
    EXPECT_TRUE(ann->isStatic);
    EXPECT_FALSE(ann->isConst);
}

TEST(ParserTest, StaticMethod) {
    auto module = parse("class Foo {\n  static def bar() -> int {\n    return 0\n  }\n}");
    ASSERT_NE(module, nullptr);
    auto* cls = dynamic_cast<ClassDecl*>(module->body[0].get());
    ASSERT_NE(cls, nullptr);
    auto* fn = dynamic_cast<FunctionDecl*>(cls->body[0].get());
    ASSERT_NE(fn, nullptr);
    EXPECT_TRUE(fn->isStatic);
    EXPECT_FALSE(fn->hasImplicitSelf);
    EXPECT_TRUE(fn->isMethod);
}

TEST(ParserTest, StaticConst) {
    auto module = parse("class Foo {\n  static const VERSION: int = 1\n}");
    ASSERT_NE(module, nullptr);
    auto* cls = dynamic_cast<ClassDecl*>(module->body[0].get());
    ASSERT_NE(cls, nullptr);
    auto* ann = dynamic_cast<AnnAssignStmt*>(cls->body[0].get());
    ASSERT_NE(ann, nullptr);
    EXPECT_TRUE(ann->isStatic);
    EXPECT_TRUE(ann->isConst);
}

TEST(ParserTest, StaticNotInPyMode) {
    // In .py mode, 'static' is just an identifier
    auto module = parse("static = 5", false);
    ASSERT_NE(module, nullptr);
}

// --- def() anonymous constructor ---

TEST(ParserTest, DefCtorBasic) {
    auto module = parse("class Foo {\n  def(x: int) {\n    self.x = x\n  }\n}");
    ASSERT_NE(module, nullptr);
    auto* cls = dynamic_cast<ClassDecl*>(module->body[0].get());
    ASSERT_NE(cls, nullptr);
    auto* fn = dynamic_cast<FunctionDecl*>(cls->body[0].get());
    ASSERT_NE(fn, nullptr);
    EXPECT_EQ(fn->name, "__init__");
    EXPECT_TRUE(fn->isConstructor);
    EXPECT_TRUE(fn->isMethod);
    EXPECT_TRUE(fn->hasImplicitSelf);
    EXPECT_EQ(fn->constructorIndex, 0);
}

TEST(ParserTest, DefCtorMultiple) {
    auto module = parse(
        "class Point {\n"
        "  def(x: int, y: int) {\n"
        "    self.x = x\n"
        "    self.y = y\n"
        "  }\n"
        "  def(xy: int) {\n"
        "    self.x = xy\n"
        "    self.y = xy\n"
        "  }\n"
        "}");
    ASSERT_NE(module, nullptr);
    auto* cls = dynamic_cast<ClassDecl*>(module->body[0].get());
    ASSERT_NE(cls, nullptr);
    EXPECT_EQ(cls->body.size(), 2u);
    auto* fn0 = dynamic_cast<FunctionDecl*>(cls->body[0].get());
    auto* fn1 = dynamic_cast<FunctionDecl*>(cls->body[1].get());
    ASSERT_NE(fn0, nullptr);
    ASSERT_NE(fn1, nullptr);
    EXPECT_EQ(fn0->constructorIndex, 0);
    EXPECT_EQ(fn1->constructorIndex, 1);
    EXPECT_EQ(fn0->params.size(), 2u);
    EXPECT_EQ(fn1->params.size(), 1u);
}

TEST(ParserTest, DefCtorWithDefInit) {
    // Mixing def() and def __init__() - both get __init__ name
    auto module = parse(
        "class Foo {\n"
        "  def(x: int) {\n"
        "    self.x = x\n"
        "  }\n"
        "  def __init__(y: int, z: int) {\n"
        "    self.y = y\n"
        "    self.z = z\n"
        "  }\n"
        "}");
    ASSERT_NE(module, nullptr);
    auto* cls = dynamic_cast<ClassDecl*>(module->body[0].get());
    ASSERT_NE(cls, nullptr);
    auto* fn0 = dynamic_cast<FunctionDecl*>(cls->body[0].get());
    auto* fn1 = dynamic_cast<FunctionDecl*>(cls->body[1].get());
    EXPECT_EQ(fn0->name, "__init__");
    EXPECT_EQ(fn1->name, "__init__");
    EXPECT_EQ(fn0->constructorIndex, 0);
    EXPECT_EQ(fn1->constructorIndex, 1);
    EXPECT_TRUE(fn0->isConstructor);
    EXPECT_FALSE(fn1->isConstructor); // def __init__ is NOT isConstructor
}

//===----------------------------------------------------------------------===//
// extern "C" FFI Tests
//===----------------------------------------------------------------------===//

TEST(ParserTest, ExternCSingleFunc) {
    auto module = parse(
        "extern \"C\" def puts(s: str) -> int\n"
    );
    ASSERT_NE(module, nullptr);
    ASSERT_EQ(module->body.size(), 1u);
    auto* fn = dynamic_cast<FunctionDecl*>(module->body[0].get());
    ASSERT_NE(fn, nullptr);
    EXPECT_EQ(fn->name, "puts");
    EXPECT_TRUE(fn->isExtern);
    EXPECT_TRUE(fn->externLib.empty());
    EXPECT_EQ(fn->params.size(), 1u);
    EXPECT_TRUE(fn->body.empty());
}

TEST(ParserTest, ExternCFromLib) {
    auto module = parse(
        "extern \"C\" from \"curl\" {\n"
        "  def curl_easy_init() -> ptr\n"
        "  def curl_easy_cleanup(handle: ptr) -> int\n"
        "}\n"
    );
    ASSERT_NE(module, nullptr);
    ASSERT_EQ(module->body.size(), 2u);

    auto* fn0 = dynamic_cast<FunctionDecl*>(module->body[0].get());
    ASSERT_NE(fn0, nullptr);
    EXPECT_EQ(fn0->name, "curl_easy_init");
    EXPECT_TRUE(fn0->isExtern);
    EXPECT_EQ(fn0->externLib, "curl");
    EXPECT_EQ(fn0->params.size(), 0u);

    auto* fn1 = dynamic_cast<FunctionDecl*>(module->body[1].get());
    ASSERT_NE(fn1, nullptr);
    EXPECT_EQ(fn1->name, "curl_easy_cleanup");
    EXPECT_TRUE(fn1->isExtern);
    EXPECT_EQ(fn1->externLib, "curl");
    EXPECT_EQ(fn1->params.size(), 1u);
}

TEST(ParserTest, ExternCWithPtrType) {
    auto module = parse(
        "extern \"C\" def malloc(size: int) -> ptr\n"
        "extern \"C\" def free(p: ptr)\n"
    );
    ASSERT_NE(module, nullptr);
    ASSERT_EQ(module->body.size(), 2u);

    auto* fn0 = dynamic_cast<FunctionDecl*>(module->body[0].get());
    ASSERT_NE(fn0, nullptr);
    EXPECT_EQ(fn0->name, "malloc");
    EXPECT_TRUE(fn0->isExtern);
    auto* retType = dynamic_cast<NamedTypeExpr*>(fn0->returnType.get());
    ASSERT_NE(retType, nullptr);
    EXPECT_EQ(retType->name, "ptr");

    auto* fn1 = dynamic_cast<FunctionDecl*>(module->body[1].get());
    ASSERT_NE(fn1, nullptr);
    EXPECT_EQ(fn1->name, "free");
    EXPECT_TRUE(fn1->isExtern);
    EXPECT_TRUE(fn1->body.empty());
}

TEST(ParserTest, ExternCNotInPyMode) {
    // extern keyword should not be parsed in .py mode
    auto module = parse(
        "extern = 42\n",
        false);
    ASSERT_NE(module, nullptr);
    // Should parse as an assignment (extern is just an identifier in .py mode)
    EXPECT_GE(module->body.size(), 1u);
}

//===----------------------------------------------------------------------===//
// @staticmethod / @classmethod decorator wiring tests
//===----------------------------------------------------------------------===//

TEST(ParserTest, StaticmethodDecoratorPy) {
    auto module = parse(
        "class Foo:\n"
        "    @staticmethod\n"
        "    def bar(x: int) -> int:\n"
        "        return x * 2\n",
        false);
    ASSERT_NE(module, nullptr);
    auto* cls = dynamic_cast<ClassDecl*>(module->body[0].get());
    ASSERT_NE(cls, nullptr);
    auto* method = dynamic_cast<FunctionDecl*>(cls->body[0].get());
    ASSERT_NE(method, nullptr);
    EXPECT_EQ(method->name, "bar");
    EXPECT_TRUE(method->isStatic);
    EXPECT_FALSE(method->isClassMethod);
    EXPECT_TRUE(method->isMethod);
}

TEST(ParserTest, ClassmethodDecoratorPy) {
    auto module = parse(
        "class Foo:\n"
        "    @classmethod\n"
        "    def create(cls) -> Foo:\n"
        "        pass\n",
        false);
    ASSERT_NE(module, nullptr);
    auto* cls = dynamic_cast<ClassDecl*>(module->body[0].get());
    ASSERT_NE(cls, nullptr);
    auto* method = dynamic_cast<FunctionDecl*>(cls->body[0].get());
    ASSERT_NE(method, nullptr);
    EXPECT_EQ(method->name, "create");
    EXPECT_TRUE(method->isStatic);
    EXPECT_TRUE(method->isClassMethod);
    EXPECT_TRUE(method->isMethod);
}

TEST(ParserTest, StaticmethodDecoratorDr) {
    // @staticmethod should also work in .dr mode
    auto module = parse(
        "class Foo {\n"
        "  @staticmethod\n"
        "  def bar(x: int) -> int {\n"
        "    return x * 2\n"
        "  }\n"
        "}\n");
    ASSERT_NE(module, nullptr);
    auto* cls = dynamic_cast<ClassDecl*>(module->body[0].get());
    ASSERT_NE(cls, nullptr);
    auto* method = dynamic_cast<FunctionDecl*>(cls->body[0].get());
    ASSERT_NE(method, nullptr);
    EXPECT_EQ(method->name, "bar");
    EXPECT_TRUE(method->isStatic);
}

TEST(ParserTest, NonMethodDecoratorNoEffect) {
    // @staticmethod on a module-level function should NOT set isStatic
    // (it's only valid on methods)
    auto module = parse(
        "@staticmethod\n"
        "def foo(x: int) -> int {\n"
        "  return x\n"
        "}\n");
    ASSERT_NE(module, nullptr);
    auto* func = dynamic_cast<FunctionDecl*>(module->body[0].get());
    ASSERT_NE(func, nullptr);
    EXPECT_FALSE(func->isStatic);
    EXPECT_FALSE(func->isMethod);
}

// ============================================================
// Dict Ergonomics (Decision 012 Parts 1-3)
// ============================================================

TEST(ParserTest, BareKeyDict) {
    // .dr mode: bare identifier keys become string literals
    auto module = parse("{name: 1, age: 2}");
    ASSERT_NE(module, nullptr);
    auto* exprStmt = dynamic_cast<ExprStmt*>(module->body[0].get());
    ASSERT_NE(exprStmt, nullptr);
    auto* dict = dynamic_cast<DictExpr*>(exprStmt->expr.get());
    ASSERT_NE(dict, nullptr);
    EXPECT_EQ(dict->entries.size(), 2u);
    // Keys should be StringLiteral, not NameExpr
    auto* key0 = dynamic_cast<StringLiteral*>(dict->entries[0].first.get());
    ASSERT_NE(key0, nullptr);
    EXPECT_EQ(key0->value, "name");
    auto* key1 = dynamic_cast<StringLiteral*>(dict->entries[1].first.get());
    ASSERT_NE(key1, nullptr);
    EXPECT_EQ(key1->value, "age");
}

TEST(ParserTest, BareKeyDictMixedWithQuoted) {
    // Mix bare keys with quoted keys
    auto module = parse("{name: 1, \"content-type\": 2}");
    ASSERT_NE(module, nullptr);
    auto* exprStmt = dynamic_cast<ExprStmt*>(module->body[0].get());
    auto* dict = dynamic_cast<DictExpr*>(exprStmt->expr.get());
    ASSERT_NE(dict, nullptr);
    EXPECT_EQ(dict->entries.size(), 2u);
    // First key: bare -> StringLiteral
    auto* key0 = dynamic_cast<StringLiteral*>(dict->entries[0].first.get());
    ASSERT_NE(key0, nullptr);
    EXPECT_EQ(key0->value, "name");
    // Second key: quoted -> StringLiteral
    auto* key1 = dynamic_cast<StringLiteral*>(dict->entries[1].first.get());
    ASSERT_NE(key1, nullptr);
    EXPECT_EQ(key1->value, "content-type");
}

TEST(ParserTest, BareKeyDictSingleEntry) {
    auto module = parse("{host: \"localhost\"}");
    ASSERT_NE(module, nullptr);
    auto* exprStmt = dynamic_cast<ExprStmt*>(module->body[0].get());
    auto* dict = dynamic_cast<DictExpr*>(exprStmt->expr.get());
    ASSERT_NE(dict, nullptr);
    EXPECT_EQ(dict->entries.size(), 1u);
    auto* key = dynamic_cast<StringLiteral*>(dict->entries[0].first.get());
    ASSERT_NE(key, nullptr);
    EXPECT_EQ(key->value, "host");
}

TEST(ParserTest, ComputedKeyDict) {
    // Computed key: (expr) evaluates the expression
    auto module = parse("{(x): 1, name: 2}");
    ASSERT_NE(module, nullptr);
    auto* exprStmt = dynamic_cast<ExprStmt*>(module->body[0].get());
    auto* dict = dynamic_cast<DictExpr*>(exprStmt->expr.get());
    ASSERT_NE(dict, nullptr);
    EXPECT_EQ(dict->entries.size(), 2u);
    // First key: computed -> NameExpr (variable reference)
    auto* key0 = dynamic_cast<NameExpr*>(dict->entries[0].first.get());
    ASSERT_NE(key0, nullptr);
    EXPECT_EQ(key0->name, "x");
    // Second key: bare -> StringLiteral
    auto* key1 = dynamic_cast<StringLiteral*>(dict->entries[1].first.get());
    ASSERT_NE(key1, nullptr);
    EXPECT_EQ(key1->value, "name");
}

TEST(ParserTest, PyModeDictNotBareKey) {
    // .py mode: bare identifiers are variable lookups, NOT string keys
    auto module = parse("{name: 1}", false);
    ASSERT_NE(module, nullptr);
    auto* exprStmt = dynamic_cast<ExprStmt*>(module->body[0].get());
    auto* dict = dynamic_cast<DictExpr*>(exprStmt->expr.get());
    ASSERT_NE(dict, nullptr);
    EXPECT_EQ(dict->entries.size(), 1u);
    // Key should be NameExpr (variable lookup), NOT StringLiteral
    auto* key = dynamic_cast<NameExpr*>(dict->entries[0].first.get());
    ASSERT_NE(key, nullptr);
    EXPECT_EQ(key->name, "name");
}

TEST(ParserTest, BareKeyDictWithNumericValue) {
    auto module = parse("{status: 200, message: \"ok\"}");
    ASSERT_NE(module, nullptr);
    auto* exprStmt = dynamic_cast<ExprStmt*>(module->body[0].get());
    auto* dict = dynamic_cast<DictExpr*>(exprStmt->expr.get());
    ASSERT_NE(dict, nullptr);
    EXPECT_EQ(dict->entries.size(), 2u);
    auto* key0 = dynamic_cast<StringLiteral*>(dict->entries[0].first.get());
    ASSERT_NE(key0, nullptr);
    EXPECT_EQ(key0->value, "status");
}

//===----------------------------------------------------------------------===//
// Template Expression Tests
//===----------------------------------------------------------------------===//

TEST(ParserTest, TemplateExprParsed) {
    auto module = parse("x: str = template {hello world}");
    ASSERT_NE(module, nullptr);
    auto* ann = dynamic_cast<AnnAssignStmt*>(module->body[0].get());
    ASSERT_NE(ann, nullptr);
    auto* tmpl = dynamic_cast<TemplateExpr*>(ann->value.get());
    ASSERT_NE(tmpl, nullptr);
    EXPECT_EQ(tmpl->body, "hello world");
}

TEST(ParserTest, TemplateExprWithBraces) {
    auto module = parse("x: str = template {{\"key\": \"val\"}}");
    ASSERT_NE(module, nullptr);
    auto* ann = dynamic_cast<AnnAssignStmt*>(module->body[0].get());
    ASSERT_NE(ann, nullptr);
    auto* tmpl = dynamic_cast<TemplateExpr*>(ann->value.get());
    ASSERT_NE(tmpl, nullptr);
    EXPECT_EQ(tmpl->body, "{\"key\": \"val\"}");
}

TEST(ParserTest, TemplateFileExprParsed) {
    auto module = parse("x: str = template(\"page.html\")");
    ASSERT_NE(module, nullptr);
    auto* ann = dynamic_cast<AnnAssignStmt*>(module->body[0].get());
    ASSERT_NE(ann, nullptr);
    auto* tmpl = dynamic_cast<TemplateFileExpr*>(ann->value.get());
    ASSERT_NE(tmpl, nullptr);
    EXPECT_EQ(tmpl->filePath, "page.html");
}

TEST(ParserTest, TemplateFileExprAbsPath) {
    auto module = parse("x: str = template(\"/tmp/templates/page.html\")");
    ASSERT_NE(module, nullptr);
    auto* ann = dynamic_cast<AnnAssignStmt*>(module->body[0].get());
    ASSERT_NE(ann, nullptr);
    auto* tmpl = dynamic_cast<TemplateFileExpr*>(ann->value.get());
    ASSERT_NE(tmpl, nullptr);
    EXPECT_EQ(tmpl->filePath, "/tmp/templates/page.html");
}

TEST(ParserTest, TypedTemplateExprParsed) {
    auto module = parse("x = template[HTML] {<h1>hello</h1>}");
    ASSERT_NE(module, nullptr);
    auto* assign = dynamic_cast<AssignStmt*>(module->body[0].get());
    ASSERT_NE(assign, nullptr);
    auto* tmpl = dynamic_cast<TemplateExpr*>(assign->value.get());
    ASSERT_NE(tmpl, nullptr);
    EXPECT_EQ(tmpl->contentType, "HTML");
    EXPECT_EQ(tmpl->body, "<h1>hello</h1>");
}

TEST(ParserTest, TypedTemplateFileExprParsed) {
    auto module = parse("x = template[HTML](\"page.html\")");
    ASSERT_NE(module, nullptr);
    auto* assign = dynamic_cast<AssignStmt*>(module->body[0].get());
    ASSERT_NE(assign, nullptr);
    auto* tmpl = dynamic_cast<TemplateFileExpr*>(assign->value.get());
    ASSERT_NE(tmpl, nullptr);
    EXPECT_EQ(tmpl->contentType, "HTML");
    EXPECT_EQ(tmpl->filePath, "page.html");
}

TEST(ParserTest, UntypedTemplateHasEmptyContentType) {
    auto module = parse("x: str = template {hello}");
    ASSERT_NE(module, nullptr);
    auto* ann = dynamic_cast<AnnAssignStmt*>(module->body[0].get());
    ASSERT_NE(ann, nullptr);
    auto* tmpl = dynamic_cast<TemplateExpr*>(ann->value.get());
    ASSERT_NE(tmpl, nullptr);
    EXPECT_EQ(tmpl->contentType, "");
    EXPECT_EQ(tmpl->body, "hello");
}
