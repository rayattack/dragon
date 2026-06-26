#ifndef DRAGON_TYPE_INFERENCE_H
#define DRAGON_TYPE_INFERENCE_H

#include "dragon/AST.h"
#include "dragon/TypeChecker.h"
#include <memory>
#include <string>
#include <vector>

namespace dragon {

/// Type inference engine for Python-to-Dragon migration
/// 
/// Analyzes untyped Python code and infers types for:
/// - Function parameters and return types
/// - Variable declarations
/// - Class attributes
class TypeInference {
public:
    TypeInference();
    ~TypeInference();

    /// Infer types for a module (mutates AST to add type annotations)
    bool infer(Module& module);

    /// Get inferred type for an expression
    std::shared_ptr<Type> inferExprType(Expr* expr);

    /// Get inferred type for a variable by name
    std::shared_ptr<Type> inferVarType(const std::string& name);

    /// Check if inference was successful (no unresolvable types)
    bool hasUnresolvedTypes() const;

    /// Get list of names with unresolved types
    std::vector<std::string> unresolvedNames() const;

private:
    // Constraint-based type inference
    void collectConstraints(Module& module);
    void solveConstraints();
    void applyInferredTypes(Module& module);

    // Flow analysis for better inference
    void analyzeDataFlow(FunctionDecl& func);
    void analyzeControlFlow(FunctionDecl& func);

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace dragon

#endif // DRAGON_TYPE_INFERENCE_H
