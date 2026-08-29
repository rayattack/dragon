#include "ReplFrontend.h"

#include <algorithm>
#include <unordered_map>

#include "dragon/AstClone.h"
#include "dragon/DefiniteAssignment.h"
#include "dragon/OwnershipCheck.h"
#include "dragon/Platform.h"
#include "dragon/Sema.h"
#include "dragon/TypeChecker.h"
#include "dragon/TypeHintEnforcer.h"

namespace dragon {

namespace {

void collect(std::vector<ReplDiagnostic>& out,
             const SourceLocation& location,
             const std::string& message) {
    out.push_back({ReplDiagnostic::Level::Error, location, message});
}

SourceLocation locationIn(const std::string& filename, const SourceLocation& from) {
    SourceLocation loc = from;
    if (loc.filename.empty()) loc.filename = filename;
    return loc;
}

template <typename Diagnostics>
void collectErrors(std::vector<ReplDiagnostic>& out,
                   const std::string& filename,
                   const Diagnostics& diagnostics) {
    for (const auto& diag : diagnostics) {
        if (diag.level != std::decay_t<decltype(diag)>::Level::Error) continue;
        collect(out, locationIn(filename, diag.location), diag.message);
    }
}

template <typename Diagnostics>
void collectAll(std::vector<ReplDiagnostic>& out,
                const std::string& filename,
                const Diagnostics& diagnostics) {
    for (const auto& diag : diagnostics)
        collect(out, locationIn(filename, diag.location), diag.message);
}

bool analyzeDependency(ResolvedModule& mod,
                       const std::string& turnFilename,
                       std::vector<ReplDiagnostic>& diagnostics) {
    if (!mod.isDragon) {
        EnforcerOptions enfOpts;
        enfOpts.isImportedModule = true;
        enfOpts.importingFile = turnFilename;
        TypeHintEnforcer enforcer(enfOpts);
        if (!enforcer.enforce(*mod.ast)) {
            collectErrors(diagnostics, mod.filepath, enforcer.diagnostics());
            return false;
        }
    }

    Sema modSema;
    if (!modSema.analyze(*mod.ast)) {
        collectErrors(diagnostics, mod.filepath, modSema.diagnostics());
        return false;
    }

    DefiniteAssignment modDa;
    if (!modDa.analyze(*mod.ast)) {
        collectAll(diagnostics, mod.filepath, modDa.diagnostics());
        return false;
    }
    return true;
}

}

ReplFrontendResult replAnalyze(Module& analysisModule,
                               const std::string& turnFilename,
                               bool isDragonSurface,
                               const std::vector<std::string>& searchPaths,
                               const std::vector<std::string>& alreadyResident) {
    ReplFrontendResult result;

    if (!isDragonSurface) {
        TypeHintEnforcer enforcer;
        if (!enforcer.enforce(analysisModule)) {
            collectErrors(result.diagnostics, turnFilename, enforcer.diagnostics());
            return result;
        }
    }

    Sema sema;
    if (!sema.analyze(analysisModule)) {
        collectErrors(result.diagnostics, turnFilename, sema.diagnostics());
        return result;
    }

    ModuleResolverOptions resolverOpts;
    resolverOpts.sourceDir = platform::getTempDir() + "/";
    resolverOpts.searchPaths = searchPaths;

    ModuleResolver resolver(resolverOpts);
    result.graph = std::make_unique<ImportGraph>(
        resolver.buildGraph(analysisModule, turnFilename));

    if (result.graph->hasCycle) {
        std::string participants;
        for (size_t i = 0; i < result.graph->cycleParticipants.size(); ++i) {
            if (i > 0) participants += ", ";
            participants += result.graph->cycleParticipants[i];
        }
        collect(result.diagnostics, {turnFilename, 0, 0, 0},
                "circular import detected involving: " + participants);
        return result;
    }

    if (resolver.hasErrors()) {
        for (const auto& err : resolver.errors())
            collect(result.diagnostics, {turnFilename, 0, 0, 0}, err);
        return result;
    }

    std::unordered_map<std::string,
        std::unordered_map<std::string, std::shared_ptr<Type>>> allExports;
    std::unordered_map<std::string,
        std::unordered_map<std::string, std::shared_ptr<Type>>> allTypeExports;

    {
        std::vector<Module*> ordered;
        ordered.reserve(result.graph->modules.size() + 1);
        for (auto& mod : result.graph->modules) {
            mod.ast->moduleName = mod.name;
            ordered.push_back(mod.ast.get());
        }
        ordered.push_back(&analysisModule);
        canonicalizeTypeAliases(ordered);
    }

    std::unordered_map<std::string, std::string> moduleFilepaths;
    for (auto& mod : result.graph->modules) moduleFilepaths[mod.name] = mod.filepath;

    for (auto& mod : result.graph->modules) {
        mod.ast->moduleName = mod.name;

        if (!analyzeDependency(mod, turnFilename, result.diagnostics)) return result;

        TypeChecker modTypeChecker;
        for (auto& [modName, exports] : allExports)
            modTypeChecker.registerExternalModule(modName, exports, moduleFilepaths[modName],
                                                  allTypeExports[modName]);
        for (auto* prior : result.depModules)
            modTypeChecker.registerExternalGenerics(*prior);

        modTypeChecker.check(*mod.ast);
        if (modTypeChecker.hasErrors()) {
            collectErrors(result.diagnostics, mod.filepath, modTypeChecker.diagnostics());
            return result;
        }

        OwnershipCheck modOwn;
        if (!modOwn.analyze(*mod.ast)) {
            collectAll(result.diagnostics, mod.filepath, modOwn.diagnostics());
            return result;
        }

        allExports[mod.name] = modTypeChecker.getExports();
        allTypeExports[mod.name] = modTypeChecker.getTypeExports();
        result.depModules.push_back(mod.ast.get());

        const bool resident = std::find(alreadyResident.begin(), alreadyResident.end(),
                                        mod.name) != alreadyResident.end();
        if (!resident) result.residentModuleNames.push_back(mod.name);
    }

    TypeChecker entryTc;
    for (auto& [modName, exports] : allExports)
        entryTc.registerExternalModule(modName, exports, moduleFilepaths[modName],
                                       allTypeExports[modName]);
    for (auto* dep : result.depModules)
        entryTc.registerExternalGenerics(*dep);

    DefiniteAssignment entryDa;
    if (!entryDa.analyze(analysisModule)) {
        collectAll(result.diagnostics, turnFilename, entryDa.diagnostics());
        return result;
    }

    entryTc.check(analysisModule);
    if (entryTc.hasErrors()) {
        collectErrors(result.diagnostics, turnFilename, entryTc.diagnostics());
        return result;
    }

    OwnershipCheck entryOwn;
    if (!entryOwn.analyze(analysisModule)) {
        collectAll(result.diagnostics, turnFilename, entryOwn.diagnostics());
        return result;
    }

    result.ok = true;
    return result;
}

}
