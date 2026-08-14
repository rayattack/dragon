#ifndef DRAGON_MODULE_RESOLVER_H
#define DRAGON_MODULE_RESOLVER_H

#include "dragon/AST.h"
#include <string>
#include <vector>
#include <memory>
#include <map>

namespace dragon {

/// A resolved module with its parsed AST.
struct ResolvedModule {
    std::string name;
    std::string filepath;
    bool isDragon = false;    // .dr vs .py
    // Owning egg package if resolved from `.drx/<pkg>/` (D022); "" for
    // root-project/stdlib sources. Anchor for a future capability-enforcement pass.
    std::string packageOrigin;
    std::unique_ptr<Module> ast;
};

/// Result of building the import graph.
struct ImportGraph {
    std::vector<ResolvedModule> modules;  // topological order, dependencies first
    bool hasCycle = false;
    std::vector<std::string> cycleParticipants;
};

/// Options for module resolution.
struct ModuleResolverOptions {
    std::vector<std::string> searchPaths;  // -I flags
    std::string sourceDir;                 // entry file's dir, for relative imports
    // Project-local egg dir (`.drx/`, D022): searched between sourceDir and
    // searchPaths/stdlib. "" disables the tier.
    std::string drxDir;
    bool enableSitePackages = false;
    std::string sitePackagesPath;  // auto-detected if empty and enabled
};

/// Resolves imports by finding/lexing/parsing files into a topologically
/// ordered import graph with cycle detection.
class ModuleResolver {
public:
    explicit ModuleResolver(ModuleResolverOptions options = {});

    /// Finds the file for a module name; "" if not found.
    std::string findModuleFile(const std::string& moduleName) const;

    /// Owning egg package for a filepath, or "" if not under `.drx/`.
    std::string packageOriginFor(const std::string& filepath) const;

    /// Walks imports from an already-parsed entry module, resolving/lexing/
    /// parsing each recursively. Returns modules dependencies-first.
    ImportGraph buildGraph(Module& entryModule, const std::string& entryFile);

    const std::vector<std::string>& errors() const { return errors_; }
    bool hasErrors() const { return !errors_.empty(); }

private:
    enum class Color { White, Gray, Black };

    void dfs(const std::string& moduleName,
             std::map<std::string, Color>& colors,
             ImportGraph& graph);

    /// Enqueues source module X for `from X import Y, Z`, plus any Y/Z that
    /// resolve to submodule files (Python's submodule fallback).
    void enqueueFromImport(const FromImportStmt& fromImp,
                            std::map<std::string, Color>& colors,
                            ImportGraph& graph);

    ModuleResolverOptions options_;
    mutable std::vector<std::string> errors_;
};

} // namespace dragon

#endif // DRAGON_MODULE_RESOLVER_H
