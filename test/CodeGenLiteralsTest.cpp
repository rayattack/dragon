#include "CodeGenTestHelpers.h"

//===----------------------------------------------------------------------===//
// Literals IR Tests
//===----------------------------------------------------------------------===//

TEST(CodeGenTest, IntegerLiteral) {
    // Use print() so the constant appears in IR as a call argument
    auto ir = generateIR("print(42)");
    EXPECT_NE(ir.find("42"), std::string::npos);
    EXPECT_NE(ir.find("dragon_print_int"), std::string::npos);
}

TEST(CodeGenTest, FloatLiteral) {
    auto ir = generateIR("3.14");
    EXPECT_NE(ir.find("double"), std::string::npos);
}

TEST(CodeGenTest, StringLiteral) {
    auto ir = generateIR("\"hello\"");
    EXPECT_NE(ir.find("hello"), std::string::npos);
}

TEST(CodeGenTest, BoolLiteral) {
    // Bare True is a valid ExprStmt; constant-folded away but IR is valid
    auto ir = generateIR("True");
    EXPECT_NE(ir.find("define i32 @main("), std::string::npos);
    EXPECT_EQ(ir.find("<codegen failed"), std::string::npos);
}

//===----------------------------------------------------------------------===//
// String IR Tests
//===----------------------------------------------------------------------===//

TEST(CodeGenTest, StringIndex) {
    auto ir = generateIR("s: str = \"hello\"\nprint(s[0])");
    EXPECT_NE(ir.find("dragon_str_index"), std::string::npos);
}

TEST(CodeGenTest, StringSlice) {
    auto ir = generateIR("s: str = \"hello world\"\nt: str = s[0:5]");
    EXPECT_NE(ir.find("dragon_str_slice"), std::string::npos);
}

TEST(CodeGenTest, StringUpper) {
    auto ir = generateIR("s: str = \"hello\"\nt: str = s.upper()");
    EXPECT_NE(ir.find("dragon_str_upper"), std::string::npos);
}

TEST(CodeGenTest, StringFind) {
    auto ir = generateIR("s: str = \"hello world\"\nx: int = s.find(\"world\")");
    EXPECT_NE(ir.find("dragon_str_find"), std::string::npos);
}

TEST(CodeGenTest, StringReplace) {
    auto ir = generateIR("s: str = \"hello\"\nt: str = s.replace(\"l\", \"r\")");
    EXPECT_NE(ir.find("dragon_str_replace"), std::string::npos);
}

TEST(CodeGenTest, StringStartswith) {
    auto ir = generateIR("s: str = \"hello\"\nx: int = s.startswith(\"he\")");
    EXPECT_NE(ir.find("dragon_str_startswith"), std::string::npos);
}

TEST(CodeGenTest, StringIsDigit) {
    auto ir = generateIR("s: str = \"123\"\nx: int = s.isdigit()");
    EXPECT_NE(ir.find("dragon_str_isdigit"), std::string::npos);
}

TEST(CodeGenIR, StringCmpDeclared) {
    // dragon_str_cmp should be declared in IR when string ordering is used
    auto ir = generateIR(
        "a: str = \"x\"\n"
        "b: str = \"y\"\n"
        "c: bool = a < b\n"
    );
    EXPECT_NE(ir.find("dragon_str_cmp"), std::string::npos)
        << "Expected dragon_str_cmp call for string < comparison\nIR:\n" << ir;
}

//===----------------------------------------------------------------------===//
// F-String IR Tests
//===----------------------------------------------------------------------===//

TEST(CodeGenTest, FStringSimple) {
    auto ir = generateIR("x: int = 42\nprint(f\"value is {x}\")");
    EXPECT_NE(ir.find("dragon_int_to_str"), std::string::npos);
    EXPECT_NE(ir.find("dragon_str_concat"), std::string::npos);
}

TEST(CodeGenTest, FStringFormatSpecIR) {
    auto ir = generateIR(
        "x: float = 1.0\n"
        "s: str = f\"{x:.2f}\"\n"
    );
    EXPECT_NE(ir.find("dragon_float_format"), std::string::npos);
}

//===----------------------------------------------------------------------===//
// F-String / String RC IR Tests
//===----------------------------------------------------------------------===//

TEST(CodeGenIR, FStringIntermediateDecref) {
    // F-string with 3+ parts should emit decref for concat intermediates
    auto ir = generateIR(
        "x: int = 1\n"
        "y: int = 2\n"
        "s: str = f\"a {x} b {y} c\"\n"
    );
    // Should contain dragon_decref_str calls for intermediates
    auto count = 0;
    std::string::size_type pos = 0;
    while ((pos = ir.find("dragon_decref_str", pos)) != std::string::npos) {
        count++;
        pos += 17;
    }
    // Expect at least some decref_str calls (intermediates + conversion results)
    EXPECT_GE(count, 2)
        << "Expected decref_str for f-string intermediates\nIR:\n" << ir;
}

TEST(CodeGenIR, BinaryStringConcatChainDecref) {
    // a + b + c should decref the intermediate concat(a,b) result
    auto ir = generateIR(
        "a: str = \"x\"\n"
        "b: str = \"y\"\n"
        "c: str = \"z\"\n"
        "s: str = a + b + c\n"
    );
    // The IR should contain dragon_decref_str to clean up the a+b intermediate
    EXPECT_NE(ir.find("dragon_decref_str"), std::string::npos)
        << "Expected decref_str for concat chain intermediate\nIR:\n" << ir;
}

//===----------------------------------------------------------------------===//
// F-String E2E Tests
//===----------------------------------------------------------------------===//

TEST(CodeGenE2E, FStringArithmetic) {
    auto output = compileAndRun(
        "x: int = 3\n"
        "y: int = 4\n"
        "print(f\"{x + y}\")"
    );
    EXPECT_EQ(output, "7\n");
}

TEST(CodeGenE2E, FStringFunctionCall) {
    auto output = compileAndRun(
        "nums: list[int] = [1, 2, 3]\n"
        "print(f\"len={len(nums)}\")"
    );
    EXPECT_EQ(output, "len=3\n");
}

TEST(CodeGenE2E, FStringMultipleExprs) {
    auto output = compileAndRun(
        "a: int = 10\n"
        "b: int = 20\n"
        "print(f\"{a} + {b} = {a + b}\")"
    );
    EXPECT_EQ(output, "10 + 20 = 30\n");
}

TEST(CodeGenE2E, FStringFloatFormat) {
    auto output = compileAndRun(
        "x: float = 3.14159\n"
        "print(f\"{x:.2f}\")\n"
    );
    EXPECT_EQ(output, "3.14\n");
}

TEST(CodeGenE2E, FStringFloatFormat3) {
    auto output = compileAndRun(
        "pi: float = 3.14159265\n"
        "print(f\"{pi:.4f}\")\n"
    );
    EXPECT_EQ(output, "3.1416\n");
}

TEST(CodeGenE2E, FStringIntHex) {
    auto output = compileAndRun(
        "x: int = 255\n"
        "print(f\"{x:x}\")\n"
    );
    EXPECT_EQ(output, "ff\n");
}

TEST(CodeGenE2E, FStringIntHexUpper) {
    auto output = compileAndRun(
        "x: int = 255\n"
        "print(f\"{x:X}\")\n"
    );
    EXPECT_EQ(output, "FF\n");
}

TEST(CodeGenE2E, FStringIntOctal) {
    auto output = compileAndRun(
        "x: int = 8\n"
        "print(f\"{x:o}\")\n"
    );
    EXPECT_EQ(output, "10\n");
}

TEST(CodeGenE2E, FStringIntBinary) {
    auto output = compileAndRun(
        "x: int = 10\n"
        "print(f\"{x:b}\")\n"
    );
    EXPECT_EQ(output, "1010\n");
}

TEST(CodeGenE2E, FStringIntZeroPad) {
    auto output = compileAndRun(
        "x: int = 42\n"
        "print(f\"{x:05d}\")\n"
    );
    EXPECT_EQ(output, "00042\n");
}

TEST(CodeGenE2E, FStringMixed) {
    auto output = compileAndRun(
        "name: str = \"pi\"\n"
        "val: float = 3.14159\n"
        "print(f\"{name} = {val:.2f}\")\n"
    );
    EXPECT_EQ(output, "pi = 3.14\n");
}

TEST(CodeGenE2E, FStringMultiInterpolation) {
    // F-string with many interpolations should work correctly
    auto output = compileAndRun(
        "x: int = 1\n"
        "y: int = 2\n"
        "z: int = 3\n"
        "print(f\"{x} + {y} = {z}\")\n"
    );
    EXPECT_EQ(output, "1 + 2 = 3\n");
}

//===----------------------------------------------------------------------===//
// Format-spec validator tests (security)
//===----------------------------------------------------------------------===//
// Unvalidated, user-controlled format specs flow directly into snprintf,
// allowing %n stack writes and %s stack reads, plus a strncpy overflow on
// the 32-byte prefix buffer. The validator rejects any non-grammar input
// with ValueError; long specs are bounds-clamped as defense-in-depth.

TEST(CodeGenE2E, FStringRejectsPercentN) {
    auto output = compileAndRun(
        "x: int = 42\n"
        "try {\n"
        "  print(f\"{x:%n}\")\n"
        "} except ValueError {\n"
        "  print(\"caught\")\n"
        "}\n"
    );
    EXPECT_EQ(output, "caught\n");
}

TEST(CodeGenE2E, FStringRejectsPercentS) {
    auto output = compileAndRun(
        "x: int = 42\n"
        "try {\n"
        "  print(f\"{x:%s}\")\n"
        "} except ValueError {\n"
        "  print(\"caught\")\n"
        "}\n"
    );
    EXPECT_EQ(output, "caught\n");
}

TEST(CodeGenE2E, FStringLongAllZeroSpec) {
    // A 100-char all-zeros spec must not crash. It is a VALID Python spec
    // (leading 0 => zero-fill, the remaining zeros => width 0), so Python and
    // Dragon both render the bare value "42" - not a ValueError. (An absurd
    // *non-zero* width like 999999999 is still rejected; see
    // FStringRejectsHugeWidth.)
    auto output = compileAndRun(
        "x: int = 42\n"
        "print(f\"{x:00000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000d}\")\n"
    );
    EXPECT_EQ(output, "42\n");
}

TEST(CodeGenE2E, FStringRejectsHugeWidth) {
    // Width beyond the 1,000,000 cap raises ValueError rather than attempting a
    // gigantic allocation.
    auto output = compileAndRun(
        "x: int = 42\n"
        "try {\n"
        "  print(f\"{x:999999999d}\")\n"
        "} except ValueError {\n"
        "  print(\"caught\")\n"
        "}\n"
    );
    EXPECT_EQ(output, "caught\n");
}

TEST(CodeGenE2E, FStringRejectsFloatPercentN) {
    auto output = compileAndRun(
        "v: float = 3.14\n"
        "try {\n"
        "  print(f\"{v:%n}\")\n"
        "} except ValueError {\n"
        "  print(\"caught\")\n"
        "}\n"
    );
    EXPECT_EQ(output, "caught\n");
}

TEST(CodeGenE2E, FStringValidSpecsStillWork) {
    // Regression: validator must accept all previously-working specs.
    auto output = compileAndRun(
        "v: float = 3.14159\n"
        "x: int = 42\n"
        "print(f\"{v:.2f}\")\n"
        "print(f\"{x:x}\")\n"
        "print(f\"{x:X}\")\n"
        "print(f\"{x:o}\")\n"
        "print(f\"{x:b}\")\n"
        "print(f\"{x:05d}\")\n"
    );
    EXPECT_EQ(output, "3.14\n2a\n2A\n52\n101010\n00042\n");
}

//===----------------------------------------------------------------------===//
// String E2E Tests
//===----------------------------------------------------------------------===//

TEST(CodeGenE2E, StringOperations) {
    auto output = compileAndRun(
        "s: str = \"hello\"\n"
        "print(len(s))\n"
        "t: str = s + \" world\"\n"
        "print(t)"
    );
    EXPECT_EQ(output, "5\nhello world\n");
}

TEST(CodeGenE2E, StringIndexing) {
    auto output = compileAndRun(
        "s: str = \"Dragon\"\n"
        "print(s[0])\n"
        "print(s[-1])"
    );
    EXPECT_EQ(output, "D\nn\n");
}

TEST(CodeGenE2E, StringMethods) {
    auto output = compileAndRun(
        "s: str = \"hello\"\n"
        "print(s.upper())\n"
        "print(s.find(\"ll\"))"
    );
    EXPECT_EQ(output, "HELLO\n2\n");
}

TEST(CodeGenE2E, StringSlice) {
    auto output = compileAndRun(
        "s: str = \"hello world\"\n"
        "print(s[0:5])\n"
        "print(s[6:11])"
    );
    EXPECT_EQ(output, "hello\nworld\n");
}

TEST(CodeGenE2E, ExprStmtStringMethodNoLeak) {
    // Expression-as-statement returning a string should not crash
    // (tests that the decref on discarded result is correct)
    auto output = compileAndRun(
        "s: str = \"hello\"\n"
        "s.upper()\n"
        "print(s)\n"
    );
    EXPECT_EQ(output, "hello\n");
}

TEST(CodeGenE2E, StringConcatChainE2E) {
    // Chained string concat should produce correct result
    auto output = compileAndRun(
        "a: str = \"hello\"\n"
        "b: str = \" \"\n"
        "c: str = \"world\"\n"
        "print(a + b + c)\n"
    );
    EXPECT_EQ(output, "hello world\n");
}

TEST(CodeGenE2E, StringOrdering) {
    // Bug 3: string ordering comparisons (<, >, <=, >=) should work
    auto output = compileAndRun(
        "print(\"apple\" < \"banana\")\n"
        "print(\"cat\" > \"bat\")\n"
        "print(\"abc\" <= \"abc\")\n"
        "print(\"xyz\" >= \"xyz\")\n"
    );
    EXPECT_EQ(output, "True\nTrue\nTrue\nTrue\n");
}

TEST(CodeGenE2E, StringOrderingVariables) {
    // String ordering with variables (like fnmatch character ranges)
    auto output = compileAndRun(
        "ch: str = \"d\"\n"
        "lo: str = \"a\"\n"
        "hi: str = \"z\"\n"
        "if ch >= lo {\n"
        "  if ch <= hi {\n"
        "    print(\"in range\")\n"
        "  }\n"
        "}\n"
    );
    EXPECT_EQ(output, "in range\n");
}

TEST(CodeGenE2E, ListStrSubscript) {
    // Bug 2: list[str] subscript should return a string, not a raw pointer
    auto output = compileAndRun(
        "names: list[str] = [\"alice\", \"bob\", \"charlie\"]\n"
        "print(names[0])\n"
        "print(names[1])\n"
    );
    EXPECT_EQ(output, "alice\nbob\n");
}

TEST(CodeGenE2E, ListFloatSubscript) {
    // Bug 4: list[float] subscript should return a float, not garbage
    // Use values that produce a non-integer sum to verify float unboxing
    auto output = compileAndRun(
        "vals: list[float] = [1.5, 2.3, 3.5]\n"
        "x: float = vals[0]\n"
        "y: float = vals[1]\n"
        "print(x + y)\n"
    );
    EXPECT_EQ(output, "3.8\n");
}

//===----------------------------------------------------------------------===//
// Bytes IR Tests
//===----------------------------------------------------------------------===//

TEST(CodeGenTest, BytesLiteralIR) {
    auto ir = generateIR("b: bytes = b\"hello\"\n");
    EXPECT_NE(ir.find("dragon_bytes_from_literal"), std::string::npos);
}

TEST(CodeGenTest, BytesConcatIR) {
    auto ir = generateIR(
        "a: bytes = b\"hello\"\n"
        "b: bytes = b\" world\"\n"
        "c: bytes = a + b\n"
    );
    EXPECT_NE(ir.find("dragon_bytes_concat"), std::string::npos);
}

TEST(CodeGenTest, BytesLenIR) {
    auto ir = generateIR(
        "b: bytes = b\"hello\"\n"
        "print(len(b))\n"
    );
    EXPECT_NE(ir.find("dragon_bytes_len"), std::string::npos);
}

TEST(CodeGenTest, BytesDecodeIR) {
    auto ir = generateIR(
        "b: bytes = b\"hello\"\n"
        "s: str = b.decode()\n"
    );
    EXPECT_NE(ir.find("dragon_bytes_decode"), std::string::npos);
}

TEST(CodeGenTest, StrEncodeIR) {
    auto ir = generateIR(
        "s: str = \"hello\"\n"
        "b: bytes = s.encode()\n"
    );
    EXPECT_NE(ir.find("dragon_str_encode"), std::string::npos);
}

//===----------------------------------------------------------------------===//
// Bytes E2E Tests
//===----------------------------------------------------------------------===//

TEST(CodeGenE2E, BytesLiteralPrint) {
    auto output = compileAndRun("print(b\"hello\")\n");
    EXPECT_EQ(output, "b'hello'\n");
}

TEST(CodeGenE2E, BytesLen) {
    auto output = compileAndRun(
        "b: bytes = b\"hello\"\n"
        "print(len(b))\n"
    );
    EXPECT_EQ(output, "5\n");
}

TEST(CodeGenE2E, BytesIndex) {
    auto output = compileAndRun(
        "b: bytes = b\"hello\"\n"
        "print(b[0])\n"
    );
    EXPECT_EQ(output, "104\n");
}

TEST(CodeGenE2E, BytesNegIndex) {
    auto output = compileAndRun(
        "b: bytes = b\"hello\"\n"
        "print(b[-1])\n"
    );
    EXPECT_EQ(output, "111\n");
}

TEST(CodeGenE2E, BytesSlice) {
    auto output = compileAndRun(
        "b: bytes = b\"hello world\"\n"
        "print(b[0:5])\n"
    );
    EXPECT_EQ(output, "b'hello'\n");
}

TEST(CodeGenE2E, BytesConcat) {
    auto output = compileAndRun(
        "a: bytes = b\"hello\"\n"
        "b: bytes = b\" world\"\n"
        "print(a + b)\n"
    );
    EXPECT_EQ(output, "b'hello world'\n");
}

TEST(CodeGenE2E, BytesRepeat) {
    auto output = compileAndRun(
        "b: bytes = b\"ab\"\n"
        "print(b * 3)\n"
    );
    EXPECT_EQ(output, "b'ababab'\n");
}

TEST(CodeGenE2E, BytesEq) {
    auto output = compileAndRun(
        "a: bytes = b\"abc\"\n"
        "b: bytes = b\"abc\"\n"
        "if a == b {\n"
        "    print(\"True\")\n"
        "} else {\n"
        "    print(\"False\")\n"
        "}\n"
    );
    EXPECT_EQ(output, "True\n");
}

TEST(CodeGenE2E, BytesNeq) {
    auto output = compileAndRun(
        "a: bytes = b\"abc\"\n"
        "b: bytes = b\"def\"\n"
        "if a != b {\n"
        "    print(\"True\")\n"
        "} else {\n"
        "    print(\"False\")\n"
        "}\n"
    );
    EXPECT_EQ(output, "True\n");
}

TEST(CodeGenE2E, BytesLt) {
    auto output = compileAndRun(
        "a: bytes = b\"abc\"\n"
        "b: bytes = b\"abd\"\n"
        "if a < b {\n"
        "    print(\"True\")\n"
        "} else {\n"
        "    print(\"False\")\n"
        "}\n"
    );
    EXPECT_EQ(output, "True\n");
}

TEST(CodeGenE2E, BytesContainsInt) {
    auto output = compileAndRun(
        "b: bytes = b\"hello\"\n"
        "if 104 in b {\n"
        "    print(\"True\")\n"
        "} else {\n"
        "    print(\"False\")\n"
        "}\n"
    );
    EXPECT_EQ(output, "True\n");
}

TEST(CodeGenE2E, BytesDecode) {
    auto output = compileAndRun(
        "b: bytes = b\"hello\"\n"
        "s: str = b.decode()\n"
        "print(s)\n"
    );
    EXPECT_EQ(output, "hello\n");
}

TEST(CodeGenE2E, StrEncode) {
    auto output = compileAndRun(
        "s: str = \"hello\"\n"
        "b: bytes = s.encode()\n"
        "print(b)\n"
    );
    EXPECT_EQ(output, "b'hello'\n");
}

TEST(CodeGenE2E, BytesHex) {
    auto output = compileAndRun(
        "b: bytes = b\"hello\"\n"
        "print(b.hex())\n"
    );
    EXPECT_EQ(output, "68656c6c6f\n");
}

TEST(CodeGenE2E, BytesFromhex) {
    auto output = compileAndRun(
        "b: bytes = bytes.fromhex(\"68656c6c6f\")\n"
        "print(b)\n"
    );
    EXPECT_EQ(output, "b'hello'\n");
}

TEST(CodeGenE2E, BytesFind) {
    auto output = compileAndRun(
        "b: bytes = b\"hello\"\n"
        "print(b.find(b\"ll\"))\n"
    );
    EXPECT_EQ(output, "2\n");
}

TEST(CodeGenE2E, BytesRfind) {
    auto output = compileAndRun(
        "b: bytes = b\"hello hello\"\n"
        "print(b.rfind(b\"hello\"))\n"
    );
    EXPECT_EQ(output, "6\n");
}

TEST(CodeGenE2E, BytesCount) {
    auto output = compileAndRun(
        "b: bytes = b\"abcabc\"\n"
        "print(b.count(b\"abc\"))\n"
    );
    EXPECT_EQ(output, "2\n");
}

TEST(CodeGenE2E, BytesReplace) {
    auto output = compileAndRun(
        "b: bytes = b\"hello\"\n"
        "print(b.replace(b\"l\", b\"r\"))\n"
    );
    EXPECT_EQ(output, "b'herro'\n");
}

TEST(CodeGenE2E, BytesStartswith) {
    auto output = compileAndRun(
        "b: bytes = b\"hello\"\n"
        "print(b.startswith(b\"hel\"))\n"
    );
    EXPECT_EQ(output, "True\n");
}

TEST(CodeGenE2E, BytesUpper) {
    auto output = compileAndRun(
        "b: bytes = b\"hello\"\n"
        "print(b.upper())\n"
    );
    EXPECT_EQ(output, "b'HELLO'\n");
}

TEST(CodeGenE2E, BytesLower) {
    auto output = compileAndRun(
        "b: bytes = b\"HELLO\"\n"
        "print(b.lower())\n"
    );
    EXPECT_EQ(output, "b'hello'\n");
}

TEST(CodeGenE2E, BytesStrip) {
    auto output = compileAndRun(
        "b: bytes = b\" hello \"\n"
        "print(b.strip())\n"
    );
    EXPECT_EQ(output, "b'hello'\n");
}

TEST(CodeGenE2E, BytesSplit) {
    auto output = compileAndRun(
        "parts: list = b\"a,b,c\".split(b\",\")\n"
        "print(len(parts))\n"
    );
    EXPECT_EQ(output, "3\n");
}

TEST(CodeGenE2E, BytesIsDigit) {
    auto output = compileAndRun(
        "print(b\"123\".isdigit())\n"
    );
    EXPECT_EQ(output, "True\n");
}

//===----------------------------------------------------------------------===//
// Regression: string concat intermediate leaks
//
// Verifies that intermediate strings from runtime calls (str.upper(),
// str.lower(), str.replace(), str() coercion, slice) are decref'd when
// consumed by an outer concat expression, instead of leaking on every
// iteration.
//
// To run under valgrind:
//  valgrind --leak-check=full ./dragon_codegen_tests \
//  --gtest_filter='CodeGenE2E.ConcatIntermediate*'
// Bounded heap usage = success.
//===----------------------------------------------------------------------===//

TEST(CodeGenE2E, ConcatIntermediateUpperLowerLoop) {
    // s.upper() + s.lower() in a loop - both sides are owned intermediates.
    auto output = compileAndRun(
        "s: str = \"AbCdE\"\n"
        "last: str = \"\"\n"
        "for i in range(10000) {\n"
        "  last = s.upper() + s.lower()\n"
        "}\n"
        "print(last)\n"
    );
    EXPECT_EQ(output, "ABCDEabcde\n");
}

TEST(CodeGenE2E, ConcatIntermediateStrCoercionLoop) {
    // str(x) + str(y) in a loop - both sides go through dragon_int_to_str.
    auto output = compileAndRun(
        "last: str = \"\"\n"
        "for i in range(10000) {\n"
        "  last = str(i) + str(i + 1)\n"
        "}\n"
        "print(last)\n"
    );
    EXPECT_EQ(output, "999910000\n");
}

TEST(CodeGenE2E, ConcatIntermediateSliceLoop) {
    // s[0:3] + s[3:6] in a loop - both sides are dragon_str_slice results.
    auto output = compileAndRun(
        "s: str = \"abcdef\"\n"
        "last: str = \"\"\n"
        "for i in range(10000) {\n"
        "  last = s[0:3] + s[3:6]\n"
        "}\n"
        "print(last)\n"
    );
    EXPECT_EQ(output, "abcdef\n");
}

TEST(CodeGenE2E, ConcatIntermediateTriple) {
    // Triple concat: a.upper() + b.lower() + c.replace("x", "y")
    // The first concat result is itself a CallInst returning i8* and must be
    // decref'd by the outer concat. All three method-call intermediates must
    // also be cleaned up.
    auto output = compileAndRun(
        "a: str = \"AAA\"\n"
        "b: str = \"BBB\"\n"
        "c: str = \"xCx\"\n"
        "last: str = \"\"\n"
        "for i in range(10000) {\n"
        "  last = a.upper() + b.lower() + c.replace(\"x\", \"y\")\n"
        "}\n"
        "print(last)\n"
    );
    EXPECT_EQ(output, "AAAbbbyCy\n");
}

TEST(CodeGenE2E, ConcatIntermediateLiteralPlusStrPlusLiteral) {
    // "prefix-" + str(n) + "-suffix" - literals must NOT be decref'd
    // (they're immortal/global), but str(n) is an intermediate.
    auto output = compileAndRun(
        "last: str = \"\"\n"
        "for i in range(10000) {\n"
        "  last = \"prefix-\" + str(i) + \"-suffix\"\n"
        "}\n"
        "print(last)\n"
    );
    EXPECT_EQ(output, "prefix-9999-suffix\n");
}

//===----------------------------------------------------------------------===//
// IR-level checks: confirm decref_str is emitted for intermediates that
// would otherwise leak.
//===----------------------------------------------------------------------===//

TEST(CodeGenIR, ConcatBroadDecrefUpperLower) {
    // s.upper() + s.lower() - must decref BOTH intermediates (not just
    // dragon_str_concat results). Pre-fix, neither side was decref'd.
    auto ir = generateIR(
        "s: str = \"AbCdE\"\n"
        "r: str = s.upper() + s.lower()\n"
    );
    EXPECT_NE(ir.find("dragon_str_upper"), std::string::npos);
    EXPECT_NE(ir.find("dragon_str_lower"), std::string::npos);
    EXPECT_NE(ir.find("dragon_str_concat"), std::string::npos);
    // Count decref_str calls - expect at least 2 for the two intermediates
    // (the outer assignment may emit more for the result).
    auto count = 0;
    std::string::size_type pos = 0;
    while ((pos = ir.find("dragon_decref_str", pos)) != std::string::npos) {
        count++;
        pos += 17;
    }
    EXPECT_GE(count, 2)
        << "Expected at least 2 decref_str calls for upper/lower intermediates\n"
        << "IR:\n" << ir;
}

TEST(CodeGenIR, ConcatBroadDecrefIntToStr) {
    // str(x) + str(y) - both dragon_int_to_str results are owned intermediates.
    auto ir = generateIR(
        "x: int = 1\n"
        "y: int = 2\n"
        "r: str = str(x) + str(y)\n"
    );
    EXPECT_NE(ir.find("dragon_int_to_str"), std::string::npos);
    EXPECT_NE(ir.find("dragon_str_concat"), std::string::npos);
    auto count = 0;
    std::string::size_type pos = 0;
    while ((pos = ir.find("dragon_decref_str", pos)) != std::string::npos) {
        count++;
        pos += 17;
    }
    EXPECT_GE(count, 2)
        << "Expected at least 2 decref_str calls for int_to_str intermediates\n"
        << "IR:\n" << ir;
}

//===----------------------------------------------------------------------===//
// Regression: str.join NULL element guard
//
// Pre-fix: dragon_str_join called strlen() on each element with no NULL guard,
// crashing if any list slot was NULL. The fix skips NULL elements and only
// reads strlen on non-NULL.
//
// We can't construct a NULL-string list directly from .dr code (empty strings
// are non-NULL DragonString*), so these tests pin down the surrounding happy
// paths to lock in the behavior the fix was supposed to preserve.
//===----------------------------------------------------------------------===//

TEST(CodeGenE2E, StrJoinEmptyList) {
    auto output = compileAndRun(
        "xs: list[str] = []\n"
        "r: str = \", \".join(xs)\n"
        "print(\"[\" + r + \"]\")\n"
    );
    EXPECT_EQ(output, "[]\n");
}

TEST(CodeGenE2E, StrJoinSingleElement) {
    auto output = compileAndRun(
        "xs: list[str] = [\"only\"]\n"
        "r: str = \", \".join(xs)\n"
        "print(r)\n"
    );
    EXPECT_EQ(output, "only\n");
}

TEST(CodeGenE2E, StrJoinAllEmptyStrings) {
    // Empty strings are valid (not NULL); separator must still appear between them.
    auto output = compileAndRun(
        "xs: list[str] = [\"\", \"\", \"\"]\n"
        "r: str = \"|\".join(xs)\n"
        "print(\"[\" + r + \"]\")\n"
    );
    EXPECT_EQ(output, "[||]\n");
}

TEST(CodeGenE2E, StrJoinMixedEmpty) {
    auto output = compileAndRun(
        "xs: list[str] = [\"a\", \"\", \"b\", \"\", \"c\"]\n"
        "r: str = \",\".join(xs)\n"
        "print(r)\n"
    );
    EXPECT_EQ(output, "a,,b,,c\n");
}

TEST(CodeGenE2E, StrJoinEmptySeparator) {
    auto output = compileAndRun(
        "xs: list[str] = [\"foo\", \"bar\", \"baz\"]\n"
        "r: str = \"\".join(xs)\n"
        "print(r)\n"
    );
    EXPECT_EQ(output, "foobarbaz\n");
}

TEST(CodeGenE2E, StrJoinLoopBounded) {
    // Tight loop - verifies the NULL guard's `continue` path is well-formed
    // (early-return was the alternative; `continue` keeps the separator slot
    // count consistent so we don't get crashes or extra-separator artifacts).
    auto output = compileAndRun(
        "xs: list[str] = [\"alpha\", \"\", \"beta\"]\n"
        "last: str = \"\"\n"
        "for i in range(5000) {\n"
        "  last = \"-\".join(xs)\n"
        "}\n"
        "print(last)\n"
    );
    EXPECT_EQ(output, "alpha--beta\n");
}

//===----------------------------------------------------------------------===//
// Regression: str.replace signed arithmetic
//
// Pre-fix: `slen + count * (nlen - olen)` used unsigned size_t arithmetic.
// When replacement is shorter than the search string, `nlen - olen` wraps
// negative as a huge unsigned value. The result was accidentally correct
// via the unsigned-wrap invariant but is fragile. The fix casts to int64_t
// before subtraction.
//
// These tests exercise the three regimes: shrink (nlen < olen),
// expand (nlen > olen), equal-length, and zero-match.
//===----------------------------------------------------------------------===//

TEST(CodeGenE2E, StrReplaceShrink) {
    // Replacement shorter than search -> result smaller than input.
    auto output = compileAndRun(
        "s: str = \"xxxxxxxx\"\n"
        "r: str = s.replace(\"xx\", \"y\")\n"
        "print(r)\n"
    );
    EXPECT_EQ(output, "yyyy\n");
}

TEST(CodeGenE2E, StrReplaceExpand) {
    auto output = compileAndRun(
        "s: str = \"abcabc\"\n"
        "r: str = s.replace(\"a\", \"AAA\")\n"
        "print(r)\n"
    );
    EXPECT_EQ(output, "AAAbcAAAbc\n");
}

TEST(CodeGenE2E, StrReplaceEqualLength) {
    auto output = compileAndRun(
        "s: str = \"hello world\"\n"
        "r: str = s.replace(\"o\", \"0\")\n"
        "print(r)\n"
    );
    EXPECT_EQ(output, "hell0 w0rld\n");
}

TEST(CodeGenE2E, StrReplaceNoMatch) {
    auto output = compileAndRun(
        "s: str = \"hello\"\n"
        "r: str = s.replace(\"z\", \"Q\")\n"
        "print(r)\n"
    );
    EXPECT_EQ(output, "hello\n");
}

TEST(CodeGenE2E, StrReplaceShrinkToEmpty) {
    // Aggressive shrink: replacement empty -> exercises (0 - olen) signed math.
    auto output = compileAndRun(
        "s: str = \"a-b-c-d\"\n"
        "r: str = s.replace(\"-\", \"\")\n"
        "print(r)\n"
    );
    EXPECT_EQ(output, "abcd\n");
}

TEST(CodeGenE2E, StrReplaceFullString) {
    auto output = compileAndRun(
        "s: str = \"foobar\"\n"
        "r: str = s.replace(\"foobar\", \"\")\n"
        "print(\"[\" + r + \"]\")\n"
    );
    EXPECT_EQ(output, "[]\n");
}

//===----------------------------------------------------------------------===//
// 4.7 PEP 393-lite Unicode strings - code-point semantics
//===----------------------------------------------------------------------===//

TEST(CodeGenE2E, Utf8LenIsCodePointCount) {
    // Python 3 semantics: len() returns code-point count, not byte count.
    auto output = compileAndRun(
        "a: str = \"hello\"\n"
        "b: str = \"caf\xc3\xa9\"\n"
        "c: str = \"h\xc3\xa9llo w\xc3\xb6rld\"\n"
        "d: str = \"\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e\"\n"
        "print(len(a))\n"
        "print(len(b))\n"
        "print(len(c))\n"
        "print(len(d))\n"
    );
    EXPECT_EQ(output, "5\n4\n11\n3\n");
}

TEST(CodeGenE2E, Utf8IndexingByCodePoint) {
    auto output = compileAndRun(
        "s: str = \"caf\xc3\xa9\"\n"
        "print(s[0])\n"
        "print(s[1])\n"
        "print(s[2])\n"
        "print(s[3])\n"
        "print(s[-1])\n"
    );
    EXPECT_EQ(output, "c\na\nf\n\xc3\xa9\n\xc3\xa9\n");
}

TEST(CodeGenE2E, Utf8IndexingMultiByteOnlyString) {
    auto output = compileAndRun(
        "s: str = \"\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e\"\n"
        "print(s[0])\n"
        "print(s[1])\n"
        "print(s[2])\n"
    );
    EXPECT_EQ(output, "\xe6\x97\xa5\n\xe6\x9c\xac\n\xe8\xaa\x9e\n");
}

TEST(CodeGenE2E, Utf8SlicePreservesValidEncoding) {
    auto output = compileAndRun(
        "s: str = \"h\xc3\xa9llo w\xc3\xb6rld\"\n"
        "print(s[0:5])\n"
        "print(s[6:11])\n"
        "print(s[1:3])\n"
    );
    EXPECT_EQ(output, "h\xc3\xa9llo\nw\xc3\xb6rld\n\xc3\xa9l\n");
}

TEST(CodeGenE2E, Utf8ConcatMixedKind) {
    auto output = compileAndRun(
        "a: str = \"hello \"\n"
        "b: str = \"w\xc3\xb6rld\"\n"
        "print(a + b)\n"
        "print(\"prefix \" + b + \" suffix\")\n"
    );
    EXPECT_EQ(output, "hello w\xc3\xb6rld\nprefix w\xc3\xb6rld suffix\n");
}

TEST(CodeGenE2E, Utf8ConcatCanonicalDowngrade) {
    // Replace that strips the only non-ASCII char must produce a kind=1
    // (canonical-storage) result; len/indexing/print all remain correct.
    auto output = compileAndRun(
        "s: str = \"h\xc3\xa9llo\"\n"
        "r: str = s.replace(\"\xc3\xa9\", \"e\")\n"
        "print(r)\n"
        "print(len(r))\n"
        "print(r[1])\n"
    );
    EXPECT_EQ(output, "hello\n5\ne\n");
}

TEST(CodeGenE2E, Utf8FindAndContains) {
    auto output = compileAndRun(
        "s: str = \"h\xc3\xa9llo w\xc3\xb6rld\"\n"
        "print(s.find(\"w\xc3\xb6rld\"))\n"
        "print(s.find(\"xyz\"))\n"
        "if \"w\xc3\xb6rld\" in s {\n"
        "    print(\"yes\")\n"
        "}\n"
        "if \"missing\" in s {\n"
        "    print(\"x\")\n"
        "} else {\n"
        "    print(\"no\")\n"
        "}\n"
    );
    EXPECT_EQ(output, "6\n-1\nyes\nno\n");
}

TEST(CodeGenE2E, Utf8StartswithEndswith) {
    auto output = compileAndRun(
        "s: str = \"h\xc3\xa9llo w\xc3\xb6rld\"\n"
        "if s.startswith(\"h\xc3\xa9llo\") {\n"
        "    print(\"sp\")\n"
        "}\n"
        "if s.endswith(\"w\xc3\xb6rld\") {\n"
        "    print(\"ep\")\n"
        "}\n"
        "if s.startswith(\"goodbye\") {\n"
        "    print(\"x\")\n"
        "} else {\n"
        "    print(\"sn\")\n"
        "}\n"
    );
    EXPECT_EQ(output, "sp\nep\nsn\n");
}

TEST(CodeGenE2E, Utf8Replace) {
    auto output = compileAndRun(
        "s: str = \"caf\xc3\xa9 au lait\"\n"
        "print(s.replace(\"\xc3\xa9\", \"e\"))\n"
        "print(s.replace(\"au lait\", \"noir\"))\n"
        "print(s.replace(\"caf\xc3\xa9\", \"th\xc3\xa9\"))\n"
    );
    EXPECT_EQ(output, "cafe au lait\ncaf\xc3\xa9 noir\nth\xc3\xa9 au lait\n");
}

TEST(CodeGenE2E, Utf8DictKeys) {
    auto output = compileAndRun(
        "d: dict[str, int] = {\"caf\xc3\xa9\": 1, \"tea\": 2, \"\xe6\x97\xa5\xe6\x9c\xac\": 3}\n"
        "print(d[\"caf\xc3\xa9\"])\n"
        "print(d[\"tea\"])\n"
        "print(d[\"\xe6\x97\xa5\xe6\x9c\xac\"])\n"
        "h1: bool = \"caf\xc3\xa9\" in d\n"
        "h2: bool = \"missing\" in d\n"
        "print(h1)\n"
        "print(h2)\n"
    );
    EXPECT_EQ(output, "1\n2\n3\nTrue\nFalse\n");
}

TEST(CodeGenE2E, Utf8PrintRoundTrip) {
    auto output = compileAndRun(
        "print(\"caf\xc3\xa9\")\n"
        "print(\"\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e\")\n"
        "print(\"\xce\xb1\xce\xb2\xce\xb3\")\n"
    );
    EXPECT_EQ(output, "caf\xc3\xa9\n\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e\n\xce\xb1\xce\xb2\xce\xb3\n");
}

TEST(CodeGenE2E, Utf8AsciiUpperLowerStillWork) {
    auto output = compileAndRun(
        "print(\"Hello\".upper())\n"
        "print(\"Hello\".lower())\n"
    );
    EXPECT_EQ(output, "HELLO\nhello\n");
}

TEST(CodeGenE2E, Utf8CaseMapsLatin1) {
    // Simple Unicode case folding (algorithmic, no tables): Latin-1 Supplement
    // letters fold like ASCII. é (U+00E9) -> É (U+00C9), ö (U+00F6) -> Ö
    // (U+00D6). (Was identity-only in v0.0.1; the security pass widened folding
    // to Latin-1/Latin-Ext-A/Greek/Cyrillic so username case-compares are not
    // fooled by un-folded non-ASCII letters.)
    auto output = compileAndRun(
        "s: str = \"h\xc3\xa9llo w\xc3\xb6rld\"\n"
        "print(s.upper())\n"
    );
    EXPECT_EQ(output, "H\xc3\x89LLO W\xc3\x96RLD\n");
}

// Regression for the wire-byte-count contract of `dragon_str_byte_len_pub`.
// The helper backs HTTP `Content-Length` and `nb_send` length, where the
// caller needs the bytes that will actually travel on the wire (UTF-8). The
// pre-fix implementation returned `ds->len * ds->kind` for kind=4 strings -
// i.e. the storage byte count (4×cp_count), not the UTF-8 wire byte count
// - making clients hang waiting for bytes that never arrived. The fix walks
// the cps and sums `utf8_encode_one` widths.
//
// Test inputs cover the three wire widths the helper has to compute:
//  - "caf\xc3\xa9" (kind=4, 4 cps, 5 wire bytes - one 2-byte cp)
//  - 3 CJK cps (kind=4, 3 cps, 9 wire bytes - three 3-byte cps)
//  - pure ASCII (kind=1, 5 cps, 5 wire bytes - fast path)
TEST(CodeGenE2E, Utf8ByteLenPubReturnsWireBytes) {
    auto output = compileAndRun(
        "extern \"C\" def dragon_str_byte_len_pub(s: str) -> int\n"
        "a: str = \"caf\xc3\xa9\"\n"
        "b: str = \"\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e\"\n"
        "c: str = \"hello\"\n"
        "print(dragon_str_byte_len_pub(a))\n"
        "print(dragon_str_byte_len_pub(b))\n"
        "print(dragon_str_byte_len_pub(c))\n"
    );
    EXPECT_EQ(output, "5\n9\n5\n")
        << "byte_len_pub must return UTF-8 wire bytes, not 4×cp_count for "
           "kind=4 (would yield 16 / 12 / 5 instead of 5 / 9 / 5):\n"
        << output;
}

//===----------------------------------------------------------------------===//
// Multi-arg print() - Python sep=' ', end='\n'
//===----------------------------------------------------------------------===//

TEST(CodeGenE2E, PrintMultiArgStrings) {
    EXPECT_EQ(compileAndRun("print(\"a\", \"b\", \"c\")\n"), "a b c\n");
}

TEST(CodeGenE2E, PrintMultiArgInts) {
    EXPECT_EQ(compileAndRun("print(1, 2, 3)\n"), "1 2 3\n");
}

TEST(CodeGenE2E, PrintMultiArgMixedTypes) {
    EXPECT_EQ(compileAndRun(
        "print(\"count:\", 5, \"ratio:\", 2.5, \"ok:\", True)\n"),
        "count: 5 ratio: 2.5 ok: True\n");
}

TEST(CodeGenE2E, PrintMultiArgVariables) {
    EXPECT_EQ(compileAndRun(
        "a: int = 10\n"
        "b: str = \"hi\"\n"
        "print(a, b)\n"),
        "10 hi\n");
}

TEST(CodeGenE2E, PrintMultiArgWithList) {
    // A container as a non-final arg renders inline (no stray newline).
    EXPECT_EQ(compileAndRun(
        "xs: list[int] = [1, 2, 3]\n"
        "print(\"xs:\", xs)\n"),
        "xs: [1, 2, 3]\n");
}

TEST(CodeGenE2E, PrintSingleArgUnchanged) {
    // Single-arg formatting must be byte-identical to the pre-refactor path.
    EXPECT_EQ(compileAndRun("print(42)\n"), "42\n");
    EXPECT_EQ(compileAndRun("print(\"solo\")\n"), "solo\n");
}

TEST(CodeGenE2E, PrintEmptyThenValue) {
    EXPECT_EQ(compileAndRun("print()\nprint(\"after\")\n"), "\nafter\n");
}
