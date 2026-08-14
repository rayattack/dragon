#ifndef DRAGON_TYPE_HINT_ENFORCER_H
#define DRAGON_TYPE_HINT_ENFORCER_H

#include "dragon/AST.h"
#include "dragon/DiagnosticFormatter.h"
#include <string>
#include <vector>

namespace dragon {

/// Diagnostic produced by the TypeHintEnforcer
struct EnforcerDiagnostic {
    enum class Level { Error, Warning };
    Level level = Level::Error;
    SourceLocation location;
    std::string message;
};

/// Options for controlling type hint enforcement
struct EnforcerOptions {
    bool requireFunctionParamTypes = true;
    bool requireReturnTypes = true;
    bool requireModuleVarTypes = true;
    bool isImportedModule = false;  // true: use "Borders must be secured" message
    std::string importingFile;
};

/// Enforces PEP-484 annotation presence on .py files (TypeChecker checks
/// correctness separately). .dr files are exempt; the Parser enforces types there.
class TypeHintEnforcer {
public:
    explicit TypeHintEnforcer(EnforcerOptions options = {});

    /// Checks that every declaration has a type annotation; false on any miss.
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

} // namespace dragon

#endif // DRAGON_TYPE_HINT_ENFORCER_H
