#include "CodeGenTestHelpers.h"

TEST(CodeGenTest, TemplateSimpleIR) {
    auto ir = generateIR("x: str = template {hello}");
    EXPECT_NE(ir.find("hello"), std::string::npos);
}

TEST(CodeGenTest, TemplateInterpolationIR) {
    auto ir = generateIR("x: int = 42\ny: str = template {val=!{x}}");
    EXPECT_NE(ir.find("dragon_int_to_str"), std::string::npos);
    EXPECT_NE(ir.find("dragon_str_concat"), std::string::npos);
}

TEST(CodeGenTest, TemplatePipeFilterIR) {
    auto ir = generateIR("x: str = \"test\"\ny: str = template {!{x | html}}");
    EXPECT_NE(ir.find("dragon_template_escape_html"), std::string::npos);
}

TEST(CodeGenTest, TemplatePipeSqlFilterIR) {
    auto ir = generateIR("x: str = \"test\"\ny: str = template {!{x | sql}}");
    EXPECT_NE(ir.find("dragon_template_escape_sql"), std::string::npos);
}

TEST(CodeGenTest, TemplatePipeUrlFilterIR) {
    auto ir = generateIR("x: str = \"test\"\ny: str = template {!{x | url}}");
    EXPECT_NE(ir.find("dragon_template_escape_url"), std::string::npos);
}

TEST(CodeGenE2E, TemplateLiteralOnly) {
    auto output = compileAndRun(
        "x: str = template {hello world}\n"
        "print(x)"
    );
    EXPECT_EQ(output, "hello world\n");
}

TEST(CodeGenE2E, TemplateIntInterpolation) {
    auto output = compileAndRun(
        "x: int = 42\n"
        "print(template {value is !{x}})"
    );
    EXPECT_EQ(output, "value is 42\n");
}

TEST(CodeGenE2E, TemplateStringInterpolation) {
    auto output = compileAndRun(
        "name: str = \"World\"\n"
        "print(template {Hello !{name}!})"
    );
    EXPECT_EQ(output, "Hello World!\n");
}

TEST(CodeGenE2E, TemplateMultipleExprs) {
    auto output = compileAndRun(
        "a: int = 10\n"
        "b: int = 20\n"
        "print(template {!{a} + !{b} = !{a + b}})"
    );
    EXPECT_EQ(output, "10 + 20 = 30\n");
}

TEST(CodeGenE2E, TemplateEscapedBang) {
    auto output = compileAndRun(
        "print(template {Use !!{expr} for interpolation})"
    );
    EXPECT_EQ(output, "Use !{expr} for interpolation\n");
}

TEST(CodeGenE2E, TemplateDoubleBangLiteral) {
    auto output = compileAndRun(
        "print(template {var a = !!b; t = !! q})"
    );
    EXPECT_EQ(output, "var a = !!b; t = !! q\n");
}

TEST(CodeGenE2E, TemplateBalancedBraces) {
    auto output = compileAndRun(
        "name: str = \"test\"\n"
        "print(template {{\"key\": \"!{name}\"}})"
    );
    EXPECT_EQ(output, "{\"key\": \"test\"}\n");
}

TEST(CodeGenE2E, TemplateFloatInterpolation) {
    auto output = compileAndRun(
        "x: float = 3.14\n"
        "print(template {pi=!{x}})"
    );
    EXPECT_EQ(output, "pi=3.14\n");
}

TEST(CodeGenE2E, TemplateBoolInterpolation) {
    auto output = compileAndRun(
        "x: bool = True\n"
        "print(template {flag=!{x}})"
    );
    EXPECT_EQ(output, "flag=True\n");
}

TEST(CodeGenE2E, TemplatePipeHtml) {
    auto output = compileAndRun(
        "s: str = \"<b>hello</b>\"\n"
        "print(template {!{s | html}})"
    );
    EXPECT_EQ(output, "&lt;b&gt;hello&lt;/b&gt;\n");
}

TEST(CodeGenE2E, TemplatePipeHtmlAmpersand) {
    auto output = compileAndRun(
        "s: str = \"a&b\"\n"
        "print(template {!{s | html}})"
    );
    EXPECT_EQ(output, "a&amp;b\n");
}

TEST(CodeGenE2E, TemplatePipeHtmlQuotes) {
    auto output = compileAndRun(
        "s: str = \"a'b\"\n"
        "print(template {!{s | html}})"
    );
    EXPECT_EQ(output, "a&#x27;b\n");
}

TEST(CodeGenE2E, TemplatePipeSql) {
    auto output = compileAndRun(
        "name: str = \"O'Brien\"\n"
        "print(template {WHERE name = '!{name | sql}'})"
    );
    EXPECT_EQ(output, "WHERE name = 'O''Brien'\n");
}

TEST(CodeGenE2E, TemplatePipeUrl) {
    auto output = compileAndRun(
        "q: str = \"hello world\"\n"
        "print(template {?q=!{q | url}})"
    );
    EXPECT_EQ(output, "?q=hello%20world\n");
}

TEST(CodeGenE2E, TemplatePipeUrlSpecialChars) {
    auto output = compileAndRun(
        "s: str = \"a+b=c&d\"\n"
        "print(template {!{s | url}})"
    );
    EXPECT_EQ(output, "a%2Bb%3Dc%26d\n");
}

TEST(CodeGenE2E, TemplatePipeUserDefined) {
    auto output = compileAndRun(
        "def exclaim(s: str) -> str {\n"
        "    return s + \"!!!\"\n"
        "}\n"
        "\n"
        "name: str = \"World\"\n"
        "print(template {Hello !{name | exclaim}})"
    );
    EXPECT_EQ(output, "Hello World!!!\n");
}

TEST(CodeGenE2E, TemplatePipeMixed) {
    auto output = compileAndRun(
        "user: str = \"<admin>\"\n"
        "count: int = 5\n"
        "print(template {User: !{user | html}, count: !{count}})"
    );
    EXPECT_EQ(output, "User: &lt;admin&gt;, count: 5\n");
}

TEST(CodeGenE2E, TemplatePipeIntToHtml) {
    auto output = compileAndRun(
        "x: int = 42\n"
        "print(template {!{x | html}})"
    );
    EXPECT_EQ(output, "42\n");
}

TEST(CodeGenE2E, TemplateFileLiteralOnly) {
    std::string tplPath = "/tmp/dragon_tpl_test_" + std::to_string(getpid()) + "_lit.html";
    {
        std::ofstream f(tplPath);
        f << "Hello from file!";
    }
    auto output = compileAndRun(
        "print(template(\"" + tplPath + "\"))"
    );
    std::remove(tplPath.c_str());
    EXPECT_EQ(output, "Hello from file!\n");
}

TEST(CodeGenE2E, TemplateFileInterpolation) {
    std::string tplPath = "/tmp/dragon_tpl_test_" + std::to_string(getpid()) + "_interp.html";
    {
        std::ofstream f(tplPath);
        f << "<h1>Hello !{name}</h1>";
    }
    auto output = compileAndRun(
        "name: str = \"World\"\n"
        "print(template(\"" + tplPath + "\"))"
    );
    std::remove(tplPath.c_str());
    EXPECT_EQ(output, "<h1>Hello World</h1>\n");
}

TEST(CodeGenE2E, TemplateFileMultipleExprs) {
    std::string tplPath = "/tmp/dragon_tpl_test_" + std::to_string(getpid()) + "_multi.html";
    {
        std::ofstream f(tplPath);
        f << "!{name} is !{age} years old";
    }
    auto output = compileAndRun(
        "name: str = \"Alice\"\n"
        "age: int = 30\n"
        "print(template(\"" + tplPath + "\"))"
    );
    std::remove(tplPath.c_str());
    EXPECT_EQ(output, "Alice is 30 years old\n");
}

TEST(CodeGenE2E, TemplateFileWithPipeFilter) {
    std::string tplPath = "/tmp/dragon_tpl_test_" + std::to_string(getpid()) + "_pipe.html";
    {
        std::ofstream f(tplPath);
        f << "<div>!{user_input | html}</div>";
    }
    auto output = compileAndRun(
        "user_input: str = \"<script>alert('xss')</script>\"\n"
        "print(template(\"" + tplPath + "\"))"
    );
    std::remove(tplPath.c_str());
    EXPECT_EQ(output, "<div>&lt;script&gt;alert(&#x27;xss&#x27;)&lt;/script&gt;</div>\n");
}

TEST(CodeGenE2E, TemplateFileMultiline) {
    std::string tplPath = "/tmp/dragon_tpl_test_" + std::to_string(getpid()) + "_ml.html";
    {
        std::ofstream f(tplPath);
        f << "<html>\n<body>\n<h1>!{title}</h1>\n</body>\n</html>";
    }
    auto output = compileAndRun(
        "title: str = \"Test\"\n"
        "print(template(\"" + tplPath + "\"))"
    );
    std::remove(tplPath.c_str());
    EXPECT_EQ(output, "<html>\n<body>\n<h1>Test</h1>\n</body>\n</html>\n");
}

TEST(CodeGenE2E, TemplateFileNotFound) {
    auto output = compileAndRun(
        "print(template(\"/tmp/dragon_nonexistent_template_file.html\"))"
    );
    EXPECT_NE(output.find("codegen failed"), std::string::npos);
}

TEST(CodeGenE2E, TemplateFileEscapedBang) {
    std::string tplPath = "/tmp/dragon_tpl_test_" + std::to_string(getpid()) + "_esc.html";
    {
        std::ofstream f(tplPath);
        f << "Use !!{ for literal";
    }
    auto output = compileAndRun(
        "print(template(\"" + tplPath + "\"))"
    );
    std::remove(tplPath.c_str());
    EXPECT_EQ(output, "Use !{ for literal\n");
}

TEST(CodeGenE2E, TemplateFileBalancedBraces) {
    std::string tplPath = "/tmp/dragon_tpl_test_" + std::to_string(getpid()) + "_json.html";
    {
        std::ofstream f(tplPath);
        f << "{\"name\": \"!{name}\"}";
    }
    auto output = compileAndRun(
        "name: str = \"Dragon\"\n"
        "print(template(\"" + tplPath + "\"))"
    );
    std::remove(tplPath.c_str());
    EXPECT_EQ(output, "{\"name\": \"Dragon\"}\n");
}

static const std::string TPL_BASE =
    "class Template {\n"
    "    def(inner: str) { self._inner = inner }\n"
    "    @staticmethod\n"
    "    def escape(s: str) -> str { return s }\n"
    "    @staticmethod\n"
    "    def validate(content: str) -> None { pass }\n"
    "    def to_str() -> str { return self._inner }\n"
    "    def __str__() -> str { return self._inner }\n"
    "}\n";

TEST(CodeGenTest, TypedTemplateAutoEscapeIR) {
    auto ir = generateIR(
        TPL_BASE +
        "class SAFE(Template) {\n"
        "  def(inner: str) { self._inner = inner }\n"
        "  @staticmethod\n"
        "  def escape(s: str) -> str { return s }\n"
        "  def __str__() -> str { return self._inner }\n"
        "}\n"
        "x: str = \"test\"\n"
        "y: SAFE = template[SAFE] {!{x}}\n"
    );
    EXPECT_NE(ir.find("call ptr @SAFE_escape("), std::string::npos)
        << "Expected call to SAFE_escape in auto-escape template\nIR:\n" << ir;
    EXPECT_NE(ir.find("SAFE_new"), std::string::npos);
}

TEST(CodeGenTest, TypedTemplateRawFilterIR) {
    auto ir = generateIR(
        TPL_BASE +
        "class SAFE(Template) {\n"
        "  def(inner: str) { self._inner = inner }\n"
        "  @staticmethod\n"
        "  def escape(s: str) -> str { return s }\n"
        "  def __str__() -> str { return self._inner }\n"
        "}\n"
        "x: str = \"test\"\n"
        "y: SAFE = template[SAFE] {!{x | raw}}\n"
    );
    EXPECT_EQ(ir.find("call ptr @SAFE_escape("), std::string::npos);
    EXPECT_EQ(ir.find("call ptr @dragon_template_escape"), std::string::npos);
}

TEST(CodeGenE2E, TypedTemplateAutoEscape) {
    auto output = compileAndRun(
        TPL_BASE +
        "class YELL(Template) {\n"
        "  def(inner: str) { self._inner = inner }\n"
        "  @staticmethod\n"
        "  def escape(s: str) -> str { return s.upper() }\n"
        "  def __str__() -> str { return self._inner }\n"
        "}\n"
        "name: str = \"hello\"\n"
        "y: YELL = template[YELL] {greeting: !{name}}\n"
        "print(y)\n"
    );
    EXPECT_EQ(output, "greeting: HELLO\n");
}

TEST(CodeGenE2E, TypedTemplateRawFilter) {
    auto output = compileAndRun(
        TPL_BASE +
        "class YELL(Template) {\n"
        "  def(inner: str) { self._inner = inner }\n"
        "  @staticmethod\n"
        "  def escape(s: str) -> str { return s.upper() }\n"
        "  def __str__() -> str { return self._inner }\n"
        "}\n"
        "name: str = \"hello\"\n"
        "y: YELL = template[YELL] {greeting: !{name | raw}}\n"
        "print(y)\n"
    );
    EXPECT_EQ(output, "greeting: hello\n");
}

TEST(CodeGenE2E, TypedTemplateLiteralOnly) {
    auto output = compileAndRun(
        TPL_BASE +
        "class WRAP(Template) {\n"
        "  def(inner: str) { self._inner = inner }\n"
        "  @staticmethod\n"
        "  def escape(s: str) -> str { return s }\n"
        "  def __str__() -> str { return self._inner }\n"
        "}\n"
        "y: WRAP = template[WRAP] {just text}\n"
        "print(y)\n"
    );
    EXPECT_EQ(output, "just text\n");
}

TEST(CodeGenE2E, TypedTemplateMultipleExprs) {
    auto output = compileAndRun(
        TPL_BASE +
        "class YELL(Template) {\n"
        "  def(inner: str) { self._inner = inner }\n"
        "  @staticmethod\n"
        "  def escape(s: str) -> str { return s.upper() }\n"
        "  def __str__() -> str { return self._inner }\n"
        "}\n"
        "a: str = \"foo\"\n"
        "b: str = \"bar\"\n"
        "y: YELL = template[YELL] {!{a} and !{b}}\n"
        "print(y)\n"
    );
    EXPECT_EQ(output, "FOO and BAR\n");
}

TEST(CodeGenE2E, TypedTemplateExplicitFilterOverride) {
    auto output = compileAndRun(
        TPL_BASE +
        "class YELL(Template) {\n"
        "  def(inner: str) { self._inner = inner }\n"
        "  @staticmethod\n"
        "  def escape(s: str) -> str { return s.upper() }\n"
        "  def __str__() -> str { return self._inner }\n"
        "}\n"
        "x: str = \"<b>hi</b>\"\n"
        "y: YELL = template[YELL] {!{x | html}}\n"
        "print(y)\n"
    );
    EXPECT_EQ(output, "&lt;b&gt;hi&lt;/b&gt;\n");
}

TEST(CodeGenE2E, TypedTemplateHTMLEscape) {
    auto output = compileAndRun(
        TPL_BASE +
        "class HTML(Template) {\n"
        "  def(inner: str) { self._inner = inner }\n"
        "  @staticmethod\n"
        "  def escape(s: str) -> str {\n"
        "    s = s.replace(\"&\", \"&amp;\")\n"
        "    s = s.replace(\"<\", \"&lt;\")\n"
        "    s = s.replace(\">\", \"&gt;\")\n"
        "    return s\n"
        "  }\n"
        "}\n"
        "user: str = \"<script>alert('xss')</script>\"\n"
        "page: HTML = template[HTML] {<div>!{user}</div>}\n"
        "print(page)\n"
    );
    EXPECT_EQ(output,
        "<div>&lt;script&gt;alert('xss')&lt;/script&gt;</div>\n");
}

TEST(CodeGenE2E, TypedTemplateSQLEscape) {
    auto output = compileAndRun(
        TPL_BASE +
        "class SQL(Template) {\n"
        "  def(inner: str) { self._inner = inner }\n"
        "  @staticmethod\n"
        "  def escape(s: str) -> str { return s.replace(\"'\", \"''\") }\n"
        "}\n"
        "name: str = \"O'Brien\"\n"
        "q: SQL = template[SQL] {SELECT * FROM users WHERE name = '!{name}'}\n"
        "print(q)\n"
    );
    EXPECT_EQ(output,
        "SELECT * FROM users WHERE name = 'O''Brien'\n");
}

TEST(CodeGenE2E, TypedTemplateInheritedEscape) {
    auto output = compileAndRun(
        TPL_BASE +
        "class HTML(Template) {\n"
        "  def(inner: str) { self._inner = inner }\n"
        "  @staticmethod\n"
        "  def escape(s: str) -> str {\n"
        "    return s.replace(\"<\", \"&lt;\").replace(\">\", \"&gt;\")\n"
        "  }\n"
        "}\n"
        "class MyHTML(HTML) {\n"
        "  def(inner: str) { self._inner = inner }\n"
        "}\n"
        "x: str = \"<b>hi</b>\"\n"
        "p: MyHTML = template[MyHTML] {!{x}}\n"
        "print(p)\n"
    );
    EXPECT_EQ(output, "&lt;b&gt;hi&lt;/b&gt;\n");
}

TEST(CodeGenTest, TypedTemplateInheritedEscapeIR) {
    auto ir = generateIR(
        TPL_BASE +
        "class HTML(Template) {\n"
        "  def(inner: str) { self._inner = inner }\n"
        "  @staticmethod\n"
        "  def escape(s: str) -> str { return s }\n"
        "}\n"
        "class MyHTML(HTML) {\n"
        "  def(inner: str) { self._inner = inner }\n"
        "}\n"
        "x: str = \"a\"\n"
        "p: MyHTML = template[MyHTML] {!{x}}\n"
    );
    EXPECT_NE(ir.find("call ptr @HTML_escape("), std::string::npos)
        << "Expected parent-walk to land on HTML_escape\nIR:\n" << ir;
    EXPECT_EQ(ir.find("call ptr @MyHTML_escape("), std::string::npos);
}

TEST(CodeGenTest, TypedTemplateValidateIR) {
    auto ir = generateIR(
        TPL_BASE +
        "class CHECK(Template) {\n"
        "  def(inner: str) { self._inner = inner }\n"
        "  @staticmethod\n"
        "  def escape(s: str) -> str { return s }\n"
        "  @staticmethod\n"
        "  def validate(content: str) -> None { pass }\n"
        "}\n"
        "x: str = \"hi\"\n"
        "y: CHECK = template[CHECK] {body: !{x}}\n"
    );
    EXPECT_NE(ir.find("call void @CHECK_validate("), std::string::npos)
        << "Expected CHECK_validate to be called on the final string\n"
        << "IR:\n" << ir;
}

TEST(CodeGenTest, TypedTemplateValidateOmittedWhenNotOverridden) {
    auto ir = generateIR(
        TPL_BASE +
        "class PLAIN(Template) {\n"
        "  def(inner: str) { self._inner = inner }\n"
        "  @staticmethod\n"
        "  def escape(s: str) -> str { return s }\n"
        "}\n"
        "x: str = \"hi\"\n"
        "y: PLAIN = template[PLAIN] {!{x}}\n"
    );
    EXPECT_EQ(ir.find("call void @PLAIN_validate("), std::string::npos)
        << "PLAIN didn't override validate - must not emit a call\nIR:\n" << ir;
    EXPECT_EQ(ir.find("call void @Template_validate("), std::string::npos)
        << "Validate parent-walk is explicitly disabled to avoid no-op calls\n"
        << "IR:\n" << ir;
}

TEST(CodeGenE2E, TypedTemplateSameTypeNoDoubleEscape) {
    auto output = compileAndRun(
        TPL_BASE +
        "class HTML(Template) {\n"
        "  def(inner: str) { self._inner = inner }\n"
        "  @staticmethod\n"
        "  def escape(s: str) -> str {\n"
        "    return s.replace(\"<\", \"&lt;\").replace(\">\", \"&gt;\")\n"
        "  }\n"
        "}\n"
        "inner: HTML = template[HTML] {<b>!{\"x\"}</b>}\n"
        "outer: HTML = template[HTML] {<div>!{inner}</div>}\n"
        "print(outer)\n"
    );
    EXPECT_EQ(output, "<div><b>x</b></div>\n");
}

TEST(CodeGenIR, TemplateFilterDecrefsOwnedIntInput) {
    auto ir = generateIR(
        "n: int = 42\n"
        "y: str = template {!{n | html}}\n"
    );
    EXPECT_NE(ir.find("dragon_int_to_str"), std::string::npos);
    EXPECT_NE(ir.find("dragon_template_escape_html"), std::string::npos);
    EXPECT_NE(ir.find("dragon_decref_str"), std::string::npos)
        << "Expected decref_str for owned int_to_str result before html filter\n"
        << "IR:\n" << ir;
}

TEST(CodeGenIR, TemplateFilterDecrefsOwnedMethodCallInput) {
    auto ir = generateIR(
        "s: str = \"hi\"\n"
        "y: str = template {!{s.upper() | html}}\n"
    );
    EXPECT_NE(ir.find("dragon_str_upper"), std::string::npos);
    EXPECT_NE(ir.find("dragon_template_escape_html"), std::string::npos);
    EXPECT_NE(ir.find("dragon_decref_str"), std::string::npos)
        << "Expected decref_str for owned upper() result before html filter\n"
        << "IR:\n" << ir;
}

TEST(CodeGenIR, TemplateFilterBorrowedInputNotDoubleDecref) {
    auto ir = generateIR(
        "s: str = \"<b>x</b>\"\n"
        "y: str = template {!{s | html}}\n"
    );
    EXPECT_NE(ir.find("dragon_template_escape_html"), std::string::npos);
    auto count = countSubstring(ir, "dragon_decref_str");
    EXPECT_LE(count, 2)
        << "Borrowed filter input must not be decref'd before the filter call.\n"
        << "Saw " << count << " decref_str calls (expected <= 2).\nIR:\n" << ir;
}

TEST(CodeGenE2E, TemplateFilterBoundedLoopOwnedInput) {
    auto output = compileAndRun(
        "last: str = \"\"\n"
        "for i in range(20000) {\n"
        "  last = template {!{str(i) | html}}\n"
        "}\n"
        "print(last)\n"
    );
    EXPECT_EQ(output, "19999\n");
}

TEST(CodeGenE2E, TemplateFilterBoundedLoopMethodChain) {
    auto output = compileAndRun(
        "s: str = \"<value>\"\n"
        "last: str = \"\"\n"
        "for i in range(10000) {\n"
        "  last = template {!{s.upper() | html}}\n"
        "}\n"
        "print(last)\n"
    );
    EXPECT_EQ(output, "&lt;VALUE&gt;\n");
}

TEST(CodeGenE2E, TemplateFilterBoundedLoopUserFilter) {
    auto output = compileAndRun(
        "def shout(s: str) -> str { return s.upper() }\n"
        "last: str = \"\"\n"
        "for i in range(10000) {\n"
        "  last = template {!{str(i) | shout}}\n"
        "}\n"
        "print(last)\n"
    );
    EXPECT_EQ(output, "9999\n");
}

TEST(CodeGenE2E, TemplateFilterCorrectness) {
    // Correctness of the filter chain itself - confirms decref doesn't
    // accidentally use-after-free the filter result.
    auto output = compileAndRun(
        "n: int = 5\n"
        "y: str = template {value=!{n | html}}\n"
        "print(y)\n"
    );
    EXPECT_EQ(output, "value=5\n");
}

TEST(CodeGenE2E, TemplateBlockForLoopWithContentAlias) {
    auto output = compileAndRun(
        "items: list[str] = [\"a\", \"b\", \"c\"]\n"
        "y: str = template {<ul>!{ for x in items { :{<li>!{x}</li>} } }</ul>}\n"
        "print(y)\n"
    );
    EXPECT_EQ(output, "<ul><li>a</li><li>b</li><li>c</li></ul>\n");
}

TEST(CodeGenE2E, TemplateBlockIfElseWithContentAlias) {
    auto output = compileAndRun(
        "logged_in: bool = True\n"
        "name: str = \"Ada\"\n"
        "y: str = template {!{ if logged_in { :{Hi !{name}} } else { :{Please sign in} } }}\n"
        "print(y)\n"
    );
    EXPECT_EQ(output, "Hi Ada\n");
}

TEST(CodeGenE2E, TemplateBlockMultiStatement) {
    auto output = compileAndRun(
        "items: list[str] = [\"x\", \"y\"]\n"
        "y: str = template {!{\n"
        "    label: str = \"item\"\n"
        "    for x in items { :{[!{label}=!{x}]} }\n"
        "}}\n"
        "print(y)\n"
    );
    EXPECT_EQ(output, "[item=x][item=y]\n");
}

TEST(CodeGenE2E, TemplateBlockContextInheritance) {
    auto output = compileAndRun(
        TPL_BASE +
        "class HTML(Template) {\n"
        "  def(inner: str) { self._inner = inner }\n"
        "  @staticmethod\n"
        "  def escape(s: str) -> str {\n"
        "    return s.replace(\"<\", \"&lt;\").replace(\">\", \"&gt;\")\n"
        "  }\n"
        "}\n"
        "items: list[str] = [\"<a>\", \"<b>\"]\n"
        "page: HTML = template[HTML] {<ul>!{ for x in items { :{<li>!{x}</li>} } }</ul>}\n"
        "print(page)\n"
    );
    EXPECT_EQ(output, "<ul><li>&lt;a&gt;</li><li>&lt;b&gt;</li></ul>\n");
}

TEST(CodeGenE2E, TemplateBlockEmptyLoop) {
    auto output = compileAndRun(
        "items: list[str] = []\n"
        "y: str = template {[!{ for x in items { :{!{x}} } }]}\n"
        "print(y)\n"
    );
    EXPECT_EQ(output, "[]\n");
}

TEST(CodeGenTest, TemplateBlockEmitsListAndJoinIR) {
    auto ir = generateIR(
        "items: list[str] = [\"a\"]\n"
        "y: str = template {!{ for x in items { :{!{x}} } }}\n"
    );
    EXPECT_NE(ir.find("dragon_list_new_ptr"), std::string::npos)
        << "Block mode must allocate a list[str] buffer\nIR:\n" << ir;
    EXPECT_NE(ir.find("dragon_list_append_ptr"), std::string::npos)
        << "Block mode must append :{} fragments to the buffer\nIR:\n" << ir;
    EXPECT_NE(ir.find("dragon_str_join_ptr"), std::string::npos)
        << "Block mode must join the buffer to a single string\nIR:\n" << ir;
}

TEST(CodeGenE2E, TemplateSpreadOperator) {
    auto output = compileAndRun(
        "items: list[str] = [\"foo\", \"bar\", \"baz\"]\n"
        "y: str = template {[!{*items}]}\n"
        "print(y)\n"
    );
    EXPECT_EQ(output, "[foobarbaz]\n");
}

TEST(CodeGenE2E, TemplateJoinFilterEmpty) {
    auto output = compileAndRun(
        "items: list[str] = [\"a\", \"b\", \"c\"]\n"
        "y: str = template {!{items | join}}\n"
        "print(y)\n"
    );
    EXPECT_EQ(output, "abc\n");
}

TEST(CodeGenE2E, TemplateJoinFilterWithSeparator) {
    auto output = compileAndRun(
        "items: list[str] = [\"a\", \"b\", \"c\"]\n"
        "y: str = template {!{items | join(\", \")}}\n"
        "print(y)\n"
    );
    EXPECT_EQ(output, "a, b, c\n");
}

TEST(CodeGenE2E, TemplateJoinFilterWithExprSeparator) {
    auto output = compileAndRun(
        "items: list[str] = [\"x\", \"y\"]\n"
        "sep: str = \" | \"\n"
        "y: str = template {!{items | join(sep)}}\n"
        "print(y)\n"
    );
    EXPECT_EQ(output, "x | y\n");
}

TEST(CodeGenTest, TemplateSpreadDesugarsToJoinIR) {
    auto ir = generateIR(
        "items: list[str] = [\"a\"]\n"
        "y: str = template {!{*items}}\n"
    );
    EXPECT_NE(ir.find("dragon_str_join_ptr"), std::string::npos)
        << "Spread must lower to dragon_str_join_ptr\nIR:\n" << ir;
}

TEST(CodeGenE2E, TemplateBlockNestedLoops) {
    auto output = compileAndRun(
        "rows: list[str] = [\"r1\", \"r2\"]\n"
        "cols: list[str] = [\"c1\", \"c2\"]\n"
        "y: str = template {!{\n"
        "  for r in rows {\n"
        "    :{[!{r}:}\n"
        "    for c in cols {\n"
        "      :{!{c},}\n"
        "    }\n"
        "    :{]}\n"
        "  }\n"
        "}}\n"
        "print(y)\n"
    );
    EXPECT_EQ(output, "[r1:c1,c2,][r2:c1,c2,]\n");
}

TEST(CodeGenE2E, TemplateBlockTypedWithInheritance) {
    auto output = compileAndRun(
        TPL_BASE +
        "class HTML(Template) {\n"
        "  def(inner: str) { self._inner = inner }\n"
        "  @staticmethod\n"
        "  def escape(s: str) -> str {\n"
        "    return s.replace(\"<\", \"&lt;\").replace(\">\", \"&gt;\")\n"
        "  }\n"
        "}\n"
        "tags: list[str] = [\"<b>\", \"<i>\"]\n"
        "page: HTML = template[HTML] {<p>!{ for t in tags { :{[!{t}]} } }</p>}\n"
        "print(page)\n"
    );
    EXPECT_EQ(output, "<p>[&lt;b&gt;][&lt;i&gt;]</p>\n");
}

TEST(CodeGenE2E, TemplateBlockSpreadInsideBlock) {
    auto output = compileAndRun(
        "y: str = template {!{\n"
        "  parts: list[str] = []\n"
        "  for i in range(3) {\n"
        "    parts.append(str(i))\n"
        "  }\n"
        "  :{!{*parts}}\n"
        "}}\n"
        "print(y)\n"
    );
    EXPECT_EQ(output, "012\n");
}

TEST(ParserTest, ContentAliasOutsideInterpolationIsSyntaxError) {
    auto diags = parseErrors("y: str = :{ this is bad }\n");
    EXPECT_FALSE(diags.empty())
        << "Top-level :{ ... } must be a syntax error";
}

static const std::string SQL_TPL_BASE =
    "class Template {\n"
    "    def(inner: str) { self._inner = inner }\n"
    "    @staticmethod\n"
    "    def escape(s: str) -> str { return s }\n"
    "    def __str__() -> str { return self._inner }\n"
    "}\n"
    "class SQL(Template) {\n"
    "    def(canonical: str, phash: int, params: list[Any]) {\n"
    "        self.canonical = canonical\n"
    "        self.hash = phash\n"
    "        self.params = params\n"
    "    }\n"
    "    def __str__() -> str { return self.canonical }\n"
    "    @staticmethod\n"
    "    def build(canonical: str, params: list[Any]) -> SQL {\n"
    "        return SQL(canonical, 0, params)\n"
    "    }\n"
    "}\n";

TEST(CodeGenE2E, SqlTemplateParamExtraction) {
    auto output = compileAndRun(
        SQL_TPL_BASE +
        "min_id: int = 5\n"
        "name: str = \"O'Brien\"\n"
        "q: SQL = template[SQL] {select * from t where id > !{min_id} and name = !{name}}\n"
        "print(q.canonical)\n"
        "print(len(q.params))\n"
        "print(q.params[0])\n"
        "print(q.params[1])\n"
    );
    EXPECT_EQ(output,
        "select * from t where id > $$0 and name = $$1\n2\n5\nO'Brien\n");
}

TEST(CodeGenE2E, SqlTemplateInjectionSafety) {
    auto output = compileAndRun(
        SQL_TPL_BASE +
        "evil: str = \"'; DROP TABLE users; --\"\n"
        "q: SQL = template[SQL] {select * from t where name = !{evil}}\n"
        "print(q.canonical)\n"
        "print(q.params[0])\n"
    );
    EXPECT_EQ(output,
        "select * from t where name = $$0\n'; DROP TABLE users; --\n");
}

TEST(CodeGenE2E, SqlTemplateZeroParams) {
    auto output = compileAndRun(
        SQL_TPL_BASE +
        "q: SQL = template[SQL] {select count(*) from t}\n"
        "print(q.canonical)\n"
        "print(len(q.params))\n"
    );
    EXPECT_EQ(output, "select count(*) from t\n0\n");
}

TEST(CodeGenE2E, SqlTemplateMixedParamTypes) {
    auto output = compileAndRun(
        SQL_TPL_BASE +
        "a: int = 7\n"
        "b: float = 3.5\n"
        "c: bool = True\n"
        "d: str = \"hi\"\n"
        "q: SQL = template[SQL] {vals !{a} !{b} !{c} !{d}}\n"
        "print(q.canonical)\n"
        "print(q.params[0])\n"
        "print(q.params[1])\n"
        "print(q.params[2])\n"
        "print(q.params[3])\n"
    );
    EXPECT_EQ(output, "vals $$0 $$1 $$2 $$3\n7\n3.5\nTrue\nhi\n");
}

TEST(CodeGenTest, SqlTemplateParamExtractionIR) {
    auto ir = generateIR(
        SQL_TPL_BASE +
        "x: int = 1\n"
        "q: SQL = template[SQL] {select * from t where id = !{x}}\n"
    );
    EXPECT_NE(ir.find("dragon_list_box_new"), std::string::npos) << ir;
    EXPECT_NE(ir.find("dragon_list_box_append"), std::string::npos);
    EXPECT_NE(ir.find("SQL_new"), std::string::npos);
    EXPECT_NE(ir.find("$$0"), std::string::npos);
}

TEST(CodeGenTest, SqlTemplateInternsIdenticalCanonical) {
    auto ir = generateIR(
        SQL_TPL_BASE +
        "x: int = 1\n"
        "y: int = 2\n"
        "q1: SQL = template[SQL] {select * from t where id = !{x}}\n"
        "q2: SQL = template[SQL] {select * from t where id = !{y}}\n"
    );
    const std::string needle = "select * from t where id = $$0";
    size_t count = 0, pos = 0;
    while ((pos = ir.find(needle, pos)) != std::string::npos) {
        count++; pos += needle.size();
    }
    EXPECT_EQ(count, 1u)
        << "identical canonical must be interned to one global\nIR:\n" << ir;
}
