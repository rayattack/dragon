#ifndef DRAGON_REPL_FRONTEND_H
#define DRAGON_REPL_FRONTEND_H

#include <memory>
#include <string>
#include <vector>

#include "dragon/AST.h"
#include "dragon/ModuleResolver.h"
#include "dragon/Repl.h"

namespace dragon {

struct ReplFrontendResult {
    bool ok = false;
    std::vector<ReplDiagnostic> diagnostics;
    std::vector<Module*> depModules;
    std::vector<std::string> residentModuleNames;
    std::unique_ptr<ImportGraph> graph;
};

ReplFrontendResult replAnalyze(Module& analysisModule,
                               const std::string& turnFilename,
                               bool isDragonSurface,
                               const std::vector<std::string>& searchPaths,
                               const std::vector<std::string>& alreadyResident);

}

#endif
