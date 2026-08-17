#ifndef DRAGON_MODULE_RESOLVER_H
#define DRAGON_MODULE_RESOLVER_H

#include "dragon/AST.h"
#include <string>
#include <vector>
#include <memory>
#include <map>

namespace dragon {

struct ResolvedModule {
    std::string name;
    std::string filepath;
    bool isDragon = false;
    std::string packageOrigin;
    std::unique_ptr<Module> ast;
};

struct ImportGraph {
    std::vector<ResolvedModule> modules;
    bool hasCycle = false;
    std::vector<std::string> cycleParticipants;
};

struct ModuleResolverOptions {
    std::vector<std::string> searchPaths;
    std::string sourceDir;
    std::string drxDir;
    bool enableSitePackages = false;
    std::string sitePackagesPath;
};

class ModuleResolver {
public:
    explicit ModuleResolver(ModuleResolverOptions options = {});

    std::string findModuleFile(const std::string& moduleName) const;

    std::string packageOriginFor(const std::string& filepath) const;

    ImportGraph buildGraph(Module& entryModule, const std::string& entryFile);

    const std::vector<std::string>& errors() const { return errors_; }
    bool hasErrors() const { return !errors_.empty(); }

private:
    enum class Color { White, Gray, Black };

    void dfs(const std::string& moduleName,
             std::map<std::string, Color>& colors,
             ImportGraph& graph);

    void enqueueFromImport(const FromImportStmt& fromImp,
                            std::map<std::string, Color>& colors,
                            ImportGraph& graph);

    ModuleResolverOptions options_;
    mutable std::vector<std::string> errors_;
};

}

#endif
