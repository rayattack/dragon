/**
 * Dragon StdlibRegistry API Reference
 * ====================================
 * Source: include/dragon/StdlibRegistry.h
 *
 * Singleton registry mapping Python stdlib module names to C equivalents.
 * Used as a FALLBACK only - file-resolved modules (tracked in fileResolvedModules)
 * take priority over the registry.
 *
 * Maps: module name -> symbol name -> { cInclude, cName }
 * Example: "math" -> "sqrt" -> { "math.h", "sqrt" }
 */

#pragma once
#include <string>
#include <map>
#include <set>

// ============================================================================
// 1. STDLIB ENTRY
// ============================================================================

/**
 * Maps a single Python stdlib symbol to its C equivalent.
 */
struct StdlibEntry {
    const char* cInclude;  ///< C header to include (e.g., "math.h"), empty string if none
    const char* cName;     ///< C function/expression name (e.g., "sqrt", "M_PI")
};


// ============================================================================
// 2. STDLIB REGISTRY CLASS (Singleton)
// ============================================================================

/**
 * Singleton registry of Python stdlib module -> symbol -> C mapping.
 * Provides fallback resolution when no .dr/.py source file exists.
 */
class StdlibRegistry {
public:
    /**
     * Get the singleton registry instance.
     * Initializes on first call with all known stdlib mappings.
     * @return Reference to the global StdlibRegistry
     */
    static const StdlibRegistry& instance() { static StdlibRegistry r; return r; }

    /**
     * Look up a module by name.
     * @param moduleName Module name (e.g., "math", "os", "sys")
     * @return Pointer to symbol map, or nullptr if module not registered
     */
    const std::map<std::string, StdlibEntry>* findModule(const std::string& moduleName) const { return nullptr; }

    /**
     * Resolve a full "import module" or "import module as alias" statement.
     * Populates aliases and required C includes for all symbols in the module.
     * @param moduleName Module to import (e.g., "math")
     * @param asName Alias (e.g., "m"), or same as moduleName
     * @param outSymbolAliases [out] Map of python_name -> c_expression
     * @param outExtraIncludes [out] Set of C headers to include
     */
    void resolveImport(
        const std::string& moduleName,
        const std::string& asName,
        std::map<std::string, std::string>& outSymbolAliases,
        std::set<std::string>& outExtraIncludes
    ) const {}

    /**
     * Resolve a "from module import symbol [as alias]" statement.
     * @param moduleName Module name
     * @param symbolName Symbol to import (e.g., "sqrt")
     * @param asName Alias for the symbol
     * @param outSymbolAliases [out] Map of alias -> c_expression
     * @param outExtraIncludes [out] Set of C headers to include
     * @return true if symbol was found in the module
     */
    bool resolveFromImport(
        const std::string& moduleName,
        const std::string& symbolName,
        const std::string& asName,
        std::map<std::string, std::string>& outSymbolAliases,
        std::set<std::string>& outExtraIncludes
    ) const { return false; }

    /**
     * Resolve "from module import *" - import all symbols.
     * @param moduleName Module name
     * @param outSymbolAliases [out] Map of python_name -> c_expression for all symbols
     * @param outExtraIncludes [out] Set of all required C headers
     */
    void resolveFromImportStar(
        const std::string& moduleName,
        std::map<std::string, std::string>& outSymbolAliases,
        std::set<std::string>& outExtraIncludes
    ) const {}

private:
    StdlibRegistry() {}  // Singleton - private constructor
};
