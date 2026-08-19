#ifndef DRAGON_PARSER_H
#define DRAGON_PARSER_H

#include "dragon/Lexer.h"
#include "dragon/AST.h"
#include <memory>
#include <vector>
#include <string>

namespace dragon {

struct ParserDiagnostic {
    enum class Level { Warning, Error };
    Level level;
    SourceLocation location;
    std::string message;
};

struct ParserOptions {
    bool isDragonFile = true;
    bool requireTypes = true;
    std::string filename = "<stdin>";
};

class Parser {
public:
    Parser(std::vector<Token> tokens, ParserOptions options = {});
    ~Parser();

    Parser(const Parser&) = delete;
    Parser& operator=(const Parser&) = delete;

    std::unique_ptr<Module> parseModule();
    std::unique_ptr<Expr> parseExpression();

    static std::vector<TemplatePart> parseTemplateBody(
        const std::string& body, const SourceLocation& loc, bool isDragonFile,
        std::vector<std::string>* errorsOut = nullptr);

    std::unique_ptr<Stmt> parseStatement();
    const std::vector<ParserDiagnostic>& diagnostics() const;
    bool hasErrors() const;

private:
    Token advance();
    Token current() const;
    Token previous() const;
    Token peek() const;
    Token peekNext() const;
    bool check(TokenType type) const;
    bool match(TokenType type);
    template<typename... Types>
    bool match(Types... types) { return (match(types) || ...); }
    Token consume(TokenType type, const std::string& message);
    bool isAtEnd() const;

    void skipNewlines();

    std::unique_ptr<Expr> expression();
    std::unique_ptr<Expr> assignment();
    std::unique_ptr<Expr> ternary();
    std::unique_ptr<Expr> orExpr();
    std::unique_ptr<Expr> andExpr();
    std::unique_ptr<Expr> notExpr();
    std::unique_ptr<Expr> comparison();
    std::unique_ptr<Expr> asCast();
    std::unique_ptr<Expr> bitwiseOr();
    std::unique_ptr<Expr> bitwiseXor();
    std::unique_ptr<Expr> bitwiseAnd();
    std::unique_ptr<Expr> shift();
    std::unique_ptr<Expr> term();
    std::unique_ptr<Expr> factor();
    std::unique_ptr<Expr> unary();
    std::unique_ptr<Expr> power();
    std::unique_ptr<Expr> fireExpr();
    std::unique_ptr<Expr> awaitExpr();
    std::unique_ptr<Expr> primary();
    std::unique_ptr<Expr> call();
    std::unique_ptr<Expr> subscript();
    std::unique_ptr<Expr> attribute();
    std::unique_ptr<Expr> finishCall(std::unique_ptr<Expr> callee);
    std::unique_ptr<Expr> ownershipMarkedName();
    void rejectNonBindingOwnershipTarget(bool isDub);
    void discardOwnershipTargetSuffix();

    std::unique_ptr<Expr> parseLambda();
    std::unique_ptr<Expr> parseList();
    std::unique_ptr<Expr> parseDict();
    std::unique_ptr<Expr> parseTuple();
    std::unique_ptr<Expr> parseSet();
    std::unique_ptr<Expr> parseListComp();
    std::unique_ptr<Expr> parseDictComp();
    std::unique_ptr<Expr> parseYield();

    std::unique_ptr<Stmt> statement();
    std::unique_ptr<Stmt> simpleStatement();
    std::unique_ptr<Stmt> compoundStatement();

    std::unique_ptr<Stmt> expressionStatement();
    std::unique_ptr<Stmt> assignmentStatement();
    std::unique_ptr<Stmt> returnStatement();
    std::unique_ptr<Stmt> raiseStatement();
    std::unique_ptr<Stmt> breakStatement();
    std::unique_ptr<Stmt> continueStatement();
    std::unique_ptr<Stmt> passStatement();
    std::unique_ptr<Stmt> assertStatement();
    std::unique_ptr<Stmt> globalStatement();
    std::unique_ptr<Stmt> nonlocalStatement();
    std::unique_ptr<Stmt> deleteStatement();
    std::unique_ptr<Stmt> importStatement();
    std::unique_ptr<Stmt> fromImportStatement();

    std::unique_ptr<Stmt> ifStatement();
    std::unique_ptr<Stmt> whileStatement();
    std::unique_ptr<Stmt> forStatement();
    std::unique_ptr<Stmt> tryStatement();
    std::unique_ptr<Stmt> withStatement();
    std::unique_ptr<Stmt> threadStatement();
    std::unique_ptr<Stmt> deferStatement();
    std::unique_ptr<Stmt> matchStatement();
    MatchPattern parsePattern(bool allowCommaOr = true);
    std::unique_ptr<Stmt> functionDeclaration();
    std::unique_ptr<Stmt> classDeclaration();
    std::unique_ptr<Stmt> enumDeclaration();
    std::unique_ptr<Stmt> contractDeclaration(std::string name);

    std::unique_ptr<Stmt> constDeclaration();
    std::unique_ptr<Stmt> staticDeclaration();
    std::unique_ptr<Stmt> ownDeclaration();
    std::unique_ptr<Expr> maybeMoveRhs();
    std::unique_ptr<Stmt> externDeclaration();
    std::unique_ptr<Stmt> parseExternFuncSig(const std::string& libHint);
    std::unique_ptr<Stmt> parseProcessExternDef(const std::string& lang);

    std::vector<std::unique_ptr<Expr>> parseDecorators();

    std::unique_ptr<TypeExpr> parseType();
    std::unique_ptr<TypeExpr> parseUnionType();
    std::unique_ptr<TypeExpr> parsePrimaryType();
    std::unique_ptr<TypeExpr> parseGenericType(std::unique_ptr<TypeExpr> base);

    std::vector<TypeParam> parseTypeParams();

    std::vector<std::unique_ptr<Stmt>> parseBlock();
    std::vector<Parameter> parseParameters();

    void error(const std::string& message);
    void error(const Token& token, const std::string& message);
    void synchronize();

    bool isAtStatementBoundary() const;
    bool isAtBlockEnd() const;

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}

#endif
