#ifndef DRAGON_TYPE_CHECKER_IMPL_H
#define DRAGON_TYPE_CHECKER_IMPL_H

#include "dragon/AST.h"
#include "dragon/TypeChecker.h"

#include <memory>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace dragon {

void fillFuncMeta(FunctionType& ft, const std::vector<Parameter>& params,
                  bool isMethod, bool hasImplicitSelf,
                  bool isClassMethod = false);

struct TypeChecker::Impl {
    std::vector<TypeDiagnostic> diagnostics;

    std::shared_ptr<PrimitiveType> intType;
    std::shared_ptr<PrimitiveType> floatType;
    std::shared_ptr<PrimitiveType> boolType;
    std::shared_ptr<PrimitiveType> strType;
    std::shared_ptr<PrimitiveType> bytesType;
    std::shared_ptr<PrimitiveType> noneType;
    std::shared_ptr<AnyType> anyType;
    std::shared_ptr<NeverType> neverType;
    std::shared_ptr<UnknownType> unknownType;

    std::unordered_map<std::string, std::shared_ptr<Type>> typeNames;
    std::unordered_map<const ContractDecl*, std::shared_ptr<ContractType>> contractByDecl;

    struct Scope {
        std::unordered_map<std::string, std::shared_ptr<Type>> bindings;
    };
    std::vector<Scope> scopes;

    std::vector<std::shared_ptr<Type>> returnTypeStack;

    std::unordered_set<const LambdaExpr*> checkedLambdaBodies;

    std::unordered_map<std::string, std::shared_ptr<ModuleType>> moduleTypes;

    std::shared_ptr<ModuleType> getOrCreateModuleType(const std::string& canonicalName) {
        auto it = moduleTypes.find(canonicalName);
        if (it != moduleTypes.end()) return it->second;
        auto mt = std::make_shared<ModuleType>(canonicalName);
        moduleTypes[canonicalName] = mt;
        auto dot = canonicalName.rfind('.');
        if (dot != std::string::npos) {
            auto parent = getOrCreateModuleType(canonicalName.substr(0, dot));
            parent->submodules[canonicalName.substr(dot + 1)] = mt;
        }
        return mt;
    }

    std::unordered_map<std::string, std::shared_ptr<Type>> cachedExports;

    std::string currentFile;
    std::string currentModuleName;
    std::string currentPackage;
    const ClassType* currentClass = nullptr;
    std::unordered_map<std::string, std::string> packageKeyCache;

    const std::string& packageKey(const std::string& file);

    std::unordered_map<std::string, Type*> builtinIdentity;

    void pushScope() { scopes.push_back({}); }
    void popScope() { if (!scopes.empty()) scopes.pop_back(); }

    void define(const std::string& name, std::shared_ptr<Type> type) {
        if (!scopes.empty()) {
            scopes.back().bindings[name] = std::move(type);
        }
    }

    std::shared_ptr<Type> lookup(const std::string& name) {
        for (int i = static_cast<int>(scopes.size()) - 1; i >= 0; --i) {
            auto it = scopes[i].bindings.find(name);
            if (it != scopes[i].bindings.end()) return it->second;
        }
        return nullptr;
    }

    std::vector<std::unordered_map<std::string, std::shared_ptr<Type>>> typeParamScopes;

    int genericTemplateDepth = 0;

    std::unordered_map<std::string, ClassDecl*> genericClasses;
    std::unordered_map<std::string, FunctionDecl*> genericFunctions;

    std::unordered_set<const FunctionDecl*> schemaDecodeFns;

    std::unordered_set<const FunctionDecl*> schemaEncodeFns;

    std::unordered_map<std::string, FunctionDecl*> genericMethods;
    std::unordered_map<const ClassDecl*,
        std::unordered_map<std::string, FunctionDecl*>> genericMethodsByDecl;
    std::unordered_map<std::string, ClassDecl*> classDeclByName;

    std::unordered_map<const void*, std::string> genericTemplateModule;

    std::unordered_map<std::string,
        std::unordered_map<std::string, std::shared_ptr<Type>>> moduleImportedTypes;

    std::set<const Stmt*> genericChecked;

    struct InstReq {
        std::string key;
        std::string genericName;
        bool isClass;
        std::vector<std::shared_ptr<Type>> args;
        std::string owningClass;
        std::shared_ptr<ClassType> ownerCT;
    };
    std::vector<InstReq> pendingInsts;
    std::set<std::string> instDone;
    std::unordered_map<std::string, std::shared_ptr<Type>> stampedCallReturnType;

    Module* currentModule = nullptr;

    std::shared_ptr<Type> currentExpectedType;

    const Expr* methodRefOkExpr = nullptr;

    std::unordered_set<const Expr*> rangeValueOkExprs;

    std::unordered_map<const Expr*, long long> constIntFolds;

    int instantiationCount = 0;
    int instDepth = 0;
    bool instCapReported = false;
    bool genericsAborted = false;
    SourceLocation lastInstLoc;
    static constexpr int kMaxInstantiations = 4096;
    static constexpr int kMaxInstDepth = 200;
};

}

#endif
