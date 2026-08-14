// TODO: cache parsed modules on disk for incremental builds
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
  // No bare `python3`: popen() runs it via `/bin/sh -c`, which resolves it
  // through $PATH, so a writable earlier PATH dir (e.g. CI's ./node_modules/.bin) could hijack it.
  static const char* kPythonCandidates[] = {
      "/usr/bin/python3", "/usr/local/bin/python3", "/bin/python3", nullptr};
#endif

/// Trusted absolute Python path: honors a DRAGON_PYTHON override, else the
/// first existing candidate. Returns "" rather than falling back to $PATH.
std::string resolvePythonInterpreter() {
    if (const char* env = std::getenv("DRAGON_PYTHON")) {
        if (env[0] != '\0') {
            std::error_code ec;
            if (std::filesystem::exists(env, ec)) return std::string(env);
            return "";  // explicit override that doesn't exist: do not guess
        }
    }
    for (const char** p = kPythonCandidates; *p; ++p) {
        std::error_code ec;
        if (std::filesystem::exists(*p, ec)) return std::string(*p);
    }
    return "";
}

/// Auto-detect the Python site-packages path.
std::string detectSitePackagesPath() {
    // DRAGON_SITE_PACKAGES wins if set: no interpreter is launched at all.
    if (const char* env = std::getenv("DRAGON_SITE_PACKAGES")) {
        if (env[0] != '\0') return std::string(env);
    }
    std::string python = resolvePythonInterpreter();
    if (python.empty()) return "";  // no trusted interpreter; skip auto-detect
    // Built from a trusted absolute path + fixed args (no user input), so the
    // popen shell can't be injected or redirected via $PATH.
    std::string cmd = "\"" + python + "\"" + kPythonArgs;
    std::array<char, 256> buffer;
    std::string result;
    FILE* pipe = DRAGON_POPEN(cmd.c_str(), "r");
    if (!pipe) return "";
    while (fgets(buffer.data(), buffer.size(), pipe) != nullptr) {
        result += buffer.data();
    }
    DRAGON_PCLOSE(pipe);
    // Trim trailing newline
    while (!result.empty() && (result.back() == '\n' || result.back() == '\r')) {
        result.pop_back();
    }
    return result;
}

bool isDirectory(const std::string& path) {
    return platform::isDirectory(path);
}

} // anonymous namespace

ModuleResolver::ModuleResolver(ModuleResolverOptions options)
    : options_(std::move(options))
{
    if (options_.enableSitePackages && options_.sitePackagesPath.empty()) {
        options_.sitePackagesPath = detectSitePackagesPath();
    }
}

/// Resolve a module name to a file path. Flat file and package dir are
/// mutually exclusive (conflict = error); dotted names split into a package root plus submodule segments.
std::string ModuleResolver::findModuleFile(const std::string& moduleName) const {
    // Convert dots to directory separators: "os.path" -> "os/path"
    std::string pathName = moduleName;
    for (auto& c : pathName) {
        if (c == '.') c = '/';
    }

    // Top-level segment for conflict detection: "os.path" -> "os", "os" -> "os".
    std::string topLevel = pathName;
    auto slashPos = topLevel.find('/');
    if (slashPos != std::string::npos) {
        topLevel = topLevel.substr(0, slashPos);
    }

    auto tryFile = [](const std::string& path) -> bool {
        std::ifstream f(path);
        return f.good();
    };

    // Resolve in one base dir; returns the file, "" if not found, sets an error on conflict.
    auto resolveInDir = [&](const std::string& base) -> std::string {
        bool hasFlatDr = tryFile(base + topLevel + ".dr");
        bool hasFlatPy = tryFile(base + topLevel + ".py");
        bool hasFlat = hasFlatDr || hasFlatPy;
        bool hasDir = isDirectory(base + topLevel);

        // Rule: flat file XOR package directory - never both
        if (hasFlat && hasDir) {
            std::string flatFile = hasFlatDr
                ? (base + topLevel + ".dr")
                : (base + topLevel + ".py");
            errors_.push_back(
                "module conflict: both '" + flatFile + "' and package '"
                + base + topLevel + "/' exist - remove one");
            return "";
        }

        // --- Case 1: Flat file (no dots, or no package dir) ---
        if (hasFlat && pathName == topLevel) {
            // Simple flat module: "os" -> os.dr or os.py
            if (hasFlatDr) return base + topLevel + ".dr";
            return base + topLevel + ".py";
        }

        // --- Case 2: Package directory ---
        if (hasDir) {
            if (pathName == topLevel) {
                // Package root import ("os" -> os/os.dr or os/__init__.py); both existing is a conflict.
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
                // Package dir exists but no root file - error on direct import
                errors_.push_back(
                    "package '" + base + topLevel
                    + "/' has no root module (expected '" + topLevel + "/" + topLevel
                    + ".dr' or '" + topLevel + "/__init__.py')");
                return "";
            } else {
                // Importing a submodule: "os.path" -> os/path.dr or os/path.py
                std::string drFile = base + pathName + ".dr";
                if (tryFile(drFile)) return drFile;
                std::string pyFile = base + pathName + ".py";
                if (tryFile(pyFile)) return pyFile;
                // Could be a sub-package: os/path/path.dr or os/path/__init__.py
                if (isDirectory(base + pathName)) {
                    // Extract the last segment for sub-package root
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

    // Search source directory first
    if (!options_.sourceDir.empty()) {
        std::string result = resolveInDir(options_.sourceDir);
        if (!result.empty()) return result;
        if (!errors_.empty()) return "";  // conflict error - stop searching
    }

    // Project-local egg dir `.drx/` (D022), tried between sourceDir and stdlib.
    // `.dragon-entry` (one line: entry path) records a custom src/ layout; read via plain ifstream, no `.drs` parsing or fork.
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
                    // trim trailing CR/space
                    while (!entryRel.empty() &&
                           (entryRel.back() == '\r' || entryRel.back() == ' ' ||
                            entryRel.back() == '\n' || entryRel.back() == '\t')) {
                        entryRel.pop_back();
                    }
                }
            }
            if (!entryRel.empty()) {
                // Hint-driven: root import -> <pkg>/<entryRel>; submodules resolve in
                // the entry's directory (src/http.dr -> http.client at src/client.dr).
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
            // No hint (or miss): fall back to <pkg>/<pkg>.dr + submodule convention.
            std::string result = resolveInDir(drxBase);
            if (!result.empty()) return result;
            if (!errors_.empty()) return "";
        }
    }

    // Search additional paths (includes DRAGON_STDLIB_DIR)
    for (const auto& dir : options_.searchPaths) {
        std::string base = dir;
        if (!base.empty() && base.back() != '/') base += '/';
        std::string result = resolveInDir(base);
        if (!result.empty()) return result;
        if (!errors_.empty()) return "";
    }

    // Search site-packages (Python-only: .py and __init__.py)
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
        std::string rest = filepath.substr(drxBase.size());  // "http/src/http.dr"
        auto sl = rest.find('/');
        return sl == std::string::npos ? rest : rest.substr(0, sl);  // "http"
    }
    return "";
}

ImportGraph ModuleResolver::buildGraph(Module& entryModule, const std::string& entryFile) {
    errors_.clear();
    ImportGraph graph;

    // Extract import names from the entry module
    std::map<std::string, Color> colors;

    for (auto& stmt : entryModule.body) {
        if (auto* fromImp = dynamic_cast<FromImportStmt*>(stmt.get())) {
            enqueueFromImport(*fromImp, colors, graph);
            continue;
        }
        if (auto* imp = dynamic_cast<ImportStmt*>(stmt.get())) {
            for (auto& alias : imp->names) {
                // Only resolve local modules, not stdlib
                std::string file = findModuleFile(alias.name);
                if (!file.empty() && colors.find(alias.name) == colors.end()) {
                    colors[alias.name] = Color::White;
                    dfs(alias.name, colors, graph);
                }
            }
            continue;
        }
    }

    return graph;
}

// Enqueues the source module plus any imported name that's really a submodule
// (mirrors Python's attribute-vs-submodule `from X import Y` fallback); a miss stays silent since TypeChecker may resolve Y as a value export.
void ModuleResolver::enqueueFromImport(const FromImportStmt& fromImp,
                                        std::map<std::string, Color>& colors,
                                        ImportGraph& graph) {
    const std::string& moduleName = fromImp.module;
    if (moduleName.empty()) return;

    // Enqueue the source module (e.g. "controllers" for `from controllers import health`).
    if (!findModuleFile(moduleName).empty() && colors.find(moduleName) == colors.end()) {
        colors[moduleName] = Color::White;
        dfs(moduleName, colors, graph);
    }

    // Probe X.Y as a submodule file for each imported name; suppress any
    // diagnostic from the probe since a miss just means Y is a value export.
    for (auto& alias : fromImp.names) {
        if (alias.name.empty() || alias.name == "*") continue;
        std::string sub = moduleName + "." + alias.name;
        if (colors.find(sub) != colors.end()) continue;
        size_t errMark = errors_.size();
        std::string subFile = findModuleFile(sub);
        if (subFile.empty()) {
            // Drop any diagnostic from the probe (e.g. "package has no root").
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

    // Read the file
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

    // Determine if Dragon file
    bool isDragon = filepath.size() > 3 &&
                    filepath.substr(filepath.size() - 3) == ".dr";

    // Lex
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

    // Parse
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

    // Recursively process this module's imports
    for (auto& stmt : ast->body) {
        if (auto* fromImp = dynamic_cast<FromImportStmt*>(stmt.get())) {
            // Cycle detection must check both the source module and submodule
            // probes: enqueueFromImport only does white->gray DFS, not this check.
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
                std::string depFile = findModuleFile(alias.name);
                if (depFile.empty()) continue;

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

    // Post-order: add this module after all its dependencies
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

} // namespace dragon
