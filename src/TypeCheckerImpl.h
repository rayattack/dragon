#ifndef DRAGON_TYPE_CHECKER_IMPL_H
#define DRAGON_TYPE_CHECKER_IMPL_H

// Internal definition of TypeChecker::Impl, shared between TypeChecker.cpp (the
// core checker + visitors) and TypeCheckerGenerics.cpp (the Decision 044
// monomorphization engine). Not a public header - lives under src/.

#include "dragon/AST.h"
#include "dragon/TypeChecker.h"

#include <memory>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace dragon {

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

    // Type environment (variable name -> type) with scope stack
    struct Scope {
        std::unordered_map<std::string, std::shared_ptr<Type>> bindings;
    };
    std::vector<Scope> scopes;

    // Function return type stack (for checking return statements)
    std::vector<std::shared_ptr<Type>> returnTypeStack;

    // Module types keyed by canonical dotted path. Holds both registered
    // dependency modules and intermediate package nodes. `import x.y`
    // populates both "x" (with submodule "y") and "x.y" (with the actual
    // exports). Single source of truth for cross-module name resolution.
    std::unordered_map<std::string, std::shared_ptr<ModuleType>> moduleTypes;

    // Get-or-create a ModuleType for `canonicalName`, building the package
    // chain so `import x.y.z` registers "x", "x.y", and "x.y.z" with
    // submodule links wired (x.submodules["y"]->x.y, x.y.submodules["z"]->x.y.z).
    // Idempotent - returning the same ModuleType instance for the same name
    // across calls keeps the type-system identity stable.
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

    // D045 - access-site context for member/import privacy. Set per-module in
    // check() (each module gets a fresh TypeChecker). `currentFile` is the
    // always-populated package/same-file key; `currentModuleName` is the
    // canonical dotted name (empty for the entry module - do NOT derive package
    // from it). `currentPackage` = packageKeyOf(currentFile). `currentClass` is
    // the lexically-enclosing class (push/pop around visit(ClassDecl) body), or
    // null in a free function / module top level. `packageKeyCache` memoizes
    // the filesystem probe in packageKeyOf so repeated checks are cheap.
    std::string currentFile;
    std::string currentModuleName;
    std::string currentPackage;
    const ClassType* currentClass = nullptr;
    std::unordered_map<std::string, std::string> packageKeyCache;

    // Defined out-of-line in TypeChecker.cpp (calls the TU-local packageKeyOf).
    const std::string& packageKey(const std::string& file);

    // Snapshot of (builtin name -> shared_ptr identity) taken right after
    // builtins are defined into scope[0] of `check()`. getExports() uses
    // this to filter out the BUILTIN entries while keeping user defs that
    // happen to shadow a builtin name (e.g. `def open(...)` in stdlib/gzip.dr
    // - we want `gzip.open` to be exported, but the unshadowed `print` in
    // a different module should not be re-exported as that module's symbol).
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

    //===-----------------------------------------------------------------===//
    // D044 - generics / monomorphization state. See TypeCheckerGenerics.cpp.
    //===-----------------------------------------------------------------===//

    // Stack of type-parameter binding scopes. While checking a generic template
    // (class/function with non-empty typeParams), the active frame maps each
    // type-param name (e.g. "T") to its TypeVarType so resolveType returns the
    // type variable instead of erroring "unknown type". Nested generics (a
    // generic method of a generic class) stack additional frames.
    std::vector<std::unordered_map<std::string, std::shared_ptr<Type>>> typeParamScopes;

    // Depth counter: >0 while checking inside a generic template body. Gates the
    // unbounded-`T` restriction (no method/operator calls on a `T`-typed value).
    int genericTemplateDepth = 0;

    // Registries of this module's generic templates, keyed by name. Populated by
    // the generic pre-pass so resolveType / visit(CallExpr) can find the decl to
    // stamp at a use site.
    std::unordered_map<std::string, ClassDecl*> genericClasses;
    std::unordered_map<std::string, FunctionDecl*> genericFunctions;

    // D044+ generic METHODS - a method that declares its OWN type parameter
    // (`def m[T](...)` on a class, generic or not; the db.all[T] / D049 shape).
    // `genericMethods` is keyed "Class.method" so two classes' same-named generic
    // methods never collide. `classDeclByName` (every class, generic or not) lets
    // the monomorphizer append a stamped method into its owning class body, so
    // CodeGen emits it through the existing per-class method loops.
    std::unordered_map<std::string, FunctionDecl*> genericMethods;
    std::unordered_map<std::string, ClassDecl*> classDeclByName;

    // Defining module (canonical dotted name) of each generic template, recorded
    // when an IMPORTED module's templates are registered (registerExternalGenerics).
    // Absent => defined in the module currently being checked. The monomorphizer
    // uses it to re-check a stamped CROSS-MODULE body with the defining module's
    // exported types injected into `typeNames`, so the body's bare references to
    // sibling types (e.g. a return-type class the generic method delegates to)
    // resolve in the instantiating module - fixing cross-module generic methods
    // whose bodies touch other module-level entities.
    std::unordered_map<const void*, std::string> genericTemplateModule;

    // Generic templates already fully checked by the generic pre-pass - the main
    // module walk skips re-visiting them (they have free type vars and must not
    // be lowered; only their stamped instantiations are checked + emitted).
    std::set<const Stmt*> genericChecked;

    // A pending monomorphization request: stamp `decl` at `args`.
    struct InstReq {
        std::string key;                            // canonical name, e.g. "Box[int]"
        std::string genericName;                    // "Box" (bare method name for a method req)
        bool isClass;
        std::vector<std::shared_ptr<Type>> args;    // resolved concrete type args
        std::string owningClass;                    // non-empty => generic METHOD on this class
    };
    std::vector<InstReq> pendingInsts;       // worklist
    std::set<std::string> instDone;          // cache keys already stamped (dedup)
    // Stamped-call result types, keyed by the retargeted callee name (e.g.
    // `take[int]`). A generic call is retargeted to its stamped name on first
    // type-check; a later RE-VISIT of the same expr sees `take[int]` - no longer
    // a registered generic - and would fall through to the normal-call path,
    // clobbering the resolved return type to <unknown>. Restoring from here keeps
    // the call's type correct when there is no binding annotation to supply it
    // (e.g. `print(take([..], 2))`).
    std::unordered_map<std::string, std::shared_ptr<Type>> stampedCallReturnType;

    // The module currently being checked - the monomorphizer appends stamped
    // instantiations to its body so they flow through CodeGen unchanged.
    Module* currentModule = nullptr;

    // Expected-type hint for the immediately-following value expression, set by
    // visit(AnnAssignStmt)/visit(AssignStmt) and consumed (cleared) at the top of
    // visit(CallExpr). Lets `b: Box[int] = Box(5)` infer the construction's type
    // arguments from the binding annotation. Single-slot + consume-on-read so it
    // never leaks into a nested call's arguments.
    std::shared_ptr<Type> currentExpectedType;

    // Recursion guard for polymorphic recursion (`Foo[T]` -> `Foo[list[T]]` ...):
    // total distinct instantiations stamped this module. Hitting the cap is a
    // compile error, not a silent truncation.
    int instantiationCount = 0;    // total distinct instantiations stamped
    int instDepth = 0;             // current instantiateGenericClass recursion depth
    bool instCapReported = false;  // error emitted once when a cap is hit
    bool genericsAborted = false;  // a cap tripped - stop all further instantiation
    // Location of the nearest enclosing generic-class instantiation with a real
    // use-site position. A transitive re-instantiation (substituteType stamping
    // an `Inner[T]` field of `Outer[int]`) has no location of its own, so its
    // diagnostics inherit this instead of reporting at 0:0.
    SourceLocation lastInstLoc;
    static constexpr int kMaxInstantiations = 4096;  // breadth budget
    // Recursion-DEPTH cap. The substituteType->instantiateGenericClass chain
    // (`Foo[T]` -> `Foo[list[T]]` -> ...) recurses in C++; this bounds the native
    // stack AND catches unbounded polymorphic recursion fast. Far above any
    // legitimate nesting depth (`Box[Box[Box[int]]]` is ~3).
    static constexpr int kMaxInstDepth = 200;
};

}  // namespace dragon

#endif  // DRAGON_TYPE_CHECKER_IMPL_H
