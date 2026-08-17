#ifndef DRAGON_AST_H
#define DRAGON_AST_H

#include "dragon/Token.h"
#include <memory>
#include <string>
#include <vector>
#include <variant>
#include <optional>

namespace dragon {

class ASTVisitor;
class Type;
class ContractDecl;

class ASTNode {
public:
    virtual ~ASTNode() = default;
    virtual void accept(ASTVisitor& visitor) = 0;

    const SourceLocation& location() const { return location_; }
    void setLocation(SourceLocation loc) { location_ = std::move(loc); }

protected:
    SourceLocation location_;
};

class Expr : public ASTNode {
public:
    std::shared_ptr<Type> type;
};

class Stmt : public ASTNode {};

class TypeExpr : public ASTNode {};

class NamedTypeExpr : public TypeExpr {
public:
    std::string name;
    void accept(ASTVisitor& visitor) override;
};

class GenericTypeExpr : public TypeExpr {
public:
    std::unique_ptr<TypeExpr> base;
    std::vector<std::unique_ptr<TypeExpr>> typeArgs;
    void accept(ASTVisitor& visitor) override;
};

class OptionalTypeExpr : public TypeExpr {
public:
    std::unique_ptr<TypeExpr> inner;
    void accept(ASTVisitor& visitor) override;
};

class UnionTypeExpr : public TypeExpr {
public:
    std::vector<std::unique_ptr<TypeExpr>> types;
    void accept(ASTVisitor& visitor) override;
};

class CallableTypeExpr : public TypeExpr {
public:
    std::vector<std::unique_ptr<TypeExpr>> paramTypes;
    std::unique_ptr<TypeExpr> returnType;
    void accept(ASTVisitor& visitor) override;
};

class TupleTypeExpr : public TypeExpr {
public:
    std::vector<std::unique_ptr<TypeExpr>> elementTypes;
    void accept(ASTVisitor& visitor) override;
};

class ContractSetTypeExpr : public TypeExpr {
public:
    std::vector<std::string> names;
    void accept(ASTVisitor& visitor) override;
};

class IntegerLiteral : public Expr {
public:
    int64_t value;
    void accept(ASTVisitor& visitor) override;
};

class FloatLiteral : public Expr {
public:
    double value;
    void accept(ASTVisitor& visitor) override;
};

struct FStringPart {
    enum class Kind { Literal, Expression };
    Kind kind = Kind::Literal;
    std::string literal;
    std::unique_ptr<Expr> expr;
    std::string formatSpec;
    char conversion = 0;
};

class StringLiteral : public Expr {
public:
    std::string value;
    bool isRaw = false;
    bool isFString = false;
    bool isBytes = false;
    std::vector<FStringPart> fstringParts;
    void accept(ASTVisitor& visitor) override;
};

struct TemplatePart {
    enum class Kind { Literal, Interpolation, Block };
    Kind kind = Kind::Literal;
    std::string literal;
    std::unique_ptr<Expr> expr;
    std::vector<std::unique_ptr<Stmt>> blockStmts;
    std::string filterName;
    bool isSpread = false;
    std::string exprText;
    size_t bangPos = 0;
    bool parseFailed = false;
};

class TemplateExpr : public Expr {
public:
    std::string body;
    std::string contentType;

    std::vector<TemplatePart> templateParts;

    bool isContentAlias = false;
    void accept(ASTVisitor& visitor) override;
};

class TemplateFileExpr : public Expr {
public:
    std::string filePath;
    std::string contentType;
    void accept(ASTVisitor& visitor) override;
};

class BooleanLiteral : public Expr {
public:
    bool value;
    void accept(ASTVisitor& visitor) override;
};

class NoneLiteral : public Expr {
public:
    void accept(ASTVisitor& visitor) override;
};

class NameExpr : public Expr {
public:
    std::string name;
    bool isMoveMarked = false;
    bool isDubMarked = false;
    void accept(ASTVisitor& visitor) override;
};

class BinaryExpr : public Expr {
public:
    std::unique_ptr<Expr> left;
    Token op;
    std::unique_ptr<Expr> right;
    void accept(ASTVisitor& visitor) override;
};

class ChainedCompExpr : public Expr {
public:
    std::vector<std::unique_ptr<Expr>> operands;
    std::vector<Token> operators;
    void accept(ASTVisitor& visitor) override;
};

class WalrusExpr : public Expr {
public:
    std::string name;
    std::unique_ptr<Expr> value;
    void accept(ASTVisitor& visitor) override;
};

class UnaryExpr : public Expr {
public:
    Token op;
    std::unique_ptr<Expr> operand;
    void accept(ASTVisitor& visitor) override;
};

class CallExpr : public Expr {
public:
    std::unique_ptr<Expr> callee;
    std::vector<std::unique_ptr<Expr>> args;
    std::vector<std::pair<std::string, std::unique_ptr<Expr>>> kwArgs;
    int resolvedMethodOverload = -1;
    void accept(ASTVisitor& visitor) override;
};

class AttributeExpr : public Expr {
public:
    std::unique_ptr<Expr> object;
    std::string attribute;
    void accept(ASTVisitor& visitor) override;
};

class SubscriptExpr : public Expr {
public:
    std::unique_ptr<Expr> object;
    std::unique_ptr<Expr> index;
    void accept(ASTVisitor& visitor) override;
};

class SliceExpr : public Expr {
public:
    std::unique_ptr<Expr> lower;
    std::unique_ptr<Expr> upper;
    std::unique_ptr<Expr> step;
    void accept(ASTVisitor& visitor) override;
};

class ListExpr : public Expr {
public:
    std::vector<std::unique_ptr<Expr>> elements;
    void accept(ASTVisitor& visitor) override;
};

class TupleExpr : public Expr {
public:
    std::vector<std::unique_ptr<Expr>> elements;
    void accept(ASTVisitor& visitor) override;
};

class DictExpr : public Expr {
public:
    std::vector<std::pair<std::unique_ptr<Expr>, std::unique_ptr<Expr>>> entries;
    void accept(ASTVisitor& visitor) override;
};

class SetExpr : public Expr {
public:
    std::vector<std::unique_ptr<Expr>> elements;
    void accept(ASTVisitor& visitor) override;
};

struct CompClause {
    std::vector<std::string> varNames;
    std::unique_ptr<Expr> iterable;
    std::unique_ptr<Expr> condition;
};

class ListCompExpr : public Expr {
public:
    std::unique_ptr<Expr> element;
    std::string varName;
    std::unique_ptr<Expr> iterable;
    std::unique_ptr<Expr> condition;
    std::vector<CompClause> extraClauses;
    void accept(ASTVisitor& visitor) override;
};

class DictCompExpr : public Expr {
public:
    std::unique_ptr<Expr> key;
    std::unique_ptr<Expr> value;
    std::vector<std::string> varNames;
    std::unique_ptr<Expr> iterable;
    std::unique_ptr<Expr> condition;
    std::vector<CompClause> extraClauses;
    void accept(ASTVisitor& visitor) override;
};

class SetCompExpr : public Expr {
public:
    std::unique_ptr<Expr> element;
    std::string varName;
    std::unique_ptr<Expr> iterable;
    std::unique_ptr<Expr> condition;
    std::vector<CompClause> extraClauses;
    void accept(ASTVisitor& visitor) override;
};

class GeneratorExpr : public Expr {
public:
    std::unique_ptr<Expr> element;
    std::string varName;
    std::unique_ptr<Expr> iterable;
    std::unique_ptr<Expr> condition;
    std::vector<CompClause> extraClauses;
    void accept(ASTVisitor& visitor) override;
};

class LambdaExpr : public Expr {
public:
    struct Parameter {
        std::string name;
        std::unique_ptr<TypeExpr> type;
        std::unique_ptr<Expr> defaultValue;
    };
    std::vector<Parameter> params;
    std::unique_ptr<TypeExpr> returnType;
    std::unique_ptr<Expr> body;
    std::vector<std::unique_ptr<Stmt>> bodyStmts;
    std::vector<std::string> capturedVars;
    std::vector<std::string> mutatedCapturedVars;
    void accept(ASTVisitor& visitor) override;
};

class IfExpr : public Expr {
public:
    std::unique_ptr<Expr> condition;
    std::unique_ptr<Expr> thenExpr;
    std::unique_ptr<Expr> elseExpr;
    void accept(ASTVisitor& visitor) override;
};

class AwaitExpr : public Expr {
public:
    std::unique_ptr<Expr> operand;
    void accept(ASTVisitor& visitor) override;
};

class AsCastExpr : public Expr {
public:
    std::unique_ptr<Expr> operand;
    std::vector<std::string> contracts;
    std::vector<const ContractDecl*> resolvedDecls;
    bool fromBracedSet = false;
    void accept(ASTVisitor& visitor) override;
};

class FireExpr : public Expr {
public:
    std::unique_ptr<Expr> operand;
    std::vector<std::unique_ptr<Stmt>> bodyStmts;
    std::vector<std::string> capturedVars;
    std::vector<std::string> mutatedCapturedVars;
    void accept(ASTVisitor& visitor) override;
};

class YieldExpr : public Expr {
public:
    std::unique_ptr<Expr> value;
    bool isYieldFrom = false;
    void accept(ASTVisitor& visitor) override;
};

class StarredExpr : public Expr {
public:
    std::unique_ptr<Expr> value;
    bool isDoubleStar = false;
    void accept(ASTVisitor& visitor) override;
};

class ExprStmt : public Stmt {
public:
    std::unique_ptr<Expr> expr;
    void accept(ASTVisitor& visitor) override;
};

class AssignStmt : public Stmt {
public:
    std::vector<std::unique_ptr<Expr>> targets;
    std::unique_ptr<Expr> value;
    std::unique_ptr<TypeExpr> typeAnnotation;
    bool isConst = false;
    void accept(ASTVisitor& visitor) override;
};

class AugAssignStmt : public Stmt {
public:
    std::unique_ptr<Expr> target;
    Token op;
    std::unique_ptr<Expr> value;
    void accept(ASTVisitor& visitor) override;
};

class AnnAssignStmt : public Stmt {
public:
    std::unique_ptr<Expr> target;
    std::unique_ptr<TypeExpr> annotation;
    std::unique_ptr<Expr> value;
    bool isConst = false;
    bool isStatic = false;
    bool isOwn = false;
    bool valueIsFreshTask = false;
    void accept(ASTVisitor& visitor) override;
};

class IfStmt : public Stmt {
public:
    std::unique_ptr<Expr> condition;
    std::vector<std::unique_ptr<Stmt>> thenBody;
    std::vector<std::pair<std::unique_ptr<Expr>, std::vector<std::unique_ptr<Stmt>>>> elifClauses;
    std::vector<std::unique_ptr<Stmt>> elseBody;
    void accept(ASTVisitor& visitor) override;
};

class WhileStmt : public Stmt {
public:
    std::unique_ptr<Expr> condition;
    std::vector<std::unique_ptr<Stmt>> body;
    std::vector<std::unique_ptr<Stmt>> elseBody;
    void accept(ASTVisitor& visitor) override;
};

class ForStmt : public Stmt {
public:
    std::unique_ptr<Expr> target;
    std::unique_ptr<Expr> iterable;
    std::vector<std::unique_ptr<Stmt>> body;
    std::vector<std::unique_ptr<Stmt>> elseBody;
    void accept(ASTVisitor& visitor) override;
};

class TryStmt : public Stmt {
public:
    struct ExceptHandler {
        std::unique_ptr<TypeExpr> type;
        std::vector<std::string> altTypeNames;
        std::string name;
        std::vector<std::unique_ptr<Stmt>> body;
        bool isStar = false;
    };
    std::vector<std::unique_ptr<Stmt>> tryBody;
    std::vector<ExceptHandler> handlers;
    std::vector<std::unique_ptr<Stmt>> elseBody;
    std::vector<std::unique_ptr<Stmt>> finallyBody;
    void accept(ASTVisitor& visitor) override;
};

class WithStmt : public Stmt {
public:
    struct WithItem {
        std::unique_ptr<Expr> contextExpr;
        std::unique_ptr<Expr> optionalVars;
    };
    std::vector<WithItem> items;
    std::vector<std::unique_ptr<Stmt>> body;
    void accept(ASTVisitor& visitor) override;
};

class ThreadStmt : public Stmt {
public:
    std::vector<std::unique_ptr<Stmt>> body;
    std::vector<std::string> capturedVars;
    std::vector<std::string> mutatedCapturedVars;
    void accept(ASTVisitor& visitor) override;
};

class DeferStmt : public Stmt {
public:
    std::unique_ptr<Expr> call;
    void accept(ASTVisitor& visitor) override;
};

struct MatchPattern {
    enum class Kind {
        Literal,
        Capture,
        Wildcard,
        Sequence,
        Or,
        Class,
        Value,
    };
    Kind kind;
    std::unique_ptr<Expr> literal;
    std::string name;
    std::vector<MatchPattern> subPatterns;
    std::unique_ptr<Expr> guard;
};

class MatchStmt : public Stmt {
public:
    struct MatchCase {
        MatchPattern pattern;
        std::unique_ptr<Expr> guard;
        std::vector<std::unique_ptr<Stmt>> body;
    };
    std::unique_ptr<Expr> subject;
    std::vector<MatchCase> cases;
    void accept(ASTVisitor& visitor) override;
};

class ReturnStmt : public Stmt {
public:
    std::unique_ptr<Expr> value;
    void accept(ASTVisitor& visitor) override;
};

class RaiseStmt : public Stmt {
public:
    std::unique_ptr<Expr> exception;
    std::unique_ptr<Expr> cause;
    void accept(ASTVisitor& visitor) override;
};

class BreakStmt : public Stmt {
public:
    void accept(ASTVisitor& visitor) override;
};

class ContinueStmt : public Stmt {
public:
    void accept(ASTVisitor& visitor) override;
};

class PassStmt : public Stmt {
public:
    void accept(ASTVisitor& visitor) override;
};

class AssertStmt : public Stmt {
public:
    std::unique_ptr<Expr> test;
    std::unique_ptr<Expr> msg;
    void accept(ASTVisitor& visitor) override;
};

class GlobalStmt : public Stmt {
public:
    std::vector<std::string> names;
    void accept(ASTVisitor& visitor) override;
};

class NonlocalStmt : public Stmt {
public:
    std::vector<std::string> names;
    void accept(ASTVisitor& visitor) override;
};

class DeleteStmt : public Stmt {
public:
    std::vector<std::unique_ptr<Expr>> targets;
    std::vector<uint8_t> provenUnique;
    void accept(ASTVisitor& visitor) override;
};

class ImportStmt : public Stmt {
public:
    struct Alias {
        std::string name;
        std::string asName;
    };
    std::vector<Alias> names;
    void accept(ASTVisitor& visitor) override;
};

class FromImportStmt : public Stmt {
public:
    std::string module;
    int level = 0;
    std::vector<ImportStmt::Alias> names;
    void accept(ASTVisitor& visitor) override;
};

struct Parameter {
    std::string name;
    std::unique_ptr<TypeExpr> type;
    std::unique_ptr<Expr> defaultValue;
    bool isVarArg = false;
    bool isKwArg = false;
    bool isOwn = false;
};

struct TypeParam {
    std::string name;
    std::unique_ptr<TypeExpr> bound;
};

class FunctionDecl : public Stmt {
public:
    std::string name;
    std::vector<TypeParam> typeParams;
    std::vector<Parameter> params;
    std::unique_ptr<TypeExpr> returnType;
    std::vector<std::unique_ptr<Stmt>> body;
    std::vector<std::unique_ptr<Expr>> decorators;
    bool isAsync = false;
    bool isMethod = false;
    bool hasImplicitSelf = false;
    bool isStatic = false;
    bool isClassMethod = false;
    bool isConstructor = false;
    bool isExtern = false;
    std::string externLib;
    std::string externSymbol;
    std::string externLang;
    std::string externPath;
    bool isProperty = false;
    std::string propertySetterFor;
    int constructorIndex = -1;
    int methodOverloadIndex = -1;
    int methodOverloadCount = 1;
    int posOnlyEnd = -1;
    int kwOnlyStart = -1;
    std::vector<std::string> capturedVars;
    std::vector<std::string> mutatedCapturedVars;
    std::optional<std::string> docstring;
    std::string genericHomeModule;
    void accept(ASTVisitor& visitor) override;
};

class ContractDecl : public Stmt {
public:
    std::string name;
    std::vector<std::string> bases;
    std::vector<std::unique_ptr<FunctionDecl>> methods;
    void accept(ASTVisitor& visitor) override;
};

class ClassDecl : public Stmt {
public:
    std::string name;
    std::vector<TypeParam> typeParams;
    std::vector<std::unique_ptr<Expr>> bases;
    std::vector<std::string> promises;
    std::vector<const ContractDecl*> conformedContracts;
    std::vector<std::pair<std::string, std::unique_ptr<Expr>>> keywords;
    std::vector<std::unique_ptr<Stmt>> body;
    std::vector<std::unique_ptr<Expr>> decorators;
    std::optional<std::string> docstring;
    std::string genericHomeModule;
    void accept(ASTVisitor& visitor) override;
};

std::vector<std::string> instanceFieldOrder(const ClassDecl& cls);

class TypeAliasStmt : public Stmt {
public:
    std::string name;
    std::unique_ptr<TypeExpr> value;
    void accept(ASTVisitor& visitor) override;
};

class Module : public ASTNode {
public:
    std::string filename;
    std::string moduleName;
    bool isDragonFile = true;
    std::vector<std::unique_ptr<Stmt>> body;
    std::optional<std::string> docstring;
    void accept(ASTVisitor& visitor) override;
};

bool stmtsAlwaysTerminate(const std::vector<std::unique_ptr<Stmt>>& stmts);

class ASTVisitor {
public:
    virtual ~ASTVisitor() = default;

    virtual void visit(NamedTypeExpr& node) = 0;
    virtual void visit(GenericTypeExpr& node) = 0;
    virtual void visit(OptionalTypeExpr& node) = 0;
    virtual void visit(UnionTypeExpr& node) = 0;
    virtual void visit(CallableTypeExpr& node) = 0;
    virtual void visit(TupleTypeExpr& node) = 0;
    virtual void visit(ContractSetTypeExpr& node) = 0;

    virtual void visit(IntegerLiteral& node) = 0;
    virtual void visit(FloatLiteral& node) = 0;
    virtual void visit(StringLiteral& node) = 0;
    virtual void visit(BooleanLiteral& node) = 0;
    virtual void visit(NoneLiteral& node) = 0;
    virtual void visit(NameExpr& node) = 0;
    virtual void visit(BinaryExpr& node) = 0;
    virtual void visit(ChainedCompExpr& node) = 0;
    virtual void visit(WalrusExpr& node) = 0;
    virtual void visit(UnaryExpr& node) = 0;
    virtual void visit(CallExpr& node) = 0;
    virtual void visit(AttributeExpr& node) = 0;
    virtual void visit(SubscriptExpr& node) = 0;
    virtual void visit(SliceExpr& node) = 0;
    virtual void visit(ListExpr& node) = 0;
    virtual void visit(TupleExpr& node) = 0;
    virtual void visit(DictExpr& node) = 0;
    virtual void visit(SetExpr& node) = 0;
    virtual void visit(ListCompExpr& node) = 0;
    virtual void visit(DictCompExpr& node) = 0;
    virtual void visit(SetCompExpr& node) = 0;
    virtual void visit(GeneratorExpr& node) = 0;
    virtual void visit(LambdaExpr& node) = 0;
    virtual void visit(IfExpr& node) = 0;
    virtual void visit(AwaitExpr& node) = 0;
    virtual void visit(AsCastExpr& node) = 0;
    virtual void visit(FireExpr& node) = 0;
    virtual void visit(YieldExpr& node) = 0;
    virtual void visit(StarredExpr& node) = 0;
    virtual void visit(TemplateExpr& node) = 0;
    virtual void visit(TemplateFileExpr& node) = 0;

    virtual void visit(ExprStmt& node) = 0;
    virtual void visit(AssignStmt& node) = 0;
    virtual void visit(AugAssignStmt& node) = 0;
    virtual void visit(AnnAssignStmt& node) = 0;
    virtual void visit(IfStmt& node) = 0;
    virtual void visit(WhileStmt& node) = 0;
    virtual void visit(ForStmt& node) = 0;
    virtual void visit(TryStmt& node) = 0;
    virtual void visit(WithStmt& node) = 0;
    virtual void visit(ThreadStmt& node) = 0;
    virtual void visit(DeferStmt& node) = 0;
    virtual void visit(MatchStmt& node) = 0;
    virtual void visit(ReturnStmt& node) = 0;
    virtual void visit(RaiseStmt& node) = 0;
    virtual void visit(BreakStmt& node) = 0;
    virtual void visit(ContinueStmt& node) = 0;
    virtual void visit(PassStmt& node) = 0;
    virtual void visit(AssertStmt& node) = 0;
    virtual void visit(GlobalStmt& node) = 0;
    virtual void visit(NonlocalStmt& node) = 0;
    virtual void visit(DeleteStmt& node) = 0;
    virtual void visit(ImportStmt& node) = 0;
    virtual void visit(FromImportStmt& node) = 0;

    virtual void visit(FunctionDecl& node) = 0;
    virtual void visit(ClassDecl& node) = 0;
    virtual void visit(ContractDecl& node) = 0;
    virtual void visit(TypeAliasStmt& node) = 0;

    virtual void visit(Module& node) = 0;
};

class DefaultASTVisitor : public ASTVisitor {
public:
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
};

class ASTPrinter : public ASTVisitor {
public:
    std::string print(ASTNode& node);

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
    std::string output_;
    int indent_ = 0;

    void write(const std::string& text);
    void writeLine(const std::string& text);
    void increaseIndent();
    void decreaseIndent();
    std::string indentStr() const;
    void printPattern(MatchPattern& pat);
};

}

#endif
