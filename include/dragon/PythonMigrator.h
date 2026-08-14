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

/// Options for Python to Dragon migration.
struct MigrationOptions {
    bool useBraces = true;   // braces vs kept indentation
    bool addTypes = true;    // annotate from inference
    std::string outputFile;  // "" = stdout
    bool preserveComments = true;
};

/// Converts Python to Dragon: type inference/annotation, indentation-to-braces,
/// and Dragon-compatibility validation.
class PythonMigrator {
public:
    explicit PythonMigrator(MigrationOptions options = {});
    ~PythonMigrator();

    bool migrate(const std::string& inputFile, const std::string& outputFile);
    std::string migrateSource(const std::string& source);
    bool migrateModule(Module& module);

    const std::vector<MigrationDiagnostic>& diagnostics() const;
    bool hasErrors() const;

    /// Things that couldn't be migrated.
    std::vector<std::string> incompatibilities() const;

    // Public for helper access.
    std::string emitExpr(Expr* expr);
    std::string emitStmt(Stmt* stmt, int indent);
    std::string emitType(TypeExpr* type);

private:
    void addTypeAnnotations(Module& module);
    void convertBlocksToBraces(Module& module);
    void validateDragonCompatibility(Module& module);

    std::string emitDragon(Module& module);

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace dragon

#endif // DRAGON_PYTHON_MIGRATOR_H
