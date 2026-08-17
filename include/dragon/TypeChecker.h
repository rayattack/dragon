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

class Type {
public:
    enum class Kind {
        Int, Float, Bool, Str, Bytes, None_,
        List, Dict, Set, Tuple, Function, Task, Lock,
        Class, Instance, Any, Never, Union, Optional, TypeVar, Unknown,
        Contract,
        Ptr,
        Module
    };
    virtual ~Type() = default;
    virtual Kind kind() const = 0;
    virtual std::string toString() const = 0;
    virtual bool equals(const Type& other) const = 0;
    virtual bool isSubtypeOf(const Type& other) const;
    bool isAssignableTo(const Type& other) const;
};

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

class TaskType : public Type {
public:
    std::shared_ptr<Type> resultType;
    explicit TaskType(std::shared_ptr<Type> result) : resultType(std::move(result)) {}
    Kind kind() const override { return Kind::Task; }
    std::string toString() const override;
    bool equals(const Type& other) const override;
    bool isSubtypeOf(const Type& other) const override;
};

class LockType : public Type {
public:
    Kind kind() const override { return Kind::Lock; }
    std::string toString() const override;
    bool equals(const Type& other) const override;
    bool isSubtypeOf(const Type& other) const override;
};

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

class TupleType : public Type {
public:
    std::vector<std::shared_ptr<Type>> elementTypes;
    explicit TupleType(std::vector<std::shared_ptr<Type>> elems)
        : elementTypes(std::move(elems)) {}
    Kind kind() const override { return Kind::Tuple; }
    std::string toString() const override;
    bool equals(const Type& other) const override;
    bool isSubtypeOf(const Type& other) const override;
};

class FunctionType : public Type {
public:
    std::vector<std::shared_ptr<Type>> paramTypes;
    std::shared_ptr<Type> returnType;
    std::vector<std::string> paramNames;
    std::vector<bool> paramOwns;
    size_t requiredParams = 0;
    bool hasVarArg = false;
    bool hasKwArg = false;
    bool spawnsFreshTask = false;
    bool hasArgMeta = false;
    bool isMethod = false;
    FunctionType(std::vector<std::shared_ptr<Type>> params, std::shared_ptr<Type> ret)
        : paramTypes(std::move(params)), returnType(std::move(ret)) {}
    Kind kind() const override { return Kind::Function; }
    std::string toString() const override;
    bool equals(const Type& other) const override;
};

class ClassType : public Type {
public:
    std::string name;
    std::unordered_map<std::string, std::shared_ptr<Type>> methods;
    std::unordered_map<std::string, std::vector<std::shared_ptr<Type>>> methodOverloads;
    std::unordered_map<std::string, std::shared_ptr<Type>> fields;
    std::set<std::string> declaredFieldNames;
    std::vector<std::string> fieldOrder;
    std::shared_ptr<Type> parentClass;
    std::string definingModule;
    std::string definingFile;
    ClassDecl* decl = nullptr;
    std::set<const ContractDecl*> promisedContracts;
    int constructorCount = 0;
    bool isTypedDict = false;
    bool isEnum = false;
    ClassDecl* originDecl = nullptr;
    std::string genericOrigin;
    std::vector<std::shared_ptr<Type>> genericArgs;
    explicit ClassType(std::string n) : name(std::move(n)) {}
    Kind kind() const override { return Kind::Class; }
    std::string toString() const override;
    bool equals(const Type& other) const override;
};

class InstanceType : public Type {
public:
    std::shared_ptr<ClassType> classType;
    explicit InstanceType(std::shared_ptr<ClassType> cls) : classType(std::move(cls)) {}
    Kind kind() const override { return Kind::Instance; }
    std::string toString() const override;
    bool equals(const Type& other) const override;
    bool isSubtypeOf(const Type& other) const override;
};

class ContractType : public Type {
public:
    std::string display;
    std::vector<const ContractDecl*> atoms;
    std::unordered_map<std::string, std::shared_ptr<Type>> methods;
    std::unordered_map<std::string, const ContractDecl*> methodOwner;
    Kind kind() const override { return Kind::Contract; }
    std::string toString() const override { return display; }
    bool equals(const Type& other) const override;
    bool isSubtypeOf(const Type& other) const override;
};

class UnionType : public Type {
public:
    std::vector<std::shared_ptr<Type>> types;
    explicit UnionType(std::vector<std::shared_ptr<Type>> ts) : types(std::move(ts)) {}
    Kind kind() const override { return Kind::Union; }
    std::string toString() const override;
    bool equals(const Type& other) const override;
    bool isSubtypeOf(const Type& other) const override;
};

class AnyType : public Type {
public:
    Kind kind() const override { return Kind::Any; }
    std::string toString() const override { return "Any"; }
    bool equals(const Type& other) const override { return other.kind() == Kind::Any; }
    bool isSubtypeOf(const Type&) const override { return true; }
};

class NeverType : public Type {
public:
    Kind kind() const override { return Kind::Never; }
    std::string toString() const override { return "Never"; }
    bool equals(const Type& other) const override { return other.kind() == Kind::Never; }
    bool isSubtypeOf(const Type&) const override { return true; }
};

class UnknownType : public Type {
public:
    Kind kind() const override { return Kind::Unknown; }
    std::string toString() const override { return "<unknown>"; }
    bool equals(const Type& other) const override { return other.kind() == Kind::Unknown; }
    bool isSubtypeOf(const Type&) const override { return true; }
};

class PtrType : public Type {
public:
    Kind kind() const override { return Kind::Ptr; }
    std::string toString() const override { return "ptr"; }
    bool equals(const Type& other) const override { return other.kind() == Kind::Ptr; }
};

class TypeVarType : public Type {
public:
    std::string name;
    std::shared_ptr<Type> bound;
    explicit TypeVarType(std::string n, std::shared_ptr<Type> b = nullptr)
        : name(std::move(n)), bound(std::move(b)) {}
    Kind kind() const override { return Kind::TypeVar; }
    std::string toString() const override { return name; }
    bool equals(const Type& other) const override;
};

class ModuleType : public Type {
public:
    std::string name;
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

class TypeChecker : public ASTVisitor {
public:
    TypeChecker();
    ~TypeChecker();

    bool check(Module& module);
    const std::vector<TypeDiagnostic>& diagnostics() const;
    bool hasErrors() const;

    void registerExternalModule(const std::string& moduleName,
                                const std::unordered_map<std::string, std::shared_ptr<Type>>& exports,
                                const std::string& filepath = "");

    void registerExternalGenerics(Module& mod);

    std::unique_ptr<Stmt> synthesizeSchemaDecoder(
        const std::shared_ptr<Type>& targetType, SourceLocation loc);

    std::unique_ptr<Stmt> synthesizeSchemaEncoder(
        const std::shared_ptr<Type>& targetType, SourceLocation loc);

    std::unordered_map<std::string, std::shared_ptr<Type>> getExports() const;

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

    void registerContracts(Module& module);
    std::shared_ptr<ContractType> resolveContractRef(const std::string& name,
                                                     const SourceLocation& loc,
                                                     bool reportErrors);
    std::shared_ptr<ContractType> resolveContractSet(
        const std::vector<std::string>& names, const SourceLocation& loc,
        bool reportErrors);
    std::vector<std::string> contractConformanceProblems(
        const ClassType& cls, const ContractType& ct);
    std::shared_ptr<Type> inferType(Expr* expr);
    void propagateAnnotationToEmptyLiteral(Expr* value, const std::shared_ptr<Type>& annotType);
    bool tryExpectedTypeLiteral(Expr* value, const std::shared_ptr<Type>& expected);
    void boxNestedContainerLiteralForAny(Expr* value);
    static std::string listReprMismatchHint(const Type& from, const Type& to);
    bool diagnoseHeterogeneousLiteral(Expr* value,
                                      const std::shared_ptr<Type>& annot);
    void bindCompLoopVars(const std::vector<std::string>& names,
                          const std::shared_ptr<Type>& iterType);
    void checkCompExtraClauses(std::vector<CompClause>& clauses);
    void initBuiltinTypes();
    void error(const SourceLocation& loc, const std::string& message);
    void warning(const SourceLocation& loc, const std::string& message);

    void checkMemberPrivacy(const ClassType* declaring, const std::string& member,
                            const SourceLocation& loc);
    const ClassType* findMethodOwner(const ClassType* cls,
                                     const std::string& member) const;
    void resolveAttributeExpr(AttributeExpr& node);
    void checkModuleNamePrivacy(const ModuleType& srcModule, const std::string& name,
                                const SourceLocation& loc);
    void checkDunderDeclaration(const std::string& name, bool moduleLevel,
                                const std::string& ownerDesc, const SourceLocation& loc);

    std::shared_ptr<Type> resolveTemplateContentType(const std::string& contentType,
                                                     const SourceLocation& loc);

    void visitClassDeclBody(ClassDecl& node);

    void collectGenericTemplates(Module& module);

    std::shared_ptr<Type> lookupTypeParam(const std::string& name);

    std::shared_ptr<Type> instantiateGenericClass(ClassDecl* decl,
                                                  std::vector<std::shared_ptr<Type>> args,
                                                  const SourceLocation& loc);

    std::shared_ptr<Type> substituteType(const std::shared_ptr<Type>& t,
        const std::unordered_map<std::string, std::shared_ptr<Type>>& bindings);

    bool unifyTypeParam(const std::shared_ptr<Type>& declared,
                        const std::shared_ptr<Type>& actual,
                        std::unordered_map<std::string, std::shared_ptr<Type>>& out);

    std::unique_ptr<TypeExpr> typeToTypeExpr(const std::shared_ptr<Type>& t);

    std::string mangleInstantiation(const std::string& genericName,
                                    const std::vector<std::shared_ptr<Type>>& args);

    bool tryInstantiateGenericCall(CallExpr& node,
                                   const std::vector<std::shared_ptr<Type>>& argTypes,
                                   const std::shared_ptr<Type>& expected);

    bool tryInstantiateGenericConstruction(CallExpr& node,
                                           const std::shared_ptr<Type>& expected);

    void runMonomorphization();

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}

#endif
