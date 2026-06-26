#include "CodeGenTestHelpers.h"

//===----------------------------------------------------------------------===//
// __doc__ - Python-parity docstring access for module/function/class/instance.
// Optional[str] niche-ptr ABI: non-null is a `.rodata` C string, null encodes
// `None` (printed as "None" by dragon_print_str). See decisions/030 for the
// niche-ptr representation and the design memo for the docstring lift.
//===----------------------------------------------------------------------===//

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
    // instance.__doc__ resolves through header.class_id -> descriptor.doc;
    // matches Python's behavior where `obj.__doc__` reads `type(obj).__doc__`
    // (no per-instance docstring).
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
    // Triple-quoted with embedded newlines and indentation must round-trip
    // byte-for-byte through the .rodata constant - no Python-style
    // inspect.cleandoc trimming. Cleaning is a library concern.
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
    // f-strings are not docstrings (CPython rule). The string at body[0]
    // must NOT be treated as a docstring; .__doc__ returns None.
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
    // String literal further down the body is not the docstring.
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

//===----------------------------------------------------------------------===//
// IR-level: when no docstring is present, no per-fn or per-module global is
// emitted (lazy materialization). This is the "zero cost when absent"
// guarantee from the D031 design memo.
//===----------------------------------------------------------------------===//

TEST(CodeGenTest, DocstringAbsentFunctionEmitsNoDocGlobal) {
    auto ir = generateIR(
        "def bare(n: int) -> int { return n }\n"
        "print(bare(1))\n"
    );
    // No __doc__ access in the program - no func_doc_ constant should appear.
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
    // descriptor_create receives `ptr null` for the doc field (5th arg).
    EXPECT_NE(ir.find("dragon_class_descriptor_create"), std::string::npos);
    // No `Plain__doc` global emitted when the class has no docstring.
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
    // The docstring constant is materialized for the descriptor's doc field
    // even if no .__doc__ access is taken - descriptor init needs it.
    EXPECT_NE(ir.find("C__doc"), std::string::npos);
}

//===----------------------------------------------------------------------===//
// Method __doc__ - `MyClass.method.__doc__` and `instance.method.__doc__`.
// Methods aren't first-class values in Dragon; pattern-match the AttrExpr
// chain at codegen and emit the cached `.rodata` constant directly.
//===----------------------------------------------------------------------===//

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

//===----------------------------------------------------------------------===//
// Bare `__doc__` name resolves to the current module's docstring (Python
// parity - the compiler synthesizes the binding). Routes through the same
// .rodata cache as <mod>.__doc__.
//===----------------------------------------------------------------------===//

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
    // Inside a function body, bare `__doc__` still refers to the module
    // docstring (Python's name-resolution rule - function `__doc__` is only
    // accessed via the function object).
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
