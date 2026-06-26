#ifndef DRAGON_STDLIB_REGISTRY_H
#define DRAGON_STDLIB_REGISTRY_H

#include <string>
#include <map>
#include <set>

namespace dragon {

/// Entry mapping a Python stdlib symbol to its C equivalent
struct StdlibEntry {
    const char* cInclude;  // C header to include (e.g. "math.h"), empty string if none
    const char* cName;     // C expression/function name
};

/// Registry of Python stdlib module -> symbol -> C mapping
class StdlibRegistry {
public:
    /// Get the singleton registry instance
    static const StdlibRegistry& instance();

    /// Look up a module by name. Returns nullptr if not found.
    const std::map<std::string, StdlibEntry>* findModule(const std::string& moduleName) const;

    /// Resolve an import statement: register all symbols from a module.
    /// @param moduleName The Python module name (e.g. "math")
    /// @param asName Optional alias (e.g. "m" from "import math as m")
    /// @param outSymbolAliases Map to populate with python_name -> c_expression
    /// @param outExtraIncludes Set to populate with required C include headers
    void resolveImport(const std::string& moduleName,
                       const std::string& asName,
                       std::map<std::string, std::string>& outSymbolAliases,
                       std::set<std::string>& outExtraIncludes) const;

    /// Resolve a from-import statement: register specific symbols from a module.
    /// @param moduleName The Python module name
    /// @param symbolName The imported symbol name
    /// @param asName Optional alias
    /// @param outSymbolAliases Map to populate
    /// @param outExtraIncludes Set to populate
    /// @return true if the symbol was found in the registry
    bool resolveFromImport(const std::string& moduleName,
                           const std::string& symbolName,
                           const std::string& asName,
                           std::map<std::string, std::string>& outSymbolAliases,
                           std::set<std::string>& outExtraIncludes) const;

    /// Resolve a `from module import *` statement
    void resolveFromImportStar(const std::string& moduleName,
                               std::map<std::string, std::string>& outSymbolAliases,
                               std::set<std::string>& outExtraIncludes) const;

private:
    StdlibRegistry();
    std::map<std::string, std::map<std::string, StdlibEntry>> registry_;
};

} // namespace dragon

#endif // DRAGON_STDLIB_REGISTRY_H
