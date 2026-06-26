/**
 * Dragon ModuleResolver API Reference
 * ====================================
 * Source: include/dragon/ModuleResolver.h
 *
 * Resolves module imports, finds source files, lexes/parses them, and builds
 * an import dependency graph with topological ordering and cycle detection.
 *
 * Resolution order: source dir -> search paths (DRAGON_STDLIB_DIR) -> site-packages.
 * STRICT rule: flat file XOR package directory (never both).
 * Package root: name/name.dr (e.g., os/os.dr for the os package).
 * Dotted imports: from os.path import join -> stdlib/os/path.dr
 */

#pragma once
#include <string>
#include <vector>
#include <memory>

// Forward declarations
class Module;

// ============================================================================
// 1. RESOLVED MODULE
// ============================================================================

/**
 * A resolved module with its parsed AST, ready for compilation.
 */
struct ResolvedModule {
    std::string name;                   ///< Module name (e.g., "math_utils", "os.path")
    std::string filepath;               ///< Absolute file path of the resolved source
    bool isDragon;                      ///< true = .dr mode, false = .py mode
    std::unique_ptr<Module> ast;        ///< Parsed AST (Lexer + Parser already ran)
};


// ============================================================================
// 2. IMPORT GRAPH
// ============================================================================

/**
 * Result of building the import dependency graph.
 * Contains modules in topological order (dependencies first, entry module last).
 */
struct ImportGraph {
    std::vector<ResolvedModule> modules;        ///< Modules in dependency order
    bool hasCycle;                               ///< true if circular import detected
    std::vector<std::string> cycleParticipants; ///< Module names involved in the cycle
};


// ============================================================================
// 3. MODULE RESOLVER OPTIONS
// ============================================================================

/**
 * Configuration for module resolution.
 */
struct ModuleResolverOptions {
    std::vector<std::string> searchPaths;  ///< Additional search directories (-I flags)
    std::string sourceDir;                 ///< Directory of the entry source file
    bool enableSitePackages = false;       ///< Search Python site-packages directory
    std::string sitePackagesPath;          ///< Explicit site-packages path (auto-detected if empty)
};


// ============================================================================
// 4. MODULE RESOLVER CLASS
// ============================================================================

/**
 * Resolves module imports, lexes/parses discovered modules, and builds
 * a topologically-ordered import graph.
 */
class ModuleResolver {
public:
    /**
     * Construct a module resolver.
     * @param options Search paths, source directory, site-packages config
     */
    explicit ModuleResolver(ModuleResolverOptions options = {}) {}

    /**
     * Find the source file for a module name.
     * Searches: source dir -> search paths -> site-packages.
     * @param moduleName Dotted module name (e.g., "os.path", "math")
     * @return Absolute file path, or empty string if not found
     */
    std::string findModuleFile(const std::string& moduleName) const { return {}; }

    /**
     * Build the full import graph starting from the entry module.
     * Recursively resolves all imports, lexes/parses each module,
     * detects cycles via DFS coloring, and returns topological order.
     * @param entryModule Already-parsed entry module AST
     * @param entryFile Path to the entry source file
     * @return ImportGraph with modules in dependency order
     */
    ImportGraph buildGraph(Module& entryModule, const std::string& entryFile) { return {}; }

    /**
     * Get all errors encountered during resolution.
     * @return Vector of error message strings
     */
    const std::vector<std::string>& errors() const { return {}; }

    /**
     * Check if any errors occurred.
     * @return true if at least one error
     */
    bool hasErrors() const { return false; }

    // --- Private (listed for reference) ---
    // enum Color { White, Gray, Black }; - DFS cycle detection states
    // void dfs(moduleName, colors, graph) - recursive DFS traversal
};
