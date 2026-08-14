#include "dragon/TypeChecker.h"
#include "dragon/Privacy.h"
#include "TypeCheckerImpl.h"
#include "dragon/AstClone.h"
#include <algorithm>
#include <cassert>
#include <filesystem>
#include <functional>
#include <set>
#include <system_error>

namespace dragon {

namespace {
// Walk cls's parent chain for a name match (set by ClassDecl visit).
// Used by the D017 Template protocol check and D037 StructTemplate dispatch.
bool classExtendsByName(const ClassType* cls, const std::string& ancestor) {
    while (cls) {
        if (cls->name == ancestor) return true;
        if (!cls->parentClass || cls->parentClass->kind() != Type::Kind::Class) {
            return false;
        }
        cls = static_cast<const ClassType*>(cls->parentClass.get());
    }
    return false;
}

// D045 - normalize a source path so heterogeneous spellings (CLI arg vs
// resolver-built "./pkg/x.dr" vs absolute stdlib path) compare equal.
std::string canonFile(const std::string& filePath) {
    if (filePath.empty()) return "";
    std::error_code ec;
    auto p = std::filesystem::weakly_canonical(filePath, ec);
    return ec ? filePath : p.string();
}

// D045 - path-based package identity: a dir containing `pkg.dr` or
// `__init__.py` is a package (keyed by dir); otherwise the file is its own singleton package.
std::string packageKeyOf(const std::string& filePath) {
    if (filePath.empty()) return "";
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::path p = fs::weakly_canonical(filePath, ec);
    if (ec) p = fs::path(filePath);
    fs::path dir = p.parent_path();
    if (!dir.empty()) {
        std::string base = dir.filename().string();
        std::error_code ec2;
        if (fs::exists(dir / (base + ".dr"), ec2) ||
            fs::exists(dir / "__init__.py", ec2)) {
            return dir.string();  // part of package `dir`
        }
    }
    return p.string();  // flat singleton - keyed by the file itself
}

// D045 - class identity: pointer identity is primary (one ClassType per
// source class); name+definingFile is a fallback for stray duplicates.
bool isSameClass(const ClassType* a, const ClassType* b) {
    if (!a || !b) return false;
    if (a == b) return true;
    return a->name == b->name && !a->definingFile.empty() &&
           a->definingFile == b->definingFile;
}

// True if `c` is `d` or a subclass of `d` (walks the parentClass chain).
bool isSameOrSubclass(const ClassType* c, const ClassType* d) {
    for (const ClassType* p = c; p; ) {
        if (isSameClass(p, d)) return true;
        p = (p->parentClass && p->parentClass->kind() == Type::Kind::Class)
                ? static_cast<const ClassType*>(p->parentClass.get())
                : nullptr;
    }
    return false;
}
}  // namespace

bool Type::isSubtypeOf(const Type& other) const {
    if (equals(other)) return true;
    if (other.kind() == Kind::Any) return true;
    if (other.kind() == Kind::Union) {
        auto& ut = static_cast<const UnionType&>(other);
        for (auto& t : ut.types) {
            if (isSubtypeOf(*t)) return true;
        }
    }
    // FunctionType <: ptr sanctions passing a `def` as a C callback (pthread_create,
    // GTK/WebKit handlers, ...); only Function->ptr is admitted, not int->ptr.
    if (other.kind() == Kind::Ptr && kind() == Kind::Function) return true;
    return false;
}

bool Type::isAssignableTo(const Type& other) const {
    return isSubtypeOf(other);
}

// PrimitiveType
std::string PrimitiveType::toString() const {
    switch (kind_) {
        case Kind::Int: return "int";
        case Kind::Float: return "float";
        case Kind::Bool: return "bool";
        case Kind::Str: return "str";
        case Kind::Bytes: return "bytes";
        case Kind::None_: return "None";
        default: return "<primitive?>";
    }
}

bool PrimitiveType::equals(const Type& other) const {
    return other.kind() == kind_;
}

bool PrimitiveType::isSubtypeOf(const Type& other) const {
    if (equals(other)) return true;
    if (other.kind() == Kind::Any) return true;
    // bool <: int (Python convention)
    if (kind_ == Kind::Bool && other.kind() == Kind::Int) return true;
    // int <: float (numeric widening)
    if (kind_ == Kind::Int && other.kind() == Kind::Float) return true;
    // bool <: float (transitive: bool <: int <: float)
    if (kind_ == Kind::Bool && other.kind() == Kind::Float) return true;
    return Type::isSubtypeOf(other);
}

// ListType
std::string ListType::toString() const {
    return "list[" + elementType->toString() + "]";
}

bool ListType::equals(const Type& other) const {
    if (other.kind() != Kind::List) return false;
    return elementType->equals(*static_cast<const ListType&>(other).elementType);
}

bool ListType::isSubtypeOf(const Type& other) const {
    if (Type::isSubtypeOf(other)) return true;
    // Lists are invariant even against list[Any]: list[T] is 8B/elem native,
    // list[Any] is 16B/elem boxed, so flowing across would read the wrong stride. A fresh literal still covaries via use-site retyping.
    return false;
}

// TaskType
std::string TaskType::toString() const {
    return "Task[" + resultType->toString() + "]";
}

bool TaskType::equals(const Type& other) const {
    if (other.kind() != Kind::Task) return false;
    return resultType->equals(*static_cast<const TaskType&>(other).resultType);
}

bool TaskType::isSubtypeOf(const Type& other) const {
    if (Type::isSubtypeOf(other)) return true;
    // Same Any-only relaxation as ListType: Task[T] <: Task[Any] for any T.
    if (other.kind() == Kind::Task) {
        auto& o = static_cast<const TaskType&>(other);
        if (o.resultType->kind() == Kind::Any) return true;
    }
    return false;
}

// LockType - erases to ptr; all LockTypes are interchangeable (no payload).
std::string LockType::toString() const { return "Lock"; }

bool LockType::equals(const Type& other) const {
    return other.kind() == Kind::Lock;
}

bool LockType::isSubtypeOf(const Type& other) const {
    return Type::isSubtypeOf(other);
}

// DictType
std::string DictType::toString() const {
    return "dict[" + keyType->toString() + ", " + valueType->toString() + "]";
}

bool DictType::equals(const Type& other) const {
    if (other.kind() != Kind::Dict) return false;
    auto& o = static_cast<const DictType&>(other);
    return keyType->equals(*o.keyType) && valueType->equals(*o.valueType);
}

bool DictType::isSubtypeOf(const Type& other) const {
    if (Type::isSubtypeOf(other)) return true;
    if (other.kind() == Kind::Dict) {
        auto& o = static_cast<const DictType&>(other);
        // Same Any-only relaxation as ListType: dict[K, V] <: dict[K, Any]
        // when keys equal. Keeps dict invariance for non-Any value types.
        if (keyType->equals(*o.keyType) && o.valueType->kind() == Kind::Any)
            return true;
    }
    return false;
}

// TupleType
std::string TupleType::toString() const {
    std::string s = "tuple[";
    for (size_t i = 0; i < elementTypes.size(); ++i) {
        if (i > 0) s += ", ";
        s += elementTypes[i]->toString();
    }
    return s + "]";
}

bool TupleType::equals(const Type& other) const {
    if (other.kind() != Kind::Tuple) return false;
    auto& o = static_cast<const TupleType&>(other);
    if (elementTypes.size() != o.elementTypes.size()) return false;
    for (size_t i = 0; i < elementTypes.size(); ++i) {
        if (!elementTypes[i]->equals(*o.elementTypes[i])) return false;
    }
    return true;
}

// FunctionType
std::string FunctionType::toString() const {
    std::string s = "(";
    for (size_t i = 0; i < paramTypes.size(); ++i) {
        if (i > 0) s += ", ";
        s += paramTypes[i]->toString();
    }
    s += ") -> " + returnType->toString();
    return s;
}

bool FunctionType::equals(const Type& other) const {
    if (other.kind() != Kind::Function) return false;
    auto& o = static_cast<const FunctionType&>(other);
    if (!returnType->equals(*o.returnType)) return false;
    if (paramTypes.size() != o.paramTypes.size()) return false;
    for (size_t i = 0; i < paramTypes.size(); ++i) {
        if (!paramTypes[i]->equals(*o.paramTypes[i])) return false;
    }
    return true;
}

// ClassType
std::string ClassType::toString() const { return "type[" + name + "]"; }

bool ClassType::equals(const Type& other) const {
    if (other.kind() != Kind::Class) return false;
    return name == static_cast<const ClassType&>(other).name;
}

// InstanceType
std::string InstanceType::toString() const { return classType->name; }

bool InstanceType::equals(const Type& other) const {
    if (other.kind() != Kind::Instance) return false;
    return classType->equals(*static_cast<const InstanceType&>(other).classType);
}

bool InstanceType::isSubtypeOf(const Type& other) const {
    // equals / Any / Union handled by the base.
    if (Type::isSubtypeOf(other)) return true;
    // ADR 054 - contract conformance is declared, not inferred: an instance
    // passes iff its class (or ancestor) explicitly promised every atom; a structural match alone doesn't count.
    if (other.kind() == Kind::Contract) {
        const auto& ct = static_cast<const ContractType&>(other);
        std::set<const ContractDecl*> promised;
        for (const ClassType* cur = classType.get(); cur; ) {
            promised.insert(cur->promisedContracts.begin(),
                            cur->promisedContracts.end());
            cur = (cur->parentClass && cur->parentClass->kind() == Kind::Class)
                      ? static_cast<const ClassType*>(cur->parentClass.get())
                      : nullptr;
        }
        for (auto* need : ct.atoms)
            if (!promised.count(need)) return false;
        return true;
    }
    if (other.kind() != Kind::Instance) return false;
    const auto& o = static_cast<const InstanceType&>(other);
    if (!o.classType) return false;
    // Walk this instance's class up its parent chain; a match against the
    // target's class anywhere in the chain means subtype.
    const ClassType* cur = classType.get();
    while (cur) {
        if (cur->name == o.classType->name) return true;
        if (cur->parentClass && cur->parentClass->kind() == Kind::Class)
            cur = static_cast<const ClassType*>(cur->parentClass.get());
        else
            break;
    }
    return false;
}

// ContractType (ADR 054)
bool ContractType::equals(const Type& other) const {
    if (other.kind() != Kind::Contract) return false;
    return atoms == static_cast<const ContractType&>(other).atoms;  // sorted+deduped
}

bool ContractType::isSubtypeOf(const Type& other) const {
    if (Type::isSubtypeOf(other)) return true;
    if (other.kind() != Kind::Contract) return false;
    // Subset rule: a `{A, B}` value satisfies an `A` position.
    const auto& o = static_cast<const ContractType&>(other);
    for (auto* need : o.atoms)
        if (std::find(atoms.begin(), atoms.end(), need) == atoms.end())
            return false;
    return true;
}

// UnionType
std::string UnionType::toString() const {
    std::string s;
    for (size_t i = 0; i < types.size(); ++i) {
        if (i > 0) s += " | ";
        s += types[i]->toString();
    }
    return s;
}

bool UnionType::equals(const Type& other) const {
    if (other.kind() != Kind::Union) return false;
    auto& o = static_cast<const UnionType&>(other);
    if (types.size() != o.types.size()) return false;
    // Order-independent comparison
    for (auto& t : types) {
        bool found = false;
        for (auto& ot : o.types) {
            if (t->equals(*ot)) { found = true; break; }
        }
        if (!found) return false;
    }
    return true;
}

bool UnionType::isSubtypeOf(const Type& other) const {
    // A union is a subtype of another type if all members are subtypes
    for (auto& t : types) {
        if (!t->isSubtypeOf(other)) return false;
    }
    return true;
}

// ModuleType
// Modules are identified by their canonical dotted path; two ModuleType
// instances with the same name are equal regardless of export/submodule population timing.
bool ModuleType::equals(const Type& other) const {
    if (other.kind() != Kind::Module) return false;
    return name == static_cast<const ModuleType&>(other).name;
}

// TypeVarType
bool TypeVarType::equals(const Type& other) const {
    if (other.kind() != Kind::TypeVar) return false;
    return name == static_cast<const TypeVarType&>(other).name;
}

// TypeChecker::Impl lives in TypeCheckerImpl.h (shared with the D044
// monomorphization engine in TypeCheckerGenerics.cpp); only this one member is defined here.
const std::string& TypeChecker::Impl::packageKey(const std::string& file) {
    auto it = packageKeyCache.find(file);
    if (it != packageKeyCache.end()) return it->second;
    return packageKeyCache.emplace(file, packageKeyOf(file)).first->second;
}

TypeChecker::TypeChecker() : impl_(std::make_unique<Impl>()) {
    initBuiltinTypes();
}

TypeChecker::~TypeChecker() = default;

void TypeChecker::registerExternalModule(
    const std::string& moduleName,
    const std::unordered_map<std::string, std::shared_ptr<Type>>& exports,
    const std::string& filepath) {
    auto mt = impl_->getOrCreateModuleType(moduleName);
    mt->exports = exports;
    if (!filepath.empty()) mt->filepath = filepath;  // D045: source-file key
    // threading.Lock is a compiler intrinsic (ptr-erased LockType), not a Dragon
    // class, so inject it into exports so `from threading import Lock` resolves normally.
    if (moduleName == "threading")
        mt->exports["Lock"] = std::make_shared<LockType>();
}

std::unordered_map<std::string, std::shared_ptr<Type>> TypeChecker::getExports() const {
    // User-defined symbols only: builtins are filtered by IDENTITY not name, so
    // a same-named user redefinition (e.g. `def open` in gzip.dr) is still exported.
    std::unordered_map<std::string, std::shared_ptr<Type>> exports;
    for (auto& [name, type] : impl_->cachedExports) {
        auto bIt = impl_->builtinIdentity.find(name);
        if (bIt != impl_->builtinIdentity.end() && bIt->second == type.get()) {
            // Same shared_ptr the builtin setup left here - not user-defined.
            continue;
        }
        exports[name] = type;
    }
    return exports;
}

bool TypeChecker::check(Module& module) {
    // D045 - capture module identity for privacy checks: file path is
    // authoritative (the entry module's moduleName is empty); package is derived path-based.
    impl_->currentFile = module.filename;
    impl_->currentModuleName = module.moduleName;
    impl_->currentPackage = impl_->packageKey(module.filename);
    impl_->currentClass = nullptr;
    impl_->pushScope(); // Module scope
    // Define builtins in module scope
    impl_->define("print", std::make_shared<FunctionType>(
        std::vector<std::shared_ptr<Type>>{impl_->anyType},
        impl_->noneType));
    impl_->define("len", std::make_shared<FunctionType>(
        std::vector<std::shared_ptr<Type>>{impl_->anyType},
        impl_->intType));
    impl_->define("range", std::make_shared<FunctionType>(
        std::vector<std::shared_ptr<Type>>{impl_->intType},
        std::make_shared<ListType>(impl_->intType)));
    impl_->define("input", std::make_shared<FunctionType>(
        std::vector<std::shared_ptr<Type>>{impl_->strType},
        impl_->strType));
    impl_->define("int", std::make_shared<FunctionType>(
        std::vector<std::shared_ptr<Type>>{impl_->anyType},
        impl_->intType));
    impl_->define("float", std::make_shared<FunctionType>(
        std::vector<std::shared_ptr<Type>>{impl_->anyType},
        impl_->floatType));
    impl_->define("str", std::make_shared<FunctionType>(
        std::vector<std::shared_ptr<Type>>{impl_->anyType},
        impl_->strType));
    impl_->define("bool", std::make_shared<FunctionType>(
        std::vector<std::shared_ptr<Type>>{impl_->anyType},
        impl_->boolType));
    impl_->define("bytes", std::make_shared<FunctionType>(
        std::vector<std::shared_ptr<Type>>{impl_->anyType},
        impl_->bytesType));
    impl_->define("abs", std::make_shared<FunctionType>(
        std::vector<std::shared_ptr<Type>>{impl_->anyType},
        impl_->intType));
    impl_->define("min", std::make_shared<FunctionType>(
        std::vector<std::shared_ptr<Type>>{impl_->anyType},
        impl_->anyType));
    impl_->define("max", std::make_shared<FunctionType>(
        std::vector<std::shared_ptr<Type>>{impl_->anyType},
        impl_->anyType));
    // type(x) returns the type's NAME as a str (ADR 025: classes aren't values);
    // declaring the return as str keeps boxing correct (TAG_STR, not the int fallback).
    impl_->define("type", std::make_shared<FunctionType>(
        std::vector<std::shared_ptr<Type>>{impl_->anyType},
        impl_->strType));
    impl_->define("isinstance", std::make_shared<FunctionType>(
        std::vector<std::shared_ptr<Type>>{impl_->anyType, impl_->anyType},
        impl_->boolType));
    impl_->define("enumerate", std::make_shared<FunctionType>(
        std::vector<std::shared_ptr<Type>>{impl_->anyType},
        impl_->anyType));
    impl_->define("zip", std::make_shared<FunctionType>(
        std::vector<std::shared_ptr<Type>>{impl_->anyType},
        impl_->anyType));
    impl_->define("map", std::make_shared<FunctionType>(
        std::vector<std::shared_ptr<Type>>{impl_->anyType, impl_->anyType},
        impl_->anyType));
    impl_->define("filter", std::make_shared<FunctionType>(
        std::vector<std::shared_ptr<Type>>{impl_->anyType, impl_->anyType},
        impl_->anyType));
    impl_->define("sorted", std::make_shared<FunctionType>(
        std::vector<std::shared_ptr<Type>>{impl_->anyType},
        impl_->anyType));
    impl_->define("reversed", std::make_shared<FunctionType>(
        std::vector<std::shared_ptr<Type>>{impl_->anyType},
        impl_->anyType));
    // File I/O has no builtin: open/make/push live in stdlib/io.dr and are
    // reached via `from io import open, make, push` like any stdlib symbol.
    impl_->define("True", impl_->boolType);
    impl_->define("False", impl_->boolType);
    impl_->define("None", impl_->noneType);

    // Snapshot the identity of every builtin we just defined, so getExports()
    // can distinguish "still the builtin" from "user redefined this name".
    impl_->builtinIdentity.clear();
    if (!impl_->scopes.empty()) {
        for (auto& [name, type] : impl_->scopes[0].bindings) {
            impl_->builtinIdentity[name] = type.get();
        }
    }

    // Import-binding pre-pass: bind imports before the class/function pre-passes
    // so a signature referencing an imported type resolves (else the param stayed Unknown, the call silently typed Any, and it mis-boxed downstream). Diagnostics here are rolled back to avoid duplicates.
    for (auto& stmt : module.body) {
        size_t diagBefore = impl_->diagnostics.size();
        if (auto* imp = dynamic_cast<ImportStmt*>(stmt.get())) {
            visit(*imp);
        } else if (auto* fimp = dynamic_cast<FromImportStmt*>(stmt.get())) {
            visit(*fimp);
        }
        if (impl_->diagnostics.size() > diagBefore)
            impl_->diagnostics.resize(diagBefore);
    }

    // Class-layout pre-pass (mirrors CodeGen's): register class names and
    // annotated field types before any method body, so a forward reference to a later class doesn't default its field type to int and mis-box.
    // Sub-pass 1: register all class names so field annotations referencing
    // sibling classes resolve.
    for (auto& stmt : module.body) {
        auto* cd = dynamic_cast<ClassDecl*>(stmt.get());
        if (!cd) continue;
        bool isTD = false;
        for (auto& base : cd->bases)
            if (auto* bn = dynamic_cast<NameExpr*>(base.get()))
                if (bn->name == "TypedDict") isTD = true;
        if (isTD || impl_->typeNames.count(cd->name)) continue;
        auto classType = std::make_shared<ClassType>(cd->name);
        impl_->typeNames[cd->name] = std::make_shared<InstanceType>(classType);
        impl_->define(cd->name, classType);
    }
    // ADR 054 contract pre-pass: register contracts before field types resolve
    // (fields/params may be contract-typed); class shells from sub-pass 1 are already in typeNames.
    registerContracts(module);

    // Sub-pass 2: register annotated field types onto each ClassType.
    for (auto& stmt : module.body) {
        auto* cd = dynamic_cast<ClassDecl*>(stmt.get());
        if (!cd) continue;
        // D044 - a generic class's field types reference unbound type params here;
        // collectGenericTemplates populates them later with type-param scope active.
        if (!cd->typeParams.empty()) continue;
        auto tnIt = impl_->typeNames.find(cd->name);
        if (tnIt == impl_->typeNames.end()) continue;
        auto inst = std::dynamic_pointer_cast<InstanceType>(tnIt->second);
        if (!inst || !inst->classType) continue;
        auto& fields = inst->classType->fields;
        // Only register if the annotation resolves CLEANLY (no diagnostics, concrete
        // type); anything else is left for visit(ClassDecl), and diagnostics here are rolled back.
        auto tryRegister = [&](const std::string& fname, TypeExpr* ann) {
            if (!ann || fields.count(fname)) return;
            size_t diagBefore = impl_->diagnostics.size();
            auto t = resolveType(ann);
            bool clean = impl_->diagnostics.size() == diagBefore &&
                         t && t->kind() != Type::Kind::Unknown;
            if (impl_->diagnostics.size() > diagBefore)
                impl_->diagnostics.resize(diagBefore);
            if (clean) fields[fname] = t;
        };
        // (a) class-body `name: T [= v]`
        for (auto& s : cd->body) {
            if (auto* ann = dynamic_cast<AnnAssignStmt*>(s.get()))
                if (auto* tgt = dynamic_cast<NameExpr*>(ann->target.get()))
                    tryRegister(tgt->name, ann->annotation.get());
        }
        // (b) constructor `self.x: T`. Method names are excluded (assigning to one
        // is a compile error elsewhere) and collected syntactically since ClassType::methods isn't populated yet.
        std::unordered_set<std::string> methodNames;
        for (auto& s : cd->body) {
            auto* fd = dynamic_cast<FunctionDecl*>(s.get());
            if (fd && fd->name != "__init__" && !fd->isConstructor &&
                !fd->isProperty)
                methodNames.insert(fd->name);
        }
        for (auto& s : cd->body) {
            auto* fd = dynamic_cast<FunctionDecl*>(s.get());
            if (!fd || !(fd->name == "__init__" || fd->isConstructor)) continue;
            for (auto& bs : fd->body) {
                auto* ann = dynamic_cast<AnnAssignStmt*>(bs.get());
                if (!ann) continue;
                auto* attr = dynamic_cast<AttributeExpr*>(ann->target.get());
                if (!attr) continue;
                auto* selfN = dynamic_cast<NameExpr*>(attr->object.get());
                if (selfN && selfN->name == "self" &&
                    !methodNames.count(attr->attribute))
                    tryRegister(attr->attribute, ann->annotation.get());
            }
        }
    }

    // Function-signature pre-pass: register module-level signatures before any
    // body check so forward calls resolve typed (an untyped forward call once caused a real string leak in http/server.dr's _encode_chunk family via a skipped owned-result drain). Unresolved signatures roll back to visit(FunctionDecl).
    for (auto& stmt : module.body) {
        auto* fd = dynamic_cast<FunctionDecl*>(stmt.get());
        if (!fd) continue;
        if (!fd->typeParams.empty()) continue;
        if (impl_->lookup(fd->name)) continue;
        size_t diagBefore = impl_->diagnostics.size();
        std::vector<std::shared_ptr<Type>> paramTypes;
        bool clean = true;
        for (auto& p : fd->params) {
            if (fd->isMethod && !fd->hasImplicitSelf && p.name == "self")
                continue;
            auto pt = resolveType(p.type.get());
            if (!pt || pt->kind() == Type::Kind::Unknown) clean = false;
            paramTypes.push_back(pt);
        }
        auto retType = resolveType(fd->returnType.get());
        if (fd->returnType && (!retType || retType->kind() == Type::Kind::Unknown))
            clean = false;
        if (impl_->diagnostics.size() > diagBefore) {
            impl_->diagnostics.resize(diagBefore);
            continue;
        }
        if (!clean) continue;
        auto externalRet = fd->isAsync ? std::static_pointer_cast<Type>(
                                             std::make_shared<TaskType>(retType))
                                       : retType;
        auto funcType = std::make_shared<FunctionType>(paramTypes, externalRet);
        fillFuncMeta(*funcType, fd->params, fd->isMethod, fd->hasImplicitSelf,
                     fd->isClassMethod);
        impl_->define(fd->name, funcType);
    }

    module.accept(*this);

    // D044 - drain the instantiation worklist to a fixpoint (stamping may enqueue
    // transitive instantiations); runs while the module scope is still active so stamped decls see module names.
    runMonomorphization();

    // Capture module-level exports before popping the scope
    if (!impl_->scopes.empty()) {
        impl_->cachedExports.clear();
        for (auto& [name, type] : impl_->scopes[0].bindings) {
            impl_->cachedExports[name] = type;
        }
    }

    impl_->popScope();
    return !hasErrors();
}

const std::vector<TypeDiagnostic>& TypeChecker::diagnostics() const {
    return impl_->diagnostics;
}

bool TypeChecker::hasErrors() const {
    for (const auto& d : impl_->diagnostics) {
        if (d.level == TypeDiagnostic::Level::Error) return true;
    }
    return false;
}

void TypeChecker::error(const SourceLocation& loc, const std::string& message) {
    impl_->diagnostics.push_back({TypeDiagnostic::Level::Error, loc, message});
}

void TypeChecker::warning(const SourceLocation& loc, const std::string& message) {
    impl_->diagnostics.push_back({TypeDiagnostic::Level::Warning, loc, message});
}

// Walks the ancestor chain for `member`: a field hit answers nullptr, a
// methods/methodOverloads hit answers the declaring class (mirrors visit(AttributeExpr)'s fields-then-methods order).
const ClassType* TypeChecker::findMethodOwner(const ClassType* cls,
                                              const std::string& member) const {
    for (const ClassType* c = cls; c; ) {
        if (c->fields.count(member)) return nullptr;
        if (c->methods.count(member) || c->methodOverloads.count(member))
            return c;
        c = (c->parentClass && c->parentClass->kind() == Type::Kind::Class)
                ? static_cast<const ClassType*>(c->parentClass.get())
                : nullptr;
    }
    return nullptr;
}

// D045 - member-access privacy; `declaring` is the ancestor class that
// declared `member`. Fails open when its source file is unknown so valid code is never spuriously rejected.
void TypeChecker::checkMemberPrivacy(const ClassType* declaring,
                                     const std::string& member,
                                     const SourceLocation& loc) {
    if (!declaring || declaring->definingFile.empty()) return;
    switch (classifyName(member)) {
        case NameVisibility::Public:
        case NameVisibility::ReservedDunder:
            return;  // public surface - always accessible
        case NameVisibility::Protected: {
            // Same package, OR the enclosing class is `declaring`/a subclass.
            if (impl_->packageKey(declaring->definingFile) == impl_->currentPackage)
                return;
            if (impl_->currentClass &&
                isSameOrSubclass(impl_->currentClass, declaring))
                return;
            error(loc, "'" + member + "' is protected; accessible only within " +
                  declaring->name + ", its subclasses, or the same package");
            return;
        }
        case NameVisibility::Private: {
            // Declaring class only - subclasses and same-package code excluded.
            if (impl_->currentClass &&
                isSameClass(impl_->currentClass, declaring))
                return;
            error(loc, "'" + member + "' is private to " + declaring->name +
                  "; subclasses and outside code cannot access it");
            return;
        }
    }
}

// D045 - module-name privacy at import/qualified access; keys on the
// EXPORTED name's shape. Fails open when the source module's file is unknown (e.g. an isolated stdlib check).
void TypeChecker::checkModuleNamePrivacy(const ModuleType& srcModule,
                                         const std::string& name,
                                         const SourceLocation& loc) {
    if (srcModule.filepath.empty()) return;
    switch (classifyName(name)) {
        case NameVisibility::Public:
        case NameVisibility::ReservedDunder:
            return;  // public / recognized metadata - importable
        case NameVisibility::Protected: {
            if (impl_->packageKey(srcModule.filepath) == impl_->currentPackage)
                return;
            error(loc, "'" + name + "' is module-private to '" + srcModule.name +
                  "'; it cannot be imported from another package (remove the "
                  "leading underscore to export it)");
            return;
        }
        case NameVisibility::Private: {
            if (canonFile(srcModule.filepath) == canonFile(impl_->currentFile))
                return;
            error(loc, "'" + name + "' is file-private to '" + srcModule.name +
                  "'; it is not importable, even by a module in the same package");
            return;
        }
    }
}

// D045 - an unrecognized `__x__` (reserved-shape) name is a compile error:
// unknown dunders are forbidden so no private-looking-but-public name can exist.
void TypeChecker::checkDunderDeclaration(const std::string& name, bool moduleLevel,
                                         const std::string& ownerDesc,
                                         const SourceLocation& loc) {
    if (classifyName(name) != NameVisibility::ReservedDunder) return;
    bool ok = moduleLevel ? isReservedModuleDunder(name) : isReservedDunder(name);
    if (!ok) {
        error(loc, "'" + name + "' is not a recognized " +
              (moduleLevel ? "module metadata name" : "special method") +
              (ownerDesc.empty() ? "" : " on " + ownerDesc) +
              "; double-underscore-dunder names are reserved");
    }
}

void TypeChecker::initBuiltinTypes() {
    impl_->intType = std::make_shared<PrimitiveType>(Type::Kind::Int);
    impl_->floatType = std::make_shared<PrimitiveType>(Type::Kind::Float);
    impl_->boolType = std::make_shared<PrimitiveType>(Type::Kind::Bool);
    impl_->strType = std::make_shared<PrimitiveType>(Type::Kind::Str);
    impl_->bytesType = std::make_shared<PrimitiveType>(Type::Kind::Bytes);
    impl_->noneType = std::make_shared<PrimitiveType>(Type::Kind::None_);
    impl_->anyType = std::make_shared<AnyType>();
    impl_->neverType = std::make_shared<NeverType>();
    impl_->unknownType = std::make_shared<UnknownType>();

    impl_->typeNames["int"] = impl_->intType;
    impl_->typeNames["intc"] = impl_->intType;  // C int FFI type (semantically int, maps to i32 in codegen)
    impl_->typeNames["float"] = impl_->floatType;
    impl_->typeNames["bool"] = impl_->boolType;
    impl_->typeNames["str"] = impl_->strType;
    impl_->typeNames["bytes"] = impl_->bytesType;
    impl_->typeNames["None"] = impl_->noneType;
    impl_->typeNames["Any"] = impl_->anyType;
    impl_->typeNames["Never"] = impl_->neverType;
    impl_->typeNames["object"] = impl_->anyType;
    impl_->typeNames["ptr"] = std::make_shared<PtrType>();
    // D025 - `type` (meta-type of class values) is modeled as Any: the concrete
    // class isn't statically known, so calls on it return Any (CodeGen tracks VarKind::Type separately).
    impl_->typeNames["type"] = impl_->anyType;
    // Default container types: bare dict -> dict[str, Any], list -> list[Any], etc.
    impl_->typeNames["dict"] = std::make_shared<DictType>(impl_->strType, impl_->anyType);
    impl_->typeNames["Dict"] = std::make_shared<DictType>(impl_->strType, impl_->anyType);
    impl_->typeNames["list"] = std::make_shared<ListType>(impl_->anyType);
    impl_->typeNames["List"] = std::make_shared<ListType>(impl_->anyType);
    impl_->typeNames["tuple"] = std::make_shared<TupleType>(std::vector<std::shared_ptr<Type>>{impl_->anyType});
    impl_->typeNames["Tuple"] = std::make_shared<TupleType>(std::vector<std::shared_ptr<Type>>{impl_->anyType});
    impl_->typeNames["set"] = std::make_shared<ListType>(impl_->anyType);
    impl_->typeNames["Set"] = std::make_shared<ListType>(impl_->anyType);
    // deque -> list[Any] for element typing (codegen routes ctor/methods via the
    // __Deque path keyed on the `deque(...)` ctor, independent of this type).
    impl_->typeNames["deque"] = std::make_shared<ListType>(impl_->anyType);
    // Bare `Task` resolves to Task[Any]; a local decl `t: Task = fire f()`
    // refines its result type from the concrete RHS (see AnnAssignStmt).
    impl_->typeNames["Task"] = std::make_shared<TaskType>(impl_->anyType);
    // `Lock` isn't seeded here; it's import-gated (`from threading import Lock`),
    // exported as a LockType by registerExternalModule and bound via FromImportStmt.
}

// Renders "name(int, str) -> bool" for conformance diagnostics.
static std::string contractSigToString(const std::string& name,
                                       const FunctionType& ft) {
    std::string s = name + "(";
    for (size_t i = 0; i < ft.paramTypes.size(); ++i) {
        if (i > 0) s += ", ";
        s += ft.paramTypes[i] ? ft.paramTypes[i]->toString() : "?";
    }
    s += ")";
    if (ft.returnType && ft.returnType->kind() != Type::Kind::None_ &&
        ft.returnType->kind() != Type::Kind::Unknown)
        s += " -> " + ft.returnType->toString();
    return s;
}

void TypeChecker::registerContracts(Module& module) {
    // Pass A - shells: register every contract NAME so composition and
    // signatures resolve independent of declaration order.
    std::vector<ContractDecl*> decls;
    for (auto& stmt : module.body) {
        auto* cd = dynamic_cast<ContractDecl*>(stmt.get());
        if (!cd) continue;
        if (impl_->contractByDecl.count(cd)) { decls.push_back(cd); continue; }
        if (impl_->typeNames.count(cd->name)) {
            error(cd->location(), "cannot declare contract '" + cd->name +
                  "': the name already refers to another type");
            continue;
        }
        auto ct = std::make_shared<ContractType>();
        ct->display = cd->name;
        if (!cd->methods.empty()) ct->atoms.push_back(cd);
        impl_->contractByDecl[cd] = ct;
        impl_->typeNames[cd->name] = ct;
        // Also bind in module scope so the contract exports like a class (`from mod
        // import Amazing` works); contracts aren't values, calling one is rejected at the call site.
        impl_->define(cd->name, ct);
        decls.push_back(cd);
    }
    // Pass B - own method signatures.
    for (auto* cd : decls) {
        auto ct = impl_->contractByDecl[cd];
        if (!ct || !ct->methods.empty()) continue;
        for (auto& m : cd->methods) {
            std::vector<std::shared_ptr<Type>> paramTypes;
            for (auto& p : m->params)
                paramTypes.push_back(resolveType(p.type.get()));
            auto ret = m->returnType ? resolveType(m->returnType.get())
                                     : impl_->noneType;
            auto ft = std::make_shared<FunctionType>(paramTypes, ret);
            fillFuncMeta(*ft, m->params, /*isMethod=*/true,
                         /*hasImplicitSelf=*/true, /*isClassMethod=*/false);
            if (ct->methods.count(m->name)) {
                error(m->location(), "contract '" + cd->name +
                      "' declares method '" + m->name + "' more than once");
                continue;
            }
            ct->methods[m->name] = ft;
            ct->methodOwner[m->name] = cd;
        }
    }
    // Pass C - composition merge (`type C(A, B) {}` unions the sets), recursive
    // with a cycle guard; bases may be local or imported (resolveContractRef covers both).
    std::set<const ContractDecl*> merging;
    std::function<void(ContractDecl*)> mergeBases = [&](ContractDecl* cd) {
        auto ct = impl_->contractByDecl[cd];
        if (!ct) return;
        if (merging.count(cd)) {
            error(cd->location(), "contract composition cycle through '" +
                  cd->name + "'");
            return;
        }
        merging.insert(cd);
        for (auto& baseName : cd->bases) {
            // Merge a sibling declared later first, so its own merge ran.
            for (auto* other : decls)
                if (other->name == baseName) { mergeBases(other); break; }
            auto base = resolveContractRef(baseName, cd->location(), true);
            if (!base) continue;
            for (auto* a : base->atoms)
                if (std::find(ct->atoms.begin(), ct->atoms.end(), a) ==
                    ct->atoms.end())
                    ct->atoms.push_back(a);
            for (auto& [mname, mtype] : base->methods) {
                auto exist = ct->methods.find(mname);
                if (exist == ct->methods.end()) {
                    ct->methods[mname] = mtype;
                    ct->methodOwner[mname] = base->methodOwner.at(mname);
                } else if (!exist->second->equals(*mtype)) {
                    error(cd->location(), "contract '" + cd->name +
                          "' composes conflicting signatures for method '" +
                          mname + "' - duplicate names must agree exactly");
                }
            }
        }
        std::sort(ct->atoms.begin(), ct->atoms.end());
        merging.erase(cd);
    };
    for (auto* cd : decls) mergeBases(cd);
    for (auto* cd : decls) {
        auto ct = impl_->contractByDecl[cd];
        if (ct && ct->methods.empty())
            error(cd->location(), "contract '" + cd->name +
                  "' declares no methods (an empty contract constrains "
                  "nothing)");
    }
}

std::shared_ptr<ContractType> TypeChecker::resolveContractRef(
    const std::string& name, const SourceLocation& loc, bool reportErrors) {
    std::shared_ptr<Type> found;
    auto it = impl_->typeNames.find(name);
    if (it != impl_->typeNames.end()) found = it->second;
    if (!found) found = impl_->lookup(name);
    if (!found) {
        if (reportErrors)
            error(loc, "unknown contract '" + name + "'");
        return nullptr;
    }
    if (found->kind() != Type::Kind::Contract) {
        if (reportErrors)
            error(loc, "'" + name + "' is not a contract (declare one with "
                  "`type " + name + " { def ... }`)");
        return nullptr;
    }
    return std::static_pointer_cast<ContractType>(found);
}

std::shared_ptr<ContractType> TypeChecker::resolveContractSet(
    const std::vector<std::string>& names, const SourceLocation& loc,
    bool reportErrors) {
    if (names.empty()) return nullptr;
    if (names.size() == 1) return resolveContractRef(names[0], loc, reportErrors);
    auto merged = std::make_shared<ContractType>();
    std::string display = "{";
    bool ok = true;
    for (size_t i = 0; i < names.size(); ++i) {
        if (i > 0) display += ", ";
        display += names[i];
        auto ct = resolveContractRef(names[i], loc, reportErrors);
        if (!ct) { ok = false; continue; }
        for (auto* a : ct->atoms)
            if (std::find(merged->atoms.begin(), merged->atoms.end(), a) ==
                merged->atoms.end())
                merged->atoms.push_back(a);
        for (auto& [mname, mtype] : ct->methods) {
            auto exist = merged->methods.find(mname);
            if (exist == merged->methods.end()) {
                merged->methods[mname] = mtype;
                merged->methodOwner[mname] = ct->methodOwner.at(mname);
            } else if (!exist->second->equals(*mtype)) {
                if (reportErrors)
                    error(loc, "contract set combines conflicting signatures "
                          "for method '" + mname + "'");
                ok = false;
            }
        }
    }
    if (!ok) return nullptr;
    display += "}";
    merged->display = display;
    std::sort(merged->atoms.begin(), merged->atoms.end());
    return merged;
}

std::vector<std::string> TypeChecker::contractConformanceProblems(
    const ClassType& cls, const ContractType& ct) {
    std::vector<std::string> problems;
    // Missing/mismatched returns compare as None so `def m()` (no annotation)
    // on either side means "returns None".
    auto norm = [&](const std::shared_ptr<Type>& t) -> std::shared_ptr<Type> {
        return (!t || t->kind() == Type::Kind::Unknown)
                   ? std::static_pointer_cast<Type>(impl_->noneType) : t;
    };
    auto sigMatches = [&](const Type& have, const FunctionType& want) {
        if (have.kind() != Type::Kind::Function) return false;
        auto& hf = static_cast<const FunctionType&>(have);
        if (hf.paramTypes.size() != want.paramTypes.size()) return false;
        for (size_t i = 0; i < hf.paramTypes.size(); ++i) {
            auto a = norm(hf.paramTypes[i]);
            auto b = norm(want.paramTypes[i]);
            if (!a->equals(*b)) return false;
        }
        return norm(hf.returnType)->equals(*norm(want.returnType));
    };
    for (auto& [mname, mtype] : ct.methods) {
        auto& want = static_cast<FunctionType&>(*mtype);
        // Flattened lookup: this class, then the parent chain.
        std::shared_ptr<Type> found;
        const std::vector<std::shared_ptr<Type>>* overloads = nullptr;
        for (const ClassType* cur = &cls; cur; ) {
            auto mIt = cur->methods.find(mname);
            if (mIt != cur->methods.end()) {
                found = mIt->second;
                auto oIt = cur->methodOverloads.find(mname);
                if (oIt != cur->methodOverloads.end()) overloads = &oIt->second;
                break;
            }
            cur = (cur->parentClass &&
                   cur->parentClass->kind() == Type::Kind::Class)
                      ? static_cast<const ClassType*>(cur->parentClass.get())
                      : nullptr;
        }
        if (!found) {
            problems.push_back("missing method '" +
                               contractSigToString(mname, want) + "'");
            continue;
        }
        bool ok = false;
        if (overloads) {
            for (auto& o : *overloads)
                if (o && sigMatches(*o, want)) { ok = true; break; }
        } else {
            ok = sigMatches(*found, want);
        }
        if (!ok)
            problems.push_back("method '" + mname +
                               "' does not match the contract signature '" +
                               contractSigToString(mname, want) + "'");
    }
    std::sort(problems.begin(), problems.end());
    return problems;
}

std::shared_ptr<Type> TypeChecker::resolveType(TypeExpr* typeExpr) {
    if (!typeExpr) return impl_->unknownType;

    if (auto* named = dynamic_cast<NamedTypeExpr*>(typeExpr)) {
        // Dotted form (`mod.Class`) walks ModuleType nodes like visit(AttributeExpr)
        // does for values: first segment is an imported module, middle segments are submodules, leaf is an exported ClassType.
        auto dot = named->name.find('.');
        if (dot != std::string::npos) {
            std::string head = named->name.substr(0, dot);
            std::string tail = named->name.substr(dot + 1);
            auto looked = impl_->lookup(head);
            if (!looked || looked->kind() != Type::Kind::Module) {
                error(named->location(), "unknown type '" + named->name + "'");
                return impl_->unknownType;
            }
            auto current = std::static_pointer_cast<ModuleType>(looked);
            while (true) {
                auto nextDot = tail.find('.');
                std::string seg = (nextDot == std::string::npos) ? tail : tail.substr(0, nextDot);
                if (nextDot == std::string::npos) {
                    auto expIt = current->exports.find(seg);
                    if (expIt == current->exports.end() ||
                        expIt->second->kind() != Type::Kind::Class) {
                        error(named->location(),
                              "unknown type '" + named->name + "'");
                        return impl_->unknownType;
                    }
                    auto cls = std::static_pointer_cast<ClassType>(expIt->second);
                    return std::make_shared<InstanceType>(cls);
                }
                auto subIt = current->submodules.find(seg);
                if (subIt == current->submodules.end()) {
                    error(named->location(), "unknown type '" + named->name + "'");
                    return impl_->unknownType;
                }
                current = subIt->second;
                tail = tail.substr(nextDot + 1);
            }
        }
        // D044 - a bare type-param name (`T`) resolves to its TypeVarType; checked
        // before typeNames so it shadows any same-named outer type inside the template body.
        if (auto tv = lookupTypeParam(named->name)) return tv;
        auto it = impl_->typeNames.find(named->name);
        if (it != impl_->typeNames.end()) return it->second;
        auto looked = impl_->lookup(named->name);
        if (looked && looked->kind() == Type::Kind::Class) {
            auto cls = std::static_pointer_cast<ClassType>(looked);
            return std::make_shared<InstanceType>(cls);
        }
        // ADR 054 - an imported contract binds in scope, not typeNames; a bare
        // contract name in type position is the contract itself.
        if (looked && looked->kind() == Type::Kind::Contract) return looked;
        error(named->location(), "unknown type '" + named->name + "'");
        return impl_->unknownType;
    }

    // ADR 054 - braced contract set `{Amazing, Speaker}` in annotation or
    // bound position: one merged intersection type.
    if (auto* cs = dynamic_cast<ContractSetTypeExpr*>(typeExpr)) {
        auto ct = resolveContractSet(cs->names, cs->location(), true);
        return ct ? std::static_pointer_cast<Type>(ct) : impl_->unknownType;
    }

    if (auto* generic = dynamic_cast<GenericTypeExpr*>(typeExpr)) {
        auto baseName = dynamic_cast<NamedTypeExpr*>(generic->base.get());
        if (!baseName) {
            error(generic->location(), "invalid generic type");
            return impl_->unknownType;
        }
        if (baseName->name == "list" && generic->typeArgs.size() == 1) {
            return std::make_shared<ListType>(resolveType(generic->typeArgs[0].get()));
        }
        if (baseName->name == "dict" && generic->typeArgs.size() == 2) {
            return std::make_shared<DictType>(
                resolveType(generic->typeArgs[0].get()),
                resolveType(generic->typeArgs[1].get()));
        }
        if (baseName->name == "tuple") {
            std::vector<std::shared_ptr<Type>> elems;
            for (auto& arg : generic->typeArgs) {
                elems.push_back(resolveType(arg.get()));
            }
            return std::make_shared<TupleType>(std::move(elems));
        }
        if (baseName->name == "set" && generic->typeArgs.size() == 1) {
            // Treat set[T] as a list[T] for now (simplified)
            return std::make_shared<ListType>(resolveType(generic->typeArgs[0].get()));
        }
        if (baseName->name == "deque" && generic->typeArgs.size() == 1) {
            // deque[T] - runtime deque; typed as list[T] for element checking
            // (codegen routes the deque ctor/methods through the __Deque path).
            return std::make_shared<ListType>(resolveType(generic->typeArgs[0].get()));
        }
        if (baseName->name == "Task" && generic->typeArgs.size() == 1) {
            return std::make_shared<TaskType>(resolveType(generic->typeArgs[0].get()));
        }
        // D044 - a user generic class instantiated at concrete args (`Box[int]`);
        // resolve each arg then build/queue the monomorphic instantiation for stamping.
        if (auto gIt = impl_->genericClasses.find(baseName->name);
            gIt != impl_->genericClasses.end()) {
            std::vector<std::shared_ptr<Type>> args;
            for (auto& a : generic->typeArgs) args.push_back(resolveType(a.get()));
            return instantiateGenericClass(gIt->second, std::move(args),
                                           generic->location());
        }
        error(generic->location(), "unknown generic type '" + baseName->name + "'");
        return impl_->unknownType;
    }

    if (auto* opt = dynamic_cast<OptionalTypeExpr*>(typeExpr)) {
        auto inner = resolveType(opt->inner.get());
        std::vector<std::shared_ptr<Type>> types = {inner, impl_->noneType};
        return std::make_shared<UnionType>(std::move(types));
    }

    if (auto* union_ = dynamic_cast<UnionTypeExpr*>(typeExpr)) {
        std::vector<std::shared_ptr<Type>> types;
        for (auto& t : union_->types) {
            types.push_back(resolveType(t.get()));
        }
        return std::make_shared<UnionType>(std::move(types));
    }

    if (auto* callable = dynamic_cast<CallableTypeExpr*>(typeExpr)) {
        std::vector<std::shared_ptr<Type>> params;
        for (auto& p : callable->paramTypes) {
            params.push_back(resolveType(p.get()));
        }
        auto ret = resolveType(callable->returnType.get());
        return std::make_shared<FunctionType>(std::move(params), ret);
    }

    if (auto* tuple = dynamic_cast<TupleTypeExpr*>(typeExpr)) {
        std::vector<std::shared_ptr<Type>> elems;
        for (auto& e : tuple->elementTypes) {
            elems.push_back(resolveType(e.get()));
        }
        return std::make_shared<TupleType>(std::move(elems));
    }

    return impl_->unknownType;
}

std::shared_ptr<Type> TypeChecker::inferType(Expr* expr) {
    if (!expr) return impl_->unknownType;
    // Visit the expression to set its type
    expr->accept(*this);
    return expr->type ? expr->type : impl_->unknownType;
}

void TypeChecker::visit(NamedTypeExpr&) {}
void TypeChecker::visit(GenericTypeExpr&) {}
void TypeChecker::visit(OptionalTypeExpr&) {}
void TypeChecker::visit(UnionTypeExpr&) {}
void TypeChecker::visit(CallableTypeExpr&) {}
void TypeChecker::visit(TupleTypeExpr&) {}

void TypeChecker::visit(IntegerLiteral& node) {
    node.type = impl_->intType;
}

void TypeChecker::visit(FloatLiteral& node) {
    node.type = impl_->floatType;
}

void TypeChecker::visit(StringLiteral& node) {
    if (node.isBytes) {
        node.type = impl_->bytesType;
    } else {
        node.type = impl_->strType;
    }
    for (auto& part : node.fstringParts) {
        if (part.kind == FStringPart::Kind::Expression && part.expr) {
            part.expr->accept(*this);
        }
    }
}

void TypeChecker::visit(TemplateExpr& node) {
    node.type = resolveTemplateContentType(node.contentType, node.location());
    // Walk each interpolation so it flows at its native type like an f-string
    // part (previously invisible: a `!{p[0]}` tuple-subscript lowered as a raw i64 pointer). Block interpolations are visited as statements so loop vars scope correctly.
    for (auto& part : node.templateParts) {
        if (part.kind == TemplatePart::Kind::Interpolation) {
            if (part.expr) part.expr->accept(*this);
        } else if (part.kind == TemplatePart::Kind::Block) {
            for (auto& stmt : part.blockStmts) {
                if (stmt) stmt->accept(*this);
            }
        }
    }
}

void TypeChecker::visit(TemplateFileExpr& node) {
    node.type = resolveTemplateContentType(node.contentType, node.location());
}

std::shared_ptr<Type> TypeChecker::resolveTemplateContentType(
        const std::string& contentType, const SourceLocation& loc) {
    if (contentType.empty()) {
        return impl_->strType;
    }

    // Resolve the content type name to a ClassType (typeNames maps register class
    // declarations during visit(ClassDecl); the scope chain holds the definition).
    std::shared_ptr<ClassType> cls;
    auto it = impl_->typeNames.find(contentType);
    if (it != impl_->typeNames.end() && it->second->kind() == Type::Kind::Instance) {
        cls = std::static_pointer_cast<InstanceType>(it->second)->classType;
    } else {
        auto looked = impl_->lookup(contentType);
        if (looked && looked->kind() == Type::Kind::Class) {
            cls = std::static_pointer_cast<ClassType>(looked);
        } else if (looked && looked->kind() == Type::Kind::Instance) {
            cls = std::static_pointer_cast<InstanceType>(looked)->classType;
        } else {
            error(loc, "template[" + contentType + "]: '" + contentType +
                  "' is not a class type");
            return impl_->strType;
        }
    }

    // D037 dispatch hook: StructTemplate shares the `template[X]` surface with
    // string-mode (D017) but lowers to a constructor-call chain; reject explicitly until that path ships, rather than silently falling through to string mode.
    if (classExtendsByName(cls.get(), "StructTemplate")) {
        error(loc, "template[" + contentType + "]: StructTemplate (structured "
              "templates) is not yet implemented");
        return std::make_shared<InstanceType>(cls);
    }

    // Template protocol check (D017 Phase 4): the content type MUST extend
    // `Template` so the auto-escape and instance-wrap codegen paths have well-defined symbols to resolve.
    if (!classExtendsByName(cls.get(), "Template")) {
        error(loc, "template[" + contentType + "]: '" + contentType +
              "' must extend Template");
        return std::make_shared<InstanceType>(cls);
    }

    return std::make_shared<InstanceType>(cls);
}

void TypeChecker::visit(BooleanLiteral& node) {
    node.type = impl_->boolType;
}

void TypeChecker::visit(NoneLiteral& node) {
    node.type = impl_->noneType;
}

void TypeChecker::visit(NameExpr& node) {
    // `__doc__` binds to the module's docstring, type Optional[str]; CodeGen
    // lowers it like `<mod>.__doc__`, and Sema pre-defines it so the bare reference doesn't error.
    if (node.name == "__doc__") {
        std::vector<std::shared_ptr<Type>> opt = {impl_->strType, impl_->noneType};
        node.type = std::make_shared<UnionType>(std::move(opt));
        return;
    }
    auto type = impl_->lookup(node.name);
    if (type) {
        node.type = type;
    } else {
        // Name might be a builtin type name used as value (like True/False/None)
        node.type = impl_->unknownType;
    }
    // E11 (docs/002 2.7): dub is a DEEP copy, so it only exists for payloads
    // that can honestly be copied (scalars, immutable str/bytes, dubable containers); resources/Any/closures/instances refuse.
    if (node.isDubMarked && node.type) {
        std::string why;
        if (!typeIsDubable(node.type.get(), why))
            error(node.location(),
                  "'" + node.name + "' is not dubable: " + why);
    }
}

bool TypeChecker::typeIsDubable(const Type* t, std::string& why) {
    if (!t) { why = "its type is unknown"; return false; }
    switch (t->kind()) {
        case Type::Kind::Int:
        case Type::Kind::Float:
        case Type::Kind::Bool:
        case Type::Kind::None_:
        case Type::Kind::Str:
        case Type::Kind::Bytes:
            return true;
        case Type::Kind::List: {
            auto& lt = static_cast<const ListType&>(*t);
            if (!lt.elementType) { why = "its element type is unknown"; return false; }
            return typeIsDubable(lt.elementType.get(), why);
        }
        case Type::Kind::Dict: {
            auto& dt = static_cast<const DictType&>(*t);
            if (!dt.valueType) { why = "its value type is unknown"; return false; }
            return typeIsDubable(dt.valueType.get(), why);
        }
        case Type::Kind::Set:
            return true;  // set elements are immutable (i64 / str)
        case Type::Kind::Tuple: {
            auto& tt = static_cast<const TupleType&>(*t);
            for (auto& e : tt.elementTypes) {
                if (!e) continue;
                auto k = e->kind();
                // A tuple dub lowers to an identity retain (immutable spine),
                // so every element must itself be immutable.
                if (k != Type::Kind::Int && k != Type::Kind::Float &&
                    k != Type::Kind::Bool && k != Type::Kind::Str &&
                    k != Type::Kind::Bytes && k != Type::Kind::None_) {
                    why = "a tuple holding a mutable element ('" +
                          e->toString() + "') cannot be identity-copied";
                    return false;
                }
            }
            return true;
        }
        case Type::Kind::Lock:
            why = "a raw OS mutex cannot be copied; wrap the resource in a "
                  "class with an own field";
            return false;
        case Type::Kind::Function:
            why = "a closure's captured environment has identity";
            return false;
        case Type::Kind::Instance:
            why = "class dub is not in v1; copy the fields you need";
            return false;
        default:
            why = "its concrete payload is not statically known ('" +
                  t->toString() + "')";
            return false;
    }
}

void TypeChecker::visit(BinaryExpr& node) {
    auto leftType = inferType(node.left.get());
    auto rightType = inferType(node.right.get());

    // Bounds - a bounded `T` unwraps to its bound `B` for operator resolution; an
    // unbounded `T` stays a TypeVar (==/!= allowed, ordering/arith rejected) until monomorphization substitutes the concrete arg.
    if (leftType && leftType->kind() == Type::Kind::TypeVar)
        if (auto b = static_cast<TypeVarType&>(*leftType).bound) leftType = b;
    if (rightType && rightType->kind() == Type::Kind::TypeVar)
        if (auto b = static_cast<TypeVarType&>(*rightType).bound) rightType = b;

    auto op = node.op.type();

    // Arithmetic operators
    if (op == TokenType::PLUS || op == TokenType::MINUS ||
        op == TokenType::STAR || op == TokenType::SLASH ||
        op == TokenType::PERCENT || op == TokenType::POWER ||
        op == TokenType::DOUBLE_SLASH) {

        if (op == TokenType::PLUS &&
            leftType->kind() == Type::Kind::Str && rightType->kind() == Type::Kind::Str) {
            node.type = impl_->strType;
            return;
        }

        if (op == TokenType::PLUS &&
            leftType->kind() == Type::Kind::Bytes && rightType->kind() == Type::Kind::Bytes) {
            node.type = impl_->bytesType;
            return;
        }

        // list + list -> list (Python's `a + b`). Dragon lists are monomorphic, so
        // element types must match; an empty-list literal adopts the other side's, and a genuine mismatch falls to the unsupported-operand error below.
        if (op == TokenType::PLUS &&
            leftType->kind() == Type::Kind::List && rightType->kind() == Type::Kind::List) {
            auto le = static_cast<ListType&>(*leftType).elementType;
            auto re = static_cast<ListType&>(*rightType).elementType;
            bool lUnknown = !le || le->kind() == Type::Kind::Unknown;
            bool rUnknown = !re || re->kind() == Type::Kind::Unknown;
            if (lUnknown) { node.type = rightType; return; }
            if (rUnknown) { node.type = leftType; return; }
            // A list[Any] on either side absorbs the other (list[T] <: list[Any]); the
            // result is list[Any] and the runtime concat boxes the concrete side's elements.
            if (le->kind() == Type::Kind::Any) { node.type = leftType; return; }
            if (re->kind() == Type::Kind::Any) { node.type = rightType; return; }
            if (le->toString() == re->toString()) { node.type = leftType; return; }
            // else: element-type mismatch -> fall through to the operand error.
        }

        if (op == TokenType::STAR) {
            if ((leftType->kind() == Type::Kind::Str && rightType->isSubtypeOf(*impl_->intType)) ||
                (leftType->isSubtypeOf(*impl_->intType) && rightType->kind() == Type::Kind::Str)) {
                node.type = impl_->strType;
                return;
            }
            if ((leftType->kind() == Type::Kind::Bytes && rightType->isSubtypeOf(*impl_->intType)) ||
                (leftType->isSubtypeOf(*impl_->intType) && rightType->kind() == Type::Kind::Bytes)) {
                node.type = impl_->bytesType;
                return;
            }
            if (leftType->kind() == Type::Kind::List && rightType->isSubtypeOf(*impl_->intType)) {
                node.type = leftType;
                return;
            }
            if (leftType->isSubtypeOf(*impl_->intType) && rightType->kind() == Type::Kind::List) {
                node.type = rightType;
                return;
            }
        }

        // Any operand -> the result is a box resolved at runtime by dragon_box_binop,
        // typed Any (not a concrete numeric). Must precede the numeric check: AnyType::isSubtypeOf(int) is true, so `Any + Any` would otherwise mistype as int.
        if (leftType->kind() == Type::Kind::Any || rightType->kind() == Type::Kind::Any) {
            node.type = impl_->anyType;
            return;
        }

        bool leftNum = leftType->isSubtypeOf(*impl_->intType) || leftType->kind() == Type::Kind::Float;
        bool rightNum = rightType->isSubtypeOf(*impl_->intType) || rightType->kind() == Type::Kind::Float;

        if (leftNum && rightNum) {
            // True division always returns float
            if (op == TokenType::SLASH) {
                node.type = impl_->floatType;
                return;
            }
            // Floor division returns int if both int, else float
            if (op == TokenType::DOUBLE_SLASH) {
                if (leftType->kind() == Type::Kind::Float || rightType->kind() == Type::Kind::Float) {
                    node.type = impl_->floatType;
                } else {
                    node.type = impl_->intType;
                }
                return;
            }
            if (leftType->kind() == Type::Kind::Float || rightType->kind() == Type::Kind::Float) {
                node.type = impl_->floatType;
            } else {
                node.type = impl_->intType;
            }
            return;
        }

        // Unknown types -- don't error
        if (leftType->kind() == Type::Kind::Unknown || rightType->kind() == Type::Kind::Unknown ||
            leftType->kind() == Type::Kind::Any || rightType->kind() == Type::Kind::Any) {
            node.type = impl_->unknownType;
            return;
        }

        // Class instances - allow through for dunder dispatch at CodeGen level
        if (leftType->kind() == Type::Kind::Instance || rightType->kind() == Type::Kind::Instance) {
            node.type = impl_->unknownType;
            return;
        }

        error(node.location(), "unsupported operand types for " + node.op.lexeme() +
              ": '" + leftType->toString() + "' and '" + rightType->toString() + "'");
        node.type = impl_->unknownType;
        return;
    }

    // Equality (==, !=): allowed on an unbounded type parameter (D044 permits
    // ==/!= comparison of `T` values).
    if (op == TokenType::EQUAL_EQUAL || op == TokenType::NOT_EQUAL) {
        node.type = impl_->boolType;
        return;
    }
    // Relational ordering requires provable ordering semantics, which an
    // unbounded `T` lacks (D044 disallows `t < t` without a bound); reject before defaulting to bool.
    if (op == TokenType::LESS || op == TokenType::LESS_EQUAL ||
        op == TokenType::GREATER || op == TokenType::GREATER_EQUAL) {
        if ((leftType && leftType->kind() == Type::Kind::TypeVar) ||
            (rightType && rightType->kind() == Type::Kind::TypeVar)) {
            error(node.location(), "cannot apply ordering operator to a value of "
                  "unbounded type parameter; declare a bound (`T: SomeClass`) "
                  "whose type supports ordering");
            node.type = impl_->unknownType;
            return;
        }
        node.type = impl_->boolType;
        return;
    }

    // Logical operators (and, or)
    if (op == TokenType::AND || op == TokenType::OR) {
        // Python semantics: 'and' returns first falsy or last, 'or' returns first truthy or last
        // For type checking, we use the union or right type
        if (op == TokenType::OR) {
            node.type = leftType; // Simplified: assume same type
        } else {
            node.type = rightType;
        }
        return;
    }

    // Bitwise operators
    if (op == TokenType::AMPERSAND || op == TokenType::PIPE ||
        op == TokenType::CARET || op == TokenType::LEFT_SHIFT ||
        op == TokenType::RIGHT_SHIFT) {
        if (leftType->isSubtypeOf(*impl_->intType) && rightType->isSubtypeOf(*impl_->intType)) {
            node.type = impl_->intType;
            return;
        }
        if (leftType->kind() == Type::Kind::Unknown || rightType->kind() == Type::Kind::Unknown ||
            leftType->kind() == Type::Kind::Any || rightType->kind() == Type::Kind::Any) {
            node.type = impl_->unknownType;
            return;
        }
        error(node.location(), "unsupported operand types for " + node.op.lexeme() +
              ": '" + leftType->toString() + "' and '" + rightType->toString() + "'");
        node.type = impl_->unknownType;
        return;
    }

    // 'in', 'not in', 'is', 'is not'
    if (op == TokenType::IN || op == TokenType::IS ||
        op == TokenType::NOT_IN || op == TokenType::IS_NOT ||
        op == TokenType::NOT) {
        node.type = impl_->boolType;
        return;
    }

    node.type = impl_->unknownType;
}

void TypeChecker::visit(ChainedCompExpr& node) {
    for (auto& op : node.operands) inferType(op.get());
    node.type = std::make_shared<PrimitiveType>(PrimitiveType::Kind::Bool);
}

void TypeChecker::visit(WalrusExpr& node) {
    auto valType = inferType(node.value.get());
    // Reject an inferred `:=` binding whose container element type is unresolved
    // (empty literal): it would silently become a de-facto Any container, boxing every insert. An annotated `x: list[int] = []` (AnnAssign) is unaffected.
    auto unresolved = [](const std::shared_ptr<Type>& t) -> const char* {
        if (!t) return nullptr;
        if (t->kind() == Type::Kind::List || t->kind() == Type::Kind::Set) {
            auto& lt = static_cast<ListType&>(*t);  // sets are ListType-backed
            if (lt.elementType && lt.elementType->kind() == Type::Kind::Unknown)
                return "list";
        } else if (t->kind() == Type::Kind::Dict) {
            auto& dt = static_cast<DictType&>(*t);
            if ((dt.keyType && dt.keyType->kind() == Type::Kind::Unknown) ||
                (dt.valueType && dt.valueType->kind() == Type::Kind::Unknown))
                return "dict";
        }
        return nullptr;
    };
    if (const char* what = unresolved(valType)) {
        std::string hint = (std::string(what) == "dict")
                               ? node.name + ": dict[str, int] = {}"
                               : node.name + ": list[int] = []";
        error(node.location(),
              "cannot infer the element type of '" + node.name +
              "' (an empty or mixed-type literal) - annotate it (e.g. `" + hint + "`)");
    }
    node.type = valType;
}

void TypeChecker::visit(UnaryExpr& node) {
    auto operandType = inferType(node.operand.get());
    // Bounds - unwrap a bounded type parameter to its bound (see visit(BinaryExpr)).
    if (operandType && operandType->kind() == Type::Kind::TypeVar)
        if (auto b = static_cast<TypeVarType&>(*operandType).bound) operandType = b;
    auto op = node.op.type();

    if (op == TokenType::MINUS || op == TokenType::PLUS) {
        if (operandType->isSubtypeOf(*impl_->intType)) {
            node.type = impl_->intType;
        } else if (operandType->kind() == Type::Kind::Float) {
            node.type = impl_->floatType;
        } else if (operandType->kind() == Type::Kind::Unknown || operandType->kind() == Type::Kind::Any) {
            node.type = impl_->unknownType;
        } else if (operandType->kind() == Type::Kind::Instance) {
            node.type = impl_->unknownType;
        } else {
            error(node.location(), "bad operand type for unary " + node.op.lexeme() +
                  ": '" + operandType->toString() + "'");
            node.type = impl_->unknownType;
        }
        return;
    }

    if (op == TokenType::NOT) {
        node.type = impl_->boolType;
        return;
    }

    if (op == TokenType::TILDE) {
        if (operandType->isSubtypeOf(*impl_->intType)) {
            node.type = impl_->intType;
        } else if (operandType->kind() == Type::Kind::Unknown || operandType->kind() == Type::Kind::Any) {
            node.type = impl_->unknownType;
        } else {
            error(node.location(), "bad operand type for unary ~: '" + operandType->toString() + "'");
            node.type = impl_->unknownType;
        }
        return;
    }

    node.type = impl_->unknownType;
}


} // namespace dragon
