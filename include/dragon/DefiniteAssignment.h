#ifndef DRAGON_DEFINITE_ASSIGNMENT_H
#define DRAGON_DEFINITE_ASSIGNMENT_H

#include "dragon/AST.h"
#include <memory>
#include <string>
#include <vector>

namespace dragon {

struct DADiagnostic {
    SourceLocation location;
    std::string message;
};

class DefiniteAssignment {
public:
    DefiniteAssignment();
    ~DefiniteAssignment();

    bool analyze(Module& module);

    const std::vector<DADiagnostic>& diagnostics() const;

    bool hasErrors() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}

#endif

