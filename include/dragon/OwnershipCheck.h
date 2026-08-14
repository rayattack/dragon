#ifndef DRAGON_OWNERSHIP_CHECK_H
#define DRAGON_OWNERSHIP_CHECK_H

#include "dragon/AST.h"
#include <memory>
#include <string>
#include <vector>

namespace dragon {

/// A diagnostic produced by the ownership pass
struct OwnDiagnostic {
    SourceLocation location;
    std::string message;
};

/// del/own/dub ownership analysis (docs/001-memory.md, ADR docs/002): forward
/// dataflow over heap-typed locals; `del` compiles only when provably sole owner.
class OwnershipCheck {
public:
    OwnershipCheck();
    ~OwnershipCheck();

    /// Analyze a module. Returns true when no ownership error was found.
    bool analyze(Module& module);

    /// All diagnostics gathered during the last analyze().
    const std::vector<OwnDiagnostic>& diagnostics() const;

    /// Whether the last analyze() reported any error.
    bool hasErrors() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace dragon

#endif // DRAGON_OWNERSHIP_CHECK_H
