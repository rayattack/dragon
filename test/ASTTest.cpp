#include <gtest/gtest.h>
#include "TestHelpers.h"

using namespace dragon;
using namespace dragon::test;

//===----------------------------------------------------------------------===//
// ASTPrinter Tests
//===----------------------------------------------------------------------===//

TEST(ASTPrinterTest, EmptyModule) {
    auto module = parse("");
    ASSERT_NE(module, nullptr);
    ASTPrinter printer;
    std::string output = printer.print(*module);
    EXPECT_NE(output.find("(module"), std::string::npos);
}

TEST(ASTPrinterTest, IntegerLiteral) {
    auto module = parse("42");
    ASSERT_NE(module, nullptr);
    ASTPrinter printer;
    std::string output = printer.print(*module);
    EXPECT_NE(output.find("(int 42)"), std::string::npos);
}

TEST(ASTPrinterTest, FloatLiteral) {
    auto module = parse("3.14");
    ASSERT_NE(module, nullptr);
    ASTPrinter printer;
    std::string output = printer.print(*module);
    EXPECT_NE(output.find("(float"), std::string::npos);
    EXPECT_NE(output.find("3.14"), std::string::npos);
}

TEST(ASTPrinterTest, StringLiteral) {
    auto module = parse("\"hello\"");
    ASSERT_NE(module, nullptr);
    ASTPrinter printer;
    std::string output = printer.print(*module);
    EXPECT_NE(output.find("(str \"hello\")"), std::string::npos);
}

TEST(ASTPrinterTest, BooleanLiteral) {
    auto module = parse("True");
    ASSERT_NE(module, nullptr);
    ASTPrinter printer;
    std::string output = printer.print(*module);
    EXPECT_NE(output.find("(bool True)"), std::string::npos);
}

TEST(ASTPrinterTest, NoneLiteral) {
    auto module = parse("None");
    ASSERT_NE(module, nullptr);
    ASTPrinter printer;
    std::string output = printer.print(*module);
    EXPECT_NE(output.find("(None)"), std::string::npos);
}

TEST(ASTPrinterTest, NameExpr) {
    auto module = parse("x");
    ASSERT_NE(module, nullptr);
    ASTPrinter printer;
    std::string output = printer.print(*module);
    EXPECT_NE(output.find("(name x)"), std::string::npos);
}

TEST(ASTPrinterTest, BinaryExpr) {
    auto module = parse("1 + 2");
    ASSERT_NE(module, nullptr);
    ASTPrinter printer;
    std::string output = printer.print(*module);
    EXPECT_NE(output.find("(binary +)"), std::string::npos);
    EXPECT_NE(output.find("(int 1)"), std::string::npos);
    EXPECT_NE(output.find("(int 2)"), std::string::npos);
}

TEST(ASTPrinterTest, UnaryExpr) {
    auto module = parse("-x");
    ASSERT_NE(module, nullptr);
    ASTPrinter printer;
    std::string output = printer.print(*module);
    EXPECT_NE(output.find("(unary -)"), std::string::npos);
    EXPECT_NE(output.find("(name x)"), std::string::npos);
}

TEST(ASTPrinterTest, CallExpr) {
    auto module = parse("f(1, 2)");
    ASSERT_NE(module, nullptr);
    ASTPrinter printer;
    std::string output = printer.print(*module);
    EXPECT_NE(output.find("(call"), std::string::npos);
    EXPECT_NE(output.find("(name f)"), std::string::npos);
    EXPECT_NE(output.find("(int 1)"), std::string::npos);
    EXPECT_NE(output.find("(int 2)"), std::string::npos);
}

TEST(ASTPrinterTest, AttributeExpr) {
    auto module = parse("obj.attr");
    ASSERT_NE(module, nullptr);
    ASTPrinter printer;
    std::string output = printer.print(*module);
    EXPECT_NE(output.find("(attribute .attr)"), std::string::npos);
    EXPECT_NE(output.find("(name obj)"), std::string::npos);
}

TEST(ASTPrinterTest, ListExpr) {
    auto module = parse("[1, 2, 3]");
    ASSERT_NE(module, nullptr);
    ASTPrinter printer;
    std::string output = printer.print(*module);
    EXPECT_NE(output.find("(list"), std::string::npos);
}

TEST(ASTPrinterTest, DictExpr) {
    auto module = parse("{\"a\": 1}");
    ASSERT_NE(module, nullptr);
    ASTPrinter printer;
    std::string output = printer.print(*module);
    EXPECT_NE(output.find("(dict"), std::string::npos);
    EXPECT_NE(output.find("(entry"), std::string::npos);
}

TEST(ASTPrinterTest, PassStatement) {
    auto module = parse("pass");
    ASSERT_NE(module, nullptr);
    ASTPrinter printer;
    std::string output = printer.print(*module);
    EXPECT_NE(output.find("(pass)"), std::string::npos);
}

TEST(ASTPrinterTest, BreakStatement) {
    auto module = parse("break");
    ASSERT_NE(module, nullptr);
    ASTPrinter printer;
    std::string output = printer.print(*module);
    EXPECT_NE(output.find("(break)"), std::string::npos);
}

TEST(ASTPrinterTest, ContinueStatement) {
    auto module = parse("continue");
    ASSERT_NE(module, nullptr);
    ASTPrinter printer;
    std::string output = printer.print(*module);
    EXPECT_NE(output.find("(continue)"), std::string::npos);
}

TEST(ASTPrinterTest, ReturnStatement) {
    auto module = parse("return 42");
    ASSERT_NE(module, nullptr);
    ASTPrinter printer;
    std::string output = printer.print(*module);
    EXPECT_NE(output.find("(return"), std::string::npos);
    EXPECT_NE(output.find("(int 42)"), std::string::npos);
}

TEST(ASTPrinterTest, ReturnNoValue) {
    auto module = parse("return");
    ASSERT_NE(module, nullptr);
    ASTPrinter printer;
    std::string output = printer.print(*module);
    EXPECT_NE(output.find("(return)"), std::string::npos);
}

TEST(ASTPrinterTest, AssignStatement) {
    auto module = parse("x = 5");
    ASSERT_NE(module, nullptr);
    ASTPrinter printer;
    std::string output = printer.print(*module);
    EXPECT_NE(output.find("(assign"), std::string::npos);
    EXPECT_NE(output.find("(name x)"), std::string::npos);
    EXPECT_NE(output.find("(int 5)"), std::string::npos);
}

TEST(ASTPrinterTest, AugAssignStatement) {
    auto module = parse("x += 1");
    ASSERT_NE(module, nullptr);
    ASTPrinter printer;
    std::string output = printer.print(*module);
    EXPECT_NE(output.find("(aug-assign +=)"), std::string::npos);
}

TEST(ASTPrinterTest, AnnAssignStatement) {
    auto module = parse("x: int = 5");
    ASSERT_NE(module, nullptr);
    ASTPrinter printer;
    std::string output = printer.print(*module);
    EXPECT_NE(output.find("(ann-assign"), std::string::npos);
    EXPECT_NE(output.find("int"), std::string::npos);
}

TEST(ASTPrinterTest, ImportStatement) {
    auto module = parse("import os");
    ASSERT_NE(module, nullptr);
    ASTPrinter printer;
    std::string output = printer.print(*module);
    EXPECT_NE(output.find("(import os)"), std::string::npos);
}

TEST(ASTPrinterTest, FromImportStatement) {
    auto module = parse("from os import path");
    ASSERT_NE(module, nullptr);
    ASTPrinter printer;
    std::string output = printer.print(*module);
    EXPECT_NE(output.find("(from os import path)"), std::string::npos);
}

TEST(ASTPrinterTest, GlobalStatement) {
    auto module = parse("global x, y");
    ASSERT_NE(module, nullptr);
    ASTPrinter printer;
    std::string output = printer.print(*module);
    EXPECT_NE(output.find("(global x, y)"), std::string::npos);
}

TEST(ASTPrinterTest, IfStatement) {
    auto module = parse("if x {\n  pass\n}");
    ASSERT_NE(module, nullptr);
    ASTPrinter printer;
    std::string output = printer.print(*module);
    EXPECT_NE(output.find("(if"), std::string::npos);
    EXPECT_NE(output.find("(cond"), std::string::npos);
    EXPECT_NE(output.find("(then"), std::string::npos);
    EXPECT_NE(output.find("(pass)"), std::string::npos);
}

TEST(ASTPrinterTest, WhileStatement) {
    auto module = parse("while x {\n  pass\n}");
    ASSERT_NE(module, nullptr);
    ASTPrinter printer;
    std::string output = printer.print(*module);
    EXPECT_NE(output.find("(while"), std::string::npos);
    EXPECT_NE(output.find("(cond"), std::string::npos);
    EXPECT_NE(output.find("(body"), std::string::npos);
}

TEST(ASTPrinterTest, ForStatement) {
    auto module = parse("for x in items {\n  pass\n}");
    ASSERT_NE(module, nullptr);
    ASTPrinter printer;
    std::string output = printer.print(*module);
    EXPECT_NE(output.find("(for"), std::string::npos);
    EXPECT_NE(output.find("(target"), std::string::npos);
    EXPECT_NE(output.find("(iter"), std::string::npos);
}

TEST(ASTPrinterTest, FunctionDecl) {
    auto module = parse("def foo(x: int) -> int {\n  return x\n}");
    ASSERT_NE(module, nullptr);
    ASTPrinter printer;
    std::string output = printer.print(*module);
    EXPECT_NE(output.find("(def foo"), std::string::npos);
    EXPECT_NE(output.find("(params"), std::string::npos);
    EXPECT_NE(output.find("x"), std::string::npos);
    EXPECT_NE(output.find("(returns:"), std::string::npos);
    EXPECT_NE(output.find("(body"), std::string::npos);
}

TEST(ASTPrinterTest, ClassDecl) {
    auto module = parse("class Foo {\n  pass\n}");
    ASSERT_NE(module, nullptr);
    ASTPrinter printer;
    std::string output = printer.print(*module);
    EXPECT_NE(output.find("(class Foo"), std::string::npos);
    EXPECT_NE(output.find("(body"), std::string::npos);
}

TEST(ASTPrinterTest, ClassWithBases) {
    auto module = parse("class Dog(Animal) {\n  pass\n}");
    ASSERT_NE(module, nullptr);
    ASTPrinter printer;
    std::string output = printer.print(*module);
    EXPECT_NE(output.find("(class Dog"), std::string::npos);
    EXPECT_NE(output.find("(bases"), std::string::npos);
    EXPECT_NE(output.find("Animal"), std::string::npos);
}

TEST(ASTPrinterTest, TryStatement) {
    auto module = parse("try {\n  pass\n} catch {\n  pass\n}");
    ASSERT_NE(module, nullptr);
    ASTPrinter printer;
    std::string output = printer.print(*module);
    EXPECT_NE(output.find("(try"), std::string::npos);
    EXPECT_NE(output.find("(catch"), std::string::npos);
}

TEST(ASTPrinterTest, AssertStatement) {
    auto module = parse("assert x, \"msg\"");
    ASSERT_NE(module, nullptr);
    ASTPrinter printer;
    std::string output = printer.print(*module);
    EXPECT_NE(output.find("(assert"), std::string::npos);
    EXPECT_NE(output.find("(name x)"), std::string::npos);
    EXPECT_NE(output.find("(msg"), std::string::npos);
}

//===----------------------------------------------------------------------===//
// DefaultASTVisitor Tests
//===----------------------------------------------------------------------===//

// Simple visitor that counts nodes
class NodeCounter : public DefaultASTVisitor {
public:
    int count = 0;

    void visit(IntegerLiteral& node) override { count++; DefaultASTVisitor::visit(node); }
    void visit(FloatLiteral& node) override { count++; DefaultASTVisitor::visit(node); }
    void visit(StringLiteral& node) override { count++; DefaultASTVisitor::visit(node); }
    void visit(BooleanLiteral& node) override { count++; DefaultASTVisitor::visit(node); }
    void visit(NoneLiteral& node) override { count++; DefaultASTVisitor::visit(node); }
    void visit(NameExpr& node) override { count++; DefaultASTVisitor::visit(node); }
    void visit(BinaryExpr& node) override { count++; DefaultASTVisitor::visit(node); }
    void visit(UnaryExpr& node) override { count++; DefaultASTVisitor::visit(node); }
    void visit(CallExpr& node) override { count++; DefaultASTVisitor::visit(node); }
    void visit(PassStmt& node) override { count++; DefaultASTVisitor::visit(node); }
    void visit(ReturnStmt& node) override { count++; DefaultASTVisitor::visit(node); }
    void visit(ExprStmt& node) override { count++; DefaultASTVisitor::visit(node); }
    void visit(FunctionDecl& node) override { count++; DefaultASTVisitor::visit(node); }
    void visit(Module& node) override { count++; DefaultASTVisitor::visit(node); }
};

TEST(DefaultASTVisitorTest, CountsModuleNode) {
    auto module = parse("");
    ASSERT_NE(module, nullptr);
    NodeCounter counter;
    module->accept(counter);
    EXPECT_EQ(counter.count, 1);  // just the Module
}

TEST(DefaultASTVisitorTest, CountsExpressionNodes) {
    auto module = parse("1 + 2");
    ASSERT_NE(module, nullptr);
    NodeCounter counter;
    module->accept(counter);
    // Module + ExprStmt + BinaryExpr + IntegerLiteral(1) + IntegerLiteral(2) = 5
    EXPECT_EQ(counter.count, 5);
}

TEST(DefaultASTVisitorTest, CountsFunctionNodes) {
    auto module = parse("def foo() {\n  pass\n}");
    ASSERT_NE(module, nullptr);
    NodeCounter counter;
    module->accept(counter);
    // Module + FunctionDecl + PassStmt = 3
    EXPECT_EQ(counter.count, 3);
}

TEST(DefaultASTVisitorTest, TraversesNestedCalls) {
    auto module = parse("f(g(x))");
    ASSERT_NE(module, nullptr);
    NodeCounter counter;
    module->accept(counter);
    // Module + ExprStmt + CallExpr(f) + NameExpr(f) + CallExpr(g) + NameExpr(g) + NameExpr(x) = 7
    EXPECT_EQ(counter.count, 7);
}
