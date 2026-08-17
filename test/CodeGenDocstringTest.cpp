#include "CodeGenTestHelpers.h"

TEST(CodeGenE2E, DocstringClassPresent) {
    auto out = compileAndRun(
        "class Greeter {\n"
        "    \"\"\"Says hello to anyone who shows up.\"\"\"\n"
        "    name: str\n"
        "    def(name: str) { self.name = name }\n"
        "}\n"
        "print(Greeter.__doc__)\n"
    );
    EXPECT_EQ(out, "Says hello to anyone who shows up.\n");
}

TEST(CodeGenE2E, DocstringClassAbsent) {
    auto out = compileAndRun(
        "class Plain {\n"
        "    name: str\n"
        "    def(name: str) { self.name = name }\n"
        "}\n"
        "print(Plain.__doc__)\n"
    );
    EXPECT_EQ(out, "None\n");
}

TEST(CodeGenE2E, DocstringFunctionPresent) {
    auto out = compileAndRun(
        "def factorial(n: int) -> int {\n"
        "    \"\"\"Compute n! recursively.\"\"\"\n"
        "    if n <= 1 { return 1 }\n"
        "    return n * factorial(n - 1)\n"
        "}\n"
        "print(factorial.__doc__)\n"
    );
    EXPECT_EQ(out, "Compute n! recursively.\n");
}

TEST(CodeGenE2E, DocstringFunctionAbsent) {
    auto out = compileAndRun(
        "def bare(n: int) -> int { return n }\n"
        "print(bare.__doc__)\n"
    );
    EXPECT_EQ(out, "None\n");
}

TEST(CodeGenE2E, DocstringInstanceInheritsClass) {
    auto out = compileAndRun(
        "class Doc {\n"
        "    \"\"\"Class doc.\"\"\"\n"
        "    x: int\n"
        "    def(x: int) { self.x = x }\n"
        "}\n"
        "d: Doc = Doc(1)\n"
        "print(d.__doc__)\n"
    );
    EXPECT_EQ(out, "Class doc.\n");
}

TEST(CodeGenE2E, DocstringInstanceNoneWhenAbsent) {
    auto out = compileAndRun(
        "class NoDoc {\n"
        "    x: int\n"
        "    def(x: int) { self.x = x }\n"
        "}\n"
        "d: NoDoc = NoDoc(1)\n"
        "print(d.__doc__)\n"
    );
    EXPECT_EQ(out, "None\n");
}

TEST(CodeGenE2E, DocstringIsNoneNarrowing) {
    auto out = compileAndRun(
        "class A { \"\"\"has doc.\"\"\" x: int  def(x: int) { self.x = x } }\n"
        "class B { x: int  def(x: int) { self.x = x } }\n"
        "if A.__doc__ is None { print(\"A:none\") } else { print(\"A:doc\") }\n"
        "if B.__doc__ is None { print(\"B:none\") } else { print(\"B:doc\") }\n"
    );
    EXPECT_EQ(out, "A:doc\nB:none\n");
}

TEST(CodeGenE2E, DocstringMultiline) {
    auto out = compileAndRun(
        "def f() -> int {\n"
        "    \"\"\"line one\n"
        "    line two\"\"\"\n"
        "    return 0\n"
        "}\n"
        "print(f.__doc__)\n"
    );
    EXPECT_EQ(out, "line one\n    line two\n");
}

TEST(CodeGenE2E, DocstringFStringNotLifted) {
    auto out = compileAndRun(
        "def f() -> int {\n"
        "    name: str = \"world\"\n"
        "    f\"hello {name}\"\n"
        "    return 0\n"
        "}\n"
        "print(f.__doc__)\n"
    );
    EXPECT_EQ(out, "None\n");
}

TEST(CodeGenE2E, DocstringNotFirstStmtIsNotLifted) {
    auto out = compileAndRun(
        "def f() -> int {\n"
        "    n: int = 0\n"
        "    \"this is not a docstring\"\n"
        "    return n\n"
        "}\n"
        "print(f.__doc__)\n"
    );
    EXPECT_EQ(out, "None\n");
}

TEST(CodeGenTest, DocstringAbsentFunctionEmitsNoDocGlobal) {
    auto ir = generateIR(
        "def bare(n: int) -> int { return n }\n"
        "print(bare(1))\n"
    );
    EXPECT_EQ(ir.find("func_doc_bare"), std::string::npos);
}

TEST(CodeGenTest, DocstringAbsentClassEmitsNullDocPtr) {
    auto ir = generateIR(
        "class Plain {\n"
        "    x: int\n"
        "    def(x: int) { self.x = x }\n"
        "}\n"
        "p: Plain = Plain(1)\n"
        "print(p.x)\n"
    );
    EXPECT_NE(ir.find("dragon_class_descriptor_create"), std::string::npos);
    EXPECT_EQ(ir.find("Plain__doc"), std::string::npos);
}

TEST(CodeGenTest, DocstringPresentClassEmitsDocGlobal) {
    auto ir = generateIR(
        "class C {\n"
        "    \"\"\"has docs.\"\"\"\n"
        "    x: int\n"
        "    def(x: int) { self.x = x }\n"
        "}\n"
        "c: C = C(1)\n"
        "print(c.x)\n"
    );
    EXPECT_NE(ir.find("C__doc"), std::string::npos);
}

TEST(CodeGenE2E, DocstringMethodViaClassChain) {
    auto out = compileAndRun(
        "class C {\n"
        "    x: int\n"
        "    def(x: int) { self.x = x }\n"
        "    def greet() -> int {\n"
        "        \"\"\"method greeting.\"\"\"\n"
        "        return self.x\n"
        "    }\n"
        "}\n"
        "print(C.greet.__doc__)\n"
    );
    EXPECT_EQ(out, "method greeting.\n");
}

TEST(CodeGenE2E, DocstringMethodViaInstanceChain) {
    auto out = compileAndRun(
        "class C {\n"
        "    x: int\n"
        "    def(x: int) { self.x = x }\n"
        "    def greet() -> int {\n"
        "        \"\"\"method greeting.\"\"\"\n"
        "        return self.x\n"
        "    }\n"
        "}\n"
        "c: C = C(1)\n"
        "print(c.greet.__doc__)\n"
    );
    EXPECT_EQ(out, "method greeting.\n");
}

TEST(CodeGenE2E, DocstringMethodAbsentReturnsNone) {
    auto out = compileAndRun(
        "class C {\n"
        "    x: int\n"
        "    def(x: int) { self.x = x }\n"
        "    def bare() -> int { return self.x }\n"
        "}\n"
        "print(C.bare.__doc__)\n"
    );
    EXPECT_EQ(out, "None\n");
}

TEST(CodeGenE2E, DocstringBareNamePresent) {
    auto out = compileAndRun(
        "\"\"\"module-level doc.\"\"\"\n"
        "print(__doc__)\n"
    );
    EXPECT_EQ(out, "module-level doc.\n");
}

TEST(CodeGenE2E, DocstringBareNameAbsentIsNone) {
    auto out = compileAndRun(
        "print(__doc__)\n"
    );
    EXPECT_EQ(out, "None\n");
}

TEST(CodeGenE2E, DocstringBareNameInsideFunction) {
    auto out = compileAndRun(
        "\"\"\"mod doc.\"\"\"\n"
        "def f() -> int {\n"
        "    \"\"\"fn doc.\"\"\"\n"
        "    print(__doc__)\n"
        "    return 0\n"
        "}\n"
        "f()\n"
    );
    EXPECT_EQ(out, "mod doc.\n");
}
