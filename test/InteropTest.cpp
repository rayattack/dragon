#include <gtest/gtest.h>
#include "TestHelpers.h"
#include "dragon/CodeGen.h"
#include "dragon/Sema.h"
#include "dragon/TypeChecker.h"
#include "dragon/ModuleResolver.h"
#include "dragon/TypeHintEnforcer.h"
#include "dragon/Platform.h"
#include <atomic>
#include <filesystem>
#include <fstream>
#include <cstdlib>
#include <cstdio>
#if defined(_WIN32)
  #include <process.h>
#else
  #include <sys/stat.h>
  #include <unistd.h>
#endif

#include "llvm/Support/TargetSelect.h"

using namespace dragon;
using namespace dragon::test;

// One-time LLVM initialization
struct LLVMInit {
    LLVMInit() {
        llvm::InitializeNativeTarget();
        llvm::InitializeNativeTargetAsmPrinter();
        llvm::InitializeNativeTargetAsmParser();
    }
};
static LLVMInit llvmInit;

// Helper: create a temp directory
static std::string makeTempDir(const std::string& suffix) {
    namespace fs = std::filesystem;
#if defined(_WIN32)
    // mkdtemp is POSIX. Roll our own with GetTempPathA + a counter.
    static std::atomic<int> counter{0};
    auto base = fs::path(dragon::platform::getTempDir())
        / ("dragon_interop_" + suffix + "_" +
           std::to_string(dragon::platform::getProcessId()) + "_" +
           std::to_string(counter.fetch_add(1)));
    std::error_code ec;
    fs::create_directories(base, ec);
    return ec ? std::string{} : base.string();
#else
    std::string tmpl = dragon::platform::getTempDir()
        + "/dragon_interop_" + suffix + "_XXXXXX";
    std::vector<char> buf(tmpl.begin(), tmpl.end());
    buf.push_back('\0');
    char* result = mkdtemp(buf.data());
    return result ? std::string(result) : "";
#endif
}

// Helper: write a file
static void writeFile(const std::string& path, const std::string& content) {
    std::ofstream out(path);
    out << content;
}

// Helper: compile and run a Dragon project via LLVM CodeGen
// Returns the exit code, captures stdout in 'output'
static int compileAndRun(const std::string& dir,
                          const std::string& entryFile,
                          const std::string& source,
                          std::string& output) {
    // Write the entry source to disk too (not just the dep modules). The real
    // driver always runs an entry file that exists on disk; D045 package-privacy
    // detection (TypeChecker `packageKeyOf`) decides "same package" by checking
    // whether the package-root file (e.g. `app/app.dr`) exists on disk. Parsing
    // the entry only in-memory made that check fail, so a legitimate
    // same-package protected import was wrongly rejected once the harness gates
    // on TypeChecker errors. Writing it makes the harness faithful.
    writeFile(entryFile, source);

    // Parse entry module
    bool isDragon = entryFile.size() > 3 &&
                    entryFile.substr(entryFile.size() - 3) == ".dr";
    auto tokens = lex(source, isDragon);
    ParserOptions parseOpts;
    parseOpts.isDragonFile = isDragon;
    parseOpts.requireTypes = isDragon;
    parseOpts.filename = entryFile;
    Parser parser(std::move(tokens), parseOpts);
    auto module = parser.parseModule();
    if (parser.hasErrors()) return -1;

    // Faithful to the real Driver: a front-end (Sema/TypeChecker) error aborts
    // compilation. The entry TypeChecker runs after imports resolve (below);
    // entry Sema runs here (imports bind their names regardless of resolution).
    Sema sema;
    if (!sema.analyze(*module)) return -5;

    // Resolve imports
    ModuleResolverOptions resolverOpts;
    resolverOpts.sourceDir = dir + "/";
#ifdef DRAGON_STDLIB_DIR
    resolverOpts.searchPaths.push_back(DRAGON_STDLIB_DIR);
#endif
    ModuleResolver resolver(resolverOpts);
    auto graph = resolver.buildGraph(*module, entryFile);
    if (graph.hasCycle) return -2;

    // Process dependencies and collect exports for cross-file type checking
    std::vector<Module*> depModules;
    std::unordered_map<std::string,
        std::unordered_map<std::string, std::shared_ptr<Type>>> allExports;
    // D045 -- mirror the Driver: thread each module's source path so member/
    // import privacy can compute packages. Without this the privacy rule would
    // silently fail-open in this test harness (divergent from the real CLI).
    std::unordered_map<std::string, std::string> moduleFilepaths;
    for (auto& mod : graph.modules) moduleFilepaths[mod.name] = mod.filepath;

    for (auto& mod : graph.modules) {
        Sema modSema;
        if (!modSema.analyze(*mod.ast)) return -5;
        TypeChecker modTc;
        for (auto& [modName, exports] : allExports) {
            modTc.registerExternalModule(modName, exports, moduleFilepaths[modName]);
        }
        // D044 cross-module generics - mirror the real Driver (Driver.cpp): a
        // module must see earlier deps' generic TEMPLATES so it can instantiate
        // them. Without this the harness compiled a call to an imported generic
        // (e.g. unittest.dr's assertEqual[T]) without stamping the template, so
        // the body's `!=` never fired and `assertEqual(1, 2)` silently passed -
        // making the interop suite assert on a program the real CLI never
        // produces.
        for (auto* prior : depModules) {
            modTc.registerExternalGenerics(*prior);
        }
        modTc.check(*mod.ast);
        if (modTc.hasErrors()) return -5;
        allExports[mod.name] = modTc.getExports();
        depModules.push_back(mod.ast.get());
    }

    // Re-run TypeChecker on the entry module with cross-file type info, gating
    // on any error. Runs even with no deps so a single-file entry's type errors
    // are caught too (the throwaway pre-resolve entry tc was removed).
    {
        TypeChecker entryTc;
        for (auto& [modName, exports] : allExports) {
            entryTc.registerExternalModule(modName, exports, moduleFilepaths[modName]);
        }
        // D044 cross-module generics - see the dep loop above. The entry module
        // must see every dependency's generic templates so a call like
        // assertEqual[T] gets stamped; this is the exact step Driver.cpp runs.
        for (auto* dep : depModules) {
            entryTc.registerExternalGenerics(*dep);
        }
        entryTc.check(*module);
        if (entryTc.hasErrors()) return -5;
    }

    // Generate LLVM IR
    CodeGenOptions codegenOpts;
#ifdef DRAGON_RUNTIME_LIB
    codegenOpts.runtimeLibPath = DRAGON_RUNTIME_LIB;
#endif
#ifdef DRAGON_SQLITE3_LIB
    codegenOpts.sqlite3LibPath = DRAGON_SQLITE3_LIB;
#endif
#ifdef DRAGON_PCRE2_LIB
    codegenOpts.pcre2LibPath = DRAGON_PCRE2_LIB;
#endif
#ifdef DRAGON_LLHTTP_LIB
    codegenOpts.llhttpLibPath = DRAGON_LLHTTP_LIB;
#endif
#ifdef DRAGON_MBEDTLS_LIB
    codegenOpts.mbedtlsLibPath = DRAGON_MBEDTLS_LIB;
#endif

    CodeGen codegen(codegenOpts);
    if (!codegen.generate(*module, depModules)) return -3;

    // Compile to object and link
    std::string objFile = dir + "/test_output.o";
    std::string exe = dir + "/test_output";

    if (!codegen.compileToObject(objFile)) return -3;
    if (!codegen.linkExecutable(exe, objFile)) {
        std::remove(objFile.c_str());
        return -3;
    }
    std::remove(objFile.c_str());

    // Run and capture output
    std::string runCmd = exe + " 2>&1";
#if defined(_WIN32)
    FILE* pipe = _popen(runCmd.c_str(), "r");
#else
    FILE* pipe = popen(runCmd.c_str(), "r");
#endif
    if (!pipe) return -4;
    char buffer[256];
    while (fgets(buffer, sizeof(buffer), pipe)) {
        output += buffer;
    }
#if defined(_WIN32)
    int exitCode = _pclose(pipe);
#else
    int exitCode = pclose(pipe);
#endif

    // Cleanup
    std::remove(exe.c_str());

    return dragon::platform::getExitCode(exitCode);
}

// Helper: resolve + type-check a multi-file project (entry + deps) the way the
// real Driver does (threading each module's filepath through
// registerExternalModule so D045 privacy is enforced) and return every error
// diagnostic's message. Used for compile-error tests where compileAndRun can't
// help (it ignores type-check errors and proceeds to codegen).
static std::vector<std::string> typeCheckProjectErrors(
        const std::string& dir, const std::string& entryFile,
        const std::string& source) {
    std::vector<std::string> errs;
    bool isDragon = entryFile.size() > 3 &&
                    entryFile.substr(entryFile.size() - 3) == ".dr";
    auto tokens = lex(source, isDragon);
    ParserOptions parseOpts;
    parseOpts.isDragonFile = isDragon;
    parseOpts.requireTypes = isDragon;
    parseOpts.filename = entryFile;
    Parser parser(std::move(tokens), parseOpts);
    auto module = parser.parseModule();
    if (parser.hasErrors()) { errs.push_back("PARSE ERROR"); return errs; }

    ModuleResolverOptions resolverOpts;
    resolverOpts.sourceDir = dir + "/";
#ifdef DRAGON_STDLIB_DIR
    resolverOpts.searchPaths.push_back(DRAGON_STDLIB_DIR);
#endif
    ModuleResolver resolver(resolverOpts);
    auto graph = resolver.buildGraph(*module, entryFile);

    std::unordered_map<std::string,
        std::unordered_map<std::string, std::shared_ptr<Type>>> allExports;
    std::unordered_map<std::string, std::string> fp;
    for (auto& mod : graph.modules) fp[mod.name] = mod.filepath;

    auto collect = [&](const TypeChecker& tc) {
        for (auto& d : tc.diagnostics())
            if (d.level == TypeDiagnostic::Level::Error) errs.push_back(d.message);
    };
    for (auto& mod : graph.modules) {
        Sema s; s.analyze(*mod.ast);
        TypeChecker tc;
        for (auto& [n, e] : allExports) tc.registerExternalModule(n, e, fp[n]);
        tc.check(*mod.ast);
        collect(tc);
        allExports[mod.name] = tc.getExports();
    }
    TypeChecker entryTc;
    for (auto& [n, e] : allExports) entryTc.registerExternalModule(n, e, fp[n]);
    entryTc.check(*module);
    collect(entryTc);
    return errs;
}

// True if any error message contains `needle`.
static bool anyContains(const std::vector<std::string>& errs, const std::string& needle) {
    for (auto& e : errs)
        if (e.find(needle) != std::string::npos) return true;
    return false;
}

// Helper: cleanup temp dir (removes all known files and the dir)
static void cleanupDir(const std::string& dir,
                        const std::vector<std::string>& files) {
    for (auto& f : files) std::remove((dir + "/" + f).c_str());
    std::error_code rmEc; std::filesystem::remove_all(dir, rmEc);
}

//===----------------------------------------------------------------------===//
// .dr imports .dr
//===----------------------------------------------------------------------===//

TEST(InteropTest, DragonImportsDragon) {
    auto dir = makeTempDir("dr_dr");
    ASSERT_FALSE(dir.empty());

    writeFile(dir + "/utils.dr",
        "def double_it(x: int) -> int {\n"
        "    return x * 2\n"
        "}\n"
    );

    std::string source =
        "from utils import double_it\n"
        "print(double_it(21))\n";

    std::string output;
    int exitCode = compileAndRun(dir, dir + "/main.dr", source, output);

    EXPECT_EQ(exitCode, 0);
    EXPECT_NE(output.find("42"), std::string::npos);

    cleanupDir(dir, {"utils.dr"});
}

//===----------------------------------------------------------------------===//
// Cross-module @property regressions:
//  - bare-attribute getter/setter on an IMPORTED class must mangle the
//  symbol with the class's owning module (`<mod>__<Class>_<attr>`), not
//  the bare `<Class>_<attr>`. A class-returning property + str() otherwise
//  aborts LLVM verification with `self` = i64 0 (Attributes.cpp/Assign.cpp).
//  - a property accessed on an EXPRESSION result (chained `b.twin.val`)
//  must invoke the getter too, not fall through to a `0` fallback.
//===----------------------------------------------------------------------===//

TEST(InteropTest, CrossModulePropertyGetterSetter) {
    auto dir = makeTempDir("xmod_property");
    ASSERT_FALSE(dir.empty());

    writeFile(dir + "/shapes.dr",
        "class Box {\n"
        "    _v: int = 0\n"
        "    @property\n"
        "    def val() -> int { return self._v }\n"
        "    @val.setter\n"
        "    def val(n: int) -> None { self._v = n }\n"
        "    @property\n"
        "    def twin() -> Box { return self }\n"
        "    def __str__() -> str { return \"box\" }\n"
        "}\n"
    );

    std::string source =
        "from shapes import Box\n"
        "b: Box = Box()\n"
        "b.val = 7\n"                          // N4: cross-module @property setter
        "print(b.val)\n"                      // N4: cross-module @property getter (scalar)
        "print(str(b.twin))\n"                // N4: class-returning property + str() (the crash case)
        "print(f\"chained={b.twin.val}\")\n"; // N5: property on an expression result (chained)

    std::string output;
    int exitCode = compileAndRun(dir, dir + "/main.dr", source, output);

    EXPECT_EQ(exitCode, 0);
    EXPECT_NE(output.find("box"), std::string::npos);     // N4: did not crash
    EXPECT_NE(output.find("chained=7"), std::string::npos); // N5: chained getter returned 7, not 0

    cleanupDir(dir, {"shapes.dr"});
}

//===----------------------------------------------------------------------===//
// __doc__ across module boundaries: `from mod import f; f.__doc__` resolves
// the imported name through the same alias map (importedFuncAliasesByModule)
// that Call / Assign use. `mod.__doc__` reads the module's lifted docstring.
//===----------------------------------------------------------------------===//

TEST(InteropTest, ImportedFunctionDocstring) {
    auto dir = makeTempDir("docstring_fromimport");
    ASSERT_FALSE(dir.empty());

    writeFile(dir + "/lib.dr",
        "\"\"\"library doc.\"\"\"\n"
        "def hello() -> str {\n"
        "    \"\"\"library hello.\"\"\"\n"
        "    return \"hi\"\n"
        "}\n"
    );

    std::string source =
        "from lib import hello\n"
        "print(hello.__doc__)\n";

    std::string output;
    int exitCode = compileAndRun(dir, dir + "/main.dr", source, output);

    EXPECT_EQ(exitCode, 0);
    EXPECT_NE(output.find("library hello."), std::string::npos);

    cleanupDir(dir, {"lib.dr"});
}

// NOTE: module-qualified call chaining (`mod.func(args).method()`) is covered
// by the dogfooded `.dr` unittest suite (test/dr/test_qualified_chain.dr).

TEST(InteropTest, ImportedModuleDocstring) {
    auto dir = makeTempDir("docstring_moduledoc");
    ASSERT_FALSE(dir.empty());

    writeFile(dir + "/lib.dr",
        "\"\"\"library module docstring.\"\"\"\n"
        "def hi() -> int { return 0 }\n"
    );

    std::string source =
        "import lib\n"
        "print(lib.__doc__)\n";

    std::string output;
    int exitCode = compileAndRun(dir, dir + "/main.dr", source, output);

    EXPECT_EQ(exitCode, 0);
    EXPECT_NE(output.find("library module docstring."), std::string::npos);

    cleanupDir(dir, {"lib.dr"});
}

//===----------------------------------------------------------------------===//
// .dr imports typed .py
//===----------------------------------------------------------------------===//

TEST(InteropTest, DragonImportsTypedPython) {
    auto dir = makeTempDir("dr_py");
    ASSERT_FALSE(dir.empty());

    writeFile(dir + "/pyutils.py",
        "def square(x: int) -> int:\n"
        "    return x * x\n"
    );

    std::string source =
        "from pyutils import square\n"
        "print(square(7))\n";

    std::string output;
    int exitCode = compileAndRun(dir, dir + "/main.dr", source, output);

    EXPECT_EQ(exitCode, 0);
    EXPECT_NE(output.find("49"), std::string::npos);

    cleanupDir(dir, {"pyutils.py"});
}

//===----------------------------------------------------------------------===//
// Single file, no imports
//===----------------------------------------------------------------------===//

TEST(InteropTest, SingleFileNoImports) {
    auto dir = makeTempDir("single");
    ASSERT_FALSE(dir.empty());

    std::string source = "print(100 + 23)\n";

    std::string output;
    int exitCode = compileAndRun(dir, dir + "/main.dr", source, output);

    EXPECT_EQ(exitCode, 0);
    EXPECT_NE(output.find("123"), std::string::npos);

    std::error_code rmEc; std::filesystem::remove_all(dir, rmEc);
}

//===----------------------------------------------------------------------===//
// Diamond import pattern (deduplicated)
//===----------------------------------------------------------------------===//

TEST(InteropTest, DiamondImportCompiles) {
    auto dir = makeTempDir("diamond_e2e");
    ASSERT_FALSE(dir.empty());

    writeFile(dir + "/base.dr",
        "def base_val() -> int {\n"
        "    return 10\n"
        "}\n"
    );
    writeFile(dir + "/left.dr",
        "from base import base_val\n"
        "def left_val() -> int {\n"
        "    return base_val() + 1\n"
        "}\n"
    );
    writeFile(dir + "/right.dr",
        "from base import base_val\n"
        "def right_val() -> int {\n"
        "    return base_val() + 2\n"
        "}\n"
    );

    std::string source =
        "from left import left_val\n"
        "from right import right_val\n"
        "print(left_val() + right_val())\n";

    std::string output;
    int exitCode = compileAndRun(dir, dir + "/main.dr", source, output);

    EXPECT_EQ(exitCode, 0);
    // base_val()=10, left=11, right=12, sum=23
    EXPECT_NE(output.find("23"), std::string::npos);

    cleanupDir(dir, {"base.dr", "left.dr", "right.dr"});
}

//===----------------------------------------------------------------------===//
// Untyped .py import produces "Borders must be secured" error
//===----------------------------------------------------------------------===//

TEST(InteropTest, UntypedPyImportRejected) {
    auto dir = makeTempDir("untyped_py");
    ASSERT_FALSE(dir.empty());

    // Untyped Python module
    writeFile(dir + "/bad_module.py",
        "def process(data):\n"
        "    return data\n"
    );

    // Parse entry module
    std::string source = "from bad_module import process\nprint(process(5))\n";
    auto tokens = lex(source, true);
    ParserOptions parseOpts;
    parseOpts.isDragonFile = true;
    parseOpts.requireTypes = true;
    parseOpts.filename = dir + "/main.dr";
    Parser parser(std::move(tokens), parseOpts);
    auto module = parser.parseModule();
    ASSERT_NE(module, nullptr);

    // Resolve imports
    ModuleResolverOptions resolverOpts;
    resolverOpts.sourceDir = dir + "/";
    ModuleResolver resolver(resolverOpts);
    auto graph = resolver.buildGraph(*module, dir + "/main.dr");

    ASSERT_EQ(graph.modules.size(), 1u);
    ASSERT_FALSE(graph.modules[0].isDragon);

    // TypeHintEnforcer should reject it
    TypeHintEnforcer enforcer;
    bool ok = enforcer.enforce(*graph.modules[0].ast);
    EXPECT_FALSE(ok);

    cleanupDir(dir, {"bad_module.py"});
}

//===----------------------------------------------------------------------===//
// String operations across modules (end-to-end)
//===----------------------------------------------------------------------===//

TEST(InteropTest, StringConcatAcrossModules) {
    auto dir = makeTempDir("str_concat");
    ASSERT_FALSE(dir.empty());

    writeFile(dir + "/greet.dr",
        "def greet(name: str) -> str {\n"
        "    return \"Hello, \" + name\n"
        "}\n"
    );

    std::string source =
        "from greet import greet\n"
        "print(greet(\"Dragon\"))\n";

    std::string output;
    int exitCode = compileAndRun(dir, dir + "/main.dr", source, output);

    EXPECT_EQ(exitCode, 0);
    EXPECT_NE(output.find("Hello, Dragon"), std::string::npos);

    cleanupDir(dir, {"greet.dr"});
}

//===----------------------------------------------------------------------===//
// Function name across modules (end-to-end)
//===----------------------------------------------------------------------===//

TEST(InteropTest, FunctionNameAcrossModules) {
    auto dir = makeTempDir("funcname");
    ASSERT_FALSE(dir.empty());

    writeFile(dir + "/ops.dr",
        "def double(x: int) -> int {\n"
        "    return x * 2\n"
        "}\n"
    );

    std::string source =
        "from ops import double\n"
        "print(double(21))\n";

    std::string output;
    int exitCode = compileAndRun(dir, dir + "/main.dr", source, output);

    EXPECT_EQ(exitCode, 0);
    EXPECT_NE(output.find("42"), std::string::npos);

    cleanupDir(dir, {"ops.dr"});
}

// Two modules each defining a `class Conflict` with identical layout --
// instantiated through the cross-module form `a.Conflict(...)` /
// `b.Conflict(...)`. Without per-module class-symbol mangling, the second
// module's `_new` body gets silently dropped at LLVM linking, both `a` and
// `b` end up sharing one body, and the test would observe a single state
// where both calls modify the same instance state.
TEST(InteropTest, ClassNameAcrossModules) {
    auto dir = makeTempDir("classname");
    ASSERT_FALSE(dir.empty());

    writeFile(dir + "/a.dr",
        "class Conflict {\n"
        "    def(x: int) {\n"
        "        self.x = x\n"
        "    }\n"
        "    def value() -> int {\n"
        "        return self.x\n"
        "    }\n"
        "}\n"
    );

    writeFile(dir + "/b.dr",
        "class Conflict {\n"
        "    def(x: int) {\n"
        "        self.x = x\n"
        "    }\n"
        "    def value() -> int {\n"
        "        return self.x\n"
        "    }\n"
        "}\n"
    );

    std::string source =
        "import a\n"
        "import b\n"
        "ac: a.Conflict = a.Conflict(1)\n"
        "bc: b.Conflict = b.Conflict(2)\n"
        "print(ac.value())\n"
        "print(bc.value())\n";

    std::string output;
    int exitCode = compileAndRun(dir, dir + "/main.dr", source, output);

    EXPECT_EQ(exitCode, 0);
    EXPECT_NE(output.find("1"), std::string::npos);
    EXPECT_NE(output.find("2"), std::string::npos);

    cleanupDir(dir, {"a.dr", "b.dr"});
}

//===----------------------------------------------------------------------===//
// Stdlib .dr module tests (Phase 13)
//===----------------------------------------------------------------------===//

TEST(InteropTest, StdlibMathSqrt) {
    auto dir = makeTempDir("stdlib_sqrt");
    ASSERT_FALSE(dir.empty());

    std::string source =
        "from math import sqrt\n"
        "print(sqrt(9.0))\n";

    std::string output;
    int exitCode = compileAndRun(dir, dir + "/main.dr", source, output);

    EXPECT_EQ(exitCode, 0);
    EXPECT_NE(output.find("3"), std::string::npos);

    std::error_code rmEc; std::filesystem::remove_all(dir, rmEc);
}

TEST(InteropTest, StdlibMathPow) {
    auto dir = makeTempDir("stdlib_pow");
    ASSERT_FALSE(dir.empty());

    std::string source =
        "from math import pow\n"
        "print(pow(2.0, 10.0))\n";

    std::string output;
    int exitCode = compileAndRun(dir, dir + "/main.dr", source, output);

    EXPECT_EQ(exitCode, 0);
    EXPECT_NE(output.find("1024"), std::string::npos);

    std::error_code rmEc; std::filesystem::remove_all(dir, rmEc);
}

TEST(InteropTest, StdlibMathPi) {
    auto dir = makeTempDir("stdlib_pi");
    ASSERT_FALSE(dir.empty());

    std::string source =
        "from math import pi\n"
        "print(pi)\n";

    std::string output;
    int exitCode = compileAndRun(dir, dir + "/main.dr", source, output);

    EXPECT_EQ(exitCode, 0);
    EXPECT_NE(output.find("3.14159"), std::string::npos);

    std::error_code rmEc; std::filesystem::remove_all(dir, rmEc);
}

TEST(InteropTest, StdlibOsCwd) {
    auto dir = makeTempDir("stdlib_cwd");
    ASSERT_FALSE(dir.empty());

    std::string source =
        "from os import cwd\n"
        "print(cwd())\n";

    std::string output;
    int exitCode = compileAndRun(dir, dir + "/main.dr", source, output);

    EXPECT_EQ(exitCode, 0);
    // cwd() should return a non-empty string (a path)
    EXPECT_FALSE(output.empty());
    EXPECT_NE(output.find("/"), std::string::npos);

    std::error_code rmEc; std::filesystem::remove_all(dir, rmEc);
}

// Regression for two bugs that surfaced together when serving markdown files
// containing non-ASCII characters from the docs site:
//
// 1. `stdlib/io.dr::File.read_all` used to return a raw `malloc`'d buffer
//  typed as `str`. The buffer had no `DragonObjectHeader`, so the caller's
//  scope-exit `dragon_decref_str` read the 16 bytes preceding the buffer
//  as a fake header and (rarely) free()'d a misaligned non-heap address --
//  surfacing downstream as `malloc_consolidate(): unaligned fastbin chunk
//  detected`.
// 2. `dragon_str_split` walked the input with `*p` and `strstr(p, sep)`, both
//  byte-oriented. For a kind=4 (UCS-4) input the first cp's high zero
//  byte was read as a NUL terminator, collapsing the entire input to its
//  first cp before any separator could match.
//
// The shared trigger is "read a file with at least one non-ASCII byte, then
// process it with normal Dragon string ops" -- exactly what the docs site's
// markdown rendering does on `decisions/027-closures.md` (em-dash at byte
// 368). A correct fix produces the cp-count length (Python parity) and a
// split that yields one entry per logical newline-separated record.
TEST(InteropTest, ReadTextNonAsciiSplit) {
    auto dir = makeTempDir("read_text_nonascii");
    ASSERT_FALSE(dir.empty());

    // Three lines, second carries an em-dash (U+2013, 0xE2 0x80 0x93). Line
    // counts: 3 lines + a trailing empty after the final \n = 4 entries.
    // Byte length: 5 + 1 + 13 + 3 + 6 + 1 + 5 + 1 = 35 bytes (em-dash is 3
    // bytes); cp length = 33.
    std::string samplePath = dir + "/sample.txt";
    writeFile(samplePath, "alpha\nmiddle has — dash\nomega\n");

    // Absolute path so the spawned binary doesn't depend on cwd; popen
    // inherits the test runner's cwd which differs from `dir`.
    std::string source =
        "from io import open\n"
        "const t: str = open(\"" + samplePath + "\").text()\n"
        "print(len(t))\n"
        "const lines: list[str] = t.split(\"\\n\")\n"
        "print(len(lines))\n"
        "i: int = 0\n"
        "while i < len(lines) {\n"
        "    print(lines[i])\n"
        "    i = i + 1\n"
        "}\n";

    std::string output;
    int exitCode = compileAndRun(dir, dir + "/main.dr", source, output);

    // Pre-Bug-B-fix: exitCode could be SIGABRT (134) from the spurious
    // decref_str on the raw FFI buffer. Post-fix: clean exit.
    EXPECT_EQ(exitCode, 0) << "process aborted — likely FFI-string-return "
                              "decref of a raw malloc'd buffer. output:\n"
                           << output;

    // Cp count (Python parity): 3 lines × content + 2 newlines + final \n.
    // "alpha" (5) + \n + "middle has -- dash" (17 cps incl. em-dash) + \n
    // + "omega" (5) + \n = 30 cps. Pre-fix paths produced 32 (byte count
    // returned via strlen on a raw FFI buffer) or 1 (kind=4 walk
    // truncated at the first cp's high zero byte).
    EXPECT_EQ(output.substr(0, 3), "30\n")
        << "len(open(file).text()) should be cp count = 30:\n" << output;

    // split("\n") should yield 4 entries: "alpha", "middle has -- dash",
    // "omega", "" (the trailing empty after the final newline). Pre-fix
    // dragon_str_split's byte-walking path returned 1 entry for kind=4
    // input.
    EXPECT_NE(output.find("\n4\n"), std::string::npos)
        << "split('\\n') on kind=4 input should return 4 entries:\n"
        << output;

    // Each line individually present in the output (catches a split that
    // produces the right count but wrong content).
    EXPECT_NE(output.find("alpha"), std::string::npos) << output;
    EXPECT_NE(output.find("middle has"), std::string::npos) << output;
    EXPECT_NE(output.find("dash"), std::string::npos) << output;
    EXPECT_NE(output.find("omega"), std::string::npos) << output;

    std::error_code rmEc; std::filesystem::remove_all(dir, rmEc);
}

TEST(InteropTest, StdlibOsGetpid) {
    auto dir = makeTempDir("stdlib_pid");
    ASSERT_FALSE(dir.empty());

    std::string source =
        "from os import pid\n"
        "print(pid())\n";

    std::string output;
    int exitCode = compileAndRun(dir, dir + "/main.dr", source, output);

    EXPECT_EQ(exitCode, 0);
    // pid should be a positive integer
    int pidVal = std::atoi(output.c_str());
    EXPECT_GT(pidVal, 0);

    std::error_code rmEc; std::filesystem::remove_all(dir, rmEc);
}

TEST(InteropTest, StdlibOsGetenv) {
    auto dir = makeTempDir("stdlib_env");
    ASSERT_FALSE(dir.empty());

    std::string source =
        "from os import environ\n"
        "print(environ[\"HOME\"])\n";  // os.environ is now a dict (Python parity)

    std::string output;
    int exitCode = compileAndRun(dir, dir + "/main.dr", source, output);

    EXPECT_EQ(exitCode, 0);
    // HOME should be a non-empty path
    EXPECT_FALSE(output.empty());
    EXPECT_NE(output.find("/"), std::string::npos);

    std::error_code rmEc; std::filesystem::remove_all(dir, rmEc);
}

TEST(InteropTest, StdlibOsPathJoin) {
    auto dir = makeTempDir("stdlib_ospath_join");
    ASSERT_FALSE(dir.empty());

    std::string source =
        "from os.path import join, basename, dirname\n"
        "print(join(\"/foo\", \"bar\"))\n"
        "print(basename(\"/foo/bar/baz.txt\"))\n"
        "print(dirname(\"/foo/bar/baz.txt\"))\n";

    std::string output;
    int exitCode = compileAndRun(dir, dir + "/main.dr", source, output);

    EXPECT_EQ(exitCode, 0);
    EXPECT_NE(output.find("/foo/bar"), std::string::npos);
    EXPECT_NE(output.find("baz.txt"), std::string::npos);

    std::error_code rmEc; std::filesystem::remove_all(dir, rmEc);
}

TEST(InteropTest, StdlibOsPathNormpath) {
    auto dir = makeTempDir("stdlib_ospath_norm");
    ASSERT_FALSE(dir.empty());

    std::string source =
        "from os.path import normpath, isabs\n"
        "print(normpath(\"/a/b/../c\"))\n"
        "print(isabs(\"/foo\"))\n"
        "print(isabs(\"foo\"))\n";

    std::string output;
    int exitCode = compileAndRun(dir, dir + "/main.dr", source, output);

    EXPECT_EQ(exitCode, 0);
    EXPECT_NE(output.find("/a/c"), std::string::npos);
    EXPECT_NE(output.find("True"), std::string::npos);
    EXPECT_NE(output.find("False"), std::string::npos);

    std::error_code rmEc; std::filesystem::remove_all(dir, rmEc);
}

TEST(InteropTest, StdlibOsPathCommonprefix) {
    auto dir = makeTempDir("stdlib_ospath_cprefix");
    ASSERT_FALSE(dir.empty());

    std::string source =
        "from os.path import commonprefix\n"
        "print(commonprefix([\"/usr/lib\", \"/usr/local\"]))\n"
        "print(commonprefix([\"abc\", \"abd\"]))\n"
        "print(commonprefix([\"foo\"]))\n";

    std::string output;
    int exitCode = compileAndRun(dir, dir + "/main.dr", source, output);

    EXPECT_EQ(exitCode, 0);
    EXPECT_NE(output.find("/usr/l"), std::string::npos);
    EXPECT_NE(output.find("ab"), std::string::npos);
    EXPECT_NE(output.find("foo"), std::string::npos);

    std::error_code rmEc; std::filesystem::remove_all(dir, rmEc);
}

TEST(InteropTest, StdlibOsPathCommonpath) {
    auto dir = makeTempDir("stdlib_ospath_cpath");
    ASSERT_FALSE(dir.empty());

    std::string source =
        "from os.path import commonpath\n"
        "print(commonpath([\"/usr/lib\", \"/usr/local/lib\"]))\n"
        "print(commonpath([\"foo/bar/baz\", \"foo/bar/quux\"]))\n"
        "print(commonpath([\"/x\", \"/y\"]))\n"
        "print(commonpath([\"a/b\", \"x/y\"]))\n";

    std::string output;
    int exitCode = compileAndRun(dir, dir + "/main.dr", source, output);

    EXPECT_EQ(exitCode, 0);
    EXPECT_NE(output.find("/usr\n"), std::string::npos);
    EXPECT_NE(output.find("foo/bar\n"), std::string::npos);
    EXPECT_NE(output.find("/\n"), std::string::npos);

    std::error_code rmEc; std::filesystem::remove_all(dir, rmEc);
}

TEST(InteropTest, StdlibOsPathSplitdrive) {
    auto dir = makeTempDir("stdlib_ospath_sdrive");
    ASSERT_FALSE(dir.empty());

    std::string source =
        "from os.path import splitdrive\n"
        "const r: list[str] = splitdrive(\"/usr/lib\")\n"
        "print(\"d=\" + r[0])\n"
        "print(\"p=\" + r[1])\n";

    std::string output;
    int exitCode = compileAndRun(dir, dir + "/main.dr", source, output);

    EXPECT_EQ(exitCode, 0);
    EXPECT_NE(output.find("d=\n"), std::string::npos);
    EXPECT_NE(output.find("p=/usr/lib"), std::string::npos);

    std::error_code rmEc; std::filesystem::remove_all(dir, rmEc);
}

TEST(InteropTest, StdlibOsPathSamefile) {
    auto dir = makeTempDir("stdlib_ospath_same");
    ASSERT_FALSE(dir.empty());

    // Create a file and confirm samefile() reports identity.
    std::string fpath = dir + "/probe.txt";
    { std::ofstream f(fpath); f << "x"; }

    std::string source =
        "from os.path import samefile\n"
        "print(samefile(\"" + fpath + "\", \"" + fpath + "\"))\n"
        "print(samefile(\"/etc\", \"" + fpath + "\"))\n";

    std::string output;
    int exitCode = compileAndRun(dir, dir + "/main.dr", source, output);

    EXPECT_EQ(exitCode, 0);
    EXPECT_NE(output.find("True"), std::string::npos);
    EXPECT_NE(output.find("False"), std::string::npos);

    std::error_code rmEc; std::filesystem::remove_all(dir, rmEc);
}

TEST(InteropTest, StdlibOsPathIsmount) {
    auto dir = makeTempDir("stdlib_ospath_mnt");
    ASSERT_FALSE(dir.empty());

    std::string source =
        "from os.path import ismount\n"
        "print(ismount(\"/\"))\n"
        "print(ismount(\"" + dir + "\"))\n";

    std::string output;
    int exitCode = compileAndRun(dir, dir + "/main.dr", source, output);

    EXPECT_EQ(exitCode, 0);
    // "/" is virtually always a mount point on POSIX.
    // The temp dir typically isn't.
    EXPECT_NE(output.find("True"), std::string::npos);
    EXPECT_NE(output.find("False"), std::string::npos);

    std::error_code rmEc; std::filesystem::remove_all(dir, rmEc);
}

TEST(InteropTest, StdlibOsScandir) {
    auto dir = makeTempDir("stdlib_scandir");
    ASSERT_FALSE(dir.empty());

    // Populate a DEDICATED subdir with a file + a subdir, and scan THAT - not
    // `dir` itself, which compileAndRun fills with harness artifacts (the entry
    // .dr it now writes to disk for faithful package detection, plus the
    // compiled object/exe). Scanning a clean subdir keeps the count stable and
    // actually tests scandir rather than incidentally counting build outputs.
    std::filesystem::create_directory(dir + "/scan");
    { std::ofstream f(dir + "/scan/a.txt"); f << "data"; }
    std::filesystem::create_directory(dir + "/scan/sub");

    std::string source =
        "from os import scandir, DirEntry\n"
        "const entries: list[DirEntry] = scandir(\"" + dir + "/scan\")\n"
        "print(len(entries))\n"
        "for e in entries {\n"
        "    print(e.name + \" file=\" + str(e.is_file()) + \" dir=\" + str(e.is_dir()))\n"
        "}\n";

    std::string output;
    int exitCode = compileAndRun(dir, dir + "/main.dr", source, output);

    EXPECT_EQ(exitCode, 0);
    // 2 entries in the clean subdir: a.txt, sub.
    EXPECT_NE(output.find("2"), std::string::npos);
    EXPECT_NE(output.find("a.txt file=True dir=False"), std::string::npos);
    EXPECT_NE(output.find("sub file=False dir=True"), std::string::npos);

    std::error_code rmEc; std::filesystem::remove_all(dir, rmEc);
}

TEST(InteropTest, StdlibOsGetExecPath) {
    auto dir = makeTempDir("stdlib_execpath");
    ASSERT_FALSE(dir.empty());

    std::string source =
        "from os import get_exec_path\n"
        "const paths: list[str] = get_exec_path()\n"
        "print(len(paths) > 0)\n"
        "print(paths[0])\n";

    std::string output;
    int exitCode = compileAndRun(dir, dir + "/main.dr", source, output);

    EXPECT_EQ(exitCode, 0);
    EXPECT_NE(output.find("True"), std::string::npos);

    std::error_code rmEc; std::filesystem::remove_all(dir, rmEc);
}

TEST(InteropTest, StdlibOsForkExecWaitpid) {
    auto dir = makeTempDir("stdlib_fork");
    ASSERT_FALSE(dir.empty());

    std::string source =
        "from os import fork_proc, waitpid_proc, execvp_path, exit_now\n"
        "const child: int = fork_proc()\n"
        "if child == 0 {\n"
        "    const args: list[str] = [\"/bin/echo\", \"hello-from-child\"]\n"
        "    execvp_path(\"/bin/echo\", args)\n"
        "    exit_now(99)\n"
        "}\n"
        "const r: list[int] = waitpid_proc(child, 0)\n"
        "print(r[0] == child)\n"
        "print(r[1] == 0)\n";

    std::string output;
    int exitCode = compileAndRun(dir, dir + "/main.dr", source, output);

    EXPECT_EQ(exitCode, 0);
    EXPECT_NE(output.find("hello-from-child"), std::string::npos);
    EXPECT_NE(output.find("True"), std::string::npos);

    std::error_code rmEc; std::filesystem::remove_all(dir, rmEc);
}

TEST(InteropTest, StdlibOsUmask) {
    auto dir = makeTempDir("stdlib_umask");
    ASSERT_FALSE(dir.empty());

    std::string source =
        "from os import umask_set\n"
        "const prev: int = umask_set(18)\n"
        "const cur: int = umask_set(prev)\n"
        "print(cur == 18)\n";

    std::string output;
    int exitCode = compileAndRun(dir, dir + "/main.dr", source, output);

    EXPECT_EQ(exitCode, 0);
    EXPECT_NE(output.find("True"), std::string::npos);

    std::error_code rmEc; std::filesystem::remove_all(dir, rmEc);
}

TEST(InteropTest, StdlibOsGetpgrp) {
    auto dir = makeTempDir("stdlib_pgrp");
    ASSERT_FALSE(dir.empty());

    std::string source =
        "from os import getpgrp_id\n"
        "print(getpgrp_id() > 0)\n";

    std::string output;
    int exitCode = compileAndRun(dir, dir + "/main.dr", source, output);

    EXPECT_EQ(exitCode, 0);
    EXPECT_NE(output.find("True"), std::string::npos);

    std::error_code rmEc; std::filesystem::remove_all(dir, rmEc);
}

TEST(InteropTest, StdlibOsRemovedirsRecursive) {
    auto dir = makeTempDir("stdlib_rmdirs");
    ASSERT_FALSE(dir.empty());

    // Source creates a/b/c, removedirs(a/b/c), then verifies parents
    // were pruned but the temp root stays (we still need it for cleanup).
    std::string source =
        "from os import makedirs, removedirs, exists\n"
        "makedirs(\"" + dir + "/a/b/c\")\n"
        "print(exists(\"" + dir + "/a/b/c\"))\n"
        "removedirs(\"" + dir + "/a/b/c\")\n"
        "print(exists(\"" + dir + "/a/b/c\"))\n"
        "print(exists(\"" + dir + "/a\"))\n";

    std::string output;
    int exitCode = compileAndRun(dir, dir + "/main.dr", source, output);

    EXPECT_EQ(exitCode, 0);
    EXPECT_NE(output.find("True"), std::string::npos);
    EXPECT_NE(output.find("False"), std::string::npos);

    std::error_code rmEc; std::filesystem::remove_all(dir, rmEc);
}

TEST(InteropTest, StdlibOsRenames) {
    auto dir = makeTempDir("stdlib_renames");
    ASSERT_FALSE(dir.empty());

    std::string source =
        "from os import makedirs, renames, exists\n"
        "from io import make\n"
        "makedirs(\"" + dir + "/src\")\n"
        "with make(\"" + dir + "/src/f.txt\") as w {\n"
        "    w.write(\"hi\")\n"
        "}\n"
        "renames(\"" + dir + "/src/f.txt\", \"" + dir + "/dst/sub/f.txt\")\n"
        "print(exists(\"" + dir + "/dst/sub/f.txt\"))\n"
        "print(exists(\"" + dir + "/src\"))\n";

    std::string output;
    int exitCode = compileAndRun(dir, dir + "/main.dr", source, output);

    EXPECT_EQ(exitCode, 0);
    EXPECT_NE(output.find("True"), std::string::npos);
    EXPECT_NE(output.find("False"), std::string::npos);

    std::error_code rmEc; std::filesystem::remove_all(dir, rmEc);
}

TEST(InteropTest, StdlibOsChown) {
    auto dir = makeTempDir("stdlib_chown");
    ASSERT_FALSE(dir.empty());

    std::string fpath = dir + "/probe.txt";
    { std::ofstream f(fpath); f << "x"; }

    // chown to current uid/gid is a no-op but exercises the FFI path.
    // Using -1 changes neither (POSIX convention).
    std::string source =
        "from os import chown_path\n"
        "chown_path(\"" + fpath + "\", -1, -1)\n"
        "print(\"ok\")\n";

    std::string output;
    int exitCode = compileAndRun(dir, dir + "/main.dr", source, output);

    EXPECT_EQ(exitCode, 0);
    EXPECT_NE(output.find("ok"), std::string::npos);

    std::error_code rmEc; std::filesystem::remove_all(dir, rmEc);
}

TEST(InteropTest, StdlibOsChrootEPERM) {
    auto dir = makeTempDir("stdlib_chroot");
    ASSERT_FALSE(dir.empty());

    // chroot needs CAP_SYS_CHROOT (root). The test only confirms the FFI
    // is reachable and raises OSError when it can't proceed.
    std::string source =
        "from os import chroot_to\n"
        "try {\n"
        "    chroot_to(\"" + dir + "\")\n"
        "    print(\"unexpected\")\n"
        "} except OSError {\n"
        "    print(\"raised\")\n"
        "}\n";

    std::string output;
    int exitCode = compileAndRun(dir, dir + "/main.dr", source, output);

    EXPECT_EQ(exitCode, 0);
    // Either succeeds (CI runs as root) or raises -- both prove the bridge works.
    EXPECT_TRUE(output.find("raised") != std::string::npos ||
                output.find("unexpected") != std::string::npos);

    std::error_code rmEc; std::filesystem::remove_all(dir, rmEc);
}

TEST(InteropTest, StdlibOsLstat) {
    auto dir = makeTempDir("stdlib_lstat");
    ASSERT_FALSE(dir.empty());

    std::string fpath = dir + "/probe.txt";
    { std::ofstream f(fpath); f << "abcd"; }

    std::string source =
        "from os import lstat_size, lstat_isfile, lstat_isdir\n"
        "print(lstat_size(\"" + fpath + "\"))\n"
        "print(lstat_isfile(\"" + fpath + "\"))\n"
        "print(lstat_isdir(\"" + dir + "\"))\n";

    std::string output;
    int exitCode = compileAndRun(dir, dir + "/main.dr", source, output);

    EXPECT_EQ(exitCode, 0);
    EXPECT_NE(output.find("4"), std::string::npos);
    EXPECT_NE(output.find("True"), std::string::npos);

    std::error_code rmEc; std::filesystem::remove_all(dir, rmEc);
}

TEST(InteropTest, StdlibOsPathExpandvars) {
    auto dir = makeTempDir("stdlib_ospath_evars");
    ASSERT_FALSE(dir.empty());

    // HOME is set in nearly all environments; the test uses it as a probe
    // and also checks unknown-var passthrough.
    std::string source =
        "from os.path import expandvars\n"
        "print(expandvars(\"${HOME}/bin\"))\n"
        "print(expandvars(\"$HOME/bin\"))\n"
        "print(expandvars(\"$NO_SUCH_VAR_XYZ/end\"))\n"
        "print(expandvars(\"plain text\"))\n";

    std::string output;
    int exitCode = compileAndRun(dir, dir + "/main.dr", source, output);

    EXPECT_EQ(exitCode, 0);
    EXPECT_NE(output.find("/bin"), std::string::npos);
    EXPECT_NE(output.find("$NO_SUCH_VAR_XYZ/end"), std::string::npos);
    EXPECT_NE(output.find("plain text"), std::string::npos);

    std::error_code rmEc; std::filesystem::remove_all(dir, rmEc);
}

TEST(InteropTest, StdlibIoFileWrite) {
    auto dir = makeTempDir("stdlib_io");
    ASSERT_FALSE(dir.empty());

    std::string testFile = dir + "/test_output.txt";
    std::string source =
        "from io import open, make\n"
        "with make(\"" + testFile + "\") as f {\n"
        "    f.write(\"hello dragon\")\n"
        "}\n"
        "const data: bytes = open(\"" + testFile + "\").bytes()\n"
        "print(data.decode(\"utf-8\"))\n";

    std::string output;
    int exitCode = compileAndRun(dir, dir + "/main.dr", source, output);

    EXPECT_EQ(exitCode, 0);
    EXPECT_NE(output.find("hello dragon"), std::string::npos);

    std::remove(testFile.c_str());
    std::error_code rmEc; std::filesystem::remove_all(dir, rmEc);
}

TEST(InteropTest, StdlibIoContextManager) {
    auto dir = makeTempDir("stdlib_ctx");
    ASSERT_FALSE(dir.empty());

    std::string testFile = dir + "/ctx_test.txt";
    std::string source =
        "from io import open, make\n"
        "with make(\"" + testFile + "\") as f {\n"
        "    f.write(\"context ok\")\n"
        "}\n"
        "const data: bytes = open(\"" + testFile + "\").bytes()\n"
        "print(data.decode(\"utf-8\"))\n";

    std::string output;
    int exitCode = compileAndRun(dir, dir + "/main.dr", source, output);

    EXPECT_EQ(exitCode, 0);
    EXPECT_NE(output.find("context ok"), std::string::npos);

    std::remove(testFile.c_str());
    std::error_code rmEc; std::filesystem::remove_all(dir, rmEc);
}

//===----------------------------------------------------------------------===//
// Stdlib threading.dr tests
//===----------------------------------------------------------------------===//

// Lock is import-gated (Python parity: `from threading import Lock`). These
// cover the full API end-to-end through real stdlib module resolution: the
// canonical typed form, blocking/non-blocking/timeout acquire, with-statement
// (incl. exception-safety).

TEST(InteropTest, StdlibThreadingLock) {
    auto dir = makeTempDir("stdlib_lock");
    ASSERT_FALSE(dir.empty());

    // Typed decl + blocking acquire/release, then `with lock { }` actually holds
    // the lock (acquire(blocking=False) inside -> False) and releases after
    // (-> True).
    std::string source =
        "from threading import Lock\n"
        "lock: Lock = Lock()\n"
        "lock.acquire()\n"
        "lock.release()\n"
        "with lock {\n"
        "  held: bool = lock.acquire(blocking=False)\n"
        "  print(held)\n"
        "}\n"
        "free: bool = lock.acquire(blocking=False)\n"
        "print(free)\n";

    std::string output;
    int exitCode = compileAndRun(dir, dir + "/main.dr", source, output);

    EXPECT_EQ(exitCode, 0);
    EXPECT_EQ(output, "False\nTrue\n");

    std::error_code rmEc; std::filesystem::remove_all(dir, rmEc);
}

TEST(InteropTest, StdlibThreadingLockTimeout) {
    auto dir = makeTempDir("stdlib_lock_to");
    ASSERT_FALSE(dir.empty());

    // timeout on a free lock -> True immediately; on a held lock -> waits up to
    // the timeout then gives up -> False (does not block forever).
    std::string source =
        "from threading import Lock\n"
        "lock: Lock = Lock()\n"
        "a: bool = lock.acquire(timeout=5.0)\n"
        "print(a)\n"
        "b: bool = lock.acquire(blocking=True, timeout=0.1)\n"
        "print(b)\n";

    std::string output;
    int exitCode = compileAndRun(dir, dir + "/main.dr", source, output);

    EXPECT_EQ(exitCode, 0);
    EXPECT_EQ(output, "True\nFalse\n");

    std::error_code rmEc; std::filesystem::remove_all(dir, rmEc);
}

TEST(InteropTest, StdlibThreadingLockWithExceptionSafe) {
    auto dir = makeTempDir("stdlib_lock_exc");
    ASSERT_FALSE(dir.empty());

    // `with lock { }` releases even when an exception escapes the body.
    std::string source =
        "from threading import Lock\n"
        "lock: Lock = Lock()\n"
        "try {\n"
        "  with lock {\n"
        "    raise ValueError(\"boom\")\n"
        "  }\n"
        "} except ValueError {\n"
        "  print(\"caught\")\n"
        "}\n"
        "freed: bool = lock.acquire(blocking=False)\n"
        "print(freed)\n";

    std::string output;
    int exitCode = compileAndRun(dir, dir + "/main.dr", source, output);

    EXPECT_EQ(exitCode, 0);
    EXPECT_EQ(output, "caught\nTrue\n");

    std::error_code rmEc; std::filesystem::remove_all(dir, rmEc);
}

TEST(InteropTest, StdlibThreadingRWLock) {
    auto dir = makeTempDir("stdlib_rwl");
    ASSERT_FALSE(dir.empty());

    std::string source =
        "from threading import RWLock\n"
        "rw: RWLock = RWLock()\n"
        "rw.acquire(write=True)\n"
        "rw.release()\n"
        "rw.acquire()\n"
        "rw.release()\n"
        "rw.destroy()\n"
        "print(\"rwlock ok\")\n";

    std::string output;
    int exitCode = compileAndRun(dir, dir + "/main.dr", source, output);

    EXPECT_EQ(exitCode, 0);
    EXPECT_NE(output.find("rwlock ok"), std::string::npos);

    std::error_code rmEc; std::filesystem::remove_all(dir, rmEc);
}

TEST(InteropTest, StdlibThreadingSemaphore) {
    auto dir = makeTempDir("stdlib_sem");
    ASSERT_FALSE(dir.empty());

    std::string source =
        "from threading import Semaphore\n"
        "s: Semaphore = Semaphore(2)\n"
        "s.acquire()\n"
        "s.acquire()\n"
        "const got: bool = s.acquire(blocking=False)\n"
        "s.release()\n"
        "s.release()\n"
        "if got {\n"
        "    print(\"unexpected\")\n"
        "} else {\n"
        "    print(\"semaphore ok\")\n"
        "}\n"
        "s.destroy()\n";

    std::string output;
    int exitCode = compileAndRun(dir, dir + "/main.dr", source, output);

    EXPECT_EQ(exitCode, 0);
    EXPECT_NE(output.find("semaphore ok"), std::string::npos);

    std::error_code rmEc; std::filesystem::remove_all(dir, rmEc);
}

TEST(InteropTest, StdlibThreadingCondition) {
    auto dir = makeTempDir("stdlib_cond");
    ASSERT_FALSE(dir.empty());

    std::string source =
        "from threading import Condition\n"
        "c: Condition = Condition()\n"
        "c.acquire()\n"
        "c.release()\n"
        "c.destroy()\n"
        "print(\"condition ok\")\n";

    std::string output;
    int exitCode = compileAndRun(dir, dir + "/main.dr", source, output);

    EXPECT_EQ(exitCode, 0);
    EXPECT_NE(output.find("condition ok"), std::string::npos);

    std::error_code rmEc; std::filesystem::remove_all(dir, rmEc);
}

//===----------------------------------------------------------------------===//
// Stdlib crypto.dr tests
//===----------------------------------------------------------------------===//

TEST(InteropTest, StdlibCryptoSha256) {
    auto dir = makeTempDir("stdlib_sha");
    ASSERT_FALSE(dir.empty());

    std::string source =
        "from crypto import sha256\n"
        "print(sha256(\"hello\"))\n";

    std::string output;
    int exitCode = compileAndRun(dir, dir + "/main.dr", source, output);

    EXPECT_EQ(exitCode, 0);
    // SHA256("hello") = 2cf24dba5fb0a30e26e83b2ac5b9e29e1b161e5c1fa7425e73043362938b9824
    EXPECT_NE(output.find("2cf24dba"), std::string::npos);

    std::error_code rmEc; std::filesystem::remove_all(dir, rmEc);
}

TEST(InteropTest, StdlibCryptoMd5) {
    auto dir = makeTempDir("stdlib_md5");
    ASSERT_FALSE(dir.empty());

    std::string source =
        "from crypto import md5\n"
        "print(md5(\"hello\"))\n";

    std::string output;
    int exitCode = compileAndRun(dir, dir + "/main.dr", source, output);

    EXPECT_EQ(exitCode, 0);
    // MD5("hello") = 5d41402abc4b2a76b9719d911017c592
    EXPECT_NE(output.find("5d41402abc4b2a76"), std::string::npos);

    std::error_code rmEc; std::filesystem::remove_all(dir, rmEc);
}

TEST(InteropTest, StdlibCryptoSha1) {
    auto dir = makeTempDir("stdlib_sha1");
    ASSERT_FALSE(dir.empty());

    std::string source =
        "from crypto import sha1\n"
        "print(sha1(\"hello\"))\n";

    std::string output;
    int exitCode = compileAndRun(dir, dir + "/main.dr", source, output);

    EXPECT_EQ(exitCode, 0);
    // SHA1("hello") = aaf4c61ddcc5e8a2dabede0f3b482cd9aea9434d
    EXPECT_NE(output.find("aaf4c61d"), std::string::npos);

    std::error_code rmEc; std::filesystem::remove_all(dir, rmEc);
}

//===----------------------------------------------------------------------===//
// Stdlib re.dr tests (bundled PCRE2)
//===----------------------------------------------------------------------===//

TEST(InteropTest, StdlibReMatch) {
    auto dir = makeTempDir("stdlib_re_match");
    ASSERT_FALSE(dir.empty());

    std::string source =
        "from re import match\n"
        "const rc: int = match(\"\\d+\", \"hello 42 world\")\n"
        "if rc > 0 {\n"
        "    print(\"matched\")\n"
        "} else {\n"
        "    print(\"no match\")\n"
        "}\n";

    std::string output;
    int exitCode = compileAndRun(dir, dir + "/main.dr", source, output);
    EXPECT_EQ(exitCode, 0);
    EXPECT_NE(output.find("matched"), std::string::npos);

    std::error_code rmEc; std::filesystem::remove_all(dir, rmEc);
}

TEST(InteropTest, StdlibReSearch) {
    auto dir = makeTempDir("stdlib_re_search");
    ASSERT_FALSE(dir.empty());

    std::string source =
        "from re import search\n"
        "const result: str = search(\"\\d+\", \"hello 42 world\")\n"
        "print(result)\n";

    std::string output;
    int exitCode = compileAndRun(dir, dir + "/main.dr", source, output);
    EXPECT_EQ(exitCode, 0);
    EXPECT_NE(output.find("42"), std::string::npos);

    std::error_code rmEc; std::filesystem::remove_all(dir, rmEc);
}

TEST(InteropTest, StdlibReCompileFind) {
    auto dir = makeTempDir("stdlib_re_find");
    ASSERT_FALSE(dir.empty());

    std::string source =
        "from re import compile, Pattern\n"
        "const p: Pattern = compile(\"[a-z]+@[a-z]+\\.com\")\n"
        "const result: str = p.find(\"contact hello@world.com today\")\n"
        "print(result)\n"
        "p.destroy()\n";

    std::string output;
    int exitCode = compileAndRun(dir, dir + "/main.dr", source, output);
    EXPECT_EQ(exitCode, 0);
    EXPECT_NE(output.find("hello@world.com"), std::string::npos);

    std::error_code rmEc; std::filesystem::remove_all(dir, rmEc);
}

//===----------------------------------------------------------------------===//
// Stdlib sqlite.dr tests
//===----------------------------------------------------------------------===//

TEST(InteropTest, StdlibSqliteBasic) {
    auto dir = makeTempDir("stdlib_sql");
    ASSERT_FALSE(dir.empty());

    std::string dbFile = dir + "/test.db";
    std::string source =
        "from sqlite import Database, Cursor\n"
        "db: Database = Database(\"" + dbFile + "\")\n"
        "db.execute(\"CREATE TABLE t (id INTEGER, name TEXT)\")\n"
        "db.execute(\"INSERT INTO t VALUES (1, 'dragon')\")\n"
        "db.execute(\"INSERT INTO t VALUES (2, 'python')\")\n"
        "c: Cursor = db.prepare(\"SELECT name FROM t WHERE id = 1\")\n"
        "const rc: int = c.step()\n"
        "if rc == 100 {\n"
        "    print(c.column_text(0))\n"
        "}\n"
        "c.finalize()\n"
        "db.close()\n";

    std::string output;
    int exitCode = compileAndRun(dir, dir + "/main.dr", source, output);

    EXPECT_EQ(exitCode, 0);
    EXPECT_NE(output.find("dragon"), std::string::npos);

    std::remove(dbFile.c_str());
    std::error_code rmEc; std::filesystem::remove_all(dir, rmEc);
}

TEST(InteropTest, StdlibSqlitePreparedBind) {
    auto dir = makeTempDir("stdlib_sqlbind");
    ASSERT_FALSE(dir.empty());

    std::string dbFile = dir + "/test_bind.db";
    std::string source =
        "from sqlite import Database, Cursor\n"
        "db: Database = Database(\"" + dbFile + "\")\n"
        "db.execute(\"CREATE TABLE users (id INTEGER, name TEXT, score REAL)\")\n"
        "c: Cursor = db.prepare(\"INSERT INTO users VALUES (?, ?, ?)\")\n"
        "c.bind_int(1, 42)\n"
        "c.bind_text(2, \"alice\")\n"
        "c.bind_float(3, 99.5)\n"
        "c.step()\n"
        "c.finalize()\n"
        "q: Cursor = db.prepare(\"SELECT name, score FROM users WHERE id = 42\")\n"
        "const rc: int = q.step()\n"
        "if rc == 100 {\n"
        "    print(q.column_text(0))\n"
        "    print(q.column_float(1))\n"
        "}\n"
        "q.finalize()\n"
        "db.close()\n";

    std::string output;
    int exitCode = compileAndRun(dir, dir + "/main.dr", source, output);

    EXPECT_EQ(exitCode, 0);
    EXPECT_NE(output.find("alice"), std::string::npos);
    EXPECT_NE(output.find("99.5"), std::string::npos);

    std::remove(dbFile.c_str());
    std::error_code rmEc; std::filesystem::remove_all(dir, rmEc);
}

TEST(InteropTest, StdlibSqliteContextManager) {
    auto dir = makeTempDir("stdlib_sqlctx");
    ASSERT_FALSE(dir.empty());

    std::string dbFile = dir + "/test_ctx.db";
    std::string source =
        "from sqlite import Database, Cursor\n"
        "with Database(\"" + dbFile + "\") as db {\n"
        "    db.execute(\"CREATE TABLE t (x INTEGER)\")\n"
        "    db.execute(\"INSERT INTO t VALUES (777)\")\n"
        "    c: Cursor = db.prepare(\"SELECT x FROM t\")\n"
        "    const rc: int = c.step()\n"
        "    if rc == 100 {\n"
        "        print(c.column_int(0))\n"
        "    }\n"
        "    c.finalize()\n"
        "}\n";

    std::string output;
    int exitCode = compileAndRun(dir, dir + "/main.dr", source, output);

    EXPECT_EQ(exitCode, 0);
    EXPECT_NE(output.find("777"), std::string::npos);

    std::remove(dbFile.c_str());
    std::error_code rmEc; std::filesystem::remove_all(dir, rmEc);
}

// NOTE: the D032 database stdlib E2E (template[SQL] param binding, dict[str,Any]
// rows, Customer(**row) conversion, injection safety) is covered by the
// dogfooded `.dr` unittest suite (test/dr/test_database.dr).

//===----------------------------------------------------------------------===//
// Stdlib socket.dr tests
//===----------------------------------------------------------------------===//

TEST(InteropTest, StdlibSocketCreate) {
    auto dir = makeTempDir("stdlib_sock");
    ASSERT_FALSE(dir.empty());

    // Test that we can create and close a socket
    std::string source =
        "from socket import TcpListener\n"
        "srv: TcpListener = TcpListener(\"127.0.0.1\", 0)\n"
        "srv.close()\n"
        "print(\"socket ok\")\n";

    std::string output;
    int exitCode = compileAndRun(dir, dir + "/main.dr", source, output);

    EXPECT_EQ(exitCode, 0);
    EXPECT_NE(output.find("socket ok"), std::string::npos);

    std::error_code rmEc; std::filesystem::remove_all(dir, rmEc);
}

//===----------------------------------------------------------------------===//
// Cross-module module-level globals: the forward-decl path used to inspect
// only NamedTypeExpr and silently fell through to i64 for `dict[str, str]`,
// `list[T]`, etc. The reader module then emitted typed dict/list ops
// expecting `ptr`, tripping the LLVM verifier.
//===----------------------------------------------------------------------===//

TEST(InteropTest, CrossModuleGlobalDict) {
    auto dir = makeTempDir("xmod_global_dict");
    ASSERT_FALSE(dir.empty());

    writeFile(dir + "/lib.dr",
        "def _build() -> dict[str, str] {\n"
        "    m: dict[str, str] = {}\n"
        "    m[\"a\"] = \"alpha\"\n"
        "    m[\"b\"] = \"beta\"\n"
        "    return m\n"
        "}\n"
        "\n"
        "const TBL: dict[str, str] = _build()\n"
        "\n"
        "def lookup(k: str) -> str {\n"
        "    if k in TBL {\n"
        "        return TBL[k]\n"
        "    }\n"
        "    return \"missing\"\n"
        "}\n"
    );

    std::string source =
        "from lib import lookup\n"
        "print(lookup(\"a\"))\n"
        "print(lookup(\"b\"))\n"
        "print(lookup(\"c\"))\n";

    std::string output;
    int exitCode = compileAndRun(dir, dir + "/main.dr", source, output);

    EXPECT_EQ(exitCode, 0);
    EXPECT_NE(output.find("alpha"), std::string::npos);
    EXPECT_NE(output.find("beta"), std::string::npos);
    EXPECT_NE(output.find("missing"), std::string::npos);

    cleanupDir(dir, {"lib.dr"});
}

TEST(InteropTest, CrossModuleGlobalList) {
    auto dir = makeTempDir("xmod_global_list");
    ASSERT_FALSE(dir.empty());

    writeFile(dir + "/lib.dr",
        "def _build() -> list[int] {\n"
        "    xs: list[int] = []\n"
        "    xs.append(10)\n"
        "    xs.append(20)\n"
        "    xs.append(30)\n"
        "    return xs\n"
        "}\n"
        "\n"
        "const NUMS: list[int] = _build()\n"
        "\n"
        "def total() -> int {\n"
        "    s: int = 0\n"
        "    for n in NUMS {\n"
        "        s = s + n\n"
        "    }\n"
        "    return s\n"
        "}\n"
    );

    std::string source =
        "from lib import total\n"
        "print(total())\n";

    std::string output;
    int exitCode = compileAndRun(dir, dir + "/main.dr", source, output);

    EXPECT_EQ(exitCode, 0);
    EXPECT_NE(output.find("60"), std::string::npos);

    cleanupDir(dir, {"lib.dr"});
}

//===----------------------------------------------------------------------===//
// Module-attribute access
//===----------------------------------------------------------------------===//
//
// These tests exercise the cross-module function-reference paths that the
// heaven-style HTTP server depends on for handler registration:
//
//  Pattern A (baseline): `from x import f` ; `f`
//  Pattern B (submodule): `from x import sub` ; `sub.f`
//  Pattern C (dotted): `import x.sub` ; `x.sub.f`
//
// All three lower to the same static LLVM Function* -- no runtime cost
// vs. a bare-name function reference. The implementation lives in:
//  src/TypeChecker.cpp (ModuleType resolution + AttributeExpr)
//  src/codegen/Attributes.cpp (static fnptr emission)

// Pattern A as a function-as-value: assign an imported function to a local,
// then call through the local. Baseline -- establishes that cross-module
// function pointers work before the fancier module-attr cases below.
TEST(InteropTest, ImportedFunctionAsValue) {
    auto dir = makeTempDir("imp_fn_value");
    ASSERT_FALSE(dir.empty());

    writeFile(dir + "/utils.dr",
        "def double_it(x: int) -> int {\n"
        "    return x * 2\n"
        "}\n"
    );

    std::string source =
        "from utils import double_it\n"
        "const cb: Callable[[int], int] = double_it\n"
        "print(cb(21))\n";

    std::string output;
    int exitCode = compileAndRun(dir, dir + "/main.dr", source, output);

    EXPECT_EQ(exitCode, 0);
    EXPECT_NE(output.find("42"), std::string::npos);

    cleanupDir(dir, {"utils.dr"});
}

// Pattern B as function-as-value: `from controllers import health` brings
// the submodule into scope, then `health.health_check` resolves through
// ModuleType.exports to a static Function*.
TEST(InteropTest, FromPackageImportSubmoduleAttrAsValue) {
    auto dir = makeTempDir("from_pkg_sub_attr");
    ASSERT_FALSE(dir.empty());

    // Package layout: controllers/controllers.dr (package root) +
    // controllers/health.dr (submodule). The "flat file XOR package"
    // rule means no controllers.dr at the top level.
    std::filesystem::create_directory(dir + "/controllers");
    writeFile(dir + "/controllers/controllers.dr", "\n");
    writeFile(dir + "/controllers/health.dr",
        "def health_check() -> int {\n"
        "    return 200\n"
        "}\n"
    );

    std::string source =
        "from controllers import health\n"
        "const cb: Callable[[], int] = health.health_check\n"
        "print(cb())\n";

    std::string output;
    int exitCode = compileAndRun(dir, dir + "/main.dr", source, output);

    EXPECT_EQ(exitCode, 0) << "stdout: " << output;
    EXPECT_NE(output.find("200"), std::string::npos);

    std::filesystem::remove_all(dir + "/controllers");
    cleanupDir(dir, {});
}

// Pattern C as function-as-value: `import controllers.health` binds the
// package name; the chain `controllers.health.health_check` walks two
// ModuleType nodes (controllers -> controllers.health -> fn export).
TEST(InteropTest, ImportPackageDotSubmoduleAttrAsValue) {
    auto dir = makeTempDir("imp_pkg_sub_attr");
    ASSERT_FALSE(dir.empty());

    std::filesystem::create_directory(dir + "/controllers");
    writeFile(dir + "/controllers/controllers.dr", "\n");
    writeFile(dir + "/controllers/health.dr",
        "def health_check() -> int {\n"
        "    return 201\n"
        "}\n"
    );

    std::string source =
        "import controllers.health\n"
        "const cb: Callable[[], int] = controllers.health.health_check\n"
        "print(cb())\n";

    std::string output;
    int exitCode = compileAndRun(dir, dir + "/main.dr", source, output);

    EXPECT_EQ(exitCode, 0) << "stdout: " << output;
    EXPECT_NE(output.find("201"), std::string::npos);

    std::filesystem::remove_all(dir + "/controllers");
    cleanupDir(dir, {});
}

// Pattern B direct call: `from x import sub; sub.f()` -- exercises the
// module-attr path through CallExpr's callee dispatch (not just value
// context). If this fails while the as-value test passes, the gap is in
// CallExpr handling of AttributeExpr callees rooted in a module.
TEST(InteropTest, FromPackageImportSubmoduleAttrDirectCall) {
    auto dir = makeTempDir("from_pkg_sub_call");
    ASSERT_FALSE(dir.empty());

    std::filesystem::create_directory(dir + "/controllers");
    writeFile(dir + "/controllers/controllers.dr", "\n");
    writeFile(dir + "/controllers/health.dr",
        "def health_check() -> int {\n"
        "    return 202\n"
        "}\n"
    );

    std::string source =
        "from controllers import health\n"
        "print(health.health_check())\n";

    std::string output;
    int exitCode = compileAndRun(dir, dir + "/main.dr", source, output);

    EXPECT_EQ(exitCode, 0) << "stdout: " << output;
    EXPECT_NE(output.find("202"), std::string::npos);

    std::filesystem::remove_all(dir + "/controllers");
    cleanupDir(dir, {});
}

// Pattern C direct call: `import x.sub; x.sub.f()` -- same dispatch
// concern as the previous test, but with a longer module chain.
TEST(InteropTest, ImportPackageDotSubmoduleAttrDirectCall) {
    auto dir = makeTempDir("imp_pkg_sub_call");
    ASSERT_FALSE(dir.empty());

    std::filesystem::create_directory(dir + "/controllers");
    writeFile(dir + "/controllers/controllers.dr", "\n");
    writeFile(dir + "/controllers/health.dr",
        "def health_check() -> int {\n"
        "    return 203\n"
        "}\n"
    );

    std::string source =
        "import controllers.health\n"
        "print(controllers.health.health_check())\n";

    std::string output;
    int exitCode = compileAndRun(dir, dir + "/main.dr", source, output);

    EXPECT_EQ(exitCode, 0) << "stdout: " << output;
    EXPECT_NE(output.find("203"), std::string::npos);

    std::filesystem::remove_all(dir + "/controllers");
    cleanupDir(dir, {});
}

//===----------------------------------------------------------------------===//
// stdlib/unittest.dr end-to-end (D033 method reflection + D039 deep equality +
// first-class exceptions). Imports the real stdlib module via the resolver's
// DRAGON_STDLIB_DIR search path and runs a TestCase through unittest.main.
//===----------------------------------------------------------------------===//

TEST(InteropTest, StdlibUnittestEndToEnd) {
    auto dir = makeTempDir("unittest_e2e");
    ASSERT_FALSE(dir.empty());

    std::string source =
        "import unittest\n"
        "def raise_value() { raise ValueError(\"boom\") }\n"
        "class SampleTests(unittest.TestCase) {\n"
        "    def test_addition() { self.assertEqual(2 + 2, 4) }\n"
        "    def test_deep_list() { self.assertEqual([1, 2, 3], [1, 2, 3]) }\n"
        "    def test_deep_dict() { self.assertEqual({\"a\": 1}, {\"a\": 1}) }\n"
        "    def test_truthy() { self.assertTrue(1 < 2) }\n"
        "    def test_in() { self.assertIn(\"ell\", \"hello\") }\n"
        "    def test_raises() { self.assertRaises(ValueError, raise_value) }\n"
        "    def test_intentional_fail() { self.assertEqual(1, 2) }\n"
        "}\n"
        "cases: list[unittest.TestCase] = [SampleTests()]\n"
        "unittest.main(cases)\n";

    std::string output;
    int exitCode = compileAndRun(dir, dir + "/main.dr", source, output);

    // The suite contains one deliberate failure, so unittest.main() must exit
    // non-zero (1) -- the CI-gating contract: a failed `.dr` suite fails
    // `dragon run` / ctest. main() calls sys.exit_code(); previously its int
    // return was discarded at module scope and the process always exited 0.
    EXPECT_EQ(exitCode, 1) << "stdout: " << output;
    // Six passing tests print `ok:`; the deliberate failure prints `FAIL:`.
    EXPECT_NE(output.find("ok: test_addition"), std::string::npos) << output;
    EXPECT_NE(output.find("ok: test_deep_list"), std::string::npos) << output;
    EXPECT_NE(output.find("ok: test_deep_dict"), std::string::npos) << output;
    EXPECT_NE(output.find("ok: test_raises"), std::string::npos) << output;
    EXPECT_NE(output.find("FAIL: test_intentional_fail"), std::string::npos) << output;
    EXPECT_NE(output.find("Ran 7 test(s); failures: 1, errors: 0"), std::string::npos) << output;
    EXPECT_NE(output.find("FAILED"), std::string::npos) << output;

    cleanupDir(dir, {});
}

//===----------------------------------------------------------------------===//
// D045 -- member & module privacy (cross-package enforcement)
//===----------------------------------------------------------------------===//

// Two flat files in the same dir are DISTINCT singleton packages (ADR), so a
// `_module-internal` name is NOT importable across them.
TEST(InteropTest, D045_CrossPackageProtectedImportRejected) {
    auto dir = makeTempDir("d045_proto_import");
    ASSERT_FALSE(dir.empty());
    writeFile(dir + "/lib.dr",
        "_secret: int = 42\n"
        "public_val: int = 7\n");
    auto errs = typeCheckProjectErrors(dir, dir + "/main.dr",
        "from lib import _secret, public_val\n"
        "print(_secret)\n");
    EXPECT_TRUE(anyContains(errs, "module-private")) << "expected a privacy error";
    EXPECT_TRUE(anyContains(errs, "_secret")) << "error should name _secret";
    cleanupDir(dir, {"lib.dr", "main.dr"});
}

// A public name from another package imports and runs fine (control).
TEST(InteropTest, D045_CrossPackagePublicImportAllowed) {
    auto dir = makeTempDir("d045_pub_import");
    ASSERT_FALSE(dir.empty());
    writeFile(dir + "/lib.dr",
        "def public_add(a: int, b: int) -> int {\n"
        "    return a + b\n"
        "}\n");
    std::string out;
    int rc = compileAndRun(dir, dir + "/main.dr",
        "from lib import public_add\n"
        "print(public_add(2, 3))\n", out);
    EXPECT_EQ(rc, 0);
    EXPECT_NE(out.find("5"), std::string::npos) << out;
    cleanupDir(dir, {"lib.dr", "main.dr", "test_output", "test_output.o"});
}

// A file-private `__name` is not importable even by a sibling -- and across
// packages it is certainly rejected.
TEST(InteropTest, D045_CrossPackageFilePrivateImportRejected) {
    auto dir = makeTempDir("d045_filepriv");
    ASSERT_FALSE(dir.empty());
    writeFile(dir + "/lib.dr",
        "__hidden: int = 99\n"
        "public_val: int = 1\n");
    auto errs = typeCheckProjectErrors(dir, dir + "/main.dr",
        "from lib import __hidden\n"
        "print(__hidden)\n");
    EXPECT_TRUE(anyContains(errs, "__hidden")) << "error should name __hidden";
    EXPECT_TRUE(anyContains(errs, "file-private")) << "should mention file-private";
    cleanupDir(dir, {"lib.dr", "main.dr"});
}

// Qualified access `mod._secret` across packages is rejected too (same rule as
// the from-import, enforced at the AttributeExpr module branch).
TEST(InteropTest, D045_CrossPackageQualifiedProtectedRejected) {
    auto dir = makeTempDir("d045_qual");
    ASSERT_FALSE(dir.empty());
    writeFile(dir + "/lib.dr",
        "_secret: int = 42\n"
        "public_val: int = 7\n");
    auto errs = typeCheckProjectErrors(dir, dir + "/main.dr",
        "import lib\n"
        "print(lib._secret)\n");
    EXPECT_TRUE(anyContains(errs, "module-private")) << "expected a privacy error";
    cleanupDir(dir, {"lib.dr", "main.dr"});
}

// Same-PACKAGE protected import is allowed: a package `app/` (root app/app.dr)
// importing a `_shared` name from sibling app/internal.dr compiles and runs.
TEST(InteropTest, D045_SamePackageProtectedImportAllowed) {
    auto dir = makeTempDir("d045_samepkg");
    ASSERT_FALSE(dir.empty());
    std::error_code ec; std::filesystem::create_directories(dir + "/app", ec);
    writeFile(dir + "/app/internal.dr",
        "_shared: int = 123\n");
    std::string out;
    int rc = compileAndRun(dir, dir + "/app/app.dr",
        "from app.internal import _shared\n"
        "print(_shared)\n", out);
    EXPECT_EQ(rc, 0) << out;
    EXPECT_NE(out.find("123"), std::string::npos) << out;
    std::filesystem::remove_all(dir, ec);
}
