#ifndef DRAGON_STDLIB_REGISTRY_H
#define DRAGON_STDLIB_REGISTRY_H

#include <string>
#include <map>
#include <set>

namespace dragon {

/// Maps a Python stdlib symbol to its C equivalent.
struct StdlibEntry {
    const char* cInclude;  // header to include, "" if none
    const char* cName;     // C expression/function name
};

/// Python stdlib module -> symbol -> C mapping.
class StdlibRegistry {
public:
    static const StdlibRegistry& instance();

    /// Looks up a module by name; nullptr if not found.
    const std::map<std::string, StdlibEntry>* findModule(const std::string& moduleName) const;

    /// Registers all symbols from `import moduleName as asName`.
    void resolveImport(const std::string& moduleName,
                       const std::string& asName,
                       std::map<std::string, std::string>& outSymbolAliases,
                       std::set<std::string>& outExtraIncludes) const;

    /// Registers one symbol from `from moduleName import symbolName as asName`;
    /// returns false if the symbol isn't in the registry.
    bool resolveFromImport(const std::string& moduleName,
                           const std::string& symbolName,
                           const std::string& asName,
                           std::map<std::string, std::string>& outSymbolAliases,
                           std::set<std::string>& outExtraIncludes) const;

    /// Registers every symbol for `from moduleName import *`.
    void resolveFromImportStar(const std::string& moduleName,
                               std::map<std::string, std::string>& outSymbolAliases,
                               std::set<std::string>& outExtraIncludes) const;

private:
    StdlibRegistry();
    std::map<std::string, std::map<std::string, StdlibEntry>> registry_;
};

} // namespace dragon

#endif // DRAGON_STDLIB_REGISTRY_H
