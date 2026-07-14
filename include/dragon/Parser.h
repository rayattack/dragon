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

/// Configuration options for the parser
struct ParserOptions {
    /// If true, parsing .dr file (curly braces); if false, .py file (indentation)
    bool isDragonFile = true;
    
    /// If true, type annotations are required (Dragon mode)
    bool requireTypes = true;
    
    /// Filename for error reporting
    std::string filename = "<stdin>";
};

/// Recursive descent parser for Dragon/Python
/// 
/// Produces an AST from a token stream. Handles both brace-delimited
/// and indentation-based syntax depending on configuration.
class Parser {
public:
    Parser(std::vector<Token> tokens, ParserOptions options = {});
    ~Parser();

    // Disable copy
    Parser(const Parser&) = delete;
    Parser& operator=(const Parser&) = delete;

    /// Parse a complete module
    std::unique_ptr<Module> parseModule();

    /// Parse a single expression (for REPL)
    std::unique_ptr<Expr> parseExpression();

    /// Parse a template body (the text between the outer braces of a
    /// `template[X] { ... }` or `:{ ... }` content alias) into structured
    /// TemplateParts: literal text runs, `!{expr}` interpolations, and
    /// `!{ ...statements... }` block interpolations. Runs once so the
    /// TypeChecker and CodeGen walk one shared, typed AST instead of
    /// re-lexing the raw body at each stage. `isDragonFile` selects the sub-
    /// parser surface for the interpolation bodies. Static because it builds
    /// its own sub-lexers/sub-parsers and needs no outer parser state.
    static std::vector<TemplatePart> parseTemplateBody(
        const std::string& body, const SourceLocation& loc, bool isDragonFile);

    /// Parse a single statement
    std::unique_ptr<Stmt> parseStatement();

    /// Get all diagnostics generated during parsing
    const std::vector<ParserDiagnostic>& diagnostics() const;

    /// Check if any errors occurred
    bool hasErrors() const;

private:
    //===------------------------------------------------------------------===//
    // Token Management
    //===------------------------------------------------------------------===//
    
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

    /// Consume any sequence of NEWLINE tokens. Used to allow line breaks
    /// after a binary operator or `=` where the parser is committed to
    /// reading the next operand and a NEWLINE between them is purely
    /// cosmetic continuation (the Python/JS "trailing operator" rule).
    /// Do NOT call before *checking* an operator - that would silently
    /// merge two independent statements together.
    void skipNewlines();

    //===------------------------------------------------------------------===//
    // Expression Parsing
    //===------------------------------------------------------------------===//

    std::unique_ptr<Expr> expression();
    std::unique_ptr<Expr> assignment();
    std::unique_ptr<Expr> ternary();      // a if cond else b
    std::unique_ptr<Expr> orExpr();
    std::unique_ptr<Expr> andExpr();
    std::unique_ptr<Expr> notExpr();
    std::unique_ptr<Expr> comparison();
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

    //===------------------------------------------------------------------===//
    // Statement Parsing
    //===------------------------------------------------------------------===//

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
    std::unique_ptr<Stmt> matchStatement();
    MatchPattern parsePattern(bool allowCommaOr = true);
    std::unique_ptr<Stmt> functionDeclaration();
    std::unique_ptr<Stmt> classDeclaration();
    std::unique_ptr<Stmt> enumDeclaration();

    // Dragon-specific (.dr mode)
    std::unique_ptr<Stmt> constDeclaration();
    std::unique_ptr<Stmt> staticDeclaration();
    std::unique_ptr<Stmt> ownDeclaration();
    std::unique_ptr<Expr> maybeMoveRhs();
    std::unique_ptr<Stmt> externDeclaration();
    std::unique_ptr<Stmt> parseExternFuncSig(const std::string& libHint);

    // Helper for decorated definitions
    std::vector<std::unique_ptr<Expr>> parseDecorators();

    //===------------------------------------------------------------------===//
    // Type Annotation Parsing
    //===------------------------------------------------------------------===//

    std::unique_ptr<TypeExpr> parseType();
    std::unique_ptr<TypeExpr> parseUnionType();
    std::unique_ptr<TypeExpr> parsePrimaryType();
    std::unique_ptr<TypeExpr> parseGenericType(std::unique_ptr<TypeExpr> base);

    /// D044 - parse an optional PEP 695 type-parameter list `[T, U, ...]`
    /// immediately after a class/function name. Returns empty when no `[` is
    /// present. In declaration position a `[` is unambiguously a type-param
    /// list (subscript only follows a value expression), so no lookahead trick
    /// is needed. v1 accepts bare identifiers only; a `T: Bound` form is
    /// reserved for D046 and rejected here for now.
    std::vector<TypeParam> parseTypeParams();

    //===------------------------------------------------------------------===//
    // Block Parsing
    //===------------------------------------------------------------------===//

    /// Parse a block of statements (handles both {} and indentation)
    std::vector<std::unique_ptr<Stmt>> parseBlock();
    
    /// Parse function parameters: (x: int, y: str = "default")
    std::vector<Parameter> parseParameters();

    //===------------------------------------------------------------------===//
    // Error Handling
    //===------------------------------------------------------------------===//

    void error(const std::string& message);
    void error(const Token& token, const std::string& message);
    void synchronize();
    
    // Panic mode recovery points
    bool isAtStatementBoundary() const;
    bool isAtBlockEnd() const;

    // State
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace dragon

#endif // DRAGON_PARSER_H
