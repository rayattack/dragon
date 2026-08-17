#ifndef DRAGON_OWNERSHIP_CHECK_H
#define DRAGON_OWNERSHIP_CHECK_H

#include "dragon/AST.h"
#include <memory>
#include <string>
#include <vector>

namespace dragon {

struct OwnDiagnostic {
    SourceLocation location;
    std::string message;
};

class OwnershipCheck {
public:
    OwnershipCheck();
    ~OwnershipCheck();

    bool analyze(Module& module);

    const std::vector<OwnDiagnostic>& diagnostics() const;

    bool hasErrors() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}

#endif
