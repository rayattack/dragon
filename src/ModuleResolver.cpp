#include "dragon/ModuleResolver.h"
#include "dragon/Lexer.h"
#include "dragon/Parser.h"
#include "dragon/Platform.h"
#include <fstream>
#include <sstream>
#include <map>
#include <cstdio>
#include <cstdlib>
#include <array>
#include <filesystem>

namespace dragon {

namespace {

#if defined(_WIN32)
  #define DRAGON_POPEN  _popen
  #define DRAGON_PCLOSE _pclose
  static const char* kPythonArgs =
      " -c \"import site; print(site.getsitepackages()[0])\" 2>nul";
  static const char* kPythonCandidates[] = {
      "C:\\Python311\\python.exe", "C:\\Python310\\python.exe",
      "C:\\Python39\\python.exe", nullptr};
#else
  #define DRAGON_POPEN  popen
  #define DRAGON_PCLOSE pclose
  static const char* kPythonArgs =
      " -c \"import site; print(site.getsitepackages()[0])\" 2>/dev/null";
  static const char* kPythonCandidates[] = {
      "/usr/bin/python3", "/usr/local/bin/python3", "/bin/python3", nullptr};
#endif

std::string resolvePythonInterpreter() {
    if (const char* env = std::getenv("DRAGON_PYTHON")) {
        if (env[0] != '\0') {
            std::error_code ec;
            if (std::filesystem::exists(env, ec)) return std::string(env);
            return "";
        }
    }
    for (const char** p = kPythonCandidates; *p; ++p) {
        std::error_code ec;
        if (std::filesystem::exists(*p, ec)) return std::string(*p);
    }
    return "";
}

std::string detectSitePackagesPath() {
    if (const char* env = std::getenv("DRAGON_SITE_PACKAGES")) {
        if (env[0] != '\0') return std::string(env);
    }
    std::string python = resolvePythonInterpreter();
    if (python.empty()) return "";
    std::string cmd = "\"" + python + "\"" + kPythonArgs;
    std::array<char, 256> buffer;
    std::string result;
    FILE* pipe = DRAGON_POPEN(cmd.c_str(), "r");
    if (!pipe) return "";
    while (fgets(buffer.data(), buffer.size(), pipe) != nullptr) {
        result += buffer.data();
    }
    DRAGON_PCLOSE(pipe);
    while (!result.empty() && (result.back() == '\n' || result.back() == '\r')) {
        result.pop_back();
    }
    return result;
}

bool isDirectory(const std::string& path) {
    return platform::isDirectory(path);
}

}

ModuleResolver::ModuleResolver(ModuleResolverOptions options)
    : options_(std::move(options))
{
    if (options_.enableSitePackages && options_.sitePackagesPath.empty()) {
        options_.sitePackagesPath = detectSitePackagesPath();
    }
}

std::string ModuleResolver::findModuleFile(const std::string& moduleName) const {
    std::string pathName = moduleName;
    for (auto& c : pathName) {
        if (c == '.') c = '/';
    }

    std::string topLevel = pathName;
    auto slashPos = topLevel.find('/');
    if (slashPos != std::string::npos) {
        topLevel = topLevel.substr(0, slashPos);
    }

    auto tryFile = [](const std::string& path) -> bool {
        std::ifstream f(path);
        return f.good();
    };

    auto resolveInDir = [&](const std::string& base) -> std::string {
        bool hasFlatDr = tryFile(base + topLevel + ".dr");
        bool hasFlatPy = tryFile(base + topLevel + ".py");
        bool hasFlat = hasFlatDr || hasFlatPy;
        bool hasDir = isDirectory(base + topLevel);

        if (hasFlat && hasDir) {
            std::string flatFile = hasFlatDr
                ? (base + topLevel + ".dr")
                : (base + topLevel + ".py");
            errors_.push_back(
                "module conflict: both '" + flatFile + "' and package '"
                + base + topLevel + "/' exist - remove one");
            return "";
        }

        if (hasFlat && pathName == topLevel) {
            if (hasFlatDr) return base + topLevel + ".dr";
            return base + topLevel + ".py";
        }

        if (hasDir) {
            if (pathName == topLevel) {
                bool hasRootDr = tryFile(base + topLevel + "/" + topLevel + ".dr");
                bool hasInitPy = tryFile(base + topLevel + "/__init__.py");
                if (hasRootDr && hasInitPy) {
                    errors_.push_back(
                        "package conflict: both '" + base + topLevel + "/" + topLevel + ".dr"
                        + "' and '" + base + topLevel + "/__init__.py"
                        + "' exist - remove one");
                    return "";
                }
                if (hasRootDr) return base + topLevel + "/" + topLevel + ".dr";
                if (hasInitPy) return base + topLevel + "/__init__.py";
                errors_.push_back(
                    "package '" + base + topLevel
                    + "/' has no root module (expected '" + topLevel + "/" + topLevel
                    + ".dr' or '" + topLevel + "/__init__.py')");
                return "";
            } else {
                std::string drFile = base + pathName + ".dr";
                if (tryFile(drFile)) return drFile;
                std::string pyFile = base + pathName + ".py";
                if (tryFile(pyFile)) return pyFile;
                if (isDirectory(base + pathName)) {
                    std::string lastSeg = pathName.substr(pathName.rfind('/') + 1);
                    std::string subRootDr = base + pathName + "/" + lastSeg + ".dr";
                    if (tryFile(subRootDr)) return subRootDr;
                    std::string subInitPy = base + pathName + "/__init__.py";
                    if (tryFile(subInitPy)) return subInitPy;
                }
            }
        }

        return "";
    };

    if (!options_.sourceDir.empty()) {
        std::string result = resolveInDir(options_.sourceDir);
        if (!result.empty()) return result;
        if (!errors_.empty()) return "";
    }

    if (!options_.drxDir.empty()) {
        std::string drxBase = options_.drxDir;
        if (drxBase.back() != '/') drxBase += '/';
        std::string pkgRoot = drxBase + topLevel;
        if (isDirectory(pkgRoot)) {
            std::string entryRel;
            {
                std::ifstream hint(pkgRoot + "/.dragon-entry");
                if (hint.good()) {
                    std::getline(hint, entryRel);
                    while (!entryRel.empty() &&
                           (entryRel.back() == '\r' || entryRel.back() == ' ' ||
                            entryRel.back() == '\n' || entryRel.back() == '\t')) {
                        entryRel.pop_back();
                    }
                }
            }
            if (!entryRel.empty()) {
                std::string srcSub;
                auto sl = entryRel.rfind('/');
                if (sl != std::string::npos) srcSub = entryRel.substr(0, sl);
                std::string srcDir = srcSub.empty() ? pkgRoot : (pkgRoot + "/" + srcSub);
                if (pathName == topLevel) {
                    std::string cand = pkgRoot + "/" + entryRel;
                    if (tryFile(cand)) return cand;
                } else {
                    std::string sub = pathName.substr(topLevel.size() + 1);
                    std::string candDr = srcDir + "/" + sub + ".dr";
                    if (tryFile(candDr)) return candDr;
                    std::string candPy = srcDir + "/" + sub + ".py";
                    if (tryFile(candPy)) return candPy;
                }
            }
            std::string result = resolveInDir(drxBase);
            if (!result.empty()) return result;
            if (!errors_.empty()) return "";
        }
    }

    for (const auto& dir : options_.searchPaths) {
        std::string base = dir;
        if (!base.empty() && base.back() != '/') base += '/';
        std::string result = resolveInDir(base);
        if (!result.empty()) return result;
        if (!errors_.empty()) return "";
    }

    if (options_.enableSitePackages && !options_.sitePackagesPath.empty()) {
        std::string base = options_.sitePackagesPath;
        if (!base.empty() && base.back() != '/') base += '/';
        std::string pyFile = base + pathName + ".py";
        if (tryFile(pyFile)) return pyFile;
        std::string initPy = base + pathName + "/__init__.py";
        if (tryFile(initPy)) return initPy;
    }

    return "";
}

std::string ModuleResolver::packageOriginFor(const std::string& filepath) const {
    if (options_.drxDir.empty()) return "";
    std::string drxBase = options_.drxDir;
    if (!drxBase.empty() && drxBase.back() != '/') drxBase += '/';
    if (filepath.size() > drxBase.size() &&
        filepath.compare(0, drxBase.size(), drxBase) == 0) {
        std::string rest = filepath.substr(drxBase.size());
        auto sl = rest.find('/');
        return sl == std::string::npos ? rest : rest.substr(0, sl);
    }
    return "";
}

static bool isPseudoModule(const std::string& name) {
    return name == "typing";
}

ImportGraph ModuleResolver::buildGraph(Module& entryModule, const std::string& entryFile) {
    errors_.clear();
    ImportGraph graph;

    std::map<std::string, Color> colors;

    for (auto& stmt : entryModule.body) {
        if (auto* fromImp = dynamic_cast<FromImportStmt*>(stmt.get())) {
            enqueueFromImport(*fromImp, colors, graph);
            continue;
        }
        if (auto* imp = dynamic_cast<ImportStmt*>(stmt.get())) {
            for (auto& alias : imp->names) {
                if (isPseudoModule(alias.name)) continue;
                if (colors.find(alias.name) == colors.end()) {
                    colors[alias.name] = Color::White;
                    dfs(alias.name, colors, graph);
                }
            }
            continue;
        }
    }

    return graph;
}

void ModuleResolver::enqueueFromImport(const FromImportStmt& fromImp,
                                        std::map<std::string, Color>& colors,
                                        ImportGraph& graph) {
    const std::string& moduleName = fromImp.module;
    if (moduleName.empty()) return;

    if (isPseudoModule(moduleName)) return;
    if (colors.find(moduleName) == colors.end()) {
        colors[moduleName] = Color::White;
        dfs(moduleName, colors, graph);
    }

    for (auto& alias : fromImp.names) {
        if (alias.name.empty() || alias.name == "*") continue;
        std::string sub = moduleName + "." + alias.name;
        if (colors.find(sub) != colors.end()) continue;
        size_t errMark = errors_.size();
        std::string subFile = findModuleFile(sub);
        if (subFile.empty()) {
            if (errors_.size() > errMark) errors_.resize(errMark);
            continue;
        }
        colors[sub] = Color::White;
        dfs(sub, colors, graph);
    }
}

void ModuleResolver::dfs(const std::string& moduleName,
                          std::map<std::string, Color>& colors,
                          ImportGraph& graph) {
    colors[moduleName] = Color::Gray;

    std::string filepath = findModuleFile(moduleName);
    if (filepath.empty()) {
        if (errors_.empty()) {
            errors_.push_back("cannot find module '" + moduleName + "'");
        }
        colors[moduleName] = Color::Black;
        return;
    }

    std::ifstream file(filepath);
    if (!file) {
        errors_.push_back("cannot open module file: " + filepath);
        colors[moduleName] = Color::Black;
        return;
    }
    std::stringstream buf;
    buf << file.rdbuf();
    std::string source = buf.str();
    if (source.empty()) {
        colors[moduleName] = Color::Black;
        return;
    }

    bool isDragon = filepath.size() > 3 &&
                    filepath.substr(filepath.size() - 3) == ".dr";

    LexerOptions lexOpts;
    lexOpts.useBraceBlocks = isDragon;
    lexOpts.filename = filepath;
    Lexer lexer(source, lexOpts);
    auto tokens = lexer.tokenize();
    if (lexer.hasErrors()) {
        errors_.push_back("lexer errors in module '" + moduleName + "' (" + filepath + ")");
        colors[moduleName] = Color::Black;
        return;
    }

    ParserOptions parseOpts;
    parseOpts.isDragonFile = isDragon;
    parseOpts.requireTypes = isDragon;
    parseOpts.filename = filepath;
    Parser parser(std::move(tokens), parseOpts);
    auto ast = parser.parseModule();
    if (parser.hasErrors()) {
        errors_.push_back("parser errors in module '" + moduleName + "' (" + filepath + ")");
        colors[moduleName] = Color::Black;
        return;
    }

    for (auto& stmt : ast->body) {
        if (auto* fromImp = dynamic_cast<FromImportStmt*>(stmt.get())) {
            const std::string& depName = fromImp->module;
            if (!depName.empty()) {
                std::string depFile = findModuleFile(depName);
                if (!depFile.empty()) {
                    auto it = colors.find(depName);
                    if (it != colors.end() && it->second == Color::Gray) {
                        graph.hasCycle = true;
                        graph.cycleParticipants.push_back(depName);
                        graph.cycleParticipants.push_back(moduleName);
                    }
                }
            }
            for (auto& alias : fromImp->names) {
                if (alias.name.empty() || alias.name == "*") continue;
                std::string sub = depName + "." + alias.name;
                auto it = colors.find(sub);
                if (it != colors.end() && it->second == Color::Gray) {
                    graph.hasCycle = true;
                    graph.cycleParticipants.push_back(sub);
                    graph.cycleParticipants.push_back(moduleName);
                }
            }
            enqueueFromImport(*fromImp, colors, graph);
            continue;
        }
        if (auto* imp = dynamic_cast<ImportStmt*>(stmt.get())) {
            for (auto& alias : imp->names) {
                if (isPseudoModule(alias.name)) continue;

                auto it = colors.find(alias.name);
                if (it == colors.end()) {
                    colors[alias.name] = Color::White;
                    dfs(alias.name, colors, graph);
                } else if (it->second == Color::Gray) {
                    graph.hasCycle = true;
                    graph.cycleParticipants.push_back(alias.name);
                    graph.cycleParticipants.push_back(moduleName);
                }
            }
            continue;
        }
    }

    ResolvedModule resolved;
    resolved.name = moduleName;
    resolved.filepath = filepath;
    resolved.isDragon = isDragon;
    resolved.packageOrigin = packageOriginFor(filepath);
    ast->moduleName = moduleName;
    resolved.ast = std::move(ast);
    graph.modules.push_back(std::move(resolved));

    colors[moduleName] = Color::Black;
}

}
