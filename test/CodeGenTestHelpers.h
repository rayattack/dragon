#pragma once

#include <gtest/gtest.h>
#include "TestHelpers.h"
#include "dragon/CodeGen.h"
#include "dragon/Sema.h"
#include "dragon/TypeChecker.h"
#include "dragon/ModuleResolver.h"
#include "dragon/Platform.h"
#include <cstdlib>
#include <memory>
#include <vector>
#include <unordered_map>
#include <cstdio>
#include <fstream>
#include <sstream>
#if defined(_WIN32)
  #include <process.h>
#else
  #include <unistd.h>
#endif

#include "llvm/Support/TargetSelect.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"

#if defined(_WIN32)
  #define DRAGON_TEST_CC          "gcc"
  #define DRAGON_TEST_LINK_FLAGS  " -lpthread -lws2_32 -liphlpapi -lpsapi -luserenv 2>nul"
  #define DRAGON_TEST_DEVNULL     " 2>nul"
  #define DRAGON_TEST_EXE_EXT     ".exe"
#elif defined(__APPLE__)
  #define DRAGON_TEST_CC          "cc"
  #define DRAGON_TEST_LINK_FLAGS  " -lm -lpthread 2>/dev/null"
  #define DRAGON_TEST_DEVNULL     " 2>/dev/null"
  #define DRAGON_TEST_EXE_EXT     ""
#else
  #define DRAGON_TEST_CC          "cc"
  #define DRAGON_TEST_LINK_FLAGS  " -lm -lpthread -ldl 2>/dev/null"
  #define DRAGON_TEST_DEVNULL     " 2>/dev/null"
  #define DRAGON_TEST_EXE_EXT     ""
#endif

using namespace dragon;
using namespace dragon::test;

struct LLVMInit {
    LLVMInit() {
        llvm::InitializeNativeTarget();
        llvm::InitializeNativeTargetAsmPrinter();
        llvm::InitializeNativeTargetAsmParser();
    }
};
inline LLVMInit& getLLVMInit() {
    static LLVMInit init;
    return init;
}
static auto& llvmInit_ = getLLVMInit();

static std::string frontendResolve(Module& module, bool isDragon,
                                   ImportGraph& graph,
                                   std::vector<Module*>& depModules) {
    auto collect = [](auto& stage) {
        std::string e;
        for (auto& d : stage.diagnostics()) e += d.message + "\n";
        return e;
    };
    {
        Sema sema;
        if (!sema.analyze(module))
            return "<sema failed: " + collect(sema) + ">";
    }
    ModuleResolverOptions ropts;
    ropts.sourceDir = dragon::platform::getTempDir() +
                      std::string(1, dragon::platform::pathSeparator());
#ifdef DRAGON_STDLIB_DIR
    ropts.searchPaths.push_back(DRAGON_STDLIB_DIR);
#endif
    ModuleResolver resolver(ropts);
    graph = resolver.buildGraph(module, isDragon ? "test.dr" : "test.py");
    if (graph.hasCycle) return "<resolve failed: import cycle>";

    std::unordered_map<std::string,
        std::unordered_map<std::string, std::shared_ptr<Type>>> allExports;
    std::unordered_map<std::string, std::string> filepaths;
    for (auto& mod : graph.modules) filepaths[mod.name] = mod.filepath;
    for (auto& mod : graph.modules) {
        Sema modSema;
        if (!modSema.analyze(*mod.ast))
            return "<sema failed (" + mod.name + "): " + collect(modSema) + ">";
        TypeChecker modTc;
        for (auto& [mn, ex] : allExports)
            modTc.registerExternalModule(mn, ex, filepaths[mn]);
        modTc.check(*mod.ast);
        if (modTc.hasErrors())
            return "<typecheck failed (" + mod.name + "): " + collect(modTc) + ">";
        allExports[mod.name] = modTc.getExports();
        depModules.push_back(mod.ast.get());
    }
    TypeChecker entryTc;
    for (auto& [mn, ex] : allExports)
        entryTc.registerExternalModule(mn, ex, filepaths[mn]);
    entryTc.check(module);
    if (entryTc.hasErrors())
        return "<typecheck failed: " + collect(entryTc) + ">";
    return "";
}

static std::string generateIR(const std::string& source) {
    auto module = parse(source);
    if (!module) return "<parse failed>";
    ImportGraph graph;
    std::vector<Module*> depModules;
    if (auto fe = frontendResolve(*module, true, graph, depModules);
        !fe.empty()) return fe;
    CodeGen codegen;
    if (!codegen.generate(*module, depModules)) {
        std::string errs;
        for (auto& d : codegen.diagnostics()) errs += d.message + "\n";
        return "<codegen failed: " + errs + ">";
    }
    std::string ir;
    llvm::raw_string_ostream os(ir);
    codegen.getLLVMModule()->print(os, nullptr);
    return ir;
}

static std::string generateIRPy(const std::string& source) {
    auto module = parse(source, false);
    if (!module) return "<parse failed>";
    ImportGraph graph;
    std::vector<Module*> depModules;
    if (auto fe = frontendResolve(*module, false, graph, depModules);
        !fe.empty()) return fe;
    CodeGen codegen;
    if (!codegen.generate(*module, depModules)) {
        std::string errs;
        for (auto& d : codegen.diagnostics()) errs += d.message + "\n";
        return "<codegen failed: " + errs + ">";
    }
    std::string ir;
    llvm::raw_string_ostream os(ir);
    codegen.getLLVMModule()->print(os, nullptr);
    return ir;
}

static size_t countSubstring(const std::string& haystack, const std::string& needle) {
    if (needle.empty()) return 0;
    size_t count = 0;
    size_t pos = 0;
    while ((pos = haystack.find(needle, pos)) != std::string::npos) {
        ++count;
        pos += needle.size();
    }
    return count;
}

static std::string compileAndRunPy(const std::string& source) {
    auto module = parse(source, false);
    if (!module) return "<parse failed>";
    ImportGraph graph;
    std::vector<Module*> depModules;
    if (auto fe = frontendResolve(*module, false, graph, depModules);
        !fe.empty()) return fe;

    CodeGen codegen;
    if (!codegen.generate(*module, depModules)) {
        std::string errs;
        for (auto& d : codegen.diagnostics()) errs += d.message + "\n";
        return "<codegen failed: " + errs + ">";
    }

    std::string tmp = dragon::platform::getTempDir() + std::string(1, dragon::platform::pathSeparator());
    std::string base = tmp + "dragon_llvm_test_py_" + std::to_string(dragon::platform::getProcessId());
    std::string objFile = base + ".o";
    std::string exe = base + DRAGON_TEST_EXE_EXT;
    std::string outFile = base + ".out";

    if (!codegen.compileToObject(objFile)) return "<compile failed>";

    std::string runtimeLib = std::string(CMAKE_BINARY_DIR) + "/libdragon_runtime.a";
    std::string llhttpLib = std::string(DRAGON_LLHTTP_LIB);
    std::string cmd = std::string(DRAGON_TEST_CC) + " -o " + exe + " " + objFile + " " + runtimeLib + " " + llhttpLib;
    cmd += DRAGON_TEST_LINK_FLAGS;
    int result = std::system(cmd.c_str());
    std::remove(objFile.c_str());
    if (result != 0) return "<link failed>";

    cmd = exe + " > " + outFile + " 2>&1";
    result = std::system(cmd.c_str());
    std::remove(exe.c_str());

    std::ifstream f(outFile);
    std::stringstream ss;
    ss << f.rdbuf();
    std::remove(outFile.c_str());
    return ss.str();
}

static std::string compileAndRun(const std::string& source,
                                  const CodeGenOptions& opts = {}) {
    auto module = parse(source);
    if (!module) return "<parse failed>";
    ImportGraph graph;
    std::vector<Module*> depModules;
    if (auto fe = frontendResolve(*module, true, graph, depModules);
        !fe.empty()) return fe;

    CodeGen codegen(opts);
    if (!codegen.generate(*module, depModules)) {
        std::string errs;
        for (auto& d : codegen.diagnostics()) errs += d.message + "\n";
        return "<codegen failed: " + errs + ">";
    }

    std::string tmp = dragon::platform::getTempDir() + std::string(1, dragon::platform::pathSeparator());
    std::string base = tmp + "dragon_llvm_test_" + std::to_string(dragon::platform::getProcessId());
    std::string objFile = base + ".o";
    std::string exe = base + DRAGON_TEST_EXE_EXT;
    std::string outFile = base + ".out";

    if (!codegen.compileToObject(objFile)) return "<compile failed>";

    std::string runtimeLib = std::string(CMAKE_BINARY_DIR) + "/libdragon_runtime.a";
    std::string llhttpLib = std::string(DRAGON_LLHTTP_LIB);
    std::string cmd = std::string(DRAGON_TEST_CC) + " -o " + exe + " " + objFile + " " + runtimeLib + " " + llhttpLib;
    cmd += DRAGON_TEST_LINK_FLAGS;
    int result = std::system(cmd.c_str());
    std::remove(objFile.c_str());
    if (result != 0) return "<link failed>";

    cmd = exe + " > " + outFile + " 2>&1";
    result = std::system(cmd.c_str());
    std::remove(exe.c_str());

    std::ifstream f(outFile);
    std::stringstream ss;
    ss << f.rdbuf();
    std::remove(outFile.c_str());
    return ss.str();
}
