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

/// Definite-assignment (use-before-assignment) analysis.
///
/// Dragon variables are introduced by `x: T` (declaration) and may be left
/// without an initializer. Reading such a variable on a code path where it has
/// not yet been assigned is a compile error - this pass enforces that promise
/// (documented in `100-variables.md`). It is a structured forward "must"
/// dataflow over the AST: a no-initializer local is *tracked* and may only be
/// read once it is *definitely assigned* on every path that reaches the read.
///
/// Only function/method/module-body **locals** are tracked. Parameters,
/// `global`/`nonlocal` names, imported names, loop/with/except targets, walrus
/// bindings and any name that resolves outside the current callable are treated
/// as always-assigned (cross-procedure flow is not modeled - that residue is
/// covered by safe zero-init of heap-typed fields in CodeGen).
///
/// Runs after Sema (names already resolved); it builds its own scope tracking
/// and does not depend on Sema's symbol table.
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
