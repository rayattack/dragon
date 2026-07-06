// Decision 044 - generics monomorphization engine.
//
// The doctrine: monomorphize by default. A `class Foo[T]` / `def f[T]()` is a
// *template* - parsed and abstractly type-checked once with each type parameter
// bound to a `TypeVarType`, but never lowered. At every use site that pins
// concrete type arguments, the checker records an instantiation request and
// returns a monomorphic placeholder type. After the module is checked,
// runMonomorphization() drains the request worklist: it deep-clones each
// template's AST with the type parameters substituted to concrete TypeExprs,
// appends the resulting ordinary class/function to the module body, and
// type-checks it - which may discover transitive instantiations, so the
// worklist runs to a fixpoint. The stamped decls then flow through CodeGen
// exactly like hand-written ones (native representation, zero boxing - #1).
//
// This file holds the engine; the integration hooks (resolveType's generic
// branch, the generic pre-pass + worklist invocation in check(), the
// visit(ClassDecl/FunctionDecl) type-param binding, and the generic-call
// retargeting in visit(CallExpr)) live in TypeChecker.cpp.

#include "dragon/TypeChecker.h"
#include "TypeCheckerImpl.h"
#include "dragon/AstClone.h"

#include <functional>

namespace dragon {

namespace {

// Convert a value-position expression used as a call-site type argument into a
// TypeExpr (so resolveType can turn it into a Type). `first[int]` parses the
// `int` as a NameExpr; `first[list[int]]` parses `list[int]` as a SubscriptExpr;
// `m[a.b.C]` parses a dotted AttributeExpr. Returns nullptr for anything that
// isn't a plausible type denotation.
// A type is "concrete" (stampable) iff it contains no free type variable. An
// instantiation requested with a non-concrete argument - e.g. `Inner[T]` inside
// the body of generic `Outer[T]` - must NOT be enqueued for stamping: the
// concrete `Inner[int]` is produced transitively (substituteType) when
// `Outer[int]` is stamped. The placeholder is still built (the template body
// needs a type for it), but it never reaches the worklist.
bool typeIsConcrete(const Type* t) {
    if (!t) return true;
    switch (t->kind()) {
        case Type::Kind::TypeVar: return false;
        case Type::Kind::List:
            return typeIsConcrete(static_cast<const ListType&>(*t).elementType.get());
        case Type::Kind::Dict: {
            auto& d = static_cast<const DictType&>(*t);
            return typeIsConcrete(d.keyType.get()) && typeIsConcrete(d.valueType.get());
        }
        case Type::Kind::Tuple: {
            for (auto& e : static_cast<const TupleType&>(*t).elementTypes)
                if (!typeIsConcrete(e.get())) return false;
            return true;
        }
        case Type::Kind::Task:
            return typeIsConcrete(static_cast<const TaskType&>(*t).resultType.get());
        case Type::Kind::Union: {
            for (auto& e : static_cast<const UnionType&>(*t).types)
                if (!typeIsConcrete(e.get())) return false;
            return true;
        }
        case Type::Kind::Instance: {
            auto& i = static_cast<const InstanceType&>(*t);
            if (i.classType)
                for (auto& a : i.classType->genericArgs)
                    if (!typeIsConcrete(a.get())) return false;
            return true;
        }
        default:
            return true;  // primitives, Any, Never, Ptr, Lock, Module
    }
}

bool argsAreConcrete(const std::vector<std::shared_ptr<Type>>& args) {
    for (auto& a : args) if (!typeIsConcrete(a.get())) return false;
    return true;
}

// Structural nesting depth of a type (`list[list[int]]` -> 3). Bounds the
// otherwise-unbounded polymorphic recursion of a generic function/method that
// instantiates a strictly-deeper version of itself (`go[U]` -> `go[list[U]]` ->
// ...): those stamps drain ITERATIVELY through the worklist, so they never pass
// through instantiateGenericClass's C++-recursion depth cap. The `budget`
// guards this helper's own recursion against a pathological input.
int typeNestingDepth(const Type* t, int budget = 1024) {
    if (!t || budget <= 0) return 1;
    int d = 0;
    switch (t->kind()) {
        case Type::Kind::List:
            d = typeNestingDepth(static_cast<const ListType&>(*t).elementType.get(),
                                 budget - 1);
            break;
        case Type::Kind::Dict: {
            auto& dt = static_cast<const DictType&>(*t);
            d = std::max(typeNestingDepth(dt.keyType.get(), budget - 1),
                         typeNestingDepth(dt.valueType.get(), budget - 1));
            break;
        }
        case Type::Kind::Tuple:
            for (auto& e : static_cast<const TupleType&>(*t).elementTypes)
                d = std::max(d, typeNestingDepth(e.get(), budget - 1));
            break;
        case Type::Kind::Task:
            d = typeNestingDepth(static_cast<const TaskType&>(*t).resultType.get(),
                                 budget - 1);
            break;
        case Type::Kind::Instance: {
            auto& i = static_cast<const InstanceType&>(*t);
            if (i.classType)
                for (auto& a : i.classType->genericArgs)
                    d = std::max(d, typeNestingDepth(a.get(), budget - 1));
            break;
        }
        default:
            break;  // scalars / Any / Ptr - depth 1
    }
    return d + 1;
}

std::unique_ptr<TypeExpr> exprToTypeExpr(const Expr* e) {
    if (!e) return nullptr;
    if (auto* n = dynamic_cast<const NameExpr*>(e)) {
        auto t = std::make_unique<NamedTypeExpr>();
        t->name = n->name;
        t->setLocation(e->location());
        return t;
    }
    if (auto* a = dynamic_cast<const AttributeExpr*>(e)) {
        // Reconstruct a dotted name `pkg.mod.Class` from the attribute chain.
        std::function<bool(const Expr*, std::string&)> dotted =
            [&](const Expr* x, std::string& out) -> bool {
            if (auto* nm = dynamic_cast<const NameExpr*>(x)) { out = nm->name; return true; }
            if (auto* at = dynamic_cast<const AttributeExpr*>(x)) {
                std::string base;
                if (!dotted(at->object.get(), base)) return false;
                out = base + "." + at->attribute;
                return true;
            }
            return false;
        };
        std::string name;
        if (!dotted(a, name)) return nullptr;
        auto t = std::make_unique<NamedTypeExpr>();
        t->name = name;
        t->setLocation(e->location());
        return t;
    }
    // `int | str` in value-subscript position (an explicit union type argument,
    // e.g. `Box[int | str](...)`) parses as a `|` BinaryExpr; flatten it into a
    // UnionTypeExpr so resolveType yields the same UnionType the annotation form
    // (`x: Box[int | str]`) produces.
    if (auto* b = dynamic_cast<const BinaryExpr*>(e)) {
        if (b->op.type() == TokenType::PIPE) {
            auto u = std::make_unique<UnionTypeExpr>();
            std::function<void(const Expr*)> flatten = [&](const Expr* side) {
                auto* bb = dynamic_cast<const BinaryExpr*>(side);
                if (bb && bb->op.type() == TokenType::PIPE) {
                    flatten(bb->left.get());
                    flatten(bb->right.get());
                } else if (auto t = exprToTypeExpr(side)) {
                    u->types.push_back(std::move(t));
                }
            };
            flatten(b->left.get());
            flatten(b->right.get());
            if (u->types.size() < 2) return nullptr;
            u->setLocation(e->location());
            return u;
        }
        return nullptr;
    }
    if (auto* s = dynamic_cast<const SubscriptExpr*>(e)) {
        auto base = exprToTypeExpr(s->object.get());
        if (!base) return nullptr;
        auto g = std::make_unique<GenericTypeExpr>();
        g->base = std::move(base);
        if (auto* tup = dynamic_cast<const TupleExpr*>(s->index.get())) {
            for (auto& el : tup->elements) {
                auto a = exprToTypeExpr(el.get());
                if (!a) return nullptr;
                g->typeArgs.push_back(std::move(a));
            }
        } else {
            auto a = exprToTypeExpr(s->index.get());
            if (!a) return nullptr;
            g->typeArgs.push_back(std::move(a));
        }
        g->setLocation(e->location());
        return g;
    }
    return nullptr;
}

}  // namespace

//===----------------------------------------------------------------------===//
// Type-parameter scope lookup
//===----------------------------------------------------------------------===//

std::shared_ptr<Type> TypeChecker::lookupTypeParam(const std::string& name) {
    for (auto it = impl_->typeParamScopes.rbegin(); it != impl_->typeParamScopes.rend(); ++it) {
        auto found = it->find(name);
        if (found != it->end()) return found->second;
    }
    return nullptr;
}

//===----------------------------------------------------------------------===//
// Canonical instantiation name (cache key + stamped decl name + LLVM symbol).
// Brackets/commas cannot appear in a user identifier, so this is collision-free
// with hand-written names (Decision 044, "Cache key + name mangling").
//===----------------------------------------------------------------------===//

std::string TypeChecker::mangleInstantiation(
    const std::string& genericName,
    const std::vector<std::shared_ptr<Type>>& args) {
    std::string s = genericName + "[";
    for (size_t i = 0; i < args.size(); ++i) {
        if (i) s += ",";
        s += args[i] ? args[i]->toString() : "?";
    }
    s += "]";
    return s;
}

//===----------------------------------------------------------------------===//
// Type substitution: deep-copy a Type, replacing TypeVarType by name.
//===----------------------------------------------------------------------===//

std::shared_ptr<Type> TypeChecker::substituteType(
    const std::shared_ptr<Type>& t,
    const std::unordered_map<std::string, std::shared_ptr<Type>>& bindings) {
    if (!t) return t;
    switch (t->kind()) {
        case Type::Kind::TypeVar: {
            auto& tv = static_cast<const TypeVarType&>(*t);
            auto it = bindings.find(tv.name);
            return it != bindings.end() ? it->second : t;
        }
        case Type::Kind::List: {
            auto& l = static_cast<const ListType&>(*t);
            return std::make_shared<ListType>(substituteType(l.elementType, bindings));
        }
        case Type::Kind::Dict: {
            auto& d = static_cast<const DictType&>(*t);
            return std::make_shared<DictType>(substituteType(d.keyType, bindings),
                                              substituteType(d.valueType, bindings));
        }
        case Type::Kind::Tuple: {
            auto& tp = static_cast<const TupleType&>(*t);
            std::vector<std::shared_ptr<Type>> elems;
            for (auto& e : tp.elementTypes) elems.push_back(substituteType(e, bindings));
            return std::make_shared<TupleType>(std::move(elems));
        }
        case Type::Kind::Task: {
            auto& tk = static_cast<const TaskType&>(*t);
            return std::make_shared<TaskType>(substituteType(tk.resultType, bindings));
        }
        case Type::Kind::Union: {
            auto& u = static_cast<const UnionType&>(*t);
            std::vector<std::shared_ptr<Type>> types;
            for (auto& e : u.types) types.push_back(substituteType(e, bindings));
            return std::make_shared<UnionType>(std::move(types));
        }
        case Type::Kind::Function: {
            auto& f = static_cast<const FunctionType&>(*t);
            std::vector<std::shared_ptr<Type>> params;
            for (auto& p : f.paramTypes) params.push_back(substituteType(p, bindings));
            auto r = std::make_shared<FunctionType>(params, substituteType(f.returnType, bindings));
            r->paramNames = f.paramNames;
            r->requiredParams = f.requiredParams;
            r->hasVarArg = f.hasVarArg;
            r->hasArgMeta = f.hasArgMeta;
            r->isMethod = f.isMethod;
            return r;
        }
        case Type::Kind::Instance: {
            auto& inst = static_cast<const InstanceType&>(*t);
            auto ct = inst.classType;
            // Transitive instantiation: a `Inner[T]` field inside `Outer[T]`
            // re-instantiates to `Inner[int]` when stamping `Outer[int]`.
            if (ct && !ct->genericOrigin.empty()) {
                bool changed = false;
                std::vector<std::shared_ptr<Type>> newArgs;
                for (auto& a : ct->genericArgs) {
                    auto na = substituteType(a, bindings);
                    if (na.get() != a.get()) changed = true;
                    newArgs.push_back(na);
                }
                if (changed) {
                    auto declIt = impl_->genericClasses.find(ct->genericOrigin);
                    if (declIt != impl_->genericClasses.end())
                        return instantiateGenericClass(declIt->second, newArgs,
                                                       SourceLocation{});
                }
            }
            return t;  // ordinary concrete class - no type vars inside
        }
        default:
            return t;  // primitives, Any, Never, Ptr, Lock, Module, Unknown
    }
}

//===----------------------------------------------------------------------===//
// Type -> TypeExpr (drives AST substitution when stamping).
//===----------------------------------------------------------------------===//

std::unique_ptr<TypeExpr> TypeChecker::typeToTypeExpr(const std::shared_ptr<Type>& t) {
    auto named = [](const std::string& n) {
        auto x = std::make_unique<NamedTypeExpr>();
        x->name = n;
        return x;
    };
    if (!t) return named("Any");
    auto generic = [&](const std::string& base,
                       std::vector<std::shared_ptr<Type>> args) {
        auto g = std::make_unique<GenericTypeExpr>();
        g->base = named(base);
        for (auto& a : args) g->typeArgs.push_back(typeToTypeExpr(a));
        return g;
    };
    switch (t->kind()) {
        case Type::Kind::Int:   return named("int");
        case Type::Kind::Float: return named("float");
        case Type::Kind::Bool:  return named("bool");
        case Type::Kind::Str:   return named("str");
        case Type::Kind::Bytes: return named("bytes");
        case Type::Kind::None_: return named("None");
        case Type::Kind::Any:   return named("Any");
        case Type::Kind::Ptr:   return named("ptr");
        case Type::Kind::TypeVar: return named(static_cast<const TypeVarType&>(*t).name);
        case Type::Kind::List:
            return generic("list", {static_cast<const ListType&>(*t).elementType});
        case Type::Kind::Dict: {
            auto& d = static_cast<const DictType&>(*t);
            return generic("dict", {d.keyType, d.valueType});
        }
        case Type::Kind::Tuple:
            return generic("tuple", static_cast<const TupleType&>(*t).elementTypes);
        case Type::Kind::Task:
            return generic("Task", {static_cast<const TaskType&>(*t).resultType});
        case Type::Kind::Instance:
            // Names a concrete class (incl. a stamped instantiation like
            // "Box[int]"); resolveType finds it in typeNames.
            return named(static_cast<const InstanceType&>(*t).classType->name);
        case Type::Kind::Class:
            return named(static_cast<const ClassType&>(*t).name);
        case Type::Kind::Union: {
            auto& u = static_cast<const UnionType&>(*t);
            auto ue = std::make_unique<UnionTypeExpr>();
            for (auto& e : u.types) ue->types.push_back(typeToTypeExpr(e));
            return ue;
        }
        case Type::Kind::Function: {
            // A callable type argument (`Box[Callable[[int], int]]`) - rebuild
            // the Callable annotation so the field/param keeps its real signature
            // rather than silently degrading to Any.
            auto& f = static_cast<const FunctionType&>(*t);
            auto ce = std::make_unique<CallableTypeExpr>();
            for (auto& p : f.paramTypes) ce->paramTypes.push_back(typeToTypeExpr(p));
            ce->returnType = typeToTypeExpr(f.returnType);
            return ce;
        }
        case Type::Kind::Lock:  return named("Lock");
        case Type::Kind::Never: return named("Never");
        // Module / Unknown can't be a real concrete type argument; Any is the
        // only safe denotation and any downstream use is already an error.
        default: return named("Any");
    }
}

//===----------------------------------------------------------------------===//
// Unification: solve type parameters from a concrete actual type.
//===----------------------------------------------------------------------===//

bool TypeChecker::unifyTypeParam(
    const std::shared_ptr<Type>& declared, const std::shared_ptr<Type>& actual,
    std::unordered_map<std::string, std::shared_ptr<Type>>& out) {
    if (!declared || !actual) return true;
    if (declared->kind() == Type::Kind::TypeVar) {
        const std::string& nm = static_cast<const TypeVarType&>(*declared).name;
        auto it = out.find(nm);
        if (it == out.end()) { out[nm] = actual; return true; }
        // Already bound: a second occurrence must agree (e.g. `def f[T](a:T,b:T)`).
        return it->second && it->second->equals(*actual);
    }
    if (declared->kind() == Type::Kind::List && actual->kind() == Type::Kind::List)
        return unifyTypeParam(static_cast<const ListType&>(*declared).elementType,
                              static_cast<const ListType&>(*actual).elementType, out);
    if (declared->kind() == Type::Kind::Dict && actual->kind() == Type::Kind::Dict) {
        auto& d = static_cast<const DictType&>(*declared);
        auto& a = static_cast<const DictType&>(*actual);
        return unifyTypeParam(d.keyType, a.keyType, out) &&
               unifyTypeParam(d.valueType, a.valueType, out);
    }
    if (declared->kind() == Type::Kind::Tuple && actual->kind() == Type::Kind::Tuple) {
        auto& d = static_cast<const TupleType&>(*declared);
        auto& a = static_cast<const TupleType&>(*actual);
        if (d.elementTypes.size() != a.elementTypes.size()) return false;
        for (size_t i = 0; i < d.elementTypes.size(); ++i)
            if (!unifyTypeParam(d.elementTypes[i], a.elementTypes[i], out)) return false;
        return true;
    }
    if (declared->kind() == Type::Kind::Task && actual->kind() == Type::Kind::Task)
        return unifyTypeParam(static_cast<const TaskType&>(*declared).resultType,
                              static_cast<const TaskType&>(*actual).resultType, out);
    // No type variable to refine here - not a hard failure; the concrete check
    // happens when the stamped body is type-checked.
    return true;
}

//===----------------------------------------------------------------------===//
// Generic-class instantiation: build (once) the monomorphic placeholder type
// and record a stamping request.
//===----------------------------------------------------------------------===//

std::shared_ptr<Type> TypeChecker::instantiateGenericClass(
    ClassDecl* decl, std::vector<std::shared_ptr<Type>> args, const SourceLocation& loc) {
    // Once any cap has tripped, every further instantiation is a no-op so the
    // partially-built program drains immediately instead of doing 4096× work.
    if (impl_->genericsAborted) return impl_->unknownType;

    // A transitive re-instantiation (substituteType -> here, for an `Inner[T]`
    // field while stamping `Outer[int]`) carries no use-site location - inherit
    // the enclosing instantiation's so its diagnostics never report at 0:0. A
    // direct call (resolveType / a CallExpr) supplies a real location and records
    // it here for any transitive calls it triggers.
    const SourceLocation& effLoc = (loc.line != 0) ? loc : impl_->lastInstLoc;
    if (loc.line != 0) impl_->lastInstLoc = loc;

    if (args.size() != decl->typeParams.size()) {
        error(effLoc, "generic class '" + decl->name + "' expects " +
              std::to_string(decl->typeParams.size()) + " type argument(s), got " +
              std::to_string(args.size()));
        return impl_->unknownType;
    }
    std::string key = mangleInstantiation(decl->name, args);

    // Dedup: a second `Box[int]` reuses the placeholder built the first time.
    if (auto it = impl_->typeNames.find(key); it != impl_->typeNames.end())
        return it->second;

    // Polymorphic-recursion guard. `Foo[T]` -> `Foo[list[T]]` -> ... defeats the
    // dedup (every key is distinct) and recurses through substituteType. The
    // DEPTH cap bounds the native C++ stack and trips fast; the breadth cap
    // catches pathological non-recursive blow-ups. Either is a compile error,
    // never a silent truncation or a hang (Decision 044). On trip we set
    // genericsAborted and clear the worklist so nothing further is stamped.
    auto abortGenerics = [&](const char* why) {
        if (!impl_->instCapReported) {
            error(effLoc, std::string("too many generic instantiations of '") +
                  decl->name + "' (" + why + ", e.g. Foo[T] instantiating "
                  "Foo[list[T]]); a generic type that instantiates a strictly "
                  "deeper version of itself recurses without limit");
            impl_->instCapReported = true;
        }
        impl_->genericsAborted = true;
        impl_->pendingInsts.clear();
    };
    if (impl_->instDepth >= Impl::kMaxInstDepth) {
        abortGenerics("instantiation nested too deep");
        return impl_->unknownType;
    }
    if (++impl_->instantiationCount > Impl::kMaxInstantiations) {
        abortGenerics("too many distinct instantiations");
        return impl_->unknownType;
    }

    // The generic ClassType (TypeVar-typed members), populated by the generic
    // pre-pass. Without it we can't build the specialization's signature.
    std::shared_ptr<ClassType> genericCT;
    if (auto it = impl_->typeNames.find(decl->name); it != impl_->typeNames.end())
        if (auto inst = std::dynamic_pointer_cast<InstanceType>(it->second))
            genericCT = inst->classType;

    std::unordered_map<std::string, std::shared_ptr<Type>> bindings;
    bool boundViolated = false;
    for (size_t i = 0; i < decl->typeParams.size(); ++i) {
        bindings[decl->typeParams[i].name] = args[i];
        // Bounds - a bounded type parameter requires its concrete argument to be
        // the bound class or a subclass. Skip a still-abstract arg (a TypeVar,
        // e.g. `Inner[T]` while checking generic `Outer[T]`); its concrete form
        // is re-checked when `Outer[int]` stamps `Inner[int]` transitively.
        auto& tp = decl->typeParams[i];
        if (tp.bound && args[i] && args[i]->kind() != Type::Kind::TypeVar) {
            auto boundType = resolveType(tp.bound.get());
            if (boundType && boundType->kind() != Type::Kind::Unknown &&
                !args[i]->isSubtypeOf(*boundType)) {
                error(effLoc, "type argument '" + args[i]->toString() +
                      "' does not satisfy bound '" + boundType->toString() +
                      "' of type parameter '" + tp.name + "' in generic '" +
                      decl->name + "'");
                boundViolated = true;
            }
        }
    }

    auto ph = std::make_shared<ClassType>(key);
    ph->genericOrigin = decl->name;
    ph->genericArgs = args;
    if (genericCT) {
        ph->definingModule = genericCT->definingModule;
        ph->definingFile = genericCT->definingFile;
        ph->isTypedDict = genericCT->isTypedDict;
        ph->isEnum = genericCT->isEnum;
        ph->constructorCount = genericCT->constructorCount;
        ph->declaredFieldNames = genericCT->declaredFieldNames;
    }
    // Register BEFORE substituting members so a self-referential generic
    // (`Node[T]` with `next: Node[T]`) resolves `Node[int]` to this same
    // placeholder instead of recursing forever.
    auto phInst = std::make_shared<InstanceType>(ph);
    impl_->typeNames[key] = phInst;
    impl_->define(key, ph);

    // Enqueue for stamping ONLY when fully concrete. `Inner[T]` (T still free,
    // inside generic `Outer[T]`) builds a placeholder but is not stamped; its
    // concrete `Inner[int]` is enqueued transitively when `Outer[int]` is.
    // Bounds - do not stamp an instantiation whose argument violated a bound: the
    // diagnostic is already reported, and stamping the body with the wrong type
    // would only emit a confusing cascade (e.g. "no attribute 'speak'").
    if (argsAreConcrete(args) && !boundViolated)
        impl_->pendingInsts.push_back({key, decl->name, /*isClass=*/true, args});

    if (genericCT) {
        // Member substitution can re-enter instantiateGenericClass (transitive +
        // self-deeper generics); bracket it with the depth counter.
        impl_->instDepth++;
        for (auto& [fn, ft] : genericCT->fields) ph->fields[fn] = substituteType(ft, bindings);
        for (auto& [mn, mt] : genericCT->methods) ph->methods[mn] = substituteType(mt, bindings);
        if (genericCT->parentClass) ph->parentClass = substituteType(genericCT->parentClass, bindings);
        impl_->instDepth--;
    }
    return phInst;
}

//===----------------------------------------------------------------------===//
// Generic-function call: instantiate, retarget the callee, set the result type.
//===----------------------------------------------------------------------===//

bool TypeChecker::tryInstantiateGenericCall(
    CallExpr& node, const std::vector<std::shared_ptr<Type>>& argTypes,
    const std::shared_ptr<Type>& expected) {
    // Identify the generic function/method and any explicit `[...]` type args.
    FunctionDecl* decl = nullptr;
    std::string fnName;
    std::vector<std::unique_ptr<TypeExpr>> explicitArgs;
    // Method-call state (empty owningClass => generic FREE function).
    std::string owningClass;           // the class that DECLARES the generic method
    AttributeExpr* methodAttr = nullptr;  // the `recv.method` node, for retargeting
    const ClassType* probeCls = nullptr;  // receiver class of a method-shaped call
    std::string probeMethod;              // its method name (for a clean deferral)

    // Resolve a receiver expression to the ClassType it is an instance of.
    auto receiverClass = [&](Expr* recv) -> const ClassType* {
        auto t = inferType(recv);
        if (auto inst = std::dynamic_pointer_cast<InstanceType>(t))
            return inst->classType.get();
        return nullptr;
    };
    // Find a generic method named `m` reachable from `cls`, walking the MRO so an
    // inherited generic method (defined on a base, called on a subclass) resolves
    // to its DECLARING class - the class whose body the stamp is appended to.
    auto findGenericMethod = [&](const ClassType* cls, const std::string& m,
                                 std::string& declClass) -> FunctionDecl* {
        const ClassType* c = cls;
        for (int guard = 0; c && guard < 256; ++guard) {
            auto it = impl_->genericMethods.find(c->name + "." + m);
            if (it != impl_->genericMethods.end()) { declClass = c->name; return it->second; }
            // Stamped generic-class instantiation (`Container[int]`): its generic
            // method template is registered under the ORIGIN class name
            // (`Container.wrap`, still carrying the class's `T`). Resolve it there
            // but keep declClass = the STAMPED name, so the stamp is scoped and
            // keyed to this instantiation - double monomorphization (D044).
            if (!c->genericOrigin.empty()) {
                auto oit = impl_->genericMethods.find(c->genericOrigin + "." + m);
                if (oit != impl_->genericMethods.end()) {
                    declClass = c->name;
                    return oit->second;
                }
            }
            c = c->parentClass && c->parentClass->kind() == Type::Kind::Class
                    ? static_cast<const ClassType*>(c->parentClass.get())
                    : nullptr;
        }
        return nullptr;
    };

    if (auto* nm = dynamic_cast<NameExpr*>(node.callee.get())) {
        auto it = impl_->genericFunctions.find(nm->name);
        if (it != impl_->genericFunctions.end()) { decl = it->second; fnName = nm->name; }
    } else if (auto* sub = dynamic_cast<SubscriptExpr*>(node.callee.get())) {
        if (auto* nm = dynamic_cast<NameExpr*>(sub->object.get())) {
            auto it = impl_->genericFunctions.find(nm->name);
            if (it != impl_->genericFunctions.end()) {
                decl = it->second;
                fnName = nm->name;
                if (auto* tup = dynamic_cast<TupleExpr*>(sub->index.get())) {
                    for (auto& el : tup->elements) explicitArgs.push_back(exprToTypeExpr(el.get()));
                } else {
                    explicitArgs.push_back(exprToTypeExpr(sub->index.get()));
                }
            }
        } else if (auto* at = dynamic_cast<AttributeExpr*>(sub->object.get())) {
            // Generic METHOD with explicit type args: `recv.method[T](args)` parses
            // as CallExpr{ SubscriptExpr{ object: AttributeExpr{recv,method},
            // index: T } }. Resolve the receiver's class (NOT the AttributeExpr as a
            // dotted type name) and look the method up by class+name.
            if (const ClassType* cls = receiverClass(at->object.get())) {
                probeCls = cls; probeMethod = at->attribute;
                if (FunctionDecl* m = findGenericMethod(cls, at->attribute, owningClass)) {
                    decl = m; fnName = at->attribute; methodAttr = at;
                    if (auto* tup = dynamic_cast<TupleExpr*>(sub->index.get())) {
                        for (auto& el : tup->elements) explicitArgs.push_back(exprToTypeExpr(el.get()));
                    } else {
                        explicitArgs.push_back(exprToTypeExpr(sub->index.get()));
                    }
                }
            }
        }
    } else if (auto* at = dynamic_cast<AttributeExpr*>(node.callee.get())) {
        // Generic METHOD, type args inferred: `recv.method(args)` with no `[T]`.
        // Only a method registered as generic takes this path; an ordinary method
        // call leaves decl null and falls through to normal dispatch.
        if (const ClassType* cls = receiverClass(at->object.get())) {
            probeCls = cls; probeMethod = at->attribute;
            if (FunctionDecl* m = findGenericMethod(cls, at->attribute, owningClass)) {
                decl = m; fnName = at->attribute; methodAttr = at;
            }
        }
    }
    if (!decl) {
        // Re-visit of an already-stamped generic call: the callee was retargeted
        // to its stamped name (e.g. `take[int]`, which contains '[' - impossible
        // in a user-defined name) on an earlier type-check pass. A re-check pass
        // over the shared, already-retargeted AST (the entry module is checked by
        // more than one TypeChecker instance) must NOT fall through to the
        // normal-call path, which would clobber the resolved return type to
        // <unknown> - breaking an annotation-less use like `print(take([..], 2))`
        // that relies on the call's own type. Restore from this instance's record
        // if present; otherwise preserve the type the earlier pass set on the node.
        if (auto* nm = dynamic_cast<NameExpr*>(node.callee.get())) {
            if (nm->name.find('[') != std::string::npos) {
                auto it = impl_->stampedCallReturnType.find(nm->name);
                if (it != impl_->stampedCallReturnType.end()) node.type = it->second;
                return true;
            }
        }
        // Same re-visit case for a stamped generic METHOD: retargeting renamed
        // the AttributeExpr's attribute to the stamped name (`cast[int]` - '['
        // is impossible in a user-defined name) and collapsed the wrapping
        // SubscriptExpr. A second inference over the same node - a bare
        // reassignment (`x = r.cast[int](42)`) re-infers its RHS, and re-check
        // passes walk the shared AST - must NOT fall through to normal
        // attribute dispatch: that reported "type 'Repo' has no attribute
        // 'cast[int]'" while the const-declaration form compiled. Restore the
        // recorded return type (declaring-class key first, walking the MRO for
        // inherited stamps, then the bare name) or keep the type the first
        // pass already set on the node
        if (auto* at2 = dynamic_cast<AttributeExpr*>(node.callee.get())) {
            if (at2->attribute.find('[') != std::string::npos) {
                if (const ClassType* cls = receiverClass(at2->object.get())) {
                    const ClassType* c = cls;
                    for (int guard = 0; c && guard < 256; ++guard) {
                        auto it = impl_->stampedCallReturnType.find(
                            c->name + "." + at2->attribute);
                        if (it != impl_->stampedCallReturnType.end()) {
                            node.type = it->second;
                            return true;
                        }
                        c = c->parentClass &&
                            c->parentClass->kind() == Type::Kind::Class
                                ? static_cast<const ClassType*>(c->parentClass.get())
                                : nullptr;
                    }
                }
                auto bit = impl_->stampedCallReturnType.find(at2->attribute);
                if (bit != impl_->stampedCallReturnType.end())
                    node.type = bit->second;
                return true;
            }
        }
        return false;  // not a generic call - normal dispatch handles it
    }

    const bool isMethodCall = !owningClass.empty();
    const char* kindWord = isMethodCall ? "method" : "function";

    // The generic FunctionType (TypeVar params/return). A free function's is
    // defined by the pre-pass; a method's is built on demand here (order-
    // independent - robust to a call site that precedes the method's declaration)
    // by resolving its signature with the type-param frame temporarily bound.
    std::shared_ptr<FunctionType> genericFt;
    if (!isMethodCall) {
        genericFt = std::dynamic_pointer_cast<FunctionType>(impl_->lookup(fnName));
    } else {
        // Double monomorphization: when the receiver is a stamped generic-class
        // instantiation (`Container[int]`), push the CLASS type-param frame
        // (concrete: `T -> int`, recovered from the receiver's genericArgs) as the
        // OUTER scope, so a method signature referencing the class's `T` resolves
        // to the receiver's concrete arg. The method's own params (`U`) are bound
        // abstractly in the inner frame below; lookupTypeParam searches inner->outer
        // so a method `U` shadows a class `U` if names ever collide. No-op for a
        // generic method on a non-generic class (the existing single-mono path).
        bool pushedClassFrame = false;
        if (probeCls && !probeCls->genericOrigin.empty()) {
            if (auto gcIt = impl_->genericClasses.find(probeCls->genericOrigin);
                gcIt != impl_->genericClasses.end()) {
                std::unordered_map<std::string, std::shared_ptr<Type>> classFrame;
                auto& ctps = gcIt->second->typeParams;
                for (size_t i = 0;
                     i < ctps.size() && i < probeCls->genericArgs.size(); ++i)
                    classFrame[ctps[i].name] = probeCls->genericArgs[i];
                impl_->typeParamScopes.push_back(std::move(classFrame));
                pushedClassFrame = true;
            }
        }
        std::unordered_map<std::string, std::shared_ptr<Type>> frame;
        for (auto& tp : decl->typeParams) {
            // Bounds - carry the generic method's own type-param bounds (`m[U: B]`)
            // so the body can resolve members/operators on `U` against `B`.
            std::shared_ptr<Type> bnd =
                tp.bound ? resolveType(tp.bound.get()) : nullptr;
            frame[tp.name] = std::make_shared<TypeVarType>(tp.name, bnd);
        }
        impl_->typeParamScopes.push_back(std::move(frame));
        std::vector<std::shared_ptr<Type>> pts;
        for (size_t i = 0; i < decl->params.size(); ++i) {
            if (decl->isMethod && !decl->hasImplicitSelf && decl->params[i].name == "self")
                continue;  // .py-mode explicit self is not part of the signature
            pts.push_back(resolveType(decl->params[i].type.get()));
        }
        auto rt = resolveType(decl->returnType.get());
        genericFt = std::make_shared<FunctionType>(std::move(pts), rt);
        impl_->typeParamScopes.pop_back();
        if (pushedClassFrame) impl_->typeParamScopes.pop_back();
    }

    std::unordered_map<std::string, std::shared_ptr<Type>> bindings;
    if (!explicitArgs.empty()) {
        if (explicitArgs.size() != decl->typeParams.size()) {
            error(node.location(), std::string("generic ") + kindWord + " '" + fnName +
                  "' expects " + std::to_string(decl->typeParams.size()) +
                  " type argument(s), got " + std::to_string(explicitArgs.size()));
            return true;
        }
        for (size_t i = 0; i < decl->typeParams.size(); ++i) {
            auto t = explicitArgs[i] ? resolveType(explicitArgs[i].get()) : impl_->unknownType;
            bindings[decl->typeParams[i].name] = t;
        }
    } else if (genericFt) {
        // Infer from argument types vs declared (TypeVar-containing) params.
        for (size_t i = 0; i < genericFt->paramTypes.size() && i < argTypes.size(); ++i)
            unifyTypeParam(genericFt->paramTypes[i], argTypes[i], bindings);
    }

    // Bounds - enforce bounds on the (explicit or inferred) type arguments of a
    // generic function/method, mirroring the generic-class check above. A bound
    // `T: B` requires the resolved argument to be `B` or a subclass.
    bool boundViolated = false;
    for (auto& tp : decl->typeParams) {
        if (!tp.bound) continue;
        auto bit = bindings.find(tp.name);
        if (bit == bindings.end() || !bit->second) continue;
        auto arg = bit->second;
        if (arg->kind() == Type::Kind::TypeVar) continue;  // still abstract
        auto boundType = resolveType(tp.bound.get());
        if (boundType && boundType->kind() != Type::Kind::Unknown &&
            !arg->isSubtypeOf(*boundType)) {
            error(node.location(), "type argument '" + arg->toString() +
                  "' does not satisfy bound '" + boundType->toString() +
                  "' of type parameter '" + tp.name + "' in generic " +
                  kindWord + " '" + fnName + "'");
            boundViolated = true;
        }
    }
    // Bound already reported - resolve the call to an error type without stamping
    // the offending instantiation (its body would cascade-fail on the bad type).
    if (boundViolated) { node.type = impl_->unknownType; return true; }

    // Validate concrete (non-type-variable) parameters against the actual
    // argument types. The generic path otherwise consults args only for TypeVar
    // inference (above) and never type-checks them, so a `str` passed where a
    // `sql: SQL` parameter is declared - `db.all("SELECT 1")` - silently reached
    // codegen and failed there with no diagnostic. Mirror the conservative
    // scalar/cross-category check from the non-generic call path; skip any param
    // still carrying a type variable (it is fixed by inference, not the arg).
    if (genericFt) {
        auto isContainer = [](Type::Kind k) {
            return k == Type::Kind::List || k == Type::Kind::Dict ||
                   k == Type::Kind::Tuple || k == Type::Kind::Task;
        };
        for (size_t i = 0;
             i < genericFt->paramTypes.size() && i < argTypes.size(); ++i) {
            const auto& pt = genericFt->paramTypes[i];
            const auto& aT = argTypes[i];
            if (!pt || !aT) continue;
            if (!typeIsConcrete(pt.get())) continue;  // generic param: inferred
            auto pk = pt->kind(), ak = aT->kind();
            if (pk == Type::Kind::Unknown || pk == Type::Kind::Any ||
                ak == Type::Kind::Unknown || ak == Type::Kind::Any ||
                ak == Type::Kind::None_ || ak == Type::Kind::Union ||
                pk == Type::Kind::Union)
                continue;
            if (isContainer(ak) && ak == pk) continue;  // container invariance
            if (!aT->isSubtypeOf(*pt)) {
                error(node.location(), "argument " + std::to_string(i + 1) +
                      " of type '" + aT->toString() + "' is not assignable to "
                      "parameter type '" + pt->toString() + "'");
            }
        }
    }

    // D049 optional-[T] - if a type parameter is STILL unbound (no explicit [T],
    // not pinned by an argument), infer it from the call's EXPECTED type by
    // unifying the return type against the binding annotation. This is what makes
    // a bracket-less `c: Customer = db.one(sql)` bind T=Customer / `rows:
    // list[Customer] = db.all(sql)` bind T=Customer. Dragon bindings are always
    // annotated, so the contextful path is the norm; a genuinely context-free
    // call still errors below (clean "supply it explicitly").
    if (explicitArgs.empty() && genericFt && expected &&
        expected->kind() != Type::Kind::Unknown) {
        bool anyUnbound = false;
        for (auto& tp : decl->typeParams)
            if (bindings.find(tp.name) == bindings.end()) { anyUnbound = true; break; }
        if (anyUnbound)
            unifyTypeParam(genericFt->returnType, expected, bindings);
    }

    // Assemble the ordered argument list; any unsolved parameter is an error.
    std::vector<std::shared_ptr<Type>> args;
    for (auto& tp : decl->typeParams) {
        auto it = bindings.find(tp.name);
        if (it == bindings.end() || !it->second ||
            it->second->kind() == Type::Kind::Unknown) {
            error(node.location(), "cannot infer type parameter '" + tp.name +
                  "' for generic " + kindWord + " '" + fnName +
                  "'; supply it explicitly, e.g. " + fnName + "[...](...)");
            return true;
        }
        args.push_back(it->second);
    }

    // The stamped decl's NAME is the type-arg-only form (`all[int]`); for a method
    // the owning ClassDecl already scopes it, so the codegen symbol is
    // `<mod>__Class_all[int]`. The worklist/dedup KEY additionally carries the
    // class (`Class.all[int]`) so two classes' `all[T]` never collide in instDone.
    std::string stampedName = mangleInstantiation(fnName, args);
    std::string key = isMethodCall
                          ? mangleInstantiation(owningClass + "." + fnName, args)
                          : stampedName;
    // Only enqueue concrete instantiations (a `first[T](...)` call inside another
    // generic body defers to the enclosing instantiation, like classes do).
    if (argsAreConcrete(args) && !impl_->instDone.count(key)) {
        bool pending = false;
        for (auto& r : impl_->pendingInsts) if (r.key == key) { pending = true; break; }
        if (!pending)
            impl_->pendingInsts.push_back({key, fnName, /*isClass=*/false, args, owningClass});
    }

    // Retarget the callee to the stamped specialization - only when concrete.
    // Inside a generic body a non-concrete call (`g[T](...)`) keeps its original
    // callee so the clone can substitute `T` and retarget at stamp time.
    if (argsAreConcrete(args)) {
        if (isMethodCall) {
            // Rename the method to its stamped name; emitMethodCall reads
            // attr.attribute verbatim, so dispatch (incl. D026 vtable/MRO) is
            // unchanged. For the explicit `[T]` form the callee is the SubscriptExpr
            // wrapping the AttributeExpr - collapse it to the bare AttributeExpr so
            // CodeGen lowers an ordinary method call.
            methodAttr->attribute = stampedName;
            if (auto* sub = dynamic_cast<SubscriptExpr*>(node.callee.get()))
                node.callee = std::move(sub->object);
        } else {
            auto newCallee = std::make_unique<NameExpr>();
            newCallee->name = stampedName;
            newCallee->setLocation(node.callee->location());
            node.callee = std::move(newCallee);
        }
    }

    // Result type = the generic return type with type parameters substituted.
    node.type = genericFt ? substituteType(genericFt->returnType, bindings)
                          : impl_->unknownType;
    // Remember the result type under the stamped callee name so a later re-visit
    // of this expr (after retargeting) can restore it (see the !decl path above).
    // Methods key on the declaring/stamped-scope class too ("Repo.cast[int]") so
    // same-named stamps on different classes cannot collide.
    if (argsAreConcrete(args) && node.type) {
        if (isMethodCall)
            impl_->stampedCallReturnType[owningClass + "." + stampedName] = node.type;
        else
            impl_->stampedCallReturnType[stampedName] = node.type;
    }
    return true;
}

bool TypeChecker::tryInstantiateGenericConstruction(
    CallExpr& node, const std::shared_ptr<Type>& expected) {
    ClassDecl* decl = nullptr;
    std::string clsName;
    std::vector<std::shared_ptr<Type>> args;

    if (auto* sub = dynamic_cast<SubscriptExpr*>(node.callee.get())) {
        // Explicit: `Box[int](5)` / `Pair[int, str](...)`.
        if (auto* nm = dynamic_cast<NameExpr*>(sub->object.get())) {
            auto it = impl_->genericClasses.find(nm->name);
            if (it != impl_->genericClasses.end()) {
                decl = it->second;
                clsName = nm->name;
                std::vector<const Expr*> idxs;
                if (auto* tup = dynamic_cast<TupleExpr*>(sub->index.get())) {
                    for (auto& el : tup->elements) idxs.push_back(el.get());
                } else {
                    idxs.push_back(sub->index.get());
                }
                for (auto* ix : idxs) {
                    auto te = exprToTypeExpr(ix);
                    args.push_back(te ? resolveType(te.get()) : impl_->unknownType);
                }
            }
        }
    } else if (auto* nm = dynamic_cast<NameExpr*>(node.callee.get())) {
        // Inferred from the binding annotation: `b: Box[int] = Box(5)`.
        auto it = impl_->genericClasses.find(nm->name);
        if (it != impl_->genericClasses.end() && expected) {
            auto exInst = std::dynamic_pointer_cast<InstanceType>(expected);
            if (exInst && exInst->classType &&
                exInst->classType->genericOrigin == nm->name) {
                decl = it->second;
                clsName = nm->name;
                args = exInst->classType->genericArgs;
            } else {
                // The class is generic but no concrete instantiation is pinned -
                // a bare `Box(5)` with no (matching) annotation can't be lowered.
                error(node.location(),
                      "cannot infer type arguments for generic class '" + nm->name +
                      "'; annotate the binding (e.g. `x: " + nm->name +
                      "[int] = ...`) or instantiate explicitly (`" + nm->name +
                      "[int](...)`)");
                node.type = impl_->unknownType;
                return true;
            }
        }
    }
    if (!decl) return false;

    auto instType = instantiateGenericClass(decl, args, node.location());
    // Retarget to the stamped class name so CodeGen constructs the
    // specialization - only when concrete (a non-concrete `Inner[T](...)` inside
    // a generic body keeps its callee for the clone to substitute + retarget).
    if (argsAreConcrete(args)) {
        std::string key = mangleInstantiation(clsName, args);
        auto newCallee = std::make_unique<NameExpr>();
        newCallee->name = key;
        newCallee->setLocation(node.callee->location());
        node.callee = std::move(newCallee);
    }
    node.type = instType;
    return true;
}

//===----------------------------------------------------------------------===//
// Generic pre-pass: register + fully check every top-level generic template.
//===----------------------------------------------------------------------===//

void TypeChecker::collectGenericTemplates(Module& module) {
    impl_->currentModule = &module;
    for (auto& stmt : module.body) {
        if (auto* cd = dynamic_cast<ClassDecl*>(stmt.get())) {
            // Record EVERY class (generic or not) so the monomorphizer can append
            // a stamped generic method into its body, and scan for generic methods
            // (own-type-param methods) regardless of whether the class is generic -
            // a generic method can live on an ordinary class (the db.all[T] case).
            impl_->classDeclByName[cd->name] = cd;
            for (auto& m : cd->body) {
                auto* fd = dynamic_cast<FunctionDecl*>(m.get());
                if (!fd || fd->typeParams.empty()) continue;
                // D049 - no overloading-by-genericity: a generic method may not
                // share its name with any other method on the class. Optional-[T]
                // is ONE generic method; a bracket-less `obj.m(...)` infers T from
                // the binding annotation (Dragon mandates it), it does NOT select a
                // separate non-generic definition. Allowing both is ambiguous and
                // was the dual-definition footgun - reject it up front.
                for (auto& other : cd->body) {
                    if (other.get() == m.get()) continue;
                    auto* od = dynamic_cast<FunctionDecl*>(other.get());
                    if (od && od->name == fd->name) {
                        error(fd->location(), "method '" + fd->name + "' on class '" +
                              cd->name + "' is declared more than once; a generic "
                              "method 'def " + fd->name + "[T](...)' cannot be "
                              "overloaded by another '" + fd->name + "' definition. "
                              "Use a single generic method and let the call site "
                              "infer T (e.g. `x: Customer = obj." + fd->name + "(...)`).");
                        break;
                    }
                }
                // A method type parameter that shadows one of the generic
                // class's own type parameters makes the double-monomorphization
                // substitution ambiguous (one name, two bindings - T from the
                // class instantiation and T from the method call). Reject it;
                // the user renames the method's parameter.
                if (!cd->typeParams.empty())
                    for (auto& mtp : fd->typeParams)
                        for (auto& ctp : cd->typeParams)
                            if (mtp.name == ctp.name)
                                error(fd->location(), "method type parameter '" +
                                      mtp.name + "' on generic class '" + cd->name +
                                      "' shadows the class's type parameter '" +
                                      ctp.name + "'; rename the method's parameter");
                impl_->genericMethods[cd->name + "." + fd->name] = fd;
            }
            if (!cd->typeParams.empty())
                impl_->genericClasses[cd->name] = cd;
        } else if (auto* fd = dynamic_cast<FunctionDecl*>(stmt.get())) {
            if (fd->typeParams.empty()) continue;
            impl_->genericFunctions[fd->name] = fd;
        }
    }
    // Visit each template once (type params bound to TypeVarType inside the
    // visitors), populating its ClassType/FunctionType and abstractly checking
    // its body. Marked checked so the main module walk skips it.
    for (auto& stmt : module.body) {
        auto* cd = dynamic_cast<ClassDecl*>(stmt.get());
        auto* fd = dynamic_cast<FunctionDecl*>(stmt.get());
        bool generic = (cd && !cd->typeParams.empty()) || (fd && !fd->typeParams.empty());
        if (!generic) continue;
        stmt->accept(*this);
        impl_->genericChecked.insert(stmt.get());
    }
}

//===----------------------------------------------------------------------===//
// Cross-module generics: register an imported module's generic templates into
// this checker's instantiation registries (no visiting - the home module's
// checker already abstractly checked them; here we only need the decl pointers
// + the "this name is generic" markers so a use site can stamp them).
//===----------------------------------------------------------------------===//

void TypeChecker::registerExternalGenerics(Module& mod) {
    for (auto& stmt : mod.body) {
        if (auto* cd = dynamic_cast<ClassDecl*>(stmt.get())) {
            // Record the (imported) class so a stamped generic method can be
            // appended to its body, and surface its generic methods. A generic
            // method can live on an ordinary imported class (the db.all[T] case).
            impl_->classDeclByName.emplace(cd->name, cd);
            for (auto& m : cd->body)
                if (auto* fd = dynamic_cast<FunctionDecl*>(m.get()))
                    if (!fd->typeParams.empty()) {
                        impl_->genericMethods.emplace(cd->name + "." + fd->name, fd);
                        impl_->genericTemplateModule.emplace(fd, mod.moduleName);
                    }
            if (!cd->typeParams.empty()) {
                impl_->genericClasses.emplace(cd->name, cd);
                impl_->genericTemplateModule.emplace(cd, mod.moduleName);
            }
        } else if (auto* fd = dynamic_cast<FunctionDecl*>(stmt.get())) {
            if (!fd->typeParams.empty()) {
                impl_->genericFunctions.emplace(fd->name, fd);
                impl_->genericTemplateModule.emplace(fd, mod.moduleName);
            }
        }
    }
}

//===----------------------------------------------------------------------===//
// Worklist: stamp the transitive closure of instantiations to a fixpoint.
//===----------------------------------------------------------------------===//

void TypeChecker::runMonomorphization() {
    if (!impl_->currentModule) return;
    while (!impl_->pendingInsts.empty()) {
        if (impl_->genericsAborted) { impl_->pendingInsts.clear(); return; }
        auto req = impl_->pendingInsts.front();
        impl_->pendingInsts.erase(impl_->pendingInsts.begin());
        if (impl_->instDone.count(req.key)) continue;
        impl_->instDone.insert(req.key);
        // (The polymorphic-recursion cap lives in instantiateGenericClass, the
        // single choke point through which every class instantiation - including
        // transitive ones produced by substituteType - passes. If it tripped, it
        // stops enqueueing new work, so this loop drains and terminates.)

        // Build the AST substitution (type-param name -> concrete TypeExpr).
        // The TypeExprs are owned here for the lifetime of the clone call.
        const bool isMethodReq = !req.owningClass.empty();
        std::vector<std::unique_ptr<TypeExpr>> owned;
        TypeSubst subst;
        Stmt* template_ = nullptr;
        const std::vector<TypeParam>* tps = nullptr;
        if (req.isClass) {
            auto it = impl_->genericClasses.find(req.genericName);
            if (it == impl_->genericClasses.end()) continue;
            template_ = it->second; tps = &it->second->typeParams;
        } else if (isMethodReq) {
            // The owning class may be a stamped generic-class instantiation
            // (`Container[int]`): its generic method template is registered under
            // the ORIGIN name (`Container.wrap`) and still carries the class's
            // `T`. Resolve via the origin and seed the subst with the CLASS frame
            // (T -> the instantiation's concrete genericArgs) so cloning
            // substitutes BOTH T and U in one pass. A method on a non-generic
            // class hits the direct lookup and skips this (single mono, unchanged).
            std::shared_ptr<ClassType> ownerCT;
            if (auto tnIt = impl_->typeNames.find(req.owningClass);
                tnIt != impl_->typeNames.end())
                if (auto inst = std::dynamic_pointer_cast<InstanceType>(tnIt->second))
                    ownerCT = inst->classType;
            auto it = impl_->genericMethods.find(req.owningClass + "." + req.genericName);
            if (it == impl_->genericMethods.end() && ownerCT &&
                !ownerCT->genericOrigin.empty())
                it = impl_->genericMethods.find(
                    ownerCT->genericOrigin + "." + req.genericName);
            if (it == impl_->genericMethods.end()) continue;
            template_ = it->second; tps = &it->second->typeParams;
            if (ownerCT && !ownerCT->genericOrigin.empty()) {
                if (auto gcIt = impl_->genericClasses.find(ownerCT->genericOrigin);
                    gcIt != impl_->genericClasses.end()) {
                    auto& ctps = gcIt->second->typeParams;
                    for (size_t i = 0;
                         i < ctps.size() && i < ownerCT->genericArgs.size(); ++i) {
                        owned.push_back(typeToTypeExpr(ownerCT->genericArgs[i]));
                        subst[ctps[i].name] = owned.back().get();
                    }
                }
            }
        } else {
            auto it = impl_->genericFunctions.find(req.genericName);
            if (it == impl_->genericFunctions.end()) continue;
            template_ = it->second; tps = &it->second->typeParams;
        }
        for (size_t i = 0; i < tps->size() && i < req.args.size(); ++i) {
            owned.push_back(typeToTypeExpr(req.args[i]));
            subst[(*tps)[i].name] = owned.back().get();
        }

        // Polymorphic-recursion caps for method/function stamps (class stamps
        // are bounded in instantiateGenericClass, which the iterative worklist
        // path bypasses). A generic method/function that instantiates a strictly
        // deeper version of itself - `go[U]` -> `go[list[U]]` -> ... - produces
        // ever-distinct keys. The DEPTH cap (type-arg nesting) trips it fast and
        // early; the breadth cap is the backstop for non-recursive blow-ups.
        // D044: always a clean compile error, never a hang.
        if (!req.isClass) {
            auto abortGen = [&](const std::string& why) {
                if (!impl_->instCapReported) {
                    error(template_->location(),
                          "generic " +
                          std::string(req.owningClass.empty() ? "function" : "method") +
                          " '" + req.genericName + "' " + why + " - a definition that "
                          "instantiates a strictly deeper version of itself "
                          "recurses without limit");
                    impl_->instCapReported = true;
                }
                impl_->genericsAborted = true;
                impl_->pendingInsts.clear();
            };
            for (auto& a : req.args)
                if (typeNestingDepth(a.get()) > Impl::kMaxInstDepth) {
                    abortGen("nests its type arguments too deeply");
                    return;
                }
            if (++impl_->instantiationCount > Impl::kMaxInstantiations) {
                abortGen("has too many distinct instantiations");
                return;
            }
        }

        // D044 cross-module generics: the template was authored in this module,
        // so its body's bare names (sibling functions, module globals) resolve
        // there - not at the instantiation site. Record it so CodeGen emits the
        // stamped body under the defining module's resolution scope.
        std::string homeModule;
        if (auto modIt = impl_->genericTemplateModule.find(template_);
            modIt != impl_->genericTemplateModule.end())
            homeModule = modIt->second;

        auto cloned = cloneStmt(template_, subst);
        if (!cloned) continue;
        if (auto* cc = dynamic_cast<ClassDecl*>(cloned.get())) {
            cc->name = req.key;
            cc->typeParams.clear();  // a stamped instantiation is not a template
            cc->genericHomeModule = homeModule;
            // Register so a later generic-method stamp on this instantiation
            // (`Container[int].wrap[str]`) can append into this class body. The
            // raw pointer stays valid after the std::move into the module body.
            impl_->classDeclByName[req.key] = cc;
        } else if (auto* cf = dynamic_cast<FunctionDecl*>(cloned.get())) {
            // A method's on-disk name is the type-arg-only form (`all[int]`) - the
            // owning ClassDecl scopes it; a free function's is the full key.
            cf->name = isMethodReq ? mangleInstantiation(req.genericName, req.args) : req.key;
            cf->typeParams.clear();
            cf->genericHomeModule = homeModule;
        }

        // Cross-module re-check scope: the stamped body was authored in the
        // template's DEFINING module and may reference that module's sibling
        // class types by bare name (e.g. a generic method delegating to its
        // return-type class). Those types live in the instantiating checker only
        // as qualified module exports, so inject them into `typeNames` for the
        // re-check below (saving/restoring), mirroring import-time registration.
        std::vector<std::pair<std::string, std::shared_ptr<Type>>> savedTypeNames;
        std::vector<std::string> addedTypeNames;
        if (auto modIt = impl_->genericTemplateModule.find(template_);
            modIt != impl_->genericTemplateModule.end()) {
            if (auto mtIt = impl_->moduleTypes.find(modIt->second);
                mtIt != impl_->moduleTypes.end()) {
                for (auto& [ename, etype] : mtIt->second->exports) {
                    if (!etype || etype->kind() != Type::Kind::Class) continue;
                    if (auto ex = impl_->typeNames.find(ename); ex != impl_->typeNames.end())
                        savedTypeNames.push_back({ename, ex->second});
                    else
                        addedTypeNames.push_back(ename);
                    impl_->typeNames[ename] = std::make_shared<InstanceType>(
                        std::static_pointer_cast<ClassType>(etype));
                }
            }
        }

        if (isMethodReq) {
            // Append the stamp into its owning class body (CodeGen's per-class
            // method loops then emit `<mod>__Class_<name>`), and re-type-check it
            // with `self`/currentClass bound - the method analog of the
            // free-function re-check below. Without the self binding the stamped
            // body's `self.X` and implicit self param would be untyped.
            auto cdIt = impl_->classDeclByName.find(req.owningClass);
            if (cdIt == impl_->classDeclByName.end()) continue;
            auto* stampedFn = static_cast<FunctionDecl*>(cloned.get());
            cdIt->second->body.push_back(std::move(cloned));

            std::shared_ptr<ClassType> ownerCT;
            auto tnIt = impl_->typeNames.find(req.owningClass);
            if (tnIt != impl_->typeNames.end())
                if (auto inst = std::dynamic_pointer_cast<InstanceType>(tnIt->second))
                    ownerCT = inst->classType;
            impl_->pushScope();
            const ClassType* prevClass = impl_->currentClass;
            if (ownerCT) {
                impl_->define("self", std::make_shared<InstanceType>(ownerCT));
                impl_->currentClass = ownerCT.get();
            }
            stampedFn->accept(*this);
            if (ownerCT)
                if (auto ft = std::dynamic_pointer_cast<FunctionType>(
                        impl_->lookup(stampedFn->name)))
                    ownerCT->methods[stampedFn->name] = ft;
            impl_->currentClass = prevClass;
            impl_->popScope();
        } else {
            // Append to the module body so CodeGen emits it, then type-check it
            // (populates ClassType.fields/methods + expr->type; may enqueue more).
            Stmt* stamped = cloned.get();
            impl_->currentModule->body.push_back(std::move(cloned));
            stamped->accept(*this);
        }

        // Restore the type-name scope mutated for the cross-module re-check.
        for (auto& [n, t] : savedTypeNames) impl_->typeNames[n] = t;
        for (auto& n : addedTypeNames) impl_->typeNames.erase(n);
    }
}

}  // namespace dragon
