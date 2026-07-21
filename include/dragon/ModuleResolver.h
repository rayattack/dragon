#ifndef DRAGON_MODULE_RESOLVER_H
#define DRAGON_MODULE_RESOLVER_H

#include "dragon/AST.h"
#include <string>
#include <vector>
#include <memory>
#include <map>

namespace dragon {

/// A resolved module with its parsed AST
struct ResolvedModule {
    std::string name;         // module name (e.g., "math_utils")
    std::string filepath;     // resolved file path
    bool isDragon = false;    // .dr vs .py
    // Owning egg package name when the module resolved out of `.drx/<pkg>/`
    // (D022); empty for the root project's own sources and the stdlib. This is
    // the anchor a future capability-enforcement pass keys off (D022 §8).
    std::string packageOrigin;
    std::unique_ptr<Module> ast;
};

/// Result of building the import graph
struct ImportGraph {
    /// Modules in topological order (dependencies first, entry module last)
    std::vector<ResolvedModule> modules;
    bool hasCycle = false;
    std::vector<std::string> cycleParticipants;
};

/// Options for module resolution
struct ModuleResolverOptions {
    /// Additional search paths for modules (from -I flags)
    std::vector<std::string> searchPaths;
    /// Directory of the entry source file (for relative imports)
    std::string sourceDir;
    /// Project-local egg directory (`.drx/`, D022). Searched as a tier between
    /// sourceDir and searchPaths/stdlib. Empty disables the tier.
    std::string drxDir;
    /// Enable searching in Python site-packages
    bool enableSitePackages = false;
    /// Explicit site-packages path (auto-detected if empty and enabled)
    std::string sitePackagesPath;
};

/// Resolves module imports by finding files, lexing, and parsing them.
///
/// Builds a full import graph with cycle detection and topological ordering.
class ModuleResolver {
public:
    explicit ModuleResolver(ModuleResolverOptions options = {});

    /// Find the file for a module name. Returns empty string if not found.
    std::string findModuleFile(const std::string& moduleName) const;

    /// The owning egg package for a resolved filepath, or "" if it isn't under
    /// `.drx/` (root project / stdlib). Used to tag ResolvedModule.packageOrigin.
    std::string packageOriginFor(const std::string& filepath) const;

    /// Build the import graph starting from an already-parsed entry module.
    /// Walks import statements, resolves each to a file, lexes/parses them,
    /// and recursively processes their imports.
    /// Returns modules in topological order (dependencies first).
    ImportGraph buildGraph(Module& entryModule, const std::string& entryFile);

    /// Get errors encountered during resolution
    const std::vector<std::string>& errors() const { return errors_; }
    bool hasErrors() const { return !errors_.empty(); }

private:
    enum class Color { White, Gray, Black };

    void dfs(const std::string& moduleName,
             std::map<std::string, Color>& colors,
             ImportGraph& graph);

    /// Resolve a `from X import Y, Z` statement: enqueue source module X plus
    /// any Y/Z that resolve to submodule files (Python's submodule fallback).
    void enqueueFromImport(const FromImportStmt& fromImp,
                            std::map<std::string, Color>& colors,
                            ImportGraph& graph);

    ModuleResolverOptions options_;
    mutable std::vector<std::string> errors_;
};

} // namespace dragon

#endif // DRAGON_MODULE_RESOLVER_H
