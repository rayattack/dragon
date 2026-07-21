#include "CodeGenTestHelpers.h"

//===----------------------------------------------------------------------===//
// Control Flow IR Tests
//===----------------------------------------------------------------------===//

TEST(CodeGenTest, IfStatement) {
    auto ir = generateIR("x: int = 5\nif x > 3 {\n  print(x)\n}");
    EXPECT_NE(ir.find("br i1"), std::string::npos);
    EXPECT_NE(ir.find("then"), std::string::npos);
}

TEST(CodeGenTest, IfElseStatement) {
    auto ir = generateIR(
        "x: int = 5\n"
        "if x > 10 {\n  print(1)\n"
        "} else {\n  print(2)\n}"
    );
    EXPECT_NE(ir.find("then"), std::string::npos);
    EXPECT_NE(ir.find("else"), std::string::npos);
}

TEST(CodeGenTest, WhileLoop) {
    auto ir = generateIR("x: int = 0\nwhile x < 5 {\n  x += 1\n}");
    EXPECT_NE(ir.find("whilecond"), std::string::npos);
    EXPECT_NE(ir.find("whilebody"), std::string::npos);
    EXPECT_NE(ir.find("whileend"), std::string::npos);
}

TEST(CodeGenTest, IfFloatConditionCoercion) {
    // Float conditions should be coerced to bool via FCmpONE
    auto ir = generateIR("x: float = 1.0\nif x {\n  print(1)\n}");
    EXPECT_NE(ir.find("fcmp one"), std::string::npos);
}

TEST(CodeGenTest, WhileFloatConditionCoercion) {
    auto ir = generateIR("x: float = 1.0\nwhile x {\n  break\n}");
    EXPECT_NE(ir.find("fcmp one"), std::string::npos);
}

TEST(CodeGenTest, ElifFloatConditionCoercion) {
    // Elif with float should also be coerced (was missing f64 handling)
    auto ir = generateIR(
        "x: int = 0\n"
        "y: float = 1.0\n"
        "if x {\n  print(1)\n} elif y {\n  print(2)\n}");
    // Should have two fcmp/icmp coercions: one for if (int), one for elif (float)
    EXPECT_NE(ir.find("tobool"), std::string::npos);
}

TEST(CodeGenE2E, ElseIfChainSameAsElif) {
    // `else if` is accepted as an alias for `elif`. Mixing the two
    // in one chain works too.
    auto out = compileAndRun(
        "def cls(x: int) -> str {\n"
        "    if x > 100 {\n"
        "        return \"huge\"\n"
        "    } else if x > 50 {\n"
        "        return \"big\"\n"
        "    } elif x > 10 {\n"
        "        return \"medium\"\n"
        "    } else if x > 0 {\n"
        "        return \"tiny\"\n"
        "    } else {\n"
        "        return \"zero\"\n"
        "    }\n"
        "}\n"
        "print(cls(200))\n"
        "print(cls(75))\n"
        "print(cls(25))\n"
        "print(cls(5))\n"
        "print(cls(0))\n"
    );
    EXPECT_EQ(out, "huge\nbig\nmedium\ntiny\nzero\n");
}

TEST(CodeGenTest, ForRange) {
    auto ir = generateIR("for i in range(10) {\n  print(i)\n}");
    EXPECT_NE(ir.find("forcond"), std::string::npos);
    EXPECT_NE(ir.find("forbody"), std::string::npos);
    EXPECT_NE(ir.find("icmp slt"), std::string::npos);
}

TEST(CodeGenTest, ForRangeStartEnd) {
    auto ir = generateIR("for i in range(2, 5) {\n  print(i)\n}");
    EXPECT_NE(ir.find("forcond"), std::string::npos);
    EXPECT_NE(ir.find("store i64 2"), std::string::npos);
}

TEST(CodeGenTest, BreakStatement) {
    auto ir = generateIR("while True {\n  break\n}");
    EXPECT_NE(ir.find("br label %whileend"), std::string::npos);
}

TEST(CodeGenTest, ContinueStatement) {
    auto ir = generateIR("x: int = 0\nwhile x < 10 {\n  x += 1\n  continue\n}");
    EXPECT_NE(ir.find("br label %whilecond"), std::string::npos);
}

TEST(CodeGenTest, ReturnStatement) {
    auto ir = generateIR("def foo() -> int {\n  return 42\n}");
    EXPECT_NE(ir.find("ret i64 42"), std::string::npos);
}

TEST(CodeGenTest, PassStatement) {
    // Pass is a no-op, should generate valid IR
    auto ir = generateIR("pass");
    EXPECT_NE(ir.find("define i32 @main("), std::string::npos);
}

TEST(CodeGenTest, AssertStatement) {
    auto ir = generateIR("assert True");
    EXPECT_NE(ir.find("dragon_assert"), std::string::npos);
}

//===----------------------------------------------------------------------===//
// For-In IR Tests
//===----------------------------------------------------------------------===//

TEST(CodeGenTest, ForInList) {
    auto ir = generateIR("nums: list[int] = [1, 2, 3]\nfor x in nums {\n  print(x)\n}");
    EXPECT_NE(ir.find("dragon_list_len"), std::string::npos);
    EXPECT_NE(ir.find("dragon_list_get"), std::string::npos);
}

TEST(CodeGenTest, ForInString) {
    auto ir = generateIR("s: str = \"abc\"\nfor c in s {\n  print(c)\n}");
    EXPECT_NE(ir.find("dragon_str_len"), std::string::npos);
    EXPECT_NE(ir.find("dragon_str_index"), std::string::npos);
}

TEST(CodeGenTest, ForInDictKeys) {
    auto ir = generateIR(
        "d: dict[str, int] = {\"a\": 1, \"b\": 2}\n"
        "for k in d {\n"
        "  print(k)\n"
        "}\n"
    );
    EXPECT_NE(ir.find("dragon_dict_keys"), std::string::npos)
        << "Expected dragon_dict_keys call for 'for k in d'";
    EXPECT_NE(ir.find("dragon_list_len"), std::string::npos);
    EXPECT_NE(ir.find("dragon_list_get"), std::string::npos);
}

TEST(CodeGenTest, ForInDictItems) {
    auto ir = generateIR(
        "d: dict[str, int] = {\"a\": 1}\n"
        "for k, v in d.items() {\n"
        "  print(k)\n"
        "}\n"
    );
    EXPECT_NE(ir.find("dragon_dict_items"), std::string::npos)
        << "Expected dragon_dict_items call for 'd.items()'";
    EXPECT_NE(ir.find("dragon_tuple_get"), std::string::npos)
        << "Expected tuple unpacking for dict items";
}

//===----------------------------------------------------------------------===//
// Control Flow E2E Tests
//===----------------------------------------------------------------------===//

TEST(CodeGenE2E, ForRangeThreeArgs) {
    auto output = compileAndRun(
        "for i in range(0, 10, 3) {\n"
        "  print(i)\n"
        "}"
    );
    EXPECT_EQ(output, "0\n3\n6\n9\n");
}

TEST(CodeGenE2E, BreakContinue) {
    auto output = compileAndRun(
        "for i in range(10) {\n"
        "  if i == 3 {\n"
        "    continue\n"
        "  }\n"
        "  if i == 6 {\n"
        "    break\n"
        "  }\n"
        "  print(i)\n"
        "}"
    );
    EXPECT_EQ(output, "0\n1\n2\n4\n5\n");
}

TEST(CodeGenE2E, NestedLoops) {
    auto output = compileAndRun(
        "for i in range(3) {\n"
        "  for j in range(3) {\n"
        "    if i == j {\n"
        "      print(i)\n"
        "    }\n"
        "  }\n"
        "}"
    );
    EXPECT_EQ(output, "0\n1\n2\n");
}

TEST(CodeGenE2E, AssertPass) {
    auto output = compileAndRun(
        "assert True\n"
        "assert 1 + 1 == 2\n"
        "print(\"ok\")"
    );
    EXPECT_EQ(output, "ok\n");
}

TEST(CodeGenE2E, ForInDict) {
    auto output = compileAndRun(
        "d: dict[str, int] = {\"x\": 10, \"y\": 20}\n"
        "for k in d {\n"
        "  print(k)\n"
        "}\n"
    );
    EXPECT_EQ(output, "x\ny\n");
}

TEST(CodeGenE2E, ForInDictKeys) {
    auto output = compileAndRun(
        "d: dict[str, int] = {\"a\": 1, \"b\": 2}\n"
        "for k in d.keys() {\n"
        "  print(k)\n"
        "}\n"
    );
    EXPECT_EQ(output, "a\nb\n");
}

TEST(CodeGenE2E, ForInDictItems) {
    auto output = compileAndRun(
        "d: dict[str, int] = {\"a\": 1, \"b\": 2}\n"
        "for k, v in d.items() {\n"
        "  print(k)\n"
        "  print(v)\n"
        "}\n"
    );
    EXPECT_EQ(output, "a\n1\nb\n2\n");
}

TEST(CodeGenE2E, ForInDictValues) {
    auto output = compileAndRun(
        "d: dict[str, int] = {\"x\": 100, \"y\": 200}\n"
        "for v in d.values() {\n"
        "  print(v)\n"
        "}\n"
    );
    EXPECT_EQ(output, "100\n200\n");
}

//===----------------------------------------------------------------------===//
// Statement E2E Tests
//===----------------------------------------------------------------------===//

TEST(CodeGenE2E, DeleteStmtBasic) {
    // del should release the variable and set it to null
    auto output = compileAndRun(
        "x: int = 42\n"
        "del x\n"
        "print(\"ok\")\n"
    );
    EXPECT_EQ(output, "ok\n");
}

TEST(CodeGenE2E, DeleteStmtString) {
    // del on a string variable should decref and zero the slot
    auto output = compileAndRun(
        "s: str = \"hello\" + \" world\"\n"
        "del s\n"
        "print(\"ok\")\n"
    );
    EXPECT_EQ(output, "ok\n");
}

TEST(CodeGenTest, DeleteStmtIR) {
    // IR should contain dragon_decref_str for del on a string variable
    auto ir = generateIR(
        "s: str = \"hello\" + \" world\"\n"
        "del s\n"
    );
    EXPECT_NE(ir.find("dragon_decref_str"), std::string::npos);
}
