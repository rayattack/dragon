// D044 generics engine: stamp a native monomorphic copy of each generic per
// concrete type-arg, worklist to fixpoint. Integration hooks in TypeChecker.cpp.

#include "dragon/TypeChecker.h"
#include "TypeCheckerImpl.h"
#include "dragon/AstClone.h"

#include <functional>

namespace dragon {

namespace {

// Concrete (stampable) iff no free type var. A non-concrete arg (Inner[T] inside
// Outer[T]) is produced transitively when Outer[int] stamps, so it isn't enqueued.
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

// Structural nesting depth (list[list[int]] -> 3). Bounds polymorphic recursion
// (go[U] -> go[list[U]] -> ...) that drains iteratively past the class depth cap.
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
    // A `|` in value-subscript position (Box[int | str](...)) parses as a BinaryExpr;
    // flatten to a UnionTypeExpr so it matches the annotation form's UnionType.
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

// Type-parameter scope lookup

std::shared_ptr<Type> TypeChecker::lookupTypeParam(const std::string& name) {
    for (auto it = impl_->typeParamScopes.rbegin(); it != impl_->typeParamScopes.rend(); ++it) {
        auto found = it->find(name);
        if (found != it->end()) return found->second;
    }
    return nullptr;
}

// Canonical instantiation name (cache key / stamped decl / LLVM symbol). Brackets
// can't occur in a user identifier, so it never collides with hand-written names.

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

// Type substitution: deep-copy a Type, replacing TypeVarType by name.

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

// Type -> TypeExpr (drives AST substitution when stamping).
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
            // A callable type arg (Box[Callable[[int],int]]): rebuild the Callable
            // annotation so the field keeps its signature instead of degrading to Any.
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

// Unification: solve type parameters from a concrete actual type.

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

// Generic-class instantiation: build (once) the monomorphic placeholder type
// and record a stamping request.

std::shared_ptr<Type> TypeChecker::instantiateGenericClass(
    ClassDecl* decl, std::vector<std::shared_ptr<Type>> args, const SourceLocation& loc) {
    // Once any cap has tripped, every further instantiation is a no-op so the
    // partially-built program drains immediately instead of doing 4096× work.
    if (impl_->genericsAborted) return impl_->unknownType;

    // A transitive re-instantiation carries no use-site location; inherit the
    // enclosing one so diagnostics never report at 0:0.
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

    // Polymorphic-recursion guard: Foo[T] -> Foo[list[T]] -> ... defeats dedup.
    // Depth + breadth caps trip as a clean compile error, never a hang (D044).
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
        // Bounded T: the concrete arg must be the bound class or a subclass. A
        // still-abstract arg (TypeVar) is skipped; re-checked when it stamps concretely.
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
    // Register BEFORE substituting members so a self-referential generic (Node[T]
    // with next: Node[T]) resolves to this placeholder instead of recursing forever.
    auto phInst = std::make_shared<InstanceType>(ph);
    impl_->typeNames[key] = phInst;
    impl_->define(key, ph);

    // Enqueue only when fully concrete (a free Inner[T] is stamped transitively
    // later) and no bound was violated (else a confusing error cascade).
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

// Generic-function call: instantiate, retarget the callee, set the result type.

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
    // Find generic method `m` on `cls`, walking the MRO so an inherited one resolves
    // to its declaring class (whose body the stamp is appended to).
    auto findGenericMethod = [&](const ClassType* cls, const std::string& m,
                                 std::string& declClass) -> FunctionDecl* {
        const ClassType* c = cls;
        for (int guard = 0; c && guard < 256; ++guard) {
            auto it = impl_->genericMethods.find(c->name + "." + m);
            if (it != impl_->genericMethods.end()) { declClass = c->name; return it->second; }
            // A stamped generic class (Container[int]) keeps its method template under
            // the origin name; resolve there but key declClass to the stamped name.
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
        if (it != impl_->genericFunctions.end()) {
            // genericFunctions is name-keyed and program-global, so a stdlib
            // generic (e.g. json.decode[T]) must not hijack a same-named function
            // a package actually imported. If the name is bound in THIS scope to a
            // concrete (non-generic) function, that binding wins - defer to normal
            // dispatch. Only take the global generic when the in-scope binding is
            // itself generic, or the name isn't otherwise bound here.
            bool concreteInScope = false;
            if (auto inScope = impl_->lookup(nm->name))
                if (auto ft = std::dynamic_pointer_cast<FunctionType>(inScope)) {
                    concreteInScope = typeIsConcrete(ft->returnType.get());
                    for (auto& pt : ft->paramTypes)
                        if (!typeIsConcrete(pt.get())) concreteInScope = false;
                }
            if (!concreteInScope) { decl = it->second; fnName = nm->name; }
        }
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
            // Generic method with explicit args: recv.method[T](args). Resolve the
            // receiver's class and look the method up by class+name.
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
        // Generic method, args inferred: recv.method(args). Non-generic methods
        // leave decl null and fall through to normal dispatch.
        if (const ClassType* cls = receiverClass(at->object.get())) {
            probeCls = cls; probeMethod = at->attribute;
            if (FunctionDecl* m = findGenericMethod(cls, at->attribute, owningClass)) {
                decl = m; fnName = at->attribute; methodAttr = at;
            }
        }
    }
    if (!decl) {
        // Re-visit of an already-stamped call (callee renamed to `take[int]`, which
        // contains '['): restore the recorded return type instead of clobbering it.
        if (auto* nm = dynamic_cast<NameExpr*>(node.callee.get())) {
            if (nm->name.find('[') != std::string::npos) {
                auto it = impl_->stampedCallReturnType.find(nm->name);
                if (it != impl_->stampedCallReturnType.end()) node.type = it->second;
                return true;
            }
        }
        // Same re-visit for a stamped method (attribute renamed to `cast[int]`):
        // restore the recorded return type, keying on the declaring class then bare name.
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

    // The generic FunctionType (TypeVar params/return). A free function's comes from
    // the pre-pass; a method's is built on demand with its type-param frame bound.
    std::shared_ptr<FunctionType> genericFt;
    if (!isMethodCall) {
        genericFt = std::dynamic_pointer_cast<FunctionType>(impl_->lookup(fnName));
    } else {
        // Double monomorphization: on a stamped receiver (Container[int]), push the
        // class frame (T->int) so the method signature's class `T` resolves concretely.
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

    // Enforce bounds on the type args (T: B requires B or a subclass), mirroring
    // the generic-class check above.
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

    // Type-check concrete (non-TypeVar) params against the actual args - otherwise a
    // str passed to a `sql: SQL` param reached codegen with no diagnostic.
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

    // D049: a still-unbound T is inferred from the expected type (the binding
    // annotation), so bracket-less `c: Customer = db.one(sql)` binds T=Customer.
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

    // Stamped name is the type-arg form (all[int]); the dedup KEY additionally carries
    // the class (Class.all[int]) so two classes' all[T] never collide.
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

    // Retarget the callee to the stamped specialization, only when concrete (a
    // non-concrete g[T](...) inside a generic body retargets at stamp time).
    if (argsAreConcrete(args)) {
        if (isMethodCall) {
            // Rename the method to its stamped name (dispatch unchanged); collapse the
            // explicit-[T] SubscriptExpr to the bare AttributeExpr for CodeGen.
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
    // Record the result type under the stamped name so a later re-visit restores it
    // (methods key on the class too, so same-named stamps don't collide).
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
    // Retarget to the stamped class name (only when concrete) so CodeGen constructs
    // the specialization; a non-concrete Inner[T](...) retargets at stamp time.
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

// Generic pre-pass: register + fully check every top-level generic template.

void TypeChecker::collectGenericTemplates(Module& module) {
    impl_->currentModule = &module;
    for (auto& stmt : module.body) {
        if (auto* cd = dynamic_cast<ClassDecl*>(stmt.get())) {
            // Record EVERY class so a stamped generic method can be appended, and scan
            // for generic methods (they live on ordinary classes too - db.all[T]).
            impl_->classDeclByName[cd->name] = cd;
            for (auto& m : cd->body) {
                auto* fd = dynamic_cast<FunctionDecl*>(m.get());
                if (!fd || fd->typeParams.empty()) continue;
                // D049: a generic method may not share its name with another method
                // (a bracket-less obj.m(...) infers T; a rival definition is ambiguous).
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
                // A method type-param that shadows the class's own is ambiguous under
                // double monomorphization (one name, two bindings); reject it.
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
            if (fd->name == "decode" && module.moduleName == "json")
                impl_->schemaDecodeFns.insert(fd);
        }
    }
    // Visit each template once (type params bound to TypeVar) to populate its type
    // and abstractly check its body; marked checked so the main walk skips it.
    for (auto& stmt : module.body) {
        auto* cd = dynamic_cast<ClassDecl*>(stmt.get());
        auto* fd = dynamic_cast<FunctionDecl*>(stmt.get());
        bool generic = (cd && !cd->typeParams.empty()) || (fd && !fd->typeParams.empty());
        if (!generic) continue;
        stmt->accept(*this);
        impl_->genericChecked.insert(stmt.get());
    }
}

// Register an imported module's generic templates so a use site can stamp them
// (no visiting - the home module's checker already abstractly checked them).

void TypeChecker::registerExternalGenerics(Module& mod) {
    for (auto& stmt : mod.body) {
        if (auto* cd = dynamic_cast<ClassDecl*>(stmt.get())) {
            // Record the imported class so a stamped generic method can be appended,
            // and surface its generic methods (they live on ordinary classes - db.all[T]).
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
                if (fd->name == "decode" && mod.moduleName == "json")
                    impl_->schemaDecodeFns.insert(fd);
            }
        }
    }
    // D052 - record imported class bindings so a stamped generic body can name an
    // imported type; source exports are already registered, so resolve against them.
    for (auto& stmt : mod.body) {
        auto* fi = dynamic_cast<FromImportStmt*>(stmt.get());
        if (!fi) continue;
        auto srcIt = impl_->moduleTypes.find(fi->module);
        if (srcIt == impl_->moduleTypes.end()) continue;
        for (auto& alias : fi->names) {
            auto exIt = srcIt->second->exports.find(alias.name);
            if (exIt == srcIt->second->exports.end() || !exIt->second) continue;
            if (exIt->second->kind() != Type::Kind::Class) continue;
            const std::string bound = alias.asName.empty() ? alias.name : alias.asName;
            impl_->moduleImportedTypes[mod.moduleName][bound] = exIt->second;
        }
    }
}

// Worklist: stamp the transitive closure of instantiations to a fixpoint.

void TypeChecker::runMonomorphization() {
    if (!impl_->currentModule) return;
    while (!impl_->pendingInsts.empty()) {
        if (impl_->genericsAborted) { impl_->pendingInsts.clear(); return; }
        auto req = impl_->pendingInsts.front();
        impl_->pendingInsts.erase(impl_->pendingInsts.begin());
        if (impl_->instDone.count(req.key)) continue;
        impl_->instDone.insert(req.key);
        // (The class-recursion cap lives in instantiateGenericClass; on trip it stops
        // enqueueing, so this loop drains and terminates.)

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
            // On a stamped owning class (Container[int]) the method template lives under
            // the origin name; seed the subst with the class frame (T->arg) so cloning does both.
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

        // Recursion caps for function/method stamps (go[U] -> go[list[U]] -> ...):
        // depth + breadth caps trip as a clean compile error, never a hang (D044).
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

        // Cross-module: record the template's home module so CodeGen resolves the
        // stamped body's bare names there, not at the instantiation site (D044).
        std::string homeModule;
        if (auto modIt = impl_->genericTemplateModule.find(template_);
            modIt != impl_->genericTemplateModule.end())
            homeModule = modIt->second;

        std::unique_ptr<Stmt> cloned;
        if (!req.isClass && !isMethodReq &&
            impl_->schemaDecodeFns.count(dynamic_cast<const FunctionDecl*>(template_))) {
            // D048: generate the box-free decoder body from T's fields rather
            // than cloning the (never-lowered) template body.
            cloned = synthesizeSchemaDecoder(
                req.args.empty() ? nullptr : req.args[0], template_->location());
        } else {
            cloned = cloneStmt(template_, subst);
        }
        if (!cloned) continue;
        if (auto* cc = dynamic_cast<ClassDecl*>(cloned.get())) {
            cc->name = req.key;
            cc->typeParams.clear();  // a stamped instantiation is not a template
            cc->genericHomeModule = homeModule;
            // Register so a later method stamp (Container[int].wrap[str]) can append
            // into this class body; the raw pointer survives the std::move below.
            impl_->classDeclByName[req.key] = cc;
        } else if (auto* cf = dynamic_cast<FunctionDecl*>(cloned.get())) {
            // A method's on-disk name is the type-arg-only form (`all[int]`) - the
            // owning ClassDecl scopes it; a free function's is the full key.
            cf->name = isMethodReq ? mangleInstantiation(req.genericName, req.args) : req.key;
            cf->typeParams.clear();
            cf->genericHomeModule = homeModule;
        }

        // Inject the home module's exported class types into typeNames so the stamped
        // body's bare references to sibling types resolve at re-check (saved/restored).
        std::vector<std::pair<std::string, std::shared_ptr<Type>>> savedTypeNames;
        std::vector<std::string> addedTypeNames;
        if (auto modIt = impl_->genericTemplateModule.find(template_);
            modIt != impl_->genericTemplateModule.end()) {
            auto injectClass = [&](const std::string& ename,
                                   const std::shared_ptr<Type>& etype) {
                if (!etype || etype->kind() != Type::Kind::Class) return;
                if (auto ex = impl_->typeNames.find(ename); ex != impl_->typeNames.end())
                    savedTypeNames.push_back({ename, ex->second});
                else
                    addedTypeNames.push_back(ename);
                impl_->typeNames[ename] = std::make_shared<InstanceType>(
                    std::static_pointer_cast<ClassType>(etype));
            };
            if (auto mtIt = impl_->moduleTypes.find(modIt->second);
                mtIt != impl_->moduleTypes.end())
                for (auto& [ename, etype] : mtIt->second->exports)
                    injectClass(ename, etype);
            // D052 - also the home module's IMPORTED classes (not in its exports).
            if (auto imIt = impl_->moduleImportedTypes.find(modIt->second);
                imIt != impl_->moduleImportedTypes.end())
                for (auto& [ename, etype] : imIt->second)
                    injectClass(ename, etype);
        }

        if (isMethodReq) {
            // Append the stamp into its owning class body and re-type-check it with
            // self/currentClass bound (else self.X and implicit self would be untyped).
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

// D048 schema-decode synthesis: build decode[T]'s box-free body from T's fields.

namespace {

std::unique_ptr<NameExpr> sdName(const std::string& n, SourceLocation loc) {
    auto e = std::make_unique<NameExpr>(); e->name = n; e->setLocation(loc); return e;
}
std::unique_ptr<Expr> sdStr(const std::string& s, SourceLocation loc) {
    auto e = std::make_unique<StringLiteral>(); e->value = s; e->setLocation(loc); return e;
}
std::unique_ptr<TypeExpr> sdType(const std::string& n, SourceLocation loc) {
    auto t = std::make_unique<NamedTypeExpr>(); t->name = n; t->setLocation(loc); return t;
}
std::unique_ptr<Expr> sdBool(bool b, SourceLocation loc) {
    auto e = std::make_unique<BooleanLiteral>(); e->value = b; e->setLocation(loc); return e;
}
std::unique_ptr<Expr> sdZero(const std::string& kind, SourceLocation loc) {
    if (kind == "int") { auto e = std::make_unique<IntegerLiteral>(); e->value = 0; e->setLocation(loc); return e; }
    if (kind == "float") { auto e = std::make_unique<FloatLiteral>(); e->value = 0.0; e->setLocation(loc); return e; }
    if (kind == "bool") return sdBool(false, loc);
    return sdStr("", loc);
}
// "list:<elem>" -> list[<elem>] type expr; a scalar kind -> its NamedTypeExpr.
// Base type for a field kind (no opt wrapper): scalar, list[<elem>], or a class.
std::unique_ptr<TypeExpr> sdInnerType(const std::string& kind, SourceLocation loc) {
    if (kind.rfind("list:", 0) == 0) {
        auto g = std::make_unique<GenericTypeExpr>();
        g->base = sdType("list", loc);
        g->typeArgs.push_back(sdType(kind.substr(5), loc));
        g->setLocation(loc);
        return g;
    }
    if (kind.rfind("class:", 0) == 0) return sdType(kind.substr(6), loc);
    return sdType(kind, loc);
}
// Field type: "opt:<ik>" -> <ik> | None; otherwise the inner type.
std::unique_ptr<TypeExpr> sdFieldType(const std::string& kind, SourceLocation loc) {
    if (kind.rfind("opt:", 0) == 0) {
        auto u = std::make_unique<UnionTypeExpr>();
        u->types.push_back(sdInnerType(kind.substr(4), loc));
        u->types.push_back(sdType("None", loc));
        u->setLocation(loc);
        return u;
    }
    return sdInnerType(kind, loc);
}
std::unique_ptr<Expr> sdFieldZero(const std::string& kind, SourceLocation loc) {
    if (kind.rfind("list:", 0) == 0) {
        auto l = std::make_unique<ListExpr>();
        l->setLocation(loc);
        return l;
    }
    return sdZero(kind, loc);
}
std::unique_ptr<Expr> sdBytesEmpty(SourceLocation loc) {
    auto e = std::make_unique<StringLiteral>();
    e->value = ""; e->isBytes = true; e->setLocation(loc);
    return e;
}
std::unique_ptr<Expr> sdNone(SourceLocation loc) {
    auto e = std::make_unique<NoneLiteral>(); e->setLocation(loc); return e;
}
// decode[<cls>](<arg>) - feed a bytes expression back through decode.
std::unique_ptr<Expr> sdDecodeExpr(const std::string& cls, std::unique_ptr<Expr> arg,
                                   SourceLocation loc) {
    auto sub = std::make_unique<SubscriptExpr>();
    sub->object = sdName("decode", loc);
    sub->index = sdName(cls, loc);
    sub->setLocation(loc);
    auto call = std::make_unique<CallExpr>();
    call->callee = std::move(sub);
    call->args.push_back(std::move(arg));
    call->setLocation(loc);
    return call;
}
// Rebuild a LITERAL default so an optional field can initialize to it; nullptr
// for anything non-literal.
std::unique_ptr<Expr> sdRebuildLiteral(const Expr* d, SourceLocation loc) {
    if (auto* i = dynamic_cast<const IntegerLiteral*>(d)) {
        auto e = std::make_unique<IntegerLiteral>(); e->value = i->value; e->setLocation(loc); return e;
    }
    if (auto* f = dynamic_cast<const FloatLiteral*>(d)) {
        auto e = std::make_unique<FloatLiteral>(); e->value = f->value; e->setLocation(loc); return e;
    }
    if (auto* b = dynamic_cast<const BooleanLiteral*>(d)) return sdBool(b->value, loc);
    if (auto* s = dynamic_cast<const StringLiteral*>(d)) {
        if (s->isFString || s->isBytes) return nullptr;
        auto e = std::make_unique<StringLiteral>(); e->value = s->value; e->setLocation(loc); return e;
    }
    if (dynamic_cast<const NoneLiteral*>(d)) {
        auto e = std::make_unique<NoneLiteral>(); e->setLocation(loc); return e;
    }
    if (auto* l = dynamic_cast<const ListExpr*>(d))
        if (l->elements.empty()) { auto e = std::make_unique<ListExpr>(); e->setLocation(loc); return e; }
    return nullptr;
}
// c.method() with no args
std::unique_ptr<Expr> sdCall0(const std::string& recv, const std::string& method, SourceLocation loc) {
    auto attr = std::make_unique<AttributeExpr>();
    attr->object = sdName(recv, loc);
    attr->attribute = method;
    attr->setLocation(loc);
    auto call = std::make_unique<CallExpr>();
    call->callee = std::move(attr);
    call->setLocation(loc);
    return call;
}
std::unique_ptr<Stmt> sdExprStmt(std::unique_ptr<Expr> e, SourceLocation loc) {
    auto s = std::make_unique<ExprStmt>(); s->expr = std::move(e); s->setLocation(loc); return s;
}
// name: type = value  (reassignable local declaration)
std::unique_ptr<Stmt> sdDecl(const std::string& name, std::unique_ptr<TypeExpr> ty,
                             std::unique_ptr<Expr> val, SourceLocation loc) {
    auto s = std::make_unique<AnnAssignStmt>();
    s->target = sdName(name, loc);
    s->annotation = std::move(ty);
    s->value = std::move(val);
    s->setLocation(loc);
    return s;
}
// name = value  (reassignment)
std::unique_ptr<Stmt> sdAssign(const std::string& name, std::unique_ptr<Expr> val, SourceLocation loc) {
    auto s = std::make_unique<AssignStmt>();
    s->targets.push_back(sdName(name, loc));
    s->value = std::move(val);
    s->setLocation(loc);
    return s;
}

}  // namespace

std::unique_ptr<Stmt> TypeChecker::synthesizeSchemaDecoder(
    const std::shared_ptr<Type>& targetType, SourceLocation loc) {
    std::string className;
    if (auto inst = std::dynamic_pointer_cast<InstanceType>(targetType))
        if (inst->classType) className = inst->classType->name;
    if (className.empty()) {
        error(loc, "json.decode[T]: T must be a class type");
        return nullptr;
    }
    auto cdIt = impl_->classDeclByName.find(className);
    if (cdIt == impl_->classDeclByName.end() || !cdIt->second) {
        error(loc, "json.decode[" + className + "]: class definition not found");
        return nullptr;
    }
    FunctionDecl* ctor = nullptr;
    for (auto& m : cdIt->second->body)
        if (auto* fd = dynamic_cast<FunctionDecl*>(m.get()))
            if (fd->isConstructor || fd->name == "__init__") { ctor = fd; break; }
    if (!ctor) {
        error(loc, "json.decode[" + className + "]: class has no constructor to decode into");
        return nullptr;
    }

    // kind: scalar ("int"/"str"/"bool"/"float"), "list:<elem>", "class:<Name>",
    // or "opt:<ik>" for Optional[<ik>]. `optional` means "no seen-check" (absent
    // is allowed) - true for opt-null fields and for literal-default fields.
    struct Field { std::string name; std::string kind; bool optional; std::unique_ptr<Expr> dflt; };
    auto detectKind = [&](const TypeExpr* t) -> std::string {
        if (auto* nt = dynamic_cast<const NamedTypeExpr*>(t)) {
            const std::string& n = nt->name;
            if (n == "int" || n == "str" || n == "bool" || n == "float") return n;
            if (impl_->classDeclByName.count(n)) return "class:" + n;
            return "";
        }
        if (auto* gt = dynamic_cast<const GenericTypeExpr*>(t))
            if (auto* b = dynamic_cast<const NamedTypeExpr*>(gt->base.get()))
                if (b->name == "list" && gt->typeArgs.size() == 1)
                    if (auto* el = dynamic_cast<const NamedTypeExpr*>(gt->typeArgs[0].get())) {
                        const std::string& e = el->name;
                        if (e == "int" || e == "str" || e == "bool" || e == "float")
                            return "list:" + e;
                    }
        return "";
    };
    std::vector<Field> fields;
    for (auto& p : ctor->params) {
        if (p.name == "self") continue;
        if (p.isVarArg || p.isKwArg) {
            error(loc, "json.decode[" + className + "]: variadic constructors are not decodable");
            return nullptr;
        }
        std::string kind = detectKind(p.type.get());
        bool isOptNull = false;
        if (kind.empty()) {
            // Optional[X] is parsed as `X | None`.
            if (auto* u = dynamic_cast<UnionTypeExpr*>(p.type.get()))
                if (u->types.size() == 2) {
                    const TypeExpr* inner = nullptr;
                    bool hasNone = false;
                    for (auto& tt : u->types) {
                        auto* nt = dynamic_cast<const NamedTypeExpr*>(tt.get());
                        if (nt && nt->name == "None") hasNone = true;
                        else inner = tt.get();
                    }
                    if (hasNone && inner) {
                        std::string ik = detectKind(inner);
                        if (!ik.empty()) { kind = "opt:" + ik; isOptNull = true; }
                    }
                }
        }
        if (kind.empty()) {
            error(loc, "json.decode[" + className + "]: field '" + p.name +
                       "' is not yet decodable (int/str/bool/float, list of those, "
                       "a class, or Optional of those)");
            return nullptr;
        }
        bool optional = isOptNull;
        std::unique_ptr<Expr> dflt;
        if (p.defaultValue) {
            if (kind.rfind("class:", 0) == 0) {
                error(loc, "json.decode[" + className + "]: nested field '" + p.name +
                           "' cannot have a default (required only)");
                return nullptr;
            }
            dflt = sdRebuildLiteral(p.defaultValue.get(), loc);
            if (!dflt) {
                error(loc, "json.decode[" + className + "]: field '" + p.name +
                           "' has a non-literal default (only literal defaults are supported)");
                return nullptr;
            }
            optional = true;
        }
        if (isOptNull && !dflt) dflt = sdNone(loc);   // absent Optional -> None
        fields.push_back({p.name, kind, optional, std::move(dflt)});
    }
    if (fields.empty()) {
        error(loc, "json.decode[" + className + "]: class has no decodable fields");
        return nullptr;
    }

    auto methodFor = [](const std::string& k) -> std::string {
        if (k == "int") return "parse_int";
        if (k == "float") return "parse_float";
        if (k == "bool") return "parse_bool";
        if (k == "str") return "parse_str";
        if (k == "list:int") return "parse_int_list";
        if (k == "list:float") return "parse_float_list";
        if (k == "list:bool") return "parse_bool_list";
        return "parse_str_list";  // list:str
    };

    auto fn = std::make_unique<FunctionDecl>();
    fn->setLocation(loc);
    fn->returnType = sdType(className, loc);
    Parameter bodyParam;
    bodyParam.name = "body";
    bodyParam.type = sdType("bytes", loc);
    fn->params.push_back(std::move(bodyParam));

    // c: Cursor = Cursor(body)
    auto ctorCall = std::make_unique<CallExpr>();
    ctorCall->callee = sdName("Cursor", loc);
    ctorCall->args.push_back(sdName("body", loc));
    ctorCall->setLocation(loc);
    fn->body.push_back(sdDecl("c", sdType("Cursor", loc), std::move(ctorCall), loc));

    // per-field local + presence flag. A nested class captures raw bytes (b"" has
    // a zero; a class-typed local does not); an optional field inits to its default.
    for (auto& f : fields) {
        if (f.kind.rfind("class:", 0) == 0) {
            fn->body.push_back(sdDecl("_" + f.name + "_bytes", sdType("bytes", loc), sdBytesEmpty(loc), loc));
            fn->body.push_back(sdDecl("_" + f.name + "_seen", sdType("bool", loc), sdBool(false, loc), loc));
        } else {
            std::unique_ptr<Expr> init;
            if (f.optional) init = std::move(f.dflt);
            else init = sdFieldZero(f.kind, loc);
            fn->body.push_back(sdDecl("_" + f.name, sdFieldType(f.kind, loc), std::move(init), loc));
            if (!f.optional)
                fn->body.push_back(sdDecl("_" + f.name + "_seen", sdType("bool", loc), sdBool(false, loc), loc));
        }
    }

    fn->body.push_back(sdExprStmt(sdCall0("c", "begin_object", loc), loc));

    // while c.next_field() { k: str = c.field_key(); if/elif/else }
    auto wh = std::make_unique<WhileStmt>();
    wh->condition = sdCall0("c", "next_field", loc);
    wh->setLocation(loc);
    wh->body.push_back(sdDecl("k", sdType("str", loc), sdCall0("c", "field_key", loc), loc));

    auto cmpKey = [&](const std::string& key) -> std::unique_ptr<Expr> {
        auto bin = std::make_unique<BinaryExpr>();
        bin->left = sdName("k", loc);
        bin->op = Token(TokenType::EQUAL_EQUAL, "==", loc);
        bin->right = sdStr(key, loc);
        bin->setLocation(loc);
        return bin;
    };
    // Read a value of inner-kind `ik` from cursor c into local `L`.
    auto readInto = [&](const std::string& ik, const std::string& L) -> std::unique_ptr<Stmt> {
        if (ik.rfind("class:", 0) == 0)
            return sdAssign(L, sdDecodeExpr(ik.substr(6), sdCall0("c", "capture_value", loc), loc), loc);
        return sdAssign(L, sdCall0("c", methodFor(ik), loc), loc);
    };
    auto assignBody = [&](const Field& f) {
        std::vector<std::unique_ptr<Stmt>> b;
        if (f.kind.rfind("opt:", 0) == 0) {
            // if c.try_null() { _f = None } else { _f = <read inner> }
            auto ifn = std::make_unique<IfStmt>();
            ifn->setLocation(loc);
            ifn->condition = sdCall0("c", "try_null", loc);
            ifn->thenBody.push_back(sdAssign("_" + f.name, sdNone(loc), loc));
            ifn->elseBody.push_back(readInto(f.kind.substr(4), "_" + f.name));
            b.push_back(std::move(ifn));
        } else if (f.kind.rfind("class:", 0) == 0) {
            b.push_back(sdAssign("_" + f.name + "_bytes", sdCall0("c", "capture_value", loc), loc));
            b.push_back(sdAssign("_" + f.name + "_seen", sdBool(true, loc), loc));
        } else {
            b.push_back(sdAssign("_" + f.name, sdCall0("c", methodFor(f.kind), loc), loc));
            if (!f.optional)
                b.push_back(sdAssign("_" + f.name + "_seen", sdBool(true, loc), loc));
        }
        return b;
    };

    auto iff = std::make_unique<IfStmt>();
    iff->setLocation(loc);
    iff->condition = cmpKey(fields[0].name);
    iff->thenBody = assignBody(fields[0]);
    for (size_t i = 1; i < fields.size(); ++i)
        iff->elifClauses.emplace_back(cmpKey(fields[i].name), assignBody(fields[i]));
    iff->elseBody.push_back(sdExprStmt(sdCall0("c", "skip_value", loc), loc));
    wh->body.push_back(std::move(iff));
    fn->body.push_back(std::move(wh));

    // required-field checks (optional fields keep their default): if not _seen, raise
    for (auto& f : fields) {
        if (f.optional) continue;
        auto notseen = std::make_unique<UnaryExpr>();
        notseen->op = Token(TokenType::NOT, "not", loc);
        notseen->operand = sdName("_" + f.name + "_seen", loc);
        notseen->setLocation(loc);
        auto guard = std::make_unique<IfStmt>();
        guard->setLocation(loc);
        guard->condition = std::move(notseen);
        auto verr = std::make_unique<CallExpr>();
        verr->callee = sdName("ValueError", loc);
        verr->args.push_back(sdStr("missing required field '" + f.name + "'", loc));
        verr->setLocation(loc);
        auto raise = std::make_unique<RaiseStmt>();
        raise->exception = std::move(verr);
        raise->setLocation(loc);
        guard->thenBody.push_back(std::move(raise));
        fn->body.push_back(std::move(guard));
    }

    // return ClassName(f0=_f0, f1=_f1, ...)
    auto build = std::make_unique<CallExpr>();
    build->callee = sdName(className, loc);
    for (auto& f : fields) {
        if (f.kind.rfind("class:", 0) == 0)
            build->kwArgs.emplace_back(f.name, sdDecodeExpr(f.kind.substr(6), sdName("_" + f.name + "_bytes", loc), loc));
        else
            build->kwArgs.emplace_back(f.name, sdName("_" + f.name, loc));
    }
    build->setLocation(loc);
    auto ret = std::make_unique<ReturnStmt>();
    ret->value = std::move(build);
    ret->setLocation(loc);
    fn->body.push_back(std::move(ret));

    return fn;
}

}  // namespace dragon
