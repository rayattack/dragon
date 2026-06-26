#include "dragon/TypeInference.h"
#include <unordered_map>
#include <algorithm>

namespace dragon {

struct TypeInference::Impl {
    std::vector<std::string> unresolvedNames;

    // Inferred types for variables: name -> Type
    std::unordered_map<std::string, std::shared_ptr<Type>> varTypes;

    // Inferred function return types: funcName -> Type
    std::unordered_map<std::string, std::shared_ptr<Type>> funcReturnTypes;

    // Inferred function parameter types: funcName -> {paramName -> Type}
    std::unordered_map<std::string, std::unordered_map<std::string, std::shared_ptr<Type>>> funcParamTypes;

    // Builtin types
    std::shared_ptr<PrimitiveType> intType = std::make_shared<PrimitiveType>(Type::Kind::Int);
    std::shared_ptr<PrimitiveType> floatType = std::make_shared<PrimitiveType>(Type::Kind::Float);
    std::shared_ptr<PrimitiveType> boolType = std::make_shared<PrimitiveType>(Type::Kind::Bool);
    std::shared_ptr<PrimitiveType> strType = std::make_shared<PrimitiveType>(Type::Kind::Str);
    std::shared_ptr<PrimitiveType> noneType = std::make_shared<PrimitiveType>(Type::Kind::None_);
    std::shared_ptr<AnyType> anyType = std::make_shared<AnyType>();

    // Infer type from an expression (quick inference without full constraint solving)
    std::shared_ptr<Type> inferFromExpr(Expr* expr) {
        if (!expr) return anyType;
        if (dynamic_cast<IntegerLiteral*>(expr)) return intType;
        if (dynamic_cast<FloatLiteral*>(expr)) return floatType;
        if (dynamic_cast<StringLiteral*>(expr)) return strType;
        if (dynamic_cast<BooleanLiteral*>(expr)) return boolType;
        if (dynamic_cast<NoneLiteral*>(expr)) return noneType;

        if (auto* name = dynamic_cast<NameExpr*>(expr)) {
            if (name->name == "True" || name->name == "False") return boolType;
            if (name->name == "None") return noneType;
            auto it = varTypes.find(name->name);
            if (it != varTypes.end()) return it->second;
            return anyType;
        }

        if (auto* bin = dynamic_cast<BinaryExpr*>(expr)) {
            auto lt = inferFromExpr(bin->left.get());
            auto rt = inferFromExpr(bin->right.get());
            auto op = bin->op.type();

            // Comparison operators always return bool
            if (op == TokenType::EQUAL_EQUAL || op == TokenType::NOT_EQUAL ||
                op == TokenType::LESS || op == TokenType::LESS_EQUAL ||
                op == TokenType::GREATER || op == TokenType::GREATER_EQUAL ||
                op == TokenType::AND || op == TokenType::OR ||
                op == TokenType::IN || op == TokenType::IS) {
                return boolType;
            }

            // String concatenation
            if (op == TokenType::PLUS &&
                lt->kind() == Type::Kind::Str && rt->kind() == Type::Kind::Str) {
                return strType;
            }

            // Arithmetic
            if (op == TokenType::SLASH) return floatType;
            if (lt->kind() == Type::Kind::Float || rt->kind() == Type::Kind::Float) return floatType;
            if (lt->kind() == Type::Kind::Int && rt->kind() == Type::Kind::Int) return intType;

            return anyType;
        }

        if (auto* unary = dynamic_cast<UnaryExpr*>(expr)) {
            if (unary->op.type() == TokenType::NOT) return boolType;
            return inferFromExpr(unary->operand.get());
        }

        if (auto* call = dynamic_cast<CallExpr*>(expr)) {
            if (auto* callee = dynamic_cast<NameExpr*>(call->callee.get())) {
                // Built-in function return types
                if (callee->name == "len" || callee->name == "abs" ||
                    callee->name == "int" || callee->name == "ord") return intType;
                if (callee->name == "float") return floatType;
                if (callee->name == "str" || callee->name == "chr" ||
                    callee->name == "input" || callee->name == "repr") return strType;
                if (callee->name == "bool" || callee->name == "isinstance" ||
                    callee->name == "hasattr") return boolType;
                if (callee->name == "print") return noneType;
                if (callee->name == "range") return std::make_shared<ListType>(intType);
                if (callee->name == "sorted" || callee->name == "list" ||
                    callee->name == "reversed") return std::make_shared<ListType>(anyType);

                // User function
                auto it = funcReturnTypes.find(callee->name);
                if (it != funcReturnTypes.end()) return it->second;
            }
            return anyType;
        }

        if (auto* list = dynamic_cast<ListExpr*>(expr)) {
            if (list->elements.empty()) return std::make_shared<ListType>(anyType);
            auto elemType = inferFromExpr(list->elements[0].get());
            return std::make_shared<ListType>(elemType);
        }

        if (auto* dict = dynamic_cast<DictExpr*>(expr)) {
            if (dict->entries.empty())
                return std::make_shared<DictType>(anyType, anyType);
            auto kt = inferFromExpr(dict->entries[0].first.get());
            auto vt = inferFromExpr(dict->entries[0].second.get());
            return std::make_shared<DictType>(kt, vt);
        }

        if (auto* ifExpr = dynamic_cast<IfExpr*>(expr)) {
            return inferFromExpr(ifExpr->thenExpr.get());
        }

        if (auto* sub = dynamic_cast<SubscriptExpr*>(expr)) {
            auto objType = inferFromExpr(sub->object.get());
            if (objType->kind() == Type::Kind::List) {
                return static_cast<ListType*>(objType.get())->elementType;
            }
            if (objType->kind() == Type::Kind::Dict) {
                return static_cast<DictType*>(objType.get())->valueType;
            }
            if (objType->kind() == Type::Kind::Str) return strType;
            return anyType;
        }

        return anyType;
    }

    // Create a TypeExpr AST node from a Type
    std::unique_ptr<TypeExpr> typeToTypeExpr(const std::shared_ptr<Type>& type) {
        if (!type || type->kind() == Type::Kind::Any || type->kind() == Type::Kind::Unknown) {
            return nullptr; // No annotation for Any/Unknown
        }
        switch (type->kind()) {
            case Type::Kind::Int: { auto n = std::make_unique<NamedTypeExpr>(); n->name = "int"; return n; }
            case Type::Kind::Float: { auto n = std::make_unique<NamedTypeExpr>(); n->name = "float"; return n; }
            case Type::Kind::Bool: { auto n = std::make_unique<NamedTypeExpr>(); n->name = "bool"; return n; }
            case Type::Kind::Str: { auto n = std::make_unique<NamedTypeExpr>(); n->name = "str"; return n; }
            case Type::Kind::Bytes: { auto n = std::make_unique<NamedTypeExpr>(); n->name = "bytes"; return n; }
            case Type::Kind::None_: { auto n = std::make_unique<NamedTypeExpr>(); n->name = "None"; return n; }
            case Type::Kind::List: {
                auto* lt = static_cast<ListType*>(type.get());
                auto gen = std::make_unique<GenericTypeExpr>();
                auto base = std::make_unique<NamedTypeExpr>(); base->name = "list";
                gen->base = std::move(base);
                auto elem = typeToTypeExpr(lt->elementType);
                if (elem) gen->typeArgs.push_back(std::move(elem));
                return gen;
            }
            case Type::Kind::Dict: {
                auto* dt = static_cast<DictType*>(type.get());
                auto gen = std::make_unique<GenericTypeExpr>();
                auto base = std::make_unique<NamedTypeExpr>(); base->name = "dict";
                gen->base = std::move(base);
                auto k = typeToTypeExpr(dt->keyType);
                auto v = typeToTypeExpr(dt->valueType);
                if (k) gen->typeArgs.push_back(std::move(k));
                if (v) gen->typeArgs.push_back(std::move(v));
                return gen;
            }
            default: return nullptr;
        }
    }
};

TypeInference::TypeInference() : impl_(std::make_unique<Impl>()) {}
TypeInference::~TypeInference() = default;

bool TypeInference::infer(Module& module) {
    collectConstraints(module);
    solveConstraints();
    applyInferredTypes(module);
    return !hasUnresolvedTypes();
}

std::shared_ptr<Type> TypeInference::inferExprType(Expr* expr) {
    return impl_->inferFromExpr(expr);
}

std::shared_ptr<Type> TypeInference::inferVarType(const std::string& name) {
    auto it = impl_->varTypes.find(name);
    return (it != impl_->varTypes.end()) ? it->second : impl_->anyType;
}

bool TypeInference::hasUnresolvedTypes() const {
    return !impl_->unresolvedNames.empty();
}

std::vector<std::string> TypeInference::unresolvedNames() const {
    return impl_->unresolvedNames;
}

void TypeInference::collectConstraints(Module& module) {
    // First pass: collect function signatures by analyzing return statements
    for (auto& stmt : module.body) {
        if (auto* func = dynamic_cast<FunctionDecl*>(stmt.get())) {
            // If function already has return type annotation, use it
            if (func->returnType) continue;

            // Otherwise, infer from return statements
            std::shared_ptr<Type> retType = impl_->noneType;
            for (auto& bodyStmt : func->body) {
                if (auto* ret = dynamic_cast<ReturnStmt*>(bodyStmt.get())) {
                    if (ret->value) {
                        retType = impl_->inferFromExpr(ret->value.get());
                        break;
                    }
                }
            }
            impl_->funcReturnTypes[func->name] = retType;
        }
    }

    // Second pass: collect variable types from assignments
    for (auto& stmt : module.body) {
        if (auto* assign = dynamic_cast<AssignStmt*>(stmt.get())) {
            if (assign->value && !assign->targets.empty()) {
                auto type = impl_->inferFromExpr(assign->value.get());
                for (auto& target : assign->targets) {
                    if (auto* name = dynamic_cast<NameExpr*>(target.get())) {
                        impl_->varTypes[name->name] = type;
                    }
                }
            }
        }
        if (auto* ann = dynamic_cast<AnnAssignStmt*>(stmt.get())) {
            // Already has annotation, skip
            continue;
        }
        // Infer within function bodies too
        if (auto* func = dynamic_cast<FunctionDecl*>(stmt.get())) {
            // Infer parameter types from usage patterns
            for (auto& p : func->params) {
                if (!p.type && p.defaultValue) {
                    auto type = impl_->inferFromExpr(p.defaultValue.get());
                    impl_->funcParamTypes[func->name][p.name] = type;
                }
            }
            // Scan body for variable assignments
            for (auto& bodyStmt : func->body) {
                if (auto* assign = dynamic_cast<AssignStmt*>(bodyStmt.get())) {
                    if (assign->value && !assign->targets.empty()) {
                        auto type = impl_->inferFromExpr(assign->value.get());
                        for (auto& target : assign->targets) {
                            if (auto* name = dynamic_cast<NameExpr*>(target.get())) {
                                impl_->varTypes[name->name] = type;
                            }
                        }
                    }
                }
            }
        }
    }
}

void TypeInference::solveConstraints() {
    // Current approach: forward-propagation is done in collectConstraints.
    // Mark unresolved names: variables with Any type
    impl_->unresolvedNames.clear();
}

void TypeInference::applyInferredTypes(Module& module) {
    // Apply inferred types to AST nodes that lack annotations
    for (auto& stmt : module.body) {
        if (auto* func = dynamic_cast<FunctionDecl*>(stmt.get())) {
            // Add return type annotation if missing
            if (!func->returnType) {
                auto it = impl_->funcReturnTypes.find(func->name);
                if (it != impl_->funcReturnTypes.end()) {
                    func->returnType = impl_->typeToTypeExpr(it->second);
                }
                if (!func->returnType) {
                    auto n = std::make_unique<NamedTypeExpr>();
                    n->name = "None";
                    func->returnType = std::move(n);
                }
            }

            // Add parameter type annotations if missing
            for (auto& p : func->params) {
                if (!p.type) {
                    auto it2 = impl_->funcParamTypes.find(func->name);
                    if (it2 != impl_->funcParamTypes.end()) {
                        auto pit = it2->second.find(p.name);
                        if (pit != it2->second.end()) {
                            p.type = impl_->typeToTypeExpr(pit->second);
                        }
                    }
                    // If still no type, use Any
                    if (!p.type) {
                        auto n = std::make_unique<NamedTypeExpr>();
                        n->name = "Any";
                        p.type = std::move(n);
                    }
                }
            }
        }
    }
}

void TypeInference::analyzeDataFlow(FunctionDecl&) {
    // Advanced analysis - not needed for basic migration
}

void TypeInference::analyzeControlFlow(FunctionDecl&) {
    // Advanced analysis - not needed for basic migration
}

} // namespace dragon
