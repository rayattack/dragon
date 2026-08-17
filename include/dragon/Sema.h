#ifndef DRAGON_SEMA_H
#define DRAGON_SEMA_H

#include "dragon/AST.h"
#include <memory>
#include <string>
#include <vector>
#include <unordered_map>

namespace dragon {

struct Symbol {
    enum class Kind {
        Variable,
        Function,
        Class,
        Parameter,
        Module,
        TypeAlias
    };

    std::string name;
    Kind kind;
    std::shared_ptr<Type> type;
    SourceLocation declaration;
    bool isGlobal = false;
    bool isNonlocal = false;
    bool isInitialized = false;
    bool isConst = false;
    bool isStatic = false;
    bool isBuiltin = false;
    bool isModuleForwardDecl = false;
};

class Scope {
public:
    enum class Kind {
        Module,
        Class,
        Function,
        Block
    };

    Scope(Kind kind, Scope* parent = nullptr);
    ~Scope();

    bool define(const Symbol& symbol);

    Symbol* lookup(const std::string& name);

    Symbol* lookupLocal(const std::string& name);

    Scope* enclosingFunction();

    Scope* enclosingClass();

    Kind kind() const { return kind_; }
    Scope* parent() const { return parent_; }

private:
    Kind kind_;
    Scope* parent_;
    std::unordered_map<std::string, Symbol> symbols_;
};

struct SemaDiagnostic {
    enum class Level { Warning, Error };
    Level level;
    SourceLocation location;
    std::string message;
};

class Sema : public ASTVisitor {
public:
    Sema();
    ~Sema();

    bool analyze(Module& module);

    const std::vector<SemaDiagnostic>& diagnostics() const;

    bool hasErrors() const;

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
    void pushScope(Scope::Kind kind);
    void popScope();
    Scope* currentScope();

    void defineBuiltins();
    void resolveImport(const std::string& moduleName);
    bool isValidAssignmentTarget(Expr* expr);

    void error(const SourceLocation& loc, const std::string& message);
    void warning(const SourceLocation& loc, const std::string& message);

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}

#endif
