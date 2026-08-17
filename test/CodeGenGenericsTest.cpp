#include "CodeGenTestHelpers.h"

using namespace dragon;
using namespace dragon::test;

namespace {
const char* kBox =
    "class Box[T] {\n"
    "    def(v: T) { self.value = v }\n"
    "    def get() -> T { return self.value }\n"
    "}\n";
}

TEST(CodeGenGenericsTest, MonomorphizedFieldIsNative) {
    auto ir = generateIR(std::string(kBox) +
        "b: Box[int] = Box[int](5)\n"
        "n: int = b.get()\n"
        "print(n)\n");
    EXPECT_NE(ir.find("\"Box[int]\""), std::string::npos);
    EXPECT_NE(ir.find("%\"Box[int]\" = type"), std::string::npos);
}

TEST(CodeGenGenericsTest, InstantiationDedup) {
    auto ir = generateIR(std::string(kBox) +
        "a: Box[int] = Box[int](1)\n"
        "b: Box[int] = Box[int](2)\n"
        "print(a.get() + b.get())\n");
    EXPECT_EQ(countSubstring(ir, "%\"Box[int]\" = type"), 1u);
}

TEST(CodeGenGenericsTest, DistinctInstantiationsDistinctStructs) {
    auto ir = generateIR(std::string(kBox) +
        "a: Box[int] = Box[int](1)\n"
        "b: Box[str] = Box[str](\"x\")\n"
        "print(a.get())\n");
    EXPECT_NE(ir.find("%\"Box[int]\" = type"), std::string::npos);
    EXPECT_NE(ir.find("%\"Box[str]\" = type"), std::string::npos);
}

TEST(CodeGenGenericsTest, TemplateNotEmitted) {
    auto ir = generateIR(std::string(kBox) +
        "b: Box[int] = Box[int](5)\n"
        "print(b.get())\n");
    EXPECT_EQ(ir.find("%\"Box[T]\" = type"), std::string::npos);
    EXPECT_EQ(ir.find("Box[T]"), std::string::npos);
}

TEST(CodeGenGenericsTest, RoundTripIntAndFunction) {
    auto out = compileAndRun(std::string(kBox) +
        "def first[T](xs: list[T]) -> T { return xs[0] }\n"
        "b: Box[int] = Box[int](41)\n"
        "b.value = b.value + 1\n"
        "print(b.get())\n"
        "xs: list[int] = [7, 8]\n"
        "print(first(xs))\n");
    EXPECT_EQ(out, "42\n7\n");
}
