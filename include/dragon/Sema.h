#ifndef DRAGON_SEMA_H
#define DRAGON_SEMA_H

#include "dragon/AST.h"
#include <memory>
#include <string>
#include <vector>
#include <unordered_map>

namespace dragon {

/// Represents a symbol in the symbol table
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
    bool isConst = false;   // Dragon const binding
    bool isStatic = false;  // Dragon static member
    bool isBuiltin = false; // injected builtin (outer namespace) - may be shadowed
};

/// Represents a scope (function, class, module, etc.)
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

    /// Define a new symbol in this scope
    bool define(const Symbol& symbol);

    /// Look up a symbol, searching parent scopes if not found
    Symbol* lookup(const std::string& name);

    /// Look up a symbol in this scope only (no parent search)
    Symbol* lookupLocal(const std::string& name);

    /// Get the enclosing function scope (or nullptr)
    Scope* enclosingFunction();

    /// Get the enclosing class scope (or nullptr)
    Scope* enclosingClass();

    Kind kind() const { return kind_; }
    Scope* parent() const { return parent_; }

private:
    Kind kind_;
    Scope* parent_;
    std::unordered_map<std::string, Symbol> symbols_;
};

/// Diagnostic from semantic analysis
struct SemaDiagnostic {
    enum class Level { Warning, Error };
    Level level;
    SourceLocation location;
    std::string message;
};

/// Semantic analyzer
/// 
/// Performs:
/// - Name resolution and binding
/// - Scope analysis
/// - Import resolution
/// - Declaration checking
/// - Basic semantic validation
class Sema : public ASTVisitor {
public:
    Sema();
    ~Sema();

    /// Analyze a module
    bool analyze(Module& module);

    /// Get all diagnostics
    const std::vector<SemaDiagnostic>& diagnostics() const;

    /// Check if any errors occurred
    bool hasErrors() const;

    // Visitor methods
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
    // Scope management
    void pushScope(Scope::Kind kind);
    void popScope();
    Scope* currentScope();

    // Helper methods
    void defineBuiltins();
    void resolveImport(const std::string& moduleName);
    bool isValidAssignmentTarget(Expr* expr);
    
    // Error reporting
    void error(const SourceLocation& loc, const std::string& message);
    void warning(const SourceLocation& loc, const std::string& message);

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace dragon

#endif // DRAGON_SEMA_H
