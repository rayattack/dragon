#ifndef DRAGON_TYPE_CHECKER_H
#define DRAGON_TYPE_CHECKER_H

#include "dragon/AST.h"
#include "dragon/Sema.h"
#include <memory>
#include <string>
#include <vector>
#include <unordered_map>
#include <set>

namespace dragon {

/// Base class for all types
class Type {
public:
    enum class Kind {
        Int, Float, Bool, Str, Bytes, None_,
        List, Dict, Set, Tuple, Function, Task, Lock,
        Class, Instance, Any, Never, Union, Optional, TypeVar, Unknown,
        Ptr,  // Raw C pointer (void* / i8*), used in extern "C" FFI
        Module  // Imported module - base of x.y.z attribute chains, never a runtime value
    };
    virtual ~Type() = default;
    virtual Kind kind() const = 0;
    virtual std::string toString() const = 0;
    virtual bool equals(const Type& other) const = 0;
    virtual bool isSubtypeOf(const Type& other) const;
    bool isAssignableTo(const Type& other) const;
};

/// Primitive types: int, float, bool, str, bytes, None
class PrimitiveType : public Type {
public:
    explicit PrimitiveType(Kind k) : kind_(k) {}
    Kind kind() const override { return kind_; }
    std::string toString() const override;
    bool equals(const Type& other) const override;
    bool isSubtypeOf(const Type& other) const override;
private:
    Kind kind_;
};

/// List type: list[T]
class ListType : public Type {
public:
    std::shared_ptr<Type> elementType;
    explicit ListType(std::shared_ptr<Type> elem) : elementType(std::move(elem)) {}
    Kind kind() const override { return Kind::List; }
    std::string toString() const override;
    bool equals(const Type& other) const override;
    bool isSubtypeOf(const Type& other) const override;
};

/// Task type: Task[T] -- handle from `fire` / `async def`; erases to ptr
/// (DragonVThread*) at LLVM. T is the result type recovered by await/.join().
class TaskType : public Type {
public:
    std::shared_ptr<Type> resultType;
    explicit TaskType(std::shared_ptr<Type> result) : resultType(std::move(result)) {}
    Kind kind() const override { return Kind::Task; }
    std::string toString() const override;
    bool equals(const Type& other) const override;
    bool isSubtypeOf(const Type& other) const override;
};

/// Lock type: `threading.Lock` mutex handle. Like `Task`, it is an intrinsic
/// that erases to a bare `ptr` (pthread_mutex_t*) at LLVM - zero class-instance
/// or GC overhead. It is annotatable (`lock: Lock = Lock()`) but is not a
/// user-defined class; `Lock()`, `.acquire()/.release()/.try_lock()` and
/// `with lock { }` lower directly to `dragon_lock_*` runtime calls.
class LockType : public Type {
public:
    Kind kind() const override { return Kind::Lock; }
    std::string toString() const override;
    bool equals(const Type& other) const override;
    bool isSubtypeOf(const Type& other) const override;
};

/// Dict type: dict[K, V]
class DictType : public Type {
public:
    std::shared_ptr<Type> keyType;
    std::shared_ptr<Type> valueType;
    DictType(std::shared_ptr<Type> k, std::shared_ptr<Type> v)
        : keyType(std::move(k)), valueType(std::move(v)) {}
    Kind kind() const override { return Kind::Dict; }
    std::string toString() const override;
    bool equals(const Type& other) const override;
    bool isSubtypeOf(const Type& other) const override;
};

/// Tuple type: tuple[T1, T2, ...]
class TupleType : public Type {
public:
    std::vector<std::shared_ptr<Type>> elementTypes;
    explicit TupleType(std::vector<std::shared_ptr<Type>> elems)
        : elementTypes(std::move(elems)) {}
    Kind kind() const override { return Kind::Tuple; }
    std::string toString() const override;
    bool equals(const Type& other) const override;
};

/// Function type: (T1, T2, ...) -> R
class FunctionType : public Type {
public:
    std::vector<std::shared_ptr<Type>> paramTypes;
    std::shared_ptr<Type> returnType;
    // Call-validation metadata (M1/M2), populated only for FunctionTypes built
    // from a user FunctionDecl (ctor/method/free function). When `paramNames` is
    // empty the checker has no metadata and conservatively SKIPS kwarg/arity
    // validation - so every inline/builtin FunctionType (2-arg ctor) degrades
    // safely to the prior behavior. For methods these EXCLUDE `self`, matching
    // paramTypes. `requiredParams` counts params with no default and not *args/
    // **kwargs; `hasVarArg` is set if any param is *args/**kwargs.
    std::vector<std::string> paramNames;
    size_t requiredParams = 0;
    bool hasVarArg = false;
    // True if the signature has a `**kwargs` param (a superset of hasVarArg,
    // which is set for either `*args` OR `**kwargs`). Lets the call validator
    // tell "unknown keyword absorbed by **kwargs" from "unknown keyword is an
    // error" for a `*args`-only callee.
    bool hasKwArg = false;
    // True once fillFuncMeta has run - distinguishes a genuinely 0-parameter
    // function (empty paramNames, but checkable) from a FunctionType built
    // without metadata (builtins / Callable annotations - never checked).
    bool hasArgMeta = false;
    // True if this signature is a class method. A bare-name call resolving to a
    // method type is a name-resolution artifact (methods are only callable via
    // `self.m()`/`obj.m()`; a class-scope method can shadow a same-named module
    // function), so bare-name arity checks must skip it.
    bool isMethod = false;
    FunctionType(std::vector<std::shared_ptr<Type>> params, std::shared_ptr<Type> ret)
        : paramTypes(std::move(params)), returnType(std::move(ret)) {}
    Kind kind() const override { return Kind::Function; }
    std::string toString() const override;
    bool equals(const Type& other) const override;
};

/// Class type (the class itself, not instances)
class ClassType : public Type {
public:
    std::string name;
    std::unordered_map<std::string, std::shared_ptr<Type>> methods;
    // ADR 010 method overloading: when a class declares >1 method with the same
    // name, every overload's FunctionType is collected here in class-body order
    // (the `methods` entry holds only the last, for the single-method fast path
    // and non-call uses). A method-call site with the name present here resolves
    // by arity + parameter types to one overload (compile-time; the emitted call
    // is a direct call - zero runtime cost). Names with a single definition are
    // never inserted, so the common path is untouched.
    std::unordered_map<std::string, std::vector<std::shared_ptr<Type>>> methodOverloads;
    std::unordered_map<std::string, std::shared_ptr<Type>> fields;
    // Every field NAME the class declares (class-body `x: T` and every `self.X =`
    // / `self.X: T =` across all methods), collected syntactically in a module
    // pre-pass BEFORE any body is type-checked. Existence is a name question, not
    // a type question, so this is complete up front - letting visit(AttributeExpr)
    // reject a genuinely-undefined member immediately, even on a forward/cross-
    // class reference, without waiting for (or polluting) the precise `fields`
    // map that codegen lowers struct slots from. TYPE-CHECK ONLY - codegen never
    // reads this; `fields` remains the single source of truth for layout.
    std::set<std::string> declaredFieldNames;
    // Canonical positional field order for `match` class-pattern destructuring
    // (`case Point(x, y)`). Parent fields first, then this class's own fields in
    // declaration order (AST `instanceFieldOrder`). Filled in the class pre-pass;
    // CodeGen builds the identical list from the same helper, so the
    // position->field-name mapping agrees across stages.
    std::vector<std::string> fieldOrder;
    std::shared_ptr<Type> parentClass;
    // D045 - the module that DECLARES this class, so member-access privacy can
    // compute the declaring class's package (P_D). Stamped at visit(ClassDecl)
    // from the module being checked; survives the cross-module export boundary
    // because getExports()/registerExternalModule preserve shared_ptr identity.
    // `definingModule` is the canonical dotted name (may be empty for the entry
    // module); `definingFile` is the source path and is the authoritative,
    // always-populated key for package/same-file comparisons.
    std::string definingModule;
    std::string definingFile;
    // Number of constructors (`def()` / `__init__`) the class declares. Dragon
    // supports arity-overloaded ctors (codegen classCtorArities), but `methods`
    // stores only one __init__ FunctionType - so a call-site ctor arity check
    // (M2) is only sound when this is exactly 1.
    int constructorCount = 0;
    bool isTypedDict = false;  // TypedDict: fields have per-key types, backed by dict at runtime
    bool isEnum = false;       // class-based enum (Enum/IntEnum/StrEnum): members are
                               // singleton instances of this class (see synthesizeEnumMethods)
    // D044 - when this ClassType is a monomorphic instantiation of a generic
    // (e.g. `Box[int]`), `genericOrigin` is the generic's name ("Box") and
    // `genericArgs` are the type arguments (possibly TypeVar-containing while a
    // generic body is checked, e.g. `Inner[T]` inside `Outer[T]`). Lets
    // substituteType re-instantiate transitively (`Inner[T]` -> `Inner[int]`
    // when stamping `Outer[int]`). Empty `genericOrigin` ⇒ an ordinary class.
    std::string genericOrigin;
    std::vector<std::shared_ptr<Type>> genericArgs;
    explicit ClassType(std::string n) : name(std::move(n)) {}
    Kind kind() const override { return Kind::Class; }
    std::string toString() const override;
    bool equals(const Type& other) const override;
};

/// Instance type (instance of a class)
class InstanceType : public Type {
public:
    std::shared_ptr<ClassType> classType;
    explicit InstanceType(std::shared_ptr<ClassType> cls) : classType(std::move(cls)) {}
    Kind kind() const override { return Kind::Instance; }
    std::string toString() const override;
    bool equals(const Type& other) const override;
    // Nominal subtyping: an instance of a subclass is a subtype of an
    // instance of any ancestor (walks ClassType::parentClass). Without this
    // override `Dog <: Animal` was false even for a single value.
    bool isSubtypeOf(const Type& other) const override;
};

/// Union type: T1 | T2 | ...
class UnionType : public Type {
public:
    std::vector<std::shared_ptr<Type>> types;
    explicit UnionType(std::vector<std::shared_ptr<Type>> ts) : types(std::move(ts)) {}
    Kind kind() const override { return Kind::Union; }
    std::string toString() const override;
    bool equals(const Type& other) const override;
    bool isSubtypeOf(const Type& other) const override;
};

/// Any type -- compatible with everything
class AnyType : public Type {
public:
    Kind kind() const override { return Kind::Any; }
    std::string toString() const override { return "Any"; }
    bool equals(const Type& other) const override { return other.kind() == Kind::Any; }
    bool isSubtypeOf(const Type&) const override { return true; }
};

/// Never type -- bottom type, subtype of everything
class NeverType : public Type {
public:
    Kind kind() const override { return Kind::Never; }
    std::string toString() const override { return "Never"; }
    bool equals(const Type& other) const override { return other.kind() == Kind::Never; }
    bool isSubtypeOf(const Type&) const override { return true; }
};

/// Unknown type -- used when type cannot be determined
class UnknownType : public Type {
public:
    Kind kind() const override { return Kind::Unknown; }
    std::string toString() const override { return "<unknown>"; }
    bool equals(const Type& other) const override { return other.kind() == Kind::Unknown; }
    bool isSubtypeOf(const Type&) const override { return true; }
};

/// Raw C pointer type -- used for extern "C" FFI
class PtrType : public Type {
public:
    Kind kind() const override { return Kind::Ptr; }
    std::string toString() const override { return "ptr"; }
    bool equals(const Type& other) const override { return other.kind() == Kind::Ptr; }
};

/// TypeVar -- type variable for generics
class TypeVarType : public Type {
public:
    std::string name;
    // D046 - the bound `B` in a bounded type parameter `T: B`, resolved to its
    // type (typically an `InstanceType` of the bound class). nullptr for an
    // unbounded `T`. A bounded `T` behaves like its bound: member/operator/
    // subscript access on a `T`-typed value resolves against `bound`, and each
    // instantiation requires the concrete argument to satisfy it (be the bound
    // class or a subclass). The bound is consulted only during the abstract
    // template check + at instantiation; substituteType replaces the whole
    // TypeVar by name, so a stamped instantiation is fully concrete and codegen
    // never sees a bound.
    std::shared_ptr<Type> bound;
    explicit TypeVarType(std::string n, std::shared_ptr<Type> b = nullptr)
        : name(std::move(n)), bound(std::move(b)) {}
    Kind kind() const override { return Kind::TypeVar; }
    std::string toString() const override { return name; }
    bool equals(const Type& other) const override;
};

/// Module type -- the type of an imported module reference (e.g. the `x` in
/// `import x.y` or the `health` in `from controllers import health`).
/// Holds the module's canonical dotted name plus its top-level exports
/// (functions, classes, consts) and submodule chain. Pure compile-time
/// construct - modules never become runtime values; they only appear as
/// the base of an AttributeExpr that resolves to a static symbol.
class ModuleType : public Type {
public:
    std::string name;  // canonical dotted path, e.g. "controllers.health"
    // D045 - the module's source file path, threaded in via
    // registerExternalModule, so import/qualified-access privacy can enforce
    // `_x` ⇒ same-package and `__x` ⇒ same-file against an imported module.
    std::string filepath;
    std::unordered_map<std::string, std::shared_ptr<Type>> exports;
    std::unordered_map<std::string, std::shared_ptr<ModuleType>> submodules;
    explicit ModuleType(std::string n) : name(std::move(n)) {}
    Kind kind() const override { return Kind::Module; }
    std::string toString() const override { return "module[" + name + "]"; }
    bool equals(const Type& other) const override;
};

struct TypeDiagnostic {
    enum class Level { Warning, Error };
    Level level;
    SourceLocation location;
    std::string message;
};

/// Type checker for Dragon - enforces mandatory typing
class TypeChecker : public ASTVisitor {
public:
    TypeChecker();
    ~TypeChecker();

    bool check(Module& module);
    const std::vector<TypeDiagnostic>& diagnostics() const;
    bool hasErrors() const;

    /// Register symbols from an external module (for cross-file type checking).
    /// Must be called before check() for the importing module.
    void registerExternalModule(const std::string& moduleName,
                                const std::unordered_map<std::string, std::shared_ptr<Type>>& exports,
                                const std::string& filepath = "");

    /// D044 cross-module generics - surface an already-checked dependency
    /// module's generic templates (functions, classes, and own-type-param
    /// methods) into THIS checker's instantiation registries, so a use site in
    /// the importing module (`wrap[C](...)`, `repo.one[C]()`) can stamp them.
    /// The dependency AST is alive program-wide (single LLVM module), so this is
    /// pointer-wiring; templates are registered, never re-visited (their home
    /// module already checked them). Call before check() on the importer.
    void registerExternalGenerics(Module& mod);

    /// Get all module-level exports (functions, classes, variables) after check().
    /// Returns a map of symbol name -> type.
    std::unordered_map<std::string, std::shared_ptr<Type>> getExports() const;

    // All visitor methods from ASTVisitor
    void visit(NamedTypeExpr& node) override;
    void visit(GenericTypeExpr& node) override;
    void visit(OptionalTypeExpr& node) override;
    void visit(UnionTypeExpr& node) override;
    void visit(CallableTypeExpr& node) override;
    void visit(TupleTypeExpr& node) override;
    void visit(IntegerLiteral& node) override;
    void visit(FloatLiteral& node) override;
    void visit(StringLiteral& node) override;
    void visit(BooleanLiteral& node) override;
    void visit(NoneLiteral& node) override;
    void visit(NameExpr& node) override;
    void visit(BinaryExpr& node) override;
    void visit(ChainedCompExpr& node) override;
    void visit(WalrusExpr& node) override;
    void visit(UnaryExpr& node) override;
    void visit(CallExpr& node) override;
    void visit(AttributeExpr& node) override;
    void visit(SubscriptExpr& node) override;
    void visit(SliceExpr& node) override;
    void visit(ListExpr& node) override;
    void visit(TupleExpr& node) override;
    void visit(DictExpr& node) override;
    void visit(SetExpr& node) override;
    void visit(ListCompExpr& node) override;
    void visit(DictCompExpr& node) override;
    void visit(SetCompExpr& node) override;
    void visit(GeneratorExpr& node) override;
    void visit(LambdaExpr& node) override;
    void visit(IfExpr& node) override;
    void visit(AwaitExpr& node) override;
    void visit(FireExpr& node) override;
    void visit(YieldExpr& node) override;
    void visit(StarredExpr& node) override;
    void visit(TemplateExpr& node) override;
    void visit(TemplateFileExpr& node) override;
    void visit(ExprStmt& node) override;
    void visit(AssignStmt& node) override;
    void visit(AugAssignStmt& node) override;
    void visit(AnnAssignStmt& node) override;
    void visit(IfStmt& node) override;
    void visit(WhileStmt& node) override;
    void visit(ForStmt& node) override;
    void visit(TryStmt& node) override;
    void visit(WithStmt& node) override;
    void visit(ThreadStmt& node) override;
    void visit(MatchStmt& node) override;
    void visit(ReturnStmt& node) override;
    void visit(RaiseStmt& node) override;
    void visit(BreakStmt& node) override;
    void visit(ContinueStmt& node) override;
    void visit(PassStmt& node) override;
    void visit(AssertStmt& node) override;
    void visit(GlobalStmt& node) override;
    void visit(NonlocalStmt& node) override;
    void visit(DeleteStmt& node) override;
    void visit(ImportStmt& node) override;
    void visit(FromImportStmt& node) override;
    void visit(FunctionDecl& node) override;
    void visit(ClassDecl& node) override;
    void visit(TypeAliasStmt& node) override;
    void visit(Module& node) override;

private:
    std::shared_ptr<Type> resolveType(TypeExpr* typeExpr);
    std::shared_ptr<Type> inferType(Expr* expr);
    void propagateAnnotationToEmptyLiteral(Expr* value, const std::shared_ptr<Type>& annotType);
    // Expected-type-directed typing for a FRESH container literal. When the
    // literal's inferred element type isn't directly assignable to the
    // expected container's element type but every element IS a subtype of it,
    // retype the literal to the expected type. Sound because a freshly-built
    // literal has no prior alias - covariance at construction can't be
    // exploited. Returns true if it retyped (so the caller skips the error).
    bool tryExpectedTypeLiteral(Expr* value, const std::shared_ptr<Type>& expected);
    // If `value` is a list/set literal assigned to a container with a concrete
    // (non-Any) element type, emit a precise error for the first element that
    // isn't assignable to it. Catches the silent miscompile where first-element
    // inference matched the annotation but a later element didn't. Returns true
    // if it reported an error (so the caller skips the generic mismatch error).
    bool diagnoseHeterogeneousLiteral(Expr* value,
                                      const std::shared_ptr<Type>& annot);
    void bindCompLoopVars(const std::vector<std::string>& names,
                          const std::shared_ptr<Type>& iterType);
    void checkCompExtraClauses(std::vector<CompClause>& clauses);
    void initBuiltinTypes();
    void error(const SourceLocation& loc, const std::string& message);
    void warning(const SourceLocation& loc, const std::string& message);

    // D045 - enforced member/module privacy (all compile-time, zero codegen).
    // checkMemberPrivacy: `base.member` where `declaring` is the class that
    // declares `member`; rejects a _protected access from outside the package
    // (unless from a subclass) and a __private access from outside the
    // declaring class. checkModuleNamePrivacy: a `from mod import name` /
    // `mod.name` where `name` is _module-internal (different package) or
    // __file-private (different file). checkDunderDeclaration: a `__x__`
    // declaration not in the recognized reserved set.
    void checkMemberPrivacy(const ClassType* declaring, const std::string& member,
                            const SourceLocation& loc);
    void checkModuleNamePrivacy(const ModuleType& srcModule, const std::string& name,
                                const SourceLocation& loc);
    void checkDunderDeclaration(const std::string& name, bool moduleLevel,
                                const std::string& ownerDesc, const SourceLocation& loc);

    // D017 Phase 4 - resolve `template[X]` content type: look up X, walk
    // parent chain, enforce Template protocol, reject StructTemplate (D037
    // dispatch hook, reserved). Returns instance type of X on success, str
    // when contentType is empty (untyped), or reports an error and returns
    // an instance type so downstream type checks can proceed.
    std::shared_ptr<Type> resolveTemplateContentType(const std::string& contentType,
                                                     const SourceLocation& loc);

    //===------------------------------------------------------------------===//
    // D044 - generics / monomorphization engine (TypeCheckerGenerics.cpp).
    //===------------------------------------------------------------------===//

    // The body of visit(ClassDecl), factored out so visit(ClassDecl) can wrap it
    // in a type-parameter binding scope without threading a flag through its
    // several early returns.
    void visitClassDeclBody(ClassDecl& node);

    // Record this module's top-level generic templates and fully type-check
    // their signatures+bodies abstractly (type params bound to TypeVarType).
    // Returns the set of generic-decl names so the main walk can skip them.
    void collectGenericTemplates(Module& module);

    // Look up a bound type-parameter name in the active typeParamScopes stack;
    // returns the TypeVarType, or nullptr if `name` is not a type parameter.
    std::shared_ptr<Type> lookupTypeParam(const std::string& name);

    // resolveType helper: `base` is a user-defined generic class, `args` the
    // resolved concrete type arguments. Builds (once) a monomorphic placeholder
    // ClassType for the instantiation, records a stamping request, and returns
    // its InstanceType. `loc` is for arity/error reporting.
    std::shared_ptr<Type> instantiateGenericClass(ClassDecl* decl,
                                                  std::vector<std::shared_ptr<Type>> args,
                                                  const SourceLocation& loc);

    // Deep-substitute TypeVarType nodes in `t` per `bindings` (name -> concrete).
    std::shared_ptr<Type> substituteType(const std::shared_ptr<Type>& t,
        const std::unordered_map<std::string, std::shared_ptr<Type>>& bindings);

    // Solve type parameters by structurally matching a declared type (which may
    // contain TypeVarType) against an actual concrete type. Fills `out`. Returns
    // false on an irreconcilable mismatch. Used for generic-function arg inference.
    bool unifyTypeParam(const std::shared_ptr<Type>& declared,
                        const std::shared_ptr<Type>& actual,
                        std::unordered_map<std::string, std::shared_ptr<Type>>& out);

    // Build a TypeExpr that denotes `t` (e.g. int -> NamedTypeExpr "int",
    // list[int] -> GenericTypeExpr). Used to drive AST substitution when stamping.
    std::unique_ptr<TypeExpr> typeToTypeExpr(const std::shared_ptr<Type>& t);

    // Canonical instantiation name / cache key, e.g. "Box[int]", "Pair[int,str]".
    std::string mangleInstantiation(const std::string& genericName,
                                    const std::vector<std::shared_ptr<Type>>& args);

    // Try to resolve a CallExpr whose callee names (NameExpr) or subscripts
    // (SubscriptExpr, explicit `[T]`) a generic free function. On success it
    // records the instantiation, retargets the callee to the stamped name, sets
    // node.type to the concrete return type, and returns true. `argTypes` are the
    // already-inferred positional argument types.
    bool tryInstantiateGenericCall(CallExpr& node,
                                   const std::vector<std::shared_ptr<Type>>& argTypes,
                                   const std::shared_ptr<Type>& expected);

    // Try to resolve a CallExpr that constructs a generic class: either explicit
    // (`Box[int](5)`, callee is a SubscriptExpr on the class name) or inferred
    // from the binding's `expected` type (`b: Box[int] = Box(5)`, callee is the
    // bare class name). On success it instantiates, retargets the callee to the
    // stamped class name, sets node.type, and returns true.
    bool tryInstantiateGenericConstruction(CallExpr& node,
                                           const std::shared_ptr<Type>& expected);

    // Drain the pendingInsts worklist: clone each generic decl with its type
    // arguments substituted, append the concrete decl to the module body, and
    // type-check it (which may enqueue transitive instantiations). Runs to
    // fixpoint; hitting kMaxInstantiations is a compile error.
    void runMonomorphization();

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace dragon

#endif // DRAGON_TYPE_CHECKER_H
