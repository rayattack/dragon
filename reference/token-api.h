/**
 * Dragon Token API Reference
 * ==========================
 * Source: include/dragon/Token.h
 *
 * Defines the lexical token representation, token types, source locations,
 * and keyword lookup utilities. Tokens are the output of the Lexer and
 * input to the Parser.
 */

#pragma once
#include <string>
#include <string_view>

// ============================================================================
// 1. SOURCE LOCATION
// ============================================================================

/**
 * Tracks the origin of a token or AST node in source code.
 * Carried through every compiler stage for error reporting.
 */
struct SourceLocation {
    std::string filename;  ///< Source file name (e.g., "main.dr", "<stdin>")
    size_t line;           ///< 1-based line number
    size_t column;         ///< 1-based column number
    size_t offset;         ///< 0-based byte offset in source text
};


// ============================================================================
// 2. TOKEN TYPE ENUM
// ============================================================================

/**
 * All lexical token types produced by the Dragon lexer.
 * Covers literals, operators, delimiters, keywords, and special tokens.
 *
 * ~90+ values. Key groups:
 *
 * Literals: INTEGER, FLOAT, STRING, FSTRING, BYTES
 * Identifiers: IDENTIFIER
 * Keywords: DEF, CLASS, IF, ELIF, ELSE, WHILE, FOR, IN, RETURN,
 *  BREAK, CONTINUE, PASS, IMPORT, FROM, AS, TRY, EXCEPT,
 *  FINALLY, RAISE, WITH, ASSERT, GLOBAL, NONLOCAL, DEL,
 *  AND, OR, NOT, IS, LAMBDA, YIELD, AWAIT, ASYNC, TRUE,
 *  FALSE, NONE, MATCH, CASE, TYPE, FIRE, THREAD,
 *  CONST, STATIC, SELF, EXTERN
 * Operators: PLUS, MINUS, STAR, SLASH, DOUBLE_SLASH, PERCENT,
 *  DOUBLE_STAR, AMPERSAND, PIPE, CARET, TILDE,
 *  LEFT_SHIFT, RIGHT_SHIFT,
 *  EQUAL, NOT_EQUAL, LESS, GREATER, LESS_EQUAL, GREATER_EQUAL,
 *  ASSIGN, PLUS_ASSIGN, MINUS_ASSIGN, STAR_ASSIGN, SLASH_ASSIGN,
 *  PERCENT_ASSIGN, DOUBLE_STAR_ASSIGN, DOUBLE_SLASH_ASSIGN,
 *  AMPERSAND_ASSIGN, PIPE_ASSIGN, CARET_ASSIGN,
 *  LEFT_SHIFT_ASSIGN, RIGHT_SHIFT_ASSIGN,
 *  WALRUS (:=), ARROW (->), AT (@)
 * Delimiters: LEFT_PAREN, RIGHT_PAREN, LEFT_BRACKET, RIGHT_BRACKET,
 *  LEFT_BRACE, RIGHT_BRACE, COMMA, COLON, SEMICOLON, DOT,
 *  ELLIPSIS (...)
 * Whitespace: NEWLINE, INDENT, DEDENT
 * Special: END_OF_FILE, ERROR
 */
enum class TokenType {
    // (values omitted - see include/dragon/Token.h for full listing)
};


// ============================================================================
// 3. TOKEN CLASS
// ============================================================================

/**
 * Represents a single lexical token with its type, text, and source location.
 * Produced by Lexer, consumed by Parser.
 *
 * NOTE: STRING token lexeme includes quotes ("hello" not hello) - strip before comparing.
 */
class Token {
public:
    /** Default constructor (creates END_OF_FILE token). */
    Token() {}

    /**
     * Construct a token with type, lexeme text, and source location.
     * @param type TokenType enum value
     * @param lexeme Raw text of the token from source
     * @param location Where in source this token was found
     */
    Token(TokenType type, std::string lexeme, SourceLocation location) {}

    /** Get this token's type. */
    TokenType type() const { return {}; }

    /** Get the raw text of this token (e.g., "42", "def", "+", "\"hello\""). */
    const std::string& lexeme() const { return {}; }

    /** Get the source location of this token. */
    const SourceLocation& location() const { return {}; }

    /**
     * Check if this token is of a specific type.
     * @param t TokenType to compare against
     * @return true if this token's type matches t
     */
    bool is(TokenType t) const { return false; }

    /**
     * Check if this token matches any of several types (variadic).
     * @param types One or more TokenType values
     * @return true if this token matches any of them
     */
    template<typename... Types>
    bool isOneOf(Types... types) const { return false; }

    /** Get a human-readable string for debugging (e.g., "INTEGER '42' at line 5"). */
    std::string toString() const { return {}; }

    /**
     * Get the name of a token type as a string (e.g., "INTEGER", "DEF", "PLUS").
     * @param type TokenType enum value
     * @return Static string name
     */
    static const char* tokenTypeName(TokenType type) { return nullptr; }
};


// ============================================================================
// 4. FREE FUNCTIONS
// ============================================================================

/**
 * Check if a string is a Python/Dragon keyword.
 * @param name Identifier text to check
 * @return true if it's a reserved keyword
 */
bool isKeyword(std::string_view name) { return false; }

/**
 * Get the TokenType for a keyword string.
 * @param name Keyword text (e.g., "def", "class", "fire")
 * @return Corresponding TokenType, or TokenType::IDENTIFIER if not a keyword
 */
TokenType keywordType(std::string_view name) { return {}; }
