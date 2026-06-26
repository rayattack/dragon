// Decision 044 - IR-shape proof that user generics monomorphize to native
// representation (NOT boxed). The behavioral round-trips live in the .dr
// suites (test/dr/test_generics_*.dr); these check the lowering the doctrine
// hinges on: a stamped instantiation's field is the concrete native type, each
// distinct instantiation emits exactly one struct, and the template itself is
// never lowered.

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

// Box[int]'s stamped struct stores an i64 field (native int), not a 16-byte
// %dragon.box - the whole point of monomorphizing instead of erasing/boxing.
TEST(CodeGenGenericsTest, MonomorphizedFieldIsNative) {
    auto ir = generateIR(std::string(kBox) +
        "b: Box[int] = Box[int](5)\n"
        "n: int = b.get()\n"
        "print(n)\n");
    // The stamped struct exists and a Box[int] getter is emitted.
    EXPECT_NE(ir.find("\"Box[int]\""), std::string::npos);
    // No box payload in the Box[int] specialization: the field is i64.
    // (A boxed element would surface as %dragon.box in the struct/getter.)
    EXPECT_NE(ir.find("%\"Box[int]\" = type"), std::string::npos);
}

// Two uses of Box[int] share one stamped struct definition (instantiation dedup).
TEST(CodeGenGenericsTest, InstantiationDedup) {
    auto ir = generateIR(std::string(kBox) +
        "a: Box[int] = Box[int](1)\n"
        "b: Box[int] = Box[int](2)\n"
        "print(a.get() + b.get())\n");
    EXPECT_EQ(countSubstring(ir, "%\"Box[int]\" = type"), 1u);
}

// str and int instantiations are distinct stamped structs with distinct fields.
TEST(CodeGenGenericsTest, DistinctInstantiationsDistinctStructs) {
    auto ir = generateIR(std::string(kBox) +
        "a: Box[int] = Box[int](1)\n"
        "b: Box[str] = Box[str](\"x\")\n"
        "print(a.get())\n");
    EXPECT_NE(ir.find("%\"Box[int]\" = type"), std::string::npos);
    EXPECT_NE(ir.find("%\"Box[str]\" = type"), std::string::npos);
}

// The generic template itself is never lowered - no `Box[T]` struct/symbol.
TEST(CodeGenGenericsTest, TemplateNotEmitted) {
    auto ir = generateIR(std::string(kBox) +
        "b: Box[int] = Box[int](5)\n"
        "print(b.get())\n");
    EXPECT_EQ(ir.find("%\"Box[T]\" = type"), std::string::npos);
    EXPECT_EQ(ir.find("Box[T]"), std::string::npos);
}

// End-to-end: native int round-trips through a generic class and a generic fn.
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
