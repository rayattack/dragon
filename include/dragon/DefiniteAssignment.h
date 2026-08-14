#ifndef DRAGON_DEFINITE_ASSIGNMENT_H
#define DRAGON_DEFINITE_ASSIGNMENT_H

#include "dragon/AST.h"
#include <memory>
#include <string>
#include <vector>

namespace dragon {

/// A diagnostic produced by definite-assignment analysis.
struct DADiagnostic {
    SourceLocation location;
    std::string message;
};

/// Use-before-assignment analysis (100-variables.md): forward "must" dataflow;
/// a no-initializer local may read only once assigned on every path. Locals only.
class DefiniteAssignment {
public:
    DefiniteAssignment();
    ~DefiniteAssignment();

    /// Analyze a module. Returns true when no use-before-assignment was found.
    bool analyze(Module& module);

    /// All diagnostics gathered during the last analyze().
    const std::vector<DADiagnostic>& diagnostics() const;

    /// Whether the last analyze() reported any error.
    bool hasErrors() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace dragon

#endif // DRAGON_DEFINITE_ASSIGNMENT_H

