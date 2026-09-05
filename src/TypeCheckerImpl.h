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

bool derivesFromBuiltinException(const ClassType* c);
bool bodyContainsYield(const std::vector<std::unique_ptr<Stmt>>& body);
bool hasDeclaredBase(const ClassType& ct, const char* baseName);
bool hasDataclassDecorator(const ClassType& ct);
bool orderableTogether(const std::shared_ptr<Type>& a,
                       const std::shared_ptr<Type>& b);
bool supportsOrdering(const std::shared_ptr<Type>& t);
std::string orderingRejectionHint(const std::shared_ptr<Type>& t);
bool supportsHashing(const std::shared_ptr<Type>& t);

struct TypeChecker::Impl {
    std::vector<TypeDiagnostic> diagnostics;

    std::shared_ptr<PrimitiveType> intType;
    std::shared_ptr<PrimitiveType> floatType;
    std::shared_ptr<PrimitiveType> boolType;
    std::shared_ptr<PrimitiveType> strType;
    std::shared_ptr<PrimitiveType> bytesType;
    std::shared_ptr<PrimitiveType> noneType;
    std::shared_ptr<BoxedType> boxedType;
    std::shared_ptr<NeverType> neverType;
    std::shared_ptr<UnknownType> unknownType;

    std::unordered_map<std::string, std::shared_ptr<Type>> typeNames;
    std::unordered_map<const ContractDecl*, std::shared_ptr<ContractType>> contractByDecl;

    struct Scope {
        std::unordered_map<std::string, std::shared_ptr<Type>> bindings;
    };
    std::vector<Scope> scopes;

    std::vector<std::shared_ptr<Type>> returnTypeStack;
    std::vector<char> returnAnnotatedStack;

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

    // Set only while the checker resolves compiler-SYNTHESIZED type
    // expressions (schema decoders and the like), which may still name the box
    // internally. User source never gets this exemption.
    bool allowDynamicTierSpelling = false;

    std::unordered_map<std::string, std::shared_ptr<Type>> cachedExports;
    // module-top-level `type` aliases declared in the module being checked
    std::unordered_map<std::string, std::shared_ptr<Type>> cachedTypeExports;
    bool inExternSignature = false;

    struct ExternSignatureScope {
        Impl& impl;
        bool previous;
        ExternSignatureScope(Impl& i, bool active)
            : impl(i), previous(i.inExternSignature) {
            impl.inExternSignature = active;
        }
        ~ExternSignatureScope() { impl.inExternSignature = previous; }
    };

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

    // The declared type a name had before a flow-narrow replaced it, so the
    // narrow can be undone when the binding is written.
    std::unordered_map<std::string, std::shared_ptr<Type>> narrowedFrom;

    // End a flow-narrow: drop the narrowed binding from the innermost scope
    // that holds it so lookup falls back to the DECLARED type. Returns the
    // declared type, or null when the name was not narrowed.
    std::shared_ptr<Type> dropNarrowedBinding(const std::string& name) {
        for (int i = static_cast<int>(scopes.size()) - 1; i > 0; --i) {
            auto it = scopes[i].bindings.find(name);
            if (it == scopes[i].bindings.end()) continue;
            auto narrowed = it->second;
            for (int j = i - 1; j >= 0; --j) {
                auto outer = scopes[j].bindings.find(name);
                if (outer == scopes[j].bindings.end()) continue;
                if (outer->second && narrowed && !outer->second->equals(*narrowed)) {
                    scopes[i].bindings.erase(it);
                    return outer->second;
                }
                return nullptr;
            }
            return nullptr;
        }
        // A narrow applied in the SAME scope as the declaration overwrote it;
        // restore from the recorded pre-narrow type.
        auto nf = narrowedFrom.find(name);
        if (nf != narrowedFrom.end() && nf->second) {
            auto declared = nf->second;
            for (int i = static_cast<int>(scopes.size()) - 1; i >= 0; --i) {
                auto it = scopes[i].bindings.find(name);
                if (it == scopes[i].bindings.end()) continue;
                it->second = declared;
                break;
            }
            narrowedFrom.erase(nf);
            return declared;
        }
        return nullptr;
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

    std::vector<std::string> templateContentStack;

    std::unordered_set<std::string> plainFunctionSymbols;

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
        std::unordered_map<std::string, std::string>> templateKeysByModule;
    std::string templateScopeModule;
    std::unordered_map<const ClassDecl*, std::shared_ptr<ClassType>>
        genericClassTypeByDecl;

    static std::string qualifyTemplate(const std::string& homeModule,
                                       const std::string& name) {
        return homeModule.empty() ? name : homeModule + "." + name;
    }

    std::string templateHomeModule(const void* decl) const {
        auto it = genericTemplateModule.find(decl);
        return it == genericTemplateModule.end() ? std::string() : it->second;
    }

    std::string templateKeyOf(const void* decl, const std::string& name) const {
        return qualifyTemplate(templateHomeModule(decl), name);
    }

    void bindTemplateName(const std::string& scopeModule,
                          const std::string& localName,
                          const std::string& key) {
        templateKeysByModule[scopeModule][localName] = key;
    }

    std::string templateKeyInScope(const std::string& localName) const {
        auto scope = templateKeysByModule.find(templateScopeModule);
        if (scope == templateKeysByModule.end()) return localName;
        auto it = scope->second.find(localName);
        return it == scope->second.end() ? localName : it->second;
    }

    struct TemplateScope {
        std::string* slot;
        std::string saved;
        TemplateScope(std::string& s, const std::string& scopeModule)
            : slot(&s), saved(s) { s = scopeModule; }
        ~TemplateScope() { *slot = saved; }
    };

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
        Stmt* template_ = nullptr;
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
