#ifndef DRAGON_PYTHON_MIGRATOR_H
#define DRAGON_PYTHON_MIGRATOR_H

#include "dragon/AST.h"
#include "dragon/TypeInference.h"
#include <memory>
#include <string>
#include <vector>

namespace dragon {

/// Migration diagnostic
struct MigrationDiagnostic {
    enum class Level { Info, Warning, Error };
    Level level;
    SourceLocation location;
    std::string message;
};

/// Options for Python to Dragon migration
struct MigrationOptions {
    /// Add curly braces (true) or keep indentation (false)
    bool useBraces = true;
    
    /// Add type annotations from inference
    bool addTypes = true;
    
    /// Output file (empty = stdout)
    std::string outputFile;
    
    /// Preserve comments
    bool preserveComments = true;
};

/// Converts Python code to Dragon
/// 
/// Performs:
/// - Type inference and annotation insertion
/// - Optional conversion from indentation to braces
/// - Validation of Python compatibility
class PythonMigrator {
public:
    explicit PythonMigrator(MigrationOptions options = {});
    ~PythonMigrator();

    /// Migrate a Python file to Dragon
    bool migrate(const std::string& inputFile, const std::string& outputFile);

    /// Migrate Python source string to Dragon
    std::string migrateSource(const std::string& source);

    /// Migrate an already-parsed module
    bool migrateModule(Module& module);

    /// Get migration diagnostics
    const std::vector<MigrationDiagnostic>& diagnostics() const;

    /// Check if migration succeeded
    bool hasErrors() const;

    /// Get list of things that couldn't be migrated
    std::vector<std::string> incompatibilities() const;

    // Code emission (public for helper access)
    std::string emitExpr(Expr* expr);
    std::string emitStmt(Stmt* stmt, int indent);
    std::string emitType(TypeExpr* type);

private:
    // Transformation passes
    void addTypeAnnotations(Module& module);
    void convertBlocksToBraces(Module& module);
    void validateDragonCompatibility(Module& module);

    std::string emitDragon(Module& module);

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace dragon

#endif // DRAGON_PYTHON_MIGRATOR_H
