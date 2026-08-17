#ifndef DRAGON_TYPE_INFERENCE_H
#define DRAGON_TYPE_INFERENCE_H

#include "dragon/AST.h"
#include "dragon/TypeChecker.h"
#include <memory>
#include <string>
#include <vector>

namespace dragon {

class TypeInference {
public:
    TypeInference();
    ~TypeInference();

    bool infer(Module& module);

    std::shared_ptr<Type> inferExprType(Expr* expr);
    std::shared_ptr<Type> inferVarType(const std::string& name);

    bool hasUnresolvedTypes() const;
    std::vector<std::string> unresolvedNames() const;

private:
    void collectConstraints(Module& module);

    void applyInferredTypes(Module& module);

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}

#endif
