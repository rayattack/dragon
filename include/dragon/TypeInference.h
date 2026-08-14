#ifndef DRAGON_TYPE_INFERENCE_H
#define DRAGON_TYPE_INFERENCE_H

#include "dragon/AST.h"
#include "dragon/TypeChecker.h"
#include <memory>
#include <string>
#include <vector>

namespace dragon {

/// Python-to-Dragon migration: infers types for params/returns, variables, and class attributes.
class TypeInference {
public:
    TypeInference();
    ~TypeInference();

    /// Infers types for a module, mutating the AST to add type annotations.
    bool infer(Module& module);

    std::shared_ptr<Type> inferExprType(Expr* expr);
    std::shared_ptr<Type> inferVarType(const std::string& name);

    /// True when inference left no unresolvable types.
    bool hasUnresolvedTypes() const;
    std::vector<std::string> unresolvedNames() const;

private:
    // Constraint-based type inference
    void collectConstraints(Module& module);

    void applyInferredTypes(Module& module);

    // Flow analysis for better inference

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace dragon

#endif // DRAGON_TYPE_INFERENCE_H
