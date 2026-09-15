#include "CodeGenTestHelpers.h"
#include "CodeBlock.h"

static std::string code(const std::string& block) {
    return extractCode("CodeGenModuleTest.md", block);
}

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

    std::ifstream f(objFile, std::ios::binary | std::ios::ate);
    EXPECT_TRUE(f.good());
    EXPECT_GT(f.tellg(), 0);
    std::remove(objFile.c_str());
}

TEST(CodeGenTest, ModuleGlobalIR) {
    auto ir = generateIR(code("module_global_ir"));
    EXPECT_NE(ir.find("@global.x"), std::string::npos);
}

TEST(CodeGenIR, MultiFileForwardDecl) {
    auto depModule = parse("def helper() -> int {\n  return 42\n}\n");
    ASSERT_TRUE(depModule != nullptr);
    Sema sema1;
    sema1.analyze(*depModule);
    TypeChecker tc1;
    tc1.check(*depModule);

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

    EXPECT_NE(ir.find("@helper"), std::string::npos);
    EXPECT_NE(ir.find("@main"), std::string::npos);
}

TEST(CodeGenE2E, ModuleGlobalReadFromFunc) {
    auto output = compileAndRun(code("module_global_read_from_func"));
    EXPECT_EQ(output, "10\n");
}

TEST(CodeGenE2E, ModuleGlobalWriteFromFunc) {
    auto output = compileAndRun(code("module_global_write_from_func"));
    EXPECT_EQ(output, "20\n");
}

TEST(CodeGenE2E, ModuleGlobalAugAssign) {
    auto output = compileAndRun(code("module_global_aug_assign"));
    EXPECT_EQ(output, "3\n");
}

TEST(CodeGenE2E, ModuleGlobalString) {
    auto output = compileAndRun(code("module_global_string"));
    EXPECT_EQ(output, "Dragon\n");
}

TEST(CodeGenE2E, ModuleGlobalBool) {
    auto output = compileAndRun(code("module_global_bool"));
    EXPECT_EQ(output, "1\n");
}

TEST(CodeGenE2E, ModuleGlobalMultipleFuncs) {
    auto output = compileAndRun(code("module_global_multiple_funcs"));
    EXPECT_EQ(output, "42\n");
}

TEST(CodeGenE2E, ModuleGlobalLocalShadow) {
    auto output = compileAndRun(code("module_global_local_shadow"));
    EXPECT_EQ(output, "99\n10\n");
}

TEST(CodeGenE2E, PyGlobalKeyword) {
    auto output = compileAndRunPy(code("py_global_keyword"));
    EXPECT_EQ(output, "20\n");
}

TEST(CodeGenE2E, PyGlobalRead) {
    auto output = compileAndRunPy(code("py_global_read"));
    EXPECT_EQ(output, "10\n");
}

TEST(CodeGenE2E, HelloWorld) {
    auto output = compileAndRun("print(\"Hello, World!\")");
    EXPECT_EQ(output, "Hello, World!\n");
}

TEST(CodeGenTest, HttpParseRequestE2E) {
    auto out = compileAndRun(code("http_parse_request_e2_e"));
    EXPECT_EQ(out, "1\nGET\n/hello?name=world\n2\nhost\nlocalhost\ncontent-type\ntext/plain\n");
}

TEST(CodeGenTest, HttpBuildResponseE2E) {
    auto out = compileAndRun(code("http_build_response_e2_e"));
    EXPECT_EQ(out, "69\n");
}

TEST(CodeGenTest, HttpParseHeadWithDeclaredBodyE2E) {
    auto out = compileAndRun(code("http_parse_head_with_declared_body_e2_e"));
    EXPECT_EQ(out, "1\nPOST\n/api/data\n1.1\n");
}

TEST(CodeGenTest, NonBlockingSendRecvE2E) {
    auto out = compileAndRun(code("non_blocking_send_recv_e2_e"));
    EXPECT_EQ(out, "hello\n");
}

#include "dragon/Driver.h"
#ifndef _WIN32
  #include <sys/wait.h>
#endif

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
    auto output = driverBuildAndRun(code("arithmetic_and_function"));
    EXPECT_EQ(output, "49\n");
}

TEST(CodeGenE2E, HttpBuildResponseLongHeaders) {
    auto out = compileAndRun(code("http_build_response_long_headers"));
    EXPECT_EQ(out, "1724\n");
}

TEST(CodeGenE2E, HttpBuildResponseEmptyBody) {
    auto out = compileAndRun(code("http_build_response_empty_body"));
    EXPECT_EQ(out, "46\n");
}

TEST(CodeGenE2E, HttpBuildResponseLargeBody) {
    auto out = compileAndRun(code("http_build_response_large_body"));
    EXPECT_EQ(out, "10282\n");
}

TEST(CodeGenE2E, HttpBuildResponseLoopBounded) {
    auto out = compileAndRun(code("http_build_response_loop_bounded"));
    EXPECT_EQ(out, "69\n");
}

TEST(CodeGenE2E, HttpBuildResponseUnknownStatus) {
    auto out = compileAndRun(code("http_build_response_unknown_status"));
    EXPECT_EQ(out, "27\n");
}
