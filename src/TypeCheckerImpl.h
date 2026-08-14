#ifndef DRAGON_TYPE_CHECKER_IMPL_H
#define DRAGON_TYPE_CHECKER_IMPL_H

// TypeChecker::Impl, shared by TypeChecker.cpp (core checker + visitors) and
// TypeCheckerGenerics.cpp (D044 monomorphization). Internal, not a public header.

#include "dragon/AST.h"
#include "dragon/TypeChecker.h"

#include <memory>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace dragon {

// M1/M2 call-validation metadata filler (defined in TypeCheckerStmts.cpp); shared
// with the function-signature pre-pass so forward-referenced fns carry arg metadata.
void fillFuncMeta(FunctionType& ft, const std::vector<Parameter>& params,
                  bool isMethod, bool hasImplicitSelf,
                  bool isClassMethod = false);

struct TypeChecker::Impl {
    std::vector<TypeDiagnostic> diagnostics;

    // Built-in type singletons
    std::shared_ptr<PrimitiveType> intType;
    std::shared_ptr<PrimitiveType> floatType;
    std::shared_ptr<PrimitiveType> boolType;
    std::shared_ptr<PrimitiveType> strType;
    std::shared_ptr<PrimitiveType> bytesType;
    std::shared_ptr<PrimitiveType> noneType;
    std::shared_ptr<AnyType> anyType;
    std::shared_ptr<NeverType> neverType;
    std::shared_ptr<UnknownType> unknownType;

    // Type name -> type mapping for resolution
    std::unordered_map<std::string, std::shared_ptr<Type>> typeNames;
    // ADR 054: one shared ContractType per ContractDecl (D053 true identity),
    // so every view of a contract sees the pre-pass's filled signatures.
    std::unordered_map<const ContractDecl*, std::shared_ptr<ContractType>> contractByDecl;

    // Type environment (variable name -> type) with scope stack
    struct Scope {
        std::unordered_map<std::string, std::shared_ptr<Type>> bindings;
    };
    std::vector<Scope> scopes;

    // Function return type stack (for checking return statements)
    std::vector<std::shared_ptr<Type>> returnTypeStack;

    // Lambda bodies already walked: inferType re-visits exprs freely, so guard the
    // body walk to run once (else duplicate diagnostics and re-entered stamping).
    std::unordered_set<const LambdaExpr*> checkedLambdaBodies;

    // Module types by canonical dotted path (dep modules + package nodes):
    // `import x.y` populates "x" and "x.y". Single source for cross-module resolution.
    std::unordered_map<std::string, std::shared_ptr<ModuleType>> moduleTypes;

    // Get-or-create a ModuleType, building the package chain (`import x.y.z` wires
    // x->x.y->x.y.z). Idempotent: same name returns the same instance (stable identity).
    std::shared_ptr<ModuleType> getOrCreateModuleType(const std::string& canonicalName) {
        auto it = moduleTypes.find(canonicalName);
        if (it != moduleTypes.end()) return it->second;
        auto mt = std::make_shared<ModuleType>(canonicalName);
        moduleTypes[canonicalName] = mt;
        // Wire into the parent package, if any.
        auto dot = canonicalName.rfind('.');
        if (dot != std::string::npos) {
            auto parent = getOrCreateModuleType(canonicalName.substr(0, dot));
            parent->submodules[canonicalName.substr(dot + 1)] = mt;
        }
        return mt;
    }

    // Cached module-level exports (captured before scope is popped)
    std::unordered_map<std::string, std::shared_ptr<Type>> cachedExports;

    // D045 privacy context, set per-module in check(). currentModuleName is empty
    // for the entry module - use currentFile (the package/same-file key), not it.
    std::string currentFile;
    std::string currentModuleName;
    std::string currentPackage;
    const ClassType* currentClass = nullptr;
    std::unordered_map<std::string, std::string> packageKeyCache;

    // Defined out-of-line in TypeChecker.cpp (calls the TU-local packageKeyOf).
    const std::string& packageKey(const std::string& file);

    // Builtin name -> identity snapshot (post-builtin-setup). getExports() filters
    // builtins by identity, so a user `def open` shadowing one still exports.
    std::unordered_map<std::string, Type*> builtinIdentity;

    void pushScope() { scopes.push_back({}); }
    void popScope() { if (!scopes.empty()) scopes.pop_back(); }

    void define(const std::string& name, std::shared_ptr<Type> type) {
        if (!scopes.empty()) {
            scopes.back().bindings[name] = std::move(type);
        }
    }

    std::shared_ptr<Type> lookup(const std::string& name) {
        // Search from innermost to outermost scope
        for (int i = static_cast<int>(scopes.size()) - 1; i >= 0; --i) {
            auto it = scopes[i].bindings.find(name);
            if (it != scopes[i].bindings.end()) return it->second;
        }
        return nullptr;
    }

    // D044 generics / monomorphization state. See TypeCheckerGenerics.cpp.

    // Type-param binding scopes: while checking a template, the active frame maps
    // each param (T -> TypeVarType) so resolveType returns it, not "unknown type".
    std::vector<std::unordered_map<std::string, std::shared_ptr<Type>>> typeParamScopes;

    // Depth counter: >0 while checking inside a generic template body. Gates the
    // unbounded-`T` restriction (no method/operator calls on a `T`-typed value).
    int genericTemplateDepth = 0;

    // This module's generic templates by name (populated by the generic pre-pass)
    // so resolveType / visit(CallExpr) can find the decl to stamp at a use site.
    std::unordered_map<std::string, ClassDecl*> genericClasses;
    std::unordered_map<std::string, FunctionDecl*> genericFunctions;

    // D048 schema decoders: the stdlib `json.decode[T]` generic. The monomorphizer
    // synthesizes each stamp's body from T's fields instead of cloning a template.
    std::unordered_set<const FunctionDecl*> schemaDecodeFns;

    // D052 - `json.encode[T]`, the write-side mirror (Writer-driven, box-free).
    std::unordered_set<const FunctionDecl*> schemaEncodeFns;

    // Generic METHODS (`def m[T]` on any class; db.all[T] / D049). genericMethods is
    // keyed "Class.method"; classDeclByName lets stamps append into the owning class.
    std::unordered_map<std::string, FunctionDecl*> genericMethods;
    // Identity twin of genericMethods: owning ClassDecl -> method -> template.
    // Never collides across same-named classes; lookups prefer this.
    std::unordered_map<const ClassDecl*,
        std::unordered_map<std::string, FunctionDecl*>> genericMethodsByDecl;
    std::unordered_map<std::string, ClassDecl*> classDeclByName;

    // Defining module of each imported generic template (registerExternalGenerics);
    // the monomorphizer re-checks a stamped cross-module body with its types injected.
    std::unordered_map<const void*, std::string> genericTemplateModule;

    // D052 - per-module imported class bindings; the monomorphizer injects these
    // so a stamped cross-module body can name an imported type, not just siblings.
    std::unordered_map<std::string,
        std::unordered_map<std::string, std::shared_ptr<Type>>> moduleImportedTypes;

    // Templates already checked by the generic pre-pass; the main walk skips them
    // (free type vars, never lowered - only their stamped instantiations are).
    std::set<const Stmt*> genericChecked;

    // A pending monomorphization request: stamp `decl` at `args`.
    struct InstReq {
        std::string key;                            // canonical name, e.g. "Box[int]"
        std::string genericName;                    // "Box" (bare method name for a method req)
        bool isClass;
        std::vector<std::shared_ptr<Type>> args;    // resolved concrete type args
        std::string owningClass;                    // non-empty => generic METHOD on this class
        // True identity of the owning class (methods): preferred over the
        // by-name maps, which fold every dep first-wins.
        std::shared_ptr<ClassType> ownerCT;
    };
    std::vector<InstReq> pendingInsts;       // worklist
    std::set<std::string> instDone;          // cache keys already stamped (dedup)
    // Stamped-call result types by retargeted callee (`take[int]`): a re-visit sees
    // the stamped name (no longer a generic) and would clobber the return to unknown.
    std::unordered_map<std::string, std::shared_ptr<Type>> stampedCallReturnType;

    // The module currently being checked - the monomorphizer appends stamped
    // instantiations to its body so they flow through CodeGen unchanged.
    Module* currentModule = nullptr;

    // Expected-type hint for the next value expr (set by Ann/AssignStmt, consumed in
    // visit(CallExpr)): `b: Box[int] = Box(5)` infers args. Single-slot, consume-on-read.
    std::shared_ptr<Type> currentExpectedType;

    // The one expression currently allowed to resolve to a bare class method
    // (a call's callee, or `.__doc__`'s object); visit(AttributeExpr) rejects it elsewhere.
    const Expr* methodRefOkExpr = nullptr;

    std::unordered_set<const Expr*> rangeValueOkExprs;

    std::unordered_map<const Expr*, long long> constIntFolds;

    // Polymorphic-recursion guard (`Foo[T]` -> `Foo[list[T]]` ...): distinct stamps
    // this module. Hitting the cap is a compile error, not a silent truncation.
    int instantiationCount = 0;    // total distinct instantiations stamped
    int instDepth = 0;             // current instantiateGenericClass recursion depth
    bool instCapReported = false;  // error emitted once when a cap is hit
    bool genericsAborted = false;  // a cap tripped - stop all further instantiation
    // Nearest enclosing instantiation's use-site location; a transitive re-instantiation
    // (an `Inner[T]` field of `Outer[int]`) inherits it instead of reporting at 0:0.
    SourceLocation lastInstLoc;
    static constexpr int kMaxInstantiations = 4096;  // breadth budget
    // Recursion-DEPTH cap: substituteType->instantiateGenericClass recurses in C++;
    // bounds the native stack and catches unbounded recursion. Far above real nesting.
    static constexpr int kMaxInstDepth = 200;
};

}  // namespace dragon

#endif  // DRAGON_TYPE_CHECKER_IMPL_H
