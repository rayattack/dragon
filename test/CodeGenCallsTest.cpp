#include "CodeGenTestHelpers.h"

//===----------------------------------------------------------------------===//
// Print IR Tests
//===----------------------------------------------------------------------===//

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
    EXPECT_NE(ir.find("dragon_print_list_int"), std::string::npos);
}

TEST(CodeGenTest, PrintListStrDispatch) {
    auto ir = generateIR(
        "x: list[str] = [\"a\", \"b\"]\n"
        "print(x)\n"
    );
    EXPECT_NE(ir.find("dragon_print_list_str"), std::string::npos);
}

TEST(CodeGenTest, PrintListFloatDispatch) {
    auto ir = generateIR(
        "x: list[float] = [1.0, 2.0]\n"
        "print(x)\n"
    );
    EXPECT_NE(ir.find("dragon_print_list_float"), std::string::npos);
}

//===----------------------------------------------------------------------===//
// Math IR Tests
//===----------------------------------------------------------------------===//

TEST(CodeGenTest, MathSqrt) {
    auto ir = generateIR("from math import sqrt\nx: float = sqrt(4.0)");
    EXPECT_NE(ir.find("call double @sqrt"), std::string::npos);
}

TEST(CodeGenTest, MathPi) {
    auto ir = generateIR("import math\nx: float = math.pi");
    // LLVM stores pi as hex double 0x400921FB54442D18
    EXPECT_NE(ir.find("double"), std::string::npos);
    EXPECT_NE(ir.find("400921FB54442D18"), std::string::npos);
}

//===----------------------------------------------------------------------===//
// Builtins IR Tests
//===----------------------------------------------------------------------===//

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

// File I/O (open/make/push -> Reader/Writer) behavior is covered by the
// dogfooded unittest at test/dr/test_io_streams.dr, not by C++ IR assertions:
// the surface is ordinary Dragon classes, so there are no special runtime
// symbols to grep for.

//===----------------------------------------------------------------------===//
// Extern C IR Tests
//===----------------------------------------------------------------------===//

TEST(CodeGenTest, ExternCSingleDeclIR) {
    auto ir = generateIR(
        "extern \"C\" def puts(s: str) -> int\n"
    );
    // Should have a declaration (no body) for puts
    EXPECT_NE(ir.find("declare i64 @puts(ptr)"), std::string::npos)
        << "Expected extern puts declaration:\n" << ir;
}

TEST(CodeGenTest, ExternCFromLibIR) {
    auto ir = generateIR(
        "extern \"C\" from \"mylib\" {\n"
        "  def foo(x: int) -> int\n"
        "  def bar(s: str) -> ptr\n"
        "}\n"
    );
    EXPECT_NE(ir.find("declare i64 @foo(i64)"), std::string::npos)
        << "Expected foo declaration:\n" << ir;
    EXPECT_NE(ir.find("declare ptr @bar(ptr)"), std::string::npos)
        << "Expected bar declaration:\n" << ir;
}

TEST(CodeGenTest, ExternCPtrTypeIR) {
    auto ir = generateIR(
        "extern \"C\" def malloc(size: int) -> ptr\n"
        "extern \"C\" def free(p: ptr)\n"
    );
    EXPECT_NE(ir.find("declare ptr @malloc(i64)"), std::string::npos)
        << "Expected malloc declaration:\n" << ir;
    EXPECT_NE(ir.find("declare void @free(ptr)"), std::string::npos)
        << "Expected free declaration:\n" << ir;
}

// Regression: a discarded `ptr`-returning extern call (e.g. `memset(buf, 0,
// n)`) must NOT emit `dragon_decref_str` on the return value. The call's
// resolved type is `ptr` (no DragonObjectHeader), so a spurious
// `dragon_decref_str` would interpret the 16 bytes preceding the buffer as a
// fake header and (rarely) free a misaligned pointer - manifesting as
// `malloc_consolidate(): unaligned fastbin chunk detected` later.
//
// Pre-fix path: ExprStmt -> inferPtrValueTag returned 1 (TAG_STR) by default
// when typeKindToElemTag(Ptr) returned 0 and the if-guard `t != 0` skipped
// returning it, falling through to the "assume string" tail. Fix: trust a
// resolved expr->type unconditionally - Ptr resolves to 0 -> no decref.
TEST(CodeGenTest, ExprStmtDiscardingPtrReturnNoDecrefStr) {
    auto ir = generateIR(
        "extern \"C\" def malloc(size: int) -> ptr\n"
        "extern \"C\" def memset(s: ptr, c: intc, n: int) -> ptr\n"
        "extern \"C\" def free(p: ptr)\n"
        "const buf: ptr = malloc(64)\n"
        "memset(buf, 0, 64)\n"
        "free(buf)\n"
    );
    // The discarded `memset` must not produce a dragon_decref_str call. Any
    // such call would be against the raw malloc'd buffer and corrupt the heap.
    EXPECT_EQ(ir.find("call void @dragon_decref_str"), std::string::npos)
        << "Spurious dragon_decref_str on discarded ptr-returning call:\n" << ir;
}

//===----------------------------------------------------------------------===//
// isinstance/type E2E Tests
//===----------------------------------------------------------------------===//

TEST(CodeGenE2E, IsinstanceInt) {
    auto output = compileAndRun(
        "x: int = 42\n"
        "print(isinstance(x, int))\n"
    );
    EXPECT_EQ(output, "True\n");
}

TEST(CodeGenE2E, IsinstanceStr) {
    auto output = compileAndRun(
        "x: str = \"hello\"\n"
        "print(isinstance(x, str))\n"
        "print(isinstance(x, int))\n"
    );
    EXPECT_EQ(output, "True\nFalse\n");
}

TEST(CodeGenE2E, IsinstanceBool) {
    auto output = compileAndRun(
        "x: bool = True\n"
        "print(isinstance(x, bool))\n"
    );
    EXPECT_EQ(output, "True\n");
}

TEST(CodeGenE2E, IsinstanceList) {
    auto output = compileAndRun(
        "x: list[int] = [1, 2, 3]\n"
        "print(isinstance(x, list))\n"
        "print(isinstance(x, dict))\n"
    );
    EXPECT_EQ(output, "True\nFalse\n");
}

TEST(CodeGenE2E, TypeInt) {
    auto output = compileAndRun(
        "x: int = 42\n"
        "print(type(x))\n"
    );
    EXPECT_EQ(output, "int\n");
}

TEST(CodeGenE2E, TypeStr) {
    auto output = compileAndRun(
        "x: str = \"hello\"\n"
        "print(type(x))\n"
    );
    EXPECT_EQ(output, "str\n");
}

TEST(CodeGenE2E, TypeFloat) {
    auto output = compileAndRun(
        "x: float = 3.14\n"
        "print(type(x))\n"
    );
    EXPECT_EQ(output, "float\n");
}

TEST(CodeGenE2E, TypeBool) {
    auto output = compileAndRun(
        "x: bool = True\n"
        "print(type(x))\n"
    );
    EXPECT_EQ(output, "bool\n");
}

//===----------------------------------------------------------------------===//
// Print E2E Tests
//===----------------------------------------------------------------------===//

TEST(CodeGenE2E, PrintListStr) {
    auto output = compileAndRun(
        "x: list[str] = [\"hello\", \"world\"]\n"
        "print(x)\n"
    );
    EXPECT_EQ(output, "['hello', 'world']\n");
}

TEST(CodeGenE2E, PrintListFloat) {
    auto output = compileAndRun(
        "x: list[float] = [1.5, 2.0, 3.14]\n"
        "print(x)\n"
    );
    EXPECT_EQ(output, "[1.5, 2.0, 3.14]\n");
}

TEST(CodeGenE2E, PrintListBool) {
    auto output = compileAndRun(
        "x: list[bool] = [True, False, True]\n"
        "print(x)\n"
    );
    EXPECT_EQ(output, "[True, False, True]\n");
}

TEST(CodeGenE2E, PrintList) {
    auto output = compileAndRun(
        "x: list[int] = [1, 2, 3]\n"
        "print(x)"
    );
    EXPECT_EQ(output, "[1, 2, 3]\n");
}

//===----------------------------------------------------------------------===//
// Builtins E2E Tests
//===----------------------------------------------------------------------===//

TEST(CodeGenE2E, MinMaxTwoArgs) {
    auto out = compileAndRun("print(min(3, 7))\nprint(max(3, 7))");
    EXPECT_EQ(out, "3\n7\n");
}

TEST(CodeGenE2E, MinMaxList) {
    auto out = compileAndRun(
        "xs: list[int] = [5, 2, 8, 1, 9]\n"
        "print(min(xs))\n"
        "print(max(xs))\n"
    );
    EXPECT_EQ(out, "1\n9\n");
}

TEST(CodeGenE2E, SumList) {
    auto out = compileAndRun(
        "xs: list[int] = [1, 2, 3, 4, 5]\n"
        "print(sum(xs))\n"
    );
    EXPECT_EQ(out, "15\n");
}

TEST(CodeGenE2E, AnyAllList) {
    auto out = compileAndRun(
        "xs: list[int] = [0, 0, 1]\n"
        "ys: list[int] = [1, 2, 3]\n"
        "zs: list[int] = [0, 0, 0]\n"
        "print(any(xs))\n"
        "print(all(ys))\n"
        "print(any(zs))\n"
        "print(all(xs))\n"
    );
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
    auto out = compileAndRun(
        "xs: list[int] = [3, 1, 4, 1, 5]\n"
        "ys: list[int] = sorted(xs)\n"
        "zs: list[int] = reversed(xs)\n"
        "print(ys[0])\nprint(ys[4])\n"
        "print(zs[0])\nprint(zs[4])\n"
    );
    EXPECT_EQ(out, "1\n5\n5\n3\n");
}

// File write/read E2E (open/make/push) is covered by the dogfooded unittest at
// test/dr/test_io_streams.dr.

//===----------------------------------------------------------------------===//
// Extern C E2E Tests
//===----------------------------------------------------------------------===//

TEST(CodeGenE2E, ExternCCallPuts) {
    // Call libc puts() via extern "C"
    auto out = compileAndRun(
        "extern \"C\" def puts(s: str) -> int\n"
        "puts(\"hello from C\")\n"
    );
    EXPECT_EQ(out, "hello from C\n");
}

TEST(CodeGenE2E, ExternCCallAbs) {
    // Call libc abs() via extern "C"
    auto out = compileAndRun(
        "extern \"C\" def abs(x: int) -> int\n"
        "x: int = abs(-42)\n"
        "print(x)\n"
    );
    EXPECT_EQ(out, "42\n");
}

//===----------------------------------------------------------------------===//
// hasattr() / getattr() - runtime attribute reflection
//===----------------------------------------------------------------------===//

TEST(CodeGenIR, HasattrIR) {
    auto ir = generateIR(
        "class Foo {\n"
        "  def(x: int) {\n"
        "    self.x = x\n"
        "  }\n"
        "}\n"
        "f: Foo = Foo(1)\n"
        "b: bool = hasattr(f, \"x\")\n"
    );
    EXPECT_NE(ir.find("dragon_hasattr"), std::string::npos);
}

TEST(CodeGenIR, GetattrIR) {
    auto ir = generateIR(
        "class Foo {\n"
        "  def(x: int) {\n"
        "    self.x = x\n"
        "  }\n"
        "}\n"
        "f: Foo = Foo(1)\n"
        "v: int = getattr(f, \"x\")\n"
    );
    EXPECT_NE(ir.find("dragon_getattr"), std::string::npos);
}

TEST(CodeGenE2E, HasattrTrue) {
    auto out = compileAndRun(
        "class Point {\n"
        "  def(x: int, y: int) {\n"
        "    self.x = x\n"
        "    self.y = y\n"
        "  }\n"
        "}\n"
        "p: Point = Point(1, 2)\n"
        "if hasattr(p, \"x\") {\n"
        "  print(\"yes\")\n"
        "}\n"
    );
    EXPECT_EQ(out, "yes\n");
}

TEST(CodeGenE2E, HasattrFalse) {
    auto out = compileAndRun(
        "class Point {\n"
        "  def(x: int, y: int) {\n"
        "    self.x = x\n"
        "    self.y = y\n"
        "  }\n"
        "}\n"
        "p: Point = Point(1, 2)\n"
        "if hasattr(p, \"z\") {\n"
        "  print(\"yes\")\n"
        "} else {\n"
        "  print(\"no\")\n"
        "}\n"
    );
    EXPECT_EQ(out, "no\n");
}

TEST(CodeGenE2E, GetattrField) {
    auto out = compileAndRun(
        "class Dog {\n"
        "  def(name: str) {\n"
        "    self.name = name\n"
        "  }\n"
        "}\n"
        "d: Dog = Dog(\"Rex\")\n"
        "n: str = getattr(d, \"name\")\n"
        "print(n)\n"
    );
    EXPECT_EQ(out, "Rex\n");
}

TEST(CodeGenE2E, GetattrInt) {
    auto out = compileAndRun(
        "class Box {\n"
        "  def(val: int) {\n"
        "    self.val = val\n"
        "  }\n"
        "}\n"
        "b: Box = Box(42)\n"
        "v: int = getattr(b, \"val\")\n"
        "print(v)\n"
    );
    EXPECT_EQ(out, "42\n");
}

TEST(CodeGenE2E, GetattrDefault) {
    auto out = compileAndRun(
        "class Cfg {\n"
        "  def(port: int) {\n"
        "    self.port = port\n"
        "  }\n"
        "}\n"
        "c: Cfg = Cfg(8080)\n"
        "v: int = getattr(c, \"missing\", 9999)\n"
        "print(v)\n"
    );
    EXPECT_EQ(out, "9999\n");
}

TEST(CodeGenE2E, HasattrInherited) {
    auto out = compileAndRun(
        "class Base {\n"
        "  def(x: int) {\n"
        "    self.x = x\n"
        "  }\n"
        "}\n"
        "class Child(Base) {\n"
        "  def(x: int, y: int) {\n"
        "    self.x = x\n"
        "    self.y = y\n"
        "  }\n"
        "}\n"
        "c: Child = Child(1, 2)\n"
        "if hasattr(c, \"x\") {\n"
        "  print(\"has x\")\n"
        "}\n"
        "if hasattr(c, \"y\") {\n"
        "  print(\"has y\")\n"
        "}\n"
    );
    EXPECT_EQ(out, "has x\nhas y\n");
}
