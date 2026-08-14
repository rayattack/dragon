#ifndef DRAGON_PARSER_H
#define DRAGON_PARSER_H

#include "dragon/Lexer.h"
#include "dragon/AST.h"
#include <memory>
#include <vector>
#include <string>

namespace dragon {

/// Diagnostic message from the parser
struct ParserDiagnostic {
    enum class Level { Warning, Error };
    Level level;
    SourceLocation location;
    std::string message;
};

/// Configuration options for the parser.
struct ParserOptions {
    bool isDragonFile = true;   // .dr (braces) vs .py (indentation)
    bool requireTypes = true;   // require type annotations (Dragon mode)
    std::string filename = "<stdin>";
};

/// Recursive descent parser: token stream to AST, brace- or indentation-based.
class Parser {
public:
    Parser(std::vector<Token> tokens, ParserOptions options = {});
    ~Parser();

    Parser(const Parser&) = delete;
    Parser& operator=(const Parser&) = delete;

    std::unique_ptr<Module> parseModule();
    std::unique_ptr<Expr> parseExpression();  // for REPL

    /// Parses a `template[X] { ... }` / `:{ ... }` body into TemplateParts once
    /// (only `!!{`/`!!}` are escapes); scan defects append to `errorsOut` if given.
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

    /// Consumes NEWLINE tokens after a binary op/`=` (trailing-operator
    /// continuation). Do not call before checking an operator, or it merges statements.
    void skipNewlines();

    std::unique_ptr<Expr> expression();
    std::unique_ptr<Expr> assignment();
    std::unique_ptr<Expr> ternary();      // a if cond else b
    std::unique_ptr<Expr> orExpr();
    std::unique_ptr<Expr> andExpr();
    std::unique_ptr<Expr> notExpr();
    std::unique_ptr<Expr> comparison();
    std::unique_ptr<Expr> asCast();       // ADR 054 - `x as Amazing` / `x as {A, B}`
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

    // Literals and compounds
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
    
    // Simple statements
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

    // Compound statements
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
    /// ADR 054: `type Name { def sig... }` / `type Name(A, B) { ... }`, called
    /// with `type` and the name already consumed (`type X = T` stays in statement()).
    std::unique_ptr<Stmt> contractDeclaration(std::string name);

    // Dragon-specific (.dr mode)
    std::unique_ptr<Stmt> constDeclaration();
    std::unique_ptr<Stmt> staticDeclaration();
    std::unique_ptr<Stmt> ownDeclaration();
    std::unique_ptr<Expr> maybeMoveRhs();
    std::unique_ptr<Stmt> externDeclaration();
    std::unique_ptr<Stmt> parseExternFuncSig(const std::string& libHint);
    // D052 process-lane extern: parse the signature + `from "path"` and
    // synthesize the runs[T] wrapper body in place.
    std::unique_ptr<Stmt> parseProcessExternDef(const std::string& lang);

    std::vector<std::unique_ptr<Expr>> parseDecorators();

    std::unique_ptr<TypeExpr> parseType();
    std::unique_ptr<TypeExpr> parseUnionType();
    std::unique_ptr<TypeExpr> parsePrimaryType();
    std::unique_ptr<TypeExpr> parseGenericType(std::unique_ptr<TypeExpr> base);

    /// D044: optional PEP 695 `[T, U, ...]` after a class/function name; empty
    /// if no `[`. Bare identifiers only for now; `T: Bound` is reserved for D046.
    std::vector<TypeParam> parseTypeParams();

    std::vector<std::unique_ptr<Stmt>> parseBlock();
    std::vector<Parameter> parseParameters();  // (x: int, y: str = "default")

    void error(const std::string& message);
    void error(const Token& token, const std::string& message);
    void synchronize();

    // Panic mode recovery points.
    bool isAtStatementBoundary() const;
    bool isAtBlockEnd() const;

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace dragon

#endif // DRAGON_PARSER_H
