#include <gtest/gtest.h>
#include "TestHelpers.h"
#include "dragon/ModuleResolver.h"
#include <fstream>
#include <cstdlib>
#include <cstdio>
#include <sys/stat.h>

using namespace dragon;
using namespace dragon::test;

// Helper: create a temp directory and return its path
static std::string makeTempDir(const std::string& suffix) {
    std::string tmpl = "/tmp/dragon_test_" + suffix + "_XXXXXX";
    std::vector<char> buf(tmpl.begin(), tmpl.end());
    buf.push_back('\0');
    char* result = mkdtemp(buf.data());
    return result ? std::string(result) : "";
}

// Helper: write a file at the given path
static void writeFile(const std::string& path, const std::string& content) {
    std::ofstream out(path);
    out << content;
}

//===----------------------------------------------------------------------===//
// findModuleFile
//===----------------------------------------------------------------------===//

TEST(ModuleResolver, FindDragonModule) {
    auto dir = makeTempDir("find_dr");
    ASSERT_FALSE(dir.empty());
    writeFile(dir + "/utils.dr", "def add(x: int, y: int) -> int { return x + y }");

    ModuleResolverOptions opts;
    opts.sourceDir = dir + "/";
    ModuleResolver resolver(opts);

    auto path = resolver.findModuleFile("utils");
    EXPECT_EQ(path, dir + "/utils.dr");

    std::remove((dir + "/utils.dr").c_str());
    rmdir(dir.c_str());
}

TEST(ModuleResolver, FindPythonModule) {
    auto dir = makeTempDir("find_py");
    ASSERT_FALSE(dir.empty());
    writeFile(dir + "/utils.py", "def add(x: int, y: int) -> int:\n    return x + y\n");

    ModuleResolverOptions opts;
    opts.sourceDir = dir + "/";
    ModuleResolver resolver(opts);

    auto path = resolver.findModuleFile("utils");
    EXPECT_EQ(path, dir + "/utils.py");

    std::remove((dir + "/utils.py").c_str());
    rmdir(dir.c_str());
}

TEST(ModuleResolver, DragonPreferredOverPython) {
    auto dir = makeTempDir("prefer_dr");
    ASSERT_FALSE(dir.empty());
    writeFile(dir + "/utils.dr", "def add(x: int, y: int) -> int { return x + y }");
    writeFile(dir + "/utils.py", "def add(x: int, y: int) -> int:\n    return x + y\n");

    ModuleResolverOptions opts;
    opts.sourceDir = dir + "/";
    ModuleResolver resolver(opts);

    auto path = resolver.findModuleFile("utils");
    EXPECT_EQ(path, dir + "/utils.dr");

    std::remove((dir + "/utils.dr").c_str());
    std::remove((dir + "/utils.py").c_str());
    rmdir(dir.c_str());
}

TEST(ModuleResolver, ModuleNotFound) {
    ModuleResolverOptions opts;
    opts.sourceDir = "/tmp/nonexistent_dir/";
    ModuleResolver resolver(opts);

    auto path = resolver.findModuleFile("nosuchmodule");
    EXPECT_TRUE(path.empty());
}

TEST(ModuleResolver, FindInSearchPath) {
    auto srcDir = makeTempDir("src");
    auto libDir = makeTempDir("lib");
    ASSERT_FALSE(srcDir.empty());
    ASSERT_FALSE(libDir.empty());
    writeFile(libDir + "/helpers.dr", "def helper() -> int { return 42 }");

    ModuleResolverOptions opts;
    opts.sourceDir = srcDir + "/";
    opts.searchPaths = {libDir};
    ModuleResolver resolver(opts);

    auto path = resolver.findModuleFile("helpers");
    EXPECT_EQ(path, libDir + "/helpers.dr");

    std::remove((libDir + "/helpers.dr").c_str());
    rmdir(srcDir.c_str());
    rmdir(libDir.c_str());
}

//===----------------------------------------------------------------------===//
// buildGraph - single file (no imports)
//===----------------------------------------------------------------------===//

TEST(ModuleResolver, NoImports) {
    auto module = parse("x: int = 5\nprint(x)\n", /*isDragon=*/true);
    ASSERT_NE(module, nullptr);

    ModuleResolver resolver;
    auto graph = resolver.buildGraph(*module, "<test>");

    EXPECT_FALSE(graph.hasCycle);
    EXPECT_TRUE(graph.modules.empty());
    EXPECT_FALSE(resolver.hasErrors());
}

//===----------------------------------------------------------------------===//
// buildGraph - single import
//===----------------------------------------------------------------------===//

TEST(ModuleResolver, SingleImport) {
    auto dir = makeTempDir("single_imp");
    ASSERT_FALSE(dir.empty());
    writeFile(dir + "/utils.dr",
        "def helper(x: int) -> int {\n    return x + 1\n}\n");

    // Entry module imports utils
    auto module = parse("from utils import helper\nprint(helper(5))\n", /*isDragon=*/true);
    ASSERT_NE(module, nullptr);

    ModuleResolverOptions opts;
    opts.sourceDir = dir + "/";
    ModuleResolver resolver(opts);
    auto graph = resolver.buildGraph(*module, dir + "/main.dr");

    EXPECT_FALSE(graph.hasCycle);
    ASSERT_EQ(graph.modules.size(), 1u);
    EXPECT_EQ(graph.modules[0].name, "utils");
    EXPECT_TRUE(graph.modules[0].isDragon);
    EXPECT_NE(graph.modules[0].ast, nullptr);

    std::remove((dir + "/utils.dr").c_str());
    rmdir(dir.c_str());
}

//===----------------------------------------------------------------------===//
// buildGraph - diamond dependency
//===----------------------------------------------------------------------===//

TEST(ModuleResolver, DiamondDependency) {
    auto dir = makeTempDir("diamond");
    ASSERT_FALSE(dir.empty());

    // C depends on nothing
    writeFile(dir + "/modC.dr", "def c_func() -> int { return 1 }");
    // A depends on C
    writeFile(dir + "/modA.dr", "from modC import c_func\ndef a_func() -> int { return c_func() }");
    // B depends on C
    writeFile(dir + "/modB.dr", "from modC import c_func\ndef b_func() -> int { return c_func() }");

    // Entry depends on A and B
    auto module = parse(
        "from modA import a_func\nfrom modB import b_func\nprint(a_func() + b_func())\n",
        /*isDragon=*/true);
    ASSERT_NE(module, nullptr);

    ModuleResolverOptions opts;
    opts.sourceDir = dir + "/";
    ModuleResolver resolver(opts);
    auto graph = resolver.buildGraph(*module, dir + "/main.dr");

    EXPECT_FALSE(graph.hasCycle);
    // Should have modC, modA, modB (C first since both A and B depend on it)
    ASSERT_EQ(graph.modules.size(), 3u);
    // modC must come before modA and modB
    size_t cIdx = SIZE_MAX, aIdx = SIZE_MAX, bIdx = SIZE_MAX;
    for (size_t i = 0; i < graph.modules.size(); ++i) {
        if (graph.modules[i].name == "modC") cIdx = i;
        if (graph.modules[i].name == "modA") aIdx = i;
        if (graph.modules[i].name == "modB") bIdx = i;
    }
    ASSERT_NE(cIdx, SIZE_MAX);
    ASSERT_NE(aIdx, SIZE_MAX);
    ASSERT_NE(bIdx, SIZE_MAX);
    EXPECT_LT(cIdx, aIdx);
    EXPECT_LT(cIdx, bIdx);

    std::remove((dir + "/modA.dr").c_str());
    std::remove((dir + "/modB.dr").c_str());
    std::remove((dir + "/modC.dr").c_str());
    rmdir(dir.c_str());
}

//===----------------------------------------------------------------------===//
// buildGraph - circular import detection
//===----------------------------------------------------------------------===//

TEST(ModuleResolver, CircularImportDetected) {
    auto dir = makeTempDir("circular");
    ASSERT_FALSE(dir.empty());

    // A imports B, B imports A
    writeFile(dir + "/modA.dr", "from modB import b_func\ndef a_func() -> int { return 1 }");
    writeFile(dir + "/modB.dr", "from modA import a_func\ndef b_func() -> int { return 1 }");

    auto module = parse("from modA import a_func\nprint(a_func())\n", /*isDragon=*/true);
    ASSERT_NE(module, nullptr);

    ModuleResolverOptions opts;
    opts.sourceDir = dir + "/";
    ModuleResolver resolver(opts);
    auto graph = resolver.buildGraph(*module, dir + "/main.dr");

    EXPECT_TRUE(graph.hasCycle);
    EXPECT_FALSE(graph.cycleParticipants.empty());

    std::remove((dir + "/modA.dr").c_str());
    std::remove((dir + "/modB.dr").c_str());
    rmdir(dir.c_str());
}

//===----------------------------------------------------------------------===//
// buildGraph - .dr importing .py
//===----------------------------------------------------------------------===//

TEST(ModuleResolver, DragonImportsPython) {
    auto dir = makeTempDir("dr_imports_py");
    ASSERT_FALSE(dir.empty());
    writeFile(dir + "/pymod.py",
        "def py_func(x: int) -> int:\n    return x * 2\n");

    auto module = parse("from pymod import py_func\nprint(py_func(5))\n", /*isDragon=*/true);
    ASSERT_NE(module, nullptr);

    ModuleResolverOptions opts;
    opts.sourceDir = dir + "/";
    ModuleResolver resolver(opts);
    auto graph = resolver.buildGraph(*module, dir + "/main.dr");

    EXPECT_FALSE(graph.hasCycle);
    ASSERT_EQ(graph.modules.size(), 1u);
    EXPECT_EQ(graph.modules[0].name, "pymod");
    EXPECT_FALSE(graph.modules[0].isDragon);

    std::remove((dir + "/pymod.py").c_str());
    rmdir(dir.c_str());
}

//===----------------------------------------------------------------------===//
// buildGraph - stdlib import skipped (not local file)
//===----------------------------------------------------------------------===//

TEST(ModuleResolver, StdlibImportSkipped) {
    // import math should not try to resolve a local file
    auto module = parse("from math import sqrt\nprint(sqrt(4.0))\n", /*isDragon=*/true);
    ASSERT_NE(module, nullptr);

    ModuleResolver resolver;
    auto graph = resolver.buildGraph(*module, "<test>");

    EXPECT_FALSE(graph.hasCycle);
    EXPECT_TRUE(graph.modules.empty());
    EXPECT_FALSE(resolver.hasErrors());
}

//===----------------------------------------------------------------------===//
// buildGraph - missing module produces error
//===----------------------------------------------------------------------===//

TEST(ModuleResolver, MissingModuleInDependency) {
    auto dir = makeTempDir("missing_dep");
    ASSERT_FALSE(dir.empty());

    // modA imports missing_mod which doesn't exist
    writeFile(dir + "/modA.dr",
        "from missing_mod import something\ndef a_func() -> int { return 1 }");

    auto module = parse("from modA import a_func\n", /*isDragon=*/true);
    ASSERT_NE(module, nullptr);

    ModuleResolverOptions opts;
    opts.sourceDir = dir + "/";
    ModuleResolver resolver(opts);
    auto graph = resolver.buildGraph(*module, dir + "/main.dr");

    // modA was still resolved, but missing_mod was not found
    // The resolver should still have modA in the graph
    EXPECT_GE(graph.modules.size(), 1u);

    std::remove((dir + "/modA.dr").c_str());
    rmdir(dir.c_str());
}
