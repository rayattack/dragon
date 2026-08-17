#ifndef DRAGON_TYPE_HINT_ENFORCER_H
#define DRAGON_TYPE_HINT_ENFORCER_H

#include "dragon/AST.h"
#include "dragon/DiagnosticFormatter.h"
#include <string>
#include <vector>

namespace dragon {

struct EnforcerDiagnostic {
    enum class Level { Error, Warning };
    Level level = Level::Error;
    SourceLocation location;
    std::string message;
};

struct EnforcerOptions {
    bool requireFunctionParamTypes = true;
    bool requireReturnTypes = true;
    bool requireModuleVarTypes = true;
    bool isImportedModule = false;
    std::string importingFile;
};

class TypeHintEnforcer {
public:
    explicit TypeHintEnforcer(EnforcerOptions options = {});

    bool enforce(Module& module);

    const std::vector<EnforcerDiagnostic>& diagnostics() const { return diagnostics_; }
    bool hasErrors() const;

private:
    void checkFunction(FunctionDecl& func, bool isMethod = false);
    void checkClass(ClassDecl& cls);
    void checkModuleLevelAssign(AssignStmt& assign);

    void addError(SourceLocation loc, const std::string& message);

    EnforcerOptions options_;
    std::vector<EnforcerDiagnostic> diagnostics_;
};

}

#endif
