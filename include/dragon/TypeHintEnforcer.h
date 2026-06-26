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
    /// When true, uses "Borders must be secured" message for imported modules
    bool isImportedModule = false;
    std::string importingFile;
};

/// Enforces PEP-484 type annotations on .py files compiled by Dragon.
///
/// This is a separate pass from TypeChecker. TypeChecker validates type
/// correctness (are the types compatible?). TypeHintEnforcer validates
/// type annotation presence (did the programmer write type hints?).
///
/// Only used for .py files - .dr files have types enforced by the Parser.
class TypeHintEnforcer {
public:
    explicit TypeHintEnforcer(EnforcerOptions options = {});

    /// Walk the module AST and check that all declarations have type annotations.
    /// Returns true if no errors were found.
    bool enforce(Module& module);

    /// Get diagnostics produced during enforcement
    const std::vector<EnforcerDiagnostic>& diagnostics() const { return diagnostics_; }

    /// Whether any errors were found
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
