/**
 * Dragon Parser API Reference
 * ===========================
 * Source: include/dragon/Parser.h
 *
 * Recursive descent parser consuming a Token stream and producing an AST.
 * Supports both .dr (curly-brace) and .py (indentation) modes.
 * ~50 AST node types. Uses pendingStmts mechanism for multi-decl constructs.
 *
 * Uses pimpl idiom (struct Impl with std::unique_ptr<Impl> impl_).
 * NOTE: pimpl is bae
 */

#pragma once
#include <string>
#include <vector>
#include <memory>

// Forward declarations
class Token;
class Module;
class Expr;
class Stmt;
struct SourceLocation;

// ============================================================================
// 1. PARSER OPTIONS
// ============================================================================

/**
 * Configuration for the Parser. Controls .dr vs .py mode and type requirements.
 */
struct ParserOptions {
    bool isDragonFile = true;    ///< true = .dr mode (curly braces); false = .py mode (indentation)
    bool requireTypes = true;    ///< true = type annotations required (Dragon); false = optional (Python)
    std::string filename = "<stdin>";  ///< Filename for error reporting
};


// ============================================================================
// 2. PARSER DIAGNOSTIC
// ============================================================================

/**
 * A single diagnostic message produced during parsing.
 */
struct ParserDiagnostic {
    enum class Level { Warning, Error };

    Level level;
    SourceLocation location;
    std::string message;
};


// ============================================================================
// 3. PARSER CLASS
// ============================================================================

/**
 * Recursive descent parser. Consumes tokens, produces AST.
 * Non-copyable (pimpl).
 */
class Parser {
public:
    /**
     * Construct a Parser from a token stream.
     * @param tokens Complete token vector from the Lexer
     * @param options Configuration (mode, type requirements, filename)
     */
    Parser(std::vector<Token> tokens, ParserOptions options = {}) {}

    ~Parser() {}

    // Non-copyable
    Parser(const Parser&) = delete;
    Parser& operator=(const Parser&) = delete;

    // --- Entry Points ---

    /**
     * Parse a complete module (top-level statements until EOF).
     * Main entry point for file compilation.
     * @return Module AST node containing all top-level statements
     */
    std::unique_ptr<Module> parseModule() { return nullptr; }

    /**
     * Parse a single expression. Used for REPL and expression evaluation.
     * @return Expression AST node
     */
    std::unique_ptr<Expr> parseExpression() { return nullptr; }

    /**
     * Parse a single statement.
     * @return Statement AST node
     */
    std::unique_ptr<Stmt> parseStatement() { return nullptr; }

    // --- Diagnostics ---

    /**
     * Get all diagnostics generated during parsing.
     * @return Vector of ParserDiagnostic (warnings and errors)
     */
    const std::vector<ParserDiagnostic>& diagnostics() const { return {}; }

    /**
     * Check if any errors occurred during parsing.
     * @return true if at least one Error-level diagnostic exists
     */
    bool hasErrors() const { return false; }
};


// ============================================================================
// 4. INTERNAL PARSER METHODS (private, listed for reference)
// ============================================================================

/*
 * Token Management:
 *  advance(), current(), previous(), peek(), peekNext()
 *  check(TokenType), match(TokenType), match(Types...)
 *  consume(TokenType, message), isAtEnd()
 *
 * Expression Parsing (precedence climbing):
 *  expression() -> assignment() -> ternary() -> orExpr() -> andExpr() ->
 *  notExpr() -> comparison() -> bitwiseOr() -> bitwiseXor() -> bitwiseAnd() ->
 *  shift() -> term() -> factor() -> unary() -> power() ->
 *  fireExpr() -> awaitExpr() -> primary() -> call() -> subscript() -> attribute()
 *  finishCall(callee)
 *
 * Literal/Compound:
 *  parseLambda(), parseList(), parseDict(), parseTuple(), parseSet()
 *  parseListComp(), parseDictComp(), parseYield()
 *
 * Statement Parsing:
 *  statement() -> simpleStatement() | compoundStatement()
 *
 * Simple Statements:
 *  expressionStatement(), assignmentStatement(), returnStatement()
 *  raiseStatement(), breakStatement(), continueStatement(), passStatement()
 *  assertStatement(), globalStatement(), nonlocalStatement()
 *  deleteStatement(), importStatement(), fromImportStatement()
 *
 * Compound Statements:
 *  ifStatement(), whileStatement(), forStatement(), tryStatement()
 *  withStatement(), threadStatement(), matchStatement(), parsePattern()
 *  functionDeclaration(), classDeclaration()
 *
 * Dragon-Specific (.dr mode):
 *  constDeclaration(), staticDeclaration(), selfConstructor()
 *  externDeclaration(), parseExternFuncSig(libHint)
 *
 * Decorators:
 *  parseDecorators() -> vector of decorator expressions
 *
 * Type Annotation Parsing:
 *  parseType() -> parseUnionType() -> parsePrimaryType() -> parseGenericType(base)
 *
 * Block & Parameters:
 *  parseBlock() - handles both {} and indentation blocks
 *  parseParameters() - function parameter list with types and defaults
 *
 * Error Recovery:
 *  error(message), error(token, message)
 *  synchronize() - skip to next statement boundary
 *  isAtStatementBoundary(), isAtBlockEnd()
 */
