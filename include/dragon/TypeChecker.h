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
        Contract,  // ADR 054 - a contract (or plural contract set) value type
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

class SetType : public Type {
public:
    std::shared_ptr<Type> elementType;
    explicit SetType(std::shared_ptr<Type> elem) : elementType(std::move(elem)) {}
    Kind kind() const override { return Kind::Set; }
    std::string toString() const override;
    bool equals(const Type& other) const override;
    bool isSubtypeOf(const Type& other) const override;
};

/// Task[T]: handle from `fire`/`async def`, erases to a DragonVThread* ptr at
/// LLVM; T is the result type recovered by await/.join().
class TaskType : public Type {
public:
    std::shared_ptr<Type> resultType;
    explicit TaskType(std::shared_ptr<Type> result) : resultType(std::move(result)) {}
    Kind kind() const override { return Kind::Task; }
    std::string toString() const override;
    bool equals(const Type& other) const override;
    bool isSubtypeOf(const Type& other) const override;
};

/// `threading.Lock` mutex handle: an intrinsic erasing to a bare pthread_mutex_t*
/// ptr, not a user-defined class; ops lower directly to `dragon_lock_*` runtime calls.
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
    // Call-validation metadata (M1/M2), set only for user FunctionDecls; empty
    // paramNames skips kwarg/arity validation. Excludes `self` for methods.
    std::vector<std::string> paramNames;
    // docs/002 2.8, aligned with paramNames: true for `own p: T`. A named arg
    // to an own param without `own` is E13; `own x` to a borrowing param is E14.
    std::vector<bool> paramOwns;
    size_t requiredParams = 0;
    bool hasVarArg = false;  // any param is *args or **kwargs
    bool hasKwArg = false;  // specifically **kwargs, to name unknown-kw errors precisely
    bool spawnsFreshTask = false;
    // True once fillFuncMeta has run: a real 0-param function vs. no metadata
    // (builtins/Callable, never checked).
    bool hasArgMeta = false;
    bool isMethod = false;  // bare-name arity checks skip methods (only callable via self./obj.)
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
    // ADR 010 overloading: every overload's FunctionType for a >1-method name;
    // a call resolves by arity+params at compile time (zero runtime cost).
    std::unordered_map<std::string, std::vector<std::shared_ptr<Type>>> methodOverloads;
    std::unordered_map<std::string, std::shared_ptr<Type>> fields;
    // Every field name the class declares, from a pre-pass before any body is
    // checked. TYPE-CHECK ONLY; `fields` stays the layout source of truth.
    std::set<std::string> declaredFieldNames;
    // Positional field order for `match` destructuring (parent fields first);
    // CodeGen builds the identical list so stages agree.
    std::vector<std::string> fieldOrder;
    std::shared_ptr<Type> parentClass;
    // D045: declaring module, for member-access privacy's package computation.
    // definingFile is the authoritative, always-populated comparison key.
    std::string definingModule;
    std::string definingFile;
    // True-identity backpointer to the declaring ClassDecl; prefer over any by-name registry.
    ClassDecl* decl = nullptr;
    /// ADR 054: contract atoms promised in the header (`-> A, B`), stamped after
    /// the structural check. Casts don't land here; this is what isSubtypeOf consults.
    std::set<const ContractDecl*> promisedContracts;
    // Constructor count: `methods` stores only one __init__ FunctionType, so a
    // call-site ctor arity check (M2) is sound only when this is exactly 1.
    int constructorCount = 0;
    bool isTypedDict = false;  // TypedDict: per-key types, backed by dict at runtime
    bool isEnum = false;       // class-based enum; members are singleton instances
    // D044: true-identity backpointer to the TEMPLATE ClassDecl this was
    // stamped from (null for ordinary classes), paired with genericOrigin/Args below.
    ClassDecl* originDecl = nullptr;
    // Generic name (e.g. "Box") and type args of this instantiation (e.g.
    // `Box[int]`); lets substituteType re-instantiate transitively. Empty name = ordinary class.
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
    // Nominal subtyping: walks ClassType::parentClass so `Dog <: Animal` holds.
    bool isSubtypeOf(const Type& other) const override;
};

/// ADR 054: a contract or contract set in type position, identified by ATOM
/// (a ContractDecl declaring >=1 own method); an ordinary instance pointer at runtime.
class ContractType : public Type {
public:
    /// Source-level spelling for diagnostics ("Amazing" / "{Amazing, Speaker}").
    std::string display;
    /// Atom decls, sorted by pointer, deduped (true identity per D053).
    std::vector<const ContractDecl*> atoms;
    /// Union of every atom's method signatures: name -> FunctionType.
    std::unordered_map<std::string, std::shared_ptr<Type>> methods;
    /// Method name -> declaring atom; CodeGen slot coloring keys on (owner, name)
    /// so composed and direct views dispatch through the same slot.
    std::unordered_map<std::string, const ContractDecl*> methodOwner;
    Kind kind() const override { return Kind::Contract; }
    std::string toString() const override { return display; }
    bool equals(const Type& other) const override;
    /// Subset rule: `{A, B}` satisfies an `A` position.
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
    // D046: bound `B` in `T: B` (nullptr if unbounded); member access on `T`
    // resolves against it. substituteType erases it, so codegen never sees a bound.
    std::shared_ptr<Type> bound;
    explicit TypeVarType(std::string n, std::shared_ptr<Type> b = nullptr)
        : name(std::move(n)), bound(std::move(b)) {}
    Kind kind() const override { return Kind::TypeVar; }
    std::string toString() const override { return name; }
    bool equals(const Type& other) const override;
};

/// Type of an imported module reference (e.g. `x` in `import x.y`): its dotted
/// name, exports, and submodules. Pure compile-time; never a runtime value.
class ModuleType : public Type {
public:
    std::string name;  // canonical dotted path, e.g. "controllers.health"
    // D045: source file path, so import privacy can enforce `_x` same-package
    // and `__x` same-file against an imported module.
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

    /// D044 cross-module generics: surfaces an already-checked dependency's
    /// generic templates into this checker's registries. Call before check().
    void registerExternalGenerics(Module& mod);

    /// D048: builds the box-free `json.decode[T]` body from T's ctor params;
    /// nullptr after reporting a clean error (unknown T, non-scalar/optional field).
    std::unique_ptr<Stmt> synthesizeSchemaDecoder(
        const std::shared_ptr<Type>& targetType, SourceLocation loc);

    /// D052: the write-side mirror - build the box-free `json.encode[T]` body
    /// (Writer-driven) from T's ctor params. Same contract as the decoder.
    std::unique_ptr<Stmt> synthesizeSchemaEncoder(
        const std::shared_ptr<Type>& targetType, SourceLocation loc);

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
    void visit(ContractSetTypeExpr& node) override;
    void visit(IntegerLiteral& node) override;
    void visit(FloatLiteral& node) override;
    void visit(StringLiteral& node) override;
    void visit(BooleanLiteral& node) override;
    void visit(NoneLiteral& node) override;
    void visit(NameExpr& node) override;
    /// E11 (docs/002 2.7): dubability composes - true iff the whole payload
    /// can honestly be deep-copied; `why` names the first offender otherwise.
    bool typeIsDubable(const Type* t, std::string& why);
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
    void visit(AsCastExpr& node) override;
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
    void visit(DeferStmt& node) override;
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
    void visit(ContractDecl& node) override;
    void visit(TypeAliasStmt& node) override;
    void visit(Module& node) override;

private:
    std::shared_ptr<Type> resolveType(TypeExpr* typeExpr);

    // ADR 054 type contracts.

    /// Pre-pass: registers every ContractDecl (shell, then signatures, then
    /// merges); visit(ContractDecl) in the main walk is a no-op.
    void registerContracts(Module& module);
    /// Resolves one contract name (typeNames, then scope); nullptr if unknown
    /// or not a contract type.
    std::shared_ptr<ContractType> resolveContractRef(const std::string& name,
                                                     const SourceLocation& loc,
                                                     bool reportErrors);
    /// Merges a cast/annotation contract list into one ContractType (atoms
    /// unioned; conflicting duplicate signatures are an error).
    std::shared_ptr<ContractType> resolveContractSet(
        const std::vector<std::string>& names, const SourceLocation& loc,
        bool reportErrors);
    /// Every contract method must exist on the flattened class with an exactly
    /// matching signature (ADR 010: any overload may match). Empty = satisfied.
    std::vector<std::string> contractConformanceProblems(
        const ClassType& cls, const ContractType& ct);
    std::shared_ptr<Type> inferType(Expr* expr);
    void propagateAnnotationToEmptyLiteral(Expr* value, const std::shared_ptr<Type>& annotType);
    // Retypes a FRESH container literal to the expected type when every
    // element is a subtype of it (sound: no prior alias to exploit).
    bool tryExpectedTypeLiteral(Expr* value, const std::shared_ptr<Type>& expected);
    // A container LITERAL into Any is born boxed recursively (else a later
    // list[Any] read walks the wrong stride, 16B boxed vs native); class-descriptor literals are exempt.
    void boxNestedContainerLiteralForAny(Expr* value);
    // Hint appended when a mismatch is exactly the list[T] vs list[Any] split.
    static std::string listReprMismatchHint(const Type& from, const Type& to);
    // For a list/set literal into a concrete-element container, reports the
    // first non-assignable element (first-element-matched-but-later-didn't).
    bool diagnoseHeterogeneousLiteral(Expr* value,
                                      const std::shared_ptr<Type>& annot);
    void bindCompLoopVars(const std::vector<std::string>& names,
                          const std::shared_ptr<Type>& iterType);
    void checkCompExtraClauses(std::vector<CompClause>& clauses);
    void initBuiltinTypes();
    void error(const SourceLocation& loc, const std::string& message);
    void warning(const SourceLocation& loc, const std::string& message);

    // D045 privacy (compile-time only): rejects out-of-package _protected /
    // out-of-class __private member access.
    void checkMemberPrivacy(const ClassType* declaring, const std::string& member,
                            const SourceLocation& loc);
    // Ancestor class that declares `member` as a METHOD, or nullptr if a field
    // shadows it first / undeclared. Used to reject method reassignment/bare reads.
    const ClassType* findMethodOwner(const ClassType* cls,
                                     const std::string& member) const;
    // Body of visit(AttributeExpr): resolves the member and sets node.type;
    // the caller adds the bare-method-read check for builtin receivers.
    void resolveAttributeExpr(AttributeExpr& node);
    void checkModuleNamePrivacy(const ModuleType& srcModule, const std::string& name,
                                const SourceLocation& loc);
    void checkDunderDeclaration(const std::string& name, bool moduleLevel,
                                const std::string& ownerDesc, const SourceLocation& loc);

    // D017 Phase 4: resolves `template[X]` content type (walks parent chain,
    // enforces Template protocol); str if untyped, else X's instance type.
    std::shared_ptr<Type> resolveTemplateContentType(const std::string& contentType,
                                                     const SourceLocation& loc);

    // D044 generics/monomorphization engine (TypeCheckerGenerics.cpp).

    // Body of visit(ClassDecl), factored out so it can be wrapped in a type-
    // parameter binding scope without threading a flag through early returns.
    void visitClassDeclBody(ClassDecl& node);

    // Records top-level generic templates and type-checks them abstractly
    // (params bound to TypeVarType); returns names for the main walk to skip.
    void collectGenericTemplates(Module& module);

    // Looks up a bound type-parameter name on the active scope stack; nullptr
    // if `name` isn't a type parameter.
    std::shared_ptr<Type> lookupTypeParam(const std::string& name);

    // resolveType helper: builds (once) a monomorphic placeholder ClassType
    // for `base[args]`, records a stamping request, returns its InstanceType.
    std::shared_ptr<Type> instantiateGenericClass(ClassDecl* decl,
                                                  std::vector<std::shared_ptr<Type>> args,
                                                  const SourceLocation& loc);

    // Deep-substitutes TypeVarType nodes in `t` per `bindings` (name -> concrete).
    std::shared_ptr<Type> substituteType(const std::shared_ptr<Type>& t,
        const std::unordered_map<std::string, std::shared_ptr<Type>>& bindings);

    // Structurally matches a declared type (may contain TypeVarType) against
    // an actual type, filling `out`; false on irreconcilable mismatch.
    bool unifyTypeParam(const std::shared_ptr<Type>& declared,
                        const std::shared_ptr<Type>& actual,
                        std::unordered_map<std::string, std::shared_ptr<Type>>& out);

    // Builds a TypeExpr denoting `t` (int -> NamedTypeExpr, list[int] ->
    // GenericTypeExpr), to drive AST substitution when stamping.
    std::unique_ptr<TypeExpr> typeToTypeExpr(const std::shared_ptr<Type>& t);

    // Canonical instantiation name/cache key, e.g. "Box[int]", "Pair[int,str]".
    std::string mangleInstantiation(const std::string& genericName,
                                    const std::vector<std::shared_ptr<Type>>& args);

    // Resolves a CallExpr naming/subscripting a generic free function: records
    // the instantiation, retargets the callee, sets node.type, returns true on success.
    bool tryInstantiateGenericCall(CallExpr& node,
                                   const std::vector<std::shared_ptr<Type>>& argTypes,
                                   const std::shared_ptr<Type>& expected);

    // Resolves a CallExpr constructing a generic class, explicit (`Box[int](5)`)
    // or inferred from the binding's expected type; instantiates and retargets on success.
    bool tryInstantiateGenericConstruction(CallExpr& node,
                                           const std::shared_ptr<Type>& expected);

    // Drains the pendingInsts worklist to fixpoint: clones each generic decl
    // with substituted args and type-checks it; kMaxInstantiations is a compile error.
    void runMonomorphization();

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace dragon

#endif // DRAGON_TYPE_CHECKER_H
