#include "CodeGenTestHelpers.h"

//===----------------------------------------------------------------------===//
// Module IR Tests
//===----------------------------------------------------------------------===//

TEST(CodeGenTest, EmptyModule) {
    auto ir = generateIR("");
    EXPECT_NE(ir.find("define i32 @main("), std::string::npos);
    EXPECT_NE(ir.find("ret i32 0"), std::string::npos);
}

TEST(CodeGenTest, ModuleVerifies) {
    auto module = parse("x: int = 42\nprint(x)");
    ASSERT_NE(module, nullptr);
    Sema sema;
    sema.analyze(*module);
    TypeChecker tc;
    tc.check(*module);
    CodeGen codegen;
    EXPECT_TRUE(codegen.generate(*module));
    // Module should verify
    std::string err;
    llvm::raw_string_ostream errStream(err);
    EXPECT_FALSE(llvm::verifyModule(*codegen.getLLVMModule(), &errStream));
}

TEST(CodeGenTest, WriteIR) {
    auto module = parse("print(42)");
    ASSERT_NE(module, nullptr);
    Sema sema;
    sema.analyze(*module);
    TypeChecker tc;
    tc.check(*module);
    CodeGen codegen;
    ASSERT_TRUE(codegen.generate(*module));

    std::string irFile = "/tmp/dragon_ir_test_" + std::to_string(getpid()) + ".ll";
    EXPECT_TRUE(codegen.writeIR(irFile));

    std::ifstream f(irFile);
    ASSERT_TRUE(f.good());
    std::stringstream ss;
    ss << f.rdbuf();
    EXPECT_NE(ss.str().find("define i32 @main("), std::string::npos);
    std::remove(irFile.c_str());
}

TEST(CodeGenTest, CompileToObject) {
    auto module = parse("print(42)");
    ASSERT_NE(module, nullptr);
    Sema sema;
    sema.analyze(*module);
    TypeChecker tc;
    tc.check(*module);
    CodeGen codegen;
    ASSERT_TRUE(codegen.generate(*module));

    std::string objFile = "/tmp/dragon_obj_test_" + std::to_string(getpid()) + ".o";
    EXPECT_TRUE(codegen.compileToObject(objFile));

    // Check object file exists and has nonzero size
    std::ifstream f(objFile, std::ios::binary | std::ios::ate);
    EXPECT_TRUE(f.good());
    EXPECT_GT(f.tellg(), 0);
    std::remove(objFile.c_str());
}

TEST(CodeGenTest, ModuleGlobalIR) {
    // Module-level AnnAssignStmt should create @global.* in IR
    auto ir = generateIR(
        "x: int = 42\n"
        "def show() {\n"
        "  print(x)\n"
        "}\n"
        "show()\n"
    );
    EXPECT_NE(ir.find("@global.x"), std::string::npos);
}

// --- Multi-file CodeGen Tests ---

TEST(CodeGenIR, MultiFileForwardDecl) {
    // Dep module defines a utility function
    auto depModule = parse("def helper() -> int {\n  return 42\n}\n");
    ASSERT_TRUE(depModule != nullptr);
    Sema sema1;
    sema1.analyze(*depModule);
    TypeChecker tc1;
    tc1.check(*depModule);

    // Entry module has its own top-level code
    auto entryModule = parse("x: int = 10\nprint(x)\n");
    ASSERT_TRUE(entryModule != nullptr);
    Sema sema2;
    sema2.analyze(*entryModule);
    TypeChecker tc2;
    tc2.check(*entryModule);

    CodeGen codegen;
    std::vector<dragon::Module*> deps = {depModule.get()};
    ASSERT_TRUE(codegen.generate(*entryModule, deps));

    std::string ir;
    llvm::raw_string_ostream os(ir);
    codegen.getLLVMModule()->print(os, nullptr);

    // dep's helper function should be defined
    EXPECT_NE(ir.find("@helper"), std::string::npos);
    // entry's main function should exist
    EXPECT_NE(ir.find("@main"), std::string::npos);
}

//===----------------------------------------------------------------------===//
// Module Global E2E Tests - .dr mode (scope-chain resolution)
//===----------------------------------------------------------------------===//

TEST(CodeGenE2E, ModuleGlobalReadFromFunc) {
    // .dr mode: function reads module-level variable without `global` keyword
    auto output = compileAndRun(
        "x: int = 10\n"
        "def show() {\n"
        "  print(x)\n"
        "}\n"
        "show()\n"
    );
    EXPECT_EQ(output, "10\n");
}

TEST(CodeGenE2E, ModuleGlobalWriteFromFunc) {
    // .dr mode: function writes module-level variable (declared `global` first)
    auto output = compileAndRun(
        "x: int = 10\n"
        "def bump() {\n"
        "  global x\n"
        "  x = 20\n"
        "}\n"
        "bump()\n"
        "print(x)\n"
    );
    EXPECT_EQ(output, "20\n");
}

TEST(CodeGenE2E, ModuleGlobalAugAssign) {
    // .dr mode: function uses += on a module-level variable (declared `global` first)
    auto output = compileAndRun(
        "counter: int = 0\n"
        "def inc() {\n"
        "  global counter\n"
        "  counter += 1\n"
        "}\n"
        "inc()\n"
        "inc()\n"
        "inc()\n"
        "print(counter)\n"
    );
    EXPECT_EQ(output, "3\n");
}

TEST(CodeGenE2E, ModuleGlobalString) {
    // .dr mode: function reads a module-level string
    auto output = compileAndRun(
        "name: str = \"Dragon\"\n"
        "def greet() {\n"
        "  print(name)\n"
        "}\n"
        "greet()\n"
    );
    EXPECT_EQ(output, "Dragon\n");
}

TEST(CodeGenE2E, ModuleGlobalBool) {
    // .dr mode: function reads a module-level bool
    auto output = compileAndRun(
        "ready: bool = True\n"
        "def check() {\n"
        "  if ready {\n"
        "    print(1)\n"
        "  }\n"
        "}\n"
        "check()\n"
    );
    EXPECT_EQ(output, "1\n");
}

TEST(CodeGenE2E, ModuleGlobalMultipleFuncs) {
    // .dr mode: multiple functions share a module-level variable
    auto output = compileAndRun(
        "val: int = 0\n"
        "def set_val() {\n"
        "  global val\n"
        "  val = 42\n"
        "}\n"
        "def get_val() {\n"
        "  print(val)\n"
        "}\n"
        "set_val()\n"
        "get_val()\n"
    );
    EXPECT_EQ(output, "42\n");
}

TEST(CodeGenE2E, ModuleGlobalLocalShadow) {
    // .dr mode: local declaration (with type) shadows module global
    auto output = compileAndRun(
        "x: int = 10\n"
        "def shadow() {\n"
        "  x: int = 99\n"
        "  print(x)\n"
        "}\n"
        "shadow()\n"
        "print(x)\n"
    );
    EXPECT_EQ(output, "99\n10\n");
}

//===----------------------------------------------------------------------===//
// Module Global E2E Tests - .py mode (global keyword)
//===----------------------------------------------------------------------===//

TEST(CodeGenE2E, PyGlobalKeyword) {
    // .py mode: function uses `global x` to write module-level variable
    auto output = compileAndRunPy(
        "x: int = 10\n"
        "def bump():\n"
        "    global x\n"
        "    x = 20\n"
        "bump()\n"
        "print(x)\n"
    );
    EXPECT_EQ(output, "20\n");
}

TEST(CodeGenE2E, PyGlobalRead) {
    // .py mode: function uses `global x` to read module-level variable
    auto output = compileAndRunPy(
        "x: int = 10\n"
        "def show():\n"
        "    global x\n"
        "    print(x)\n"
        "show()\n"
    );
    EXPECT_EQ(output, "10\n");
}

//===----------------------------------------------------------------------===//
// HelloWorld E2E
//===----------------------------------------------------------------------===//

TEST(CodeGenE2E, HelloWorld) {
    auto output = compileAndRun("print(\"Hello, World!\")");
    EXPECT_EQ(output, "Hello, World!\n");
}

//===----------------------------------------------------------------------===//
// HTTP E2E Tests
//===----------------------------------------------------------------------===//

TEST(CodeGenTest, HttpParseRequestE2E) {
    // Build HTTP request with actual CRLF bytes using str concat
    auto out = compileAndRun(
        "extern \"C\" def dragon_http_parse_request(buf: str, length: int) -> ptr\n"
        "extern \"C\" def dragon_http_parsed_method(handle: ptr) -> str\n"
        "extern \"C\" def dragon_http_parsed_url(handle: ptr) -> str\n"
        "extern \"C\" def dragon_http_parsed_body(handle: ptr) -> str\n"
        "extern \"C\" def dragon_http_parsed_header_count(handle: ptr) -> int\n"
        "extern \"C\" def dragon_http_parsed_header_key(handle: ptr, idx: int) -> str\n"
        "extern \"C\" def dragon_http_parsed_header_value(handle: ptr, idx: int) -> str\n"
        "extern \"C\" def dragon_http_parsed_ok(handle: ptr) -> int\n"
        "extern \"C\" def dragon_http_parsed_free(handle: ptr)\n"
        "\n"
        "const crlf: str = \"\\r\\n\"\n"
        "const raw: str = \"GET /hello?name=world HTTP/1.1\" + crlf + \"Host: localhost\" + crlf + \"Content-Type: text/plain\" + crlf + crlf\n"
        "const parsed: ptr = dragon_http_parse_request(raw, len(raw))\n"
        "print(dragon_http_parsed_ok(parsed))\n"
        "print(dragon_http_parsed_method(parsed))\n"
        "print(dragon_http_parsed_url(parsed))\n"
        "print(dragon_http_parsed_header_count(parsed))\n"
        "print(dragon_http_parsed_header_key(parsed, 0))\n"
        "print(dragon_http_parsed_header_value(parsed, 0))\n"
        "print(dragon_http_parsed_header_key(parsed, 1))\n"
        "print(dragon_http_parsed_header_value(parsed, 1))\n"
        "dragon_http_parsed_free(parsed)\n"
    );
    EXPECT_EQ(out, "1\nGET\n/hello?name=world\n2\nhost\nlocalhost\ncontent-type\ntext/plain\n");
}

TEST(CodeGenTest, HttpBuildResponseE2E) {
    auto out = compileAndRun(
        "extern \"C\" def dragon_http_build_response(status: int, headers: str, body: str) -> str\n"
        "extern \"C\" def dragon_str_len(s: str) -> int\n"
        "\n"
        "const crlf: str = \"\\r\\n\"\n"
        "const hdrs: str = \"content-type: text/plain\" + crlf + \"content-length: 5\" + crlf\n"
        "const resp: str = dragon_http_build_response(200, hdrs, \"hello\")\n"
        "# Check length - response should contain status line + headers + blank line + body\n"
        "print(dragon_str_len(resp))\n"
    );
    // HTTP/1.1 200 OK\r\n + headers + \r\n + hello = 69
    EXPECT_EQ(out, "69\n");
}

TEST(CodeGenTest, HttpParsePostWithBodyE2E) {
    auto out = compileAndRun(
        "extern \"C\" def dragon_http_parse_request(buf: str, length: int) -> ptr\n"
        "extern \"C\" def dragon_http_parsed_method(handle: ptr) -> str\n"
        "extern \"C\" def dragon_http_parsed_url(handle: ptr) -> str\n"
        "extern \"C\" def dragon_http_parsed_body(handle: ptr) -> str\n"
        "extern \"C\" def dragon_http_parsed_ok(handle: ptr) -> int\n"
        "extern \"C\" def dragon_http_parsed_free(handle: ptr)\n"
        "\n"
        "const crlf: str = \"\\r\\n\"\n"
        "const raw: str = \"POST /api/data HTTP/1.1\" + crlf + \"Host: localhost\" + crlf + \"Content-Length: 13\" + crlf + crlf + \"{\\\"key\\\":\\\"val\\\"}\"\n"
        "const parsed: ptr = dragon_http_parse_request(raw, len(raw))\n"
        "print(dragon_http_parsed_ok(parsed))\n"
        "print(dragon_http_parsed_method(parsed))\n"
        "print(dragon_http_parsed_url(parsed))\n"
        "print(dragon_http_parsed_body(parsed))\n"
        "dragon_http_parsed_free(parsed)\n"
    );
    EXPECT_EQ(out, "1\nPOST\n/api/data\n{\"key\":\"val\"}\n");
}

TEST(CodeGenTest, NonBlockingSendRecvE2E) {
    // Test dragon_nb_send/recv via a loopback socket pair
    auto out = compileAndRun(
        "extern \"C\" def socket(domain: intc, type: intc, protocol: intc) -> intc\n"
        "extern \"C\" def bind(fd: intc, addr: ptr, addrlen: intc) -> intc\n"
        "extern \"C\" def listen(fd: intc, backlog: intc) -> intc\n"
        "extern \"C\" def accept(fd: intc, addr: ptr, addrlen: ptr) -> intc\n"
        "extern \"C\" def connect(fd: intc, addr: ptr, addrlen: intc) -> intc\n"
        "extern \"C\" def close(fd: intc) -> intc\n"
        "extern \"C\" def dragon_sockaddr_in_new(port: intc, addr: str) -> ptr\n"
        "extern \"C\" def dragon_sockaddr_in_size() -> intc\n"
        "extern \"C\" def dragon_setsockopt_reuse(fd: int)\n"
        "extern \"C\" def dragon_nb_send(fd: int, buf: str, length: int) -> int\n"
        "extern \"C\" def dragon_nb_recv_str(fd: int, max_len: int) -> str\n"
        "extern \"C\" def dragon_close_fd(fd: int)\n"
        "extern \"C\" def malloc(size: int) -> ptr\n"
        "extern \"C\" def free(p: ptr)\n"
        "\n"
        "const sfd: int = socket(2, 1, 0)\n"
        "dragon_setsockopt_reuse(sfd)\n"
        "const addr: ptr = dragon_sockaddr_in_new(19877, \"127.0.0.1\")\n"
        "const addrlen: int = dragon_sockaddr_in_size()\n"
        "bind(sfd, addr, addrlen)\n"
        "listen(sfd, 1)\n"
        "\n"
        "const cfd: int = socket(2, 1, 0)\n"
        "const addr2: ptr = dragon_sockaddr_in_new(19877, \"127.0.0.1\")\n"
        "connect(cfd, addr2, addrlen)\n"
        "\n"
        "const afd: int = accept(sfd, none, none)\n"
        "\n"
        "dragon_nb_send(cfd, \"hello\", 5)\n"
        "const msg: str = dragon_nb_recv_str(afd, 1024)\n"
        "print(msg)\n"
        "\n"
        "close(afd)\n"
        "close(cfd)\n"
        "close(sfd)\n"
        "free(addr)\n"
        "free(addr2)\n"
    );
    EXPECT_EQ(out, "hello\n");
}

//===----------------------------------------------------------------------===//
// Driver LLVM Backend Integration Tests
//===----------------------------------------------------------------------===//

#include "dragon/Driver.h"
#include <sys/wait.h>

// Helper: write source to temp .dr file, build via Driver with LLVM backend,
// execute the resulting binary, return stdout
static std::string driverBuildAndRun(const std::string& source) {
    std::string srcFile = "/tmp/dragon_drvtest_" + std::to_string(getpid()) + ".dr";
    std::string exe = "/tmp/dragon_drvtest_" + std::to_string(getpid());
    std::string outFile = "/tmp/dragon_drvtest_" + std::to_string(getpid()) + ".out";

    {
        std::ofstream f(srcFile);
        f << source;
    }

    dragon::DriverOptions opts;
    opts.action = dragon::DriverOptions::Action::Build;
    opts.inputFiles = {srcFile};
    opts.outputFile = exe;

    dragon::Driver driver;
    int buildResult = driver.run(opts);
    std::remove(srcFile.c_str());
    if (buildResult != 0) {
        std::remove(exe.c_str());
        return "<build failed>";
    }

    std::string cmd = exe + " > " + outFile + " 2>&1";
    int result = std::system(cmd.c_str());
    (void)result;
    std::remove(exe.c_str());

    std::ifstream f(outFile);
    std::stringstream ss;
    ss << f.rdbuf();
    std::remove(outFile.c_str());
    return ss.str();
}

TEST(DriverLLVM, HelloWorld) {
    auto output = driverBuildAndRun("print(\"Hello from LLVM!\")");
    EXPECT_EQ(output, "Hello from LLVM!\n");
}

TEST(DriverLLVM, ArithmeticAndFunction) {
    auto output = driverBuildAndRun(
        "def square(x: int) -> int {\n"
        "  return x * x\n"
        "}\n"
        "print(square(7))"
    );
    EXPECT_EQ(output, "49\n");
}

//===----------------------------------------------------------------------===//
// Regression: HTTP response buffer undercount
//
// Pre-fix: dragon_http_build_response computed `total` from a hand-rolled
// formula (`32 + strlen(reason) + headers_len + 4 + body_len`) and allocated
// based on that. snprintf's return (`off`) and the subsequent memcpy of body
// could write past the allocation when reason/headers were unusually long.
// The fix uses `snprintf(NULL, 0, ...)` to compute the exact prefix size,
// then allocates `prefix_len + body_len + 1`.
//
// Tests: build responses with long headers / various statuses and verify
// the result length is exactly correct (no truncation, no over-write).
//===----------------------------------------------------------------------===//

TEST(CodeGenE2E, HttpBuildResponseLongHeaders) {
    // 100 copies of "x-custom: value\r\n" = 1700-char headers section.
    // Pre-fix the 32-char fudge factor was enough for typical headers but
    // we still want to confirm the fix handles arbitrary sizes correctly.
    auto out = compileAndRun(
        "extern \"C\" def dragon_http_build_response(status: int, headers: str, body: str) -> str\n"
        "extern \"C\" def dragon_str_len(s: str) -> int\n"
        "hdrs: str = \"\"\n"
        "for i in range(100) {\n"
        "  hdrs = hdrs + \"x-custom: value\\r\\n\"\n"
        "}\n"
        "resp: str = dragon_http_build_response(200, hdrs, \"hello\")\n"
        "print(dragon_str_len(resp))\n"
    );
    // "HTTP/1.1 200 OK\r\n" (17) + 100*17 (1700) + "\r\n" (2) + "hello" (5) = 1724
    EXPECT_EQ(out, "1724\n");
}

TEST(CodeGenE2E, HttpBuildResponseEmptyBody) {
    // Body is empty string - body_len=0, but the prefix must be sized correctly.
    auto out = compileAndRun(
        "extern \"C\" def dragon_http_build_response(status: int, headers: str, body: str) -> str\n"
        "extern \"C\" def dragon_str_len(s: str) -> int\n"
        "hdrs: str = \"content-length: 0\\r\\n\"\n"
        "resp: str = dragon_http_build_response(204, hdrs, \"\")\n"
        "print(dragon_str_len(resp))\n"
    );
    // "HTTP/1.1 204 No Content\r\n" (25) + "content-length: 0\r\n" (19) + "\r\n" (2) = 46
    EXPECT_EQ(out, "46\n");
}

TEST(CodeGenE2E, HttpBuildResponseLargeBody) {
    // 10KB body + small headers - exercises memcpy(buf + off, body, body_len)
    // path. With the fix, off equals exactly prefix_len, so the body always
    // lands at a safe offset.
    auto out = compileAndRun(
        "extern \"C\" def dragon_http_build_response(status: int, headers: str, body: str) -> str\n"
        "extern \"C\" def dragon_str_len(s: str) -> int\n"
        "body: str = \"\"\n"
        "for i in range(1024) {\n"
        "  body = body + \"0123456789\"\n"
        "}\n"
        "hdrs: str = \"content-length: 10240\\r\\n\"\n"
        "resp: str = dragon_http_build_response(200, hdrs, body)\n"
        "print(dragon_str_len(resp))\n"
    );
    // "HTTP/1.1 200 OK\r\n" (17) + "content-length: 10240\r\n" (23) + "\r\n" (2) + 10240 = 10282
    EXPECT_EQ(out, "10282\n");
}

TEST(CodeGenE2E, HttpBuildResponseLoopBounded) {
    // Stress: build many responses in a tight loop. Pre-fix overruns would
    // corrupt the heap and likely crash; post-fix this is bounded.
    auto out = compileAndRun(
        "extern \"C\" def dragon_http_build_response(status: int, headers: str, body: str) -> str\n"
        "extern \"C\" def dragon_str_len(s: str) -> int\n"
        "hdrs: str = \"content-type: text/plain\\r\\ncontent-length: 5\\r\\n\"\n"
        "last_len: int = 0\n"
        "for i in range(5000) {\n"
        "  resp: str = dragon_http_build_response(200, hdrs, \"hello\")\n"
        "  last_len = dragon_str_len(resp)\n"
        "}\n"
        "print(last_len)\n"
    );
    // Same as the existing 69-char baseline, repeated 5000x without crash.
    EXPECT_EQ(out, "69\n");
}

TEST(CodeGenE2E, HttpBuildResponseUnknownStatus) {
    // Status code with no entry in the reason switch - falls through to "Unknown".
    auto out = compileAndRun(
        "extern \"C\" def dragon_http_build_response(status: int, headers: str, body: str) -> str\n"
        "extern \"C\" def dragon_str_len(s: str) -> int\n"
        "resp: str = dragon_http_build_response(999, \"\\r\\n\", \"x\")\n"
        "print(dragon_str_len(resp))\n"
    );
    // "HTTP/1.1 999 Unknown\r\n" (22) + "\r\n" (2) + "\r\n" (2) + "x" (1) = 27
    EXPECT_EQ(out, "27\n");
}
