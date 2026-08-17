#ifndef DRAGON_PYTHON_MIGRATOR_H
#define DRAGON_PYTHON_MIGRATOR_H

#include "dragon/AST.h"
#include "dragon/TypeInference.h"
#include <memory>
#include <string>
#include <vector>

namespace dragon {

struct MigrationDiagnostic {
    enum class Level { Info, Warning, Error };
    Level level;
    SourceLocation location;
    std::string message;
};

struct MigrationOptions {
    bool useBraces = true;
    bool addTypes = true;
    std::string outputFile;
    bool preserveComments = true;
};

class PythonMigrator {
public:
    explicit PythonMigrator(MigrationOptions options = {});
    ~PythonMigrator();

    bool migrate(const std::string& inputFile, const std::string& outputFile);
    std::string migrateSource(const std::string& source);
    bool migrateModule(Module& module);

    const std::vector<MigrationDiagnostic>& diagnostics() const;
    bool hasErrors() const;

    std::vector<std::string> incompatibilities() const;

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

}

#endif
