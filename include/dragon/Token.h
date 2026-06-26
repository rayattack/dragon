#ifndef DRAGON_TOKEN_H
#define DRAGON_TOKEN_H

#include <string>
#include <string_view>

namespace dragon {

/// All token types recognized by the Dragon lexer
enum class TokenType {
    // Literals
    INTEGER,
    FLOAT,
    STRING,
    BYTES,
    TRUE,
    FALSE,
    NONE,

    // Identifiers and Keywords
    IDENTIFIER,

    // Python Keywords
    AND,
    AS,
    ASSERT,
    ASYNC,
    AWAIT,
    BREAK,
    CLASS,
    CONTINUE,
    DEF,
    DEL,
    ELIF,
    ELSE,
    EXCEPT,
    FINALLY,
    FIRE,
    FOR,
    FROM,
    GLOBAL,
    IF,
    IMPORT,
    IN,
    IS,
    LAMBDA,
    NONLOCAL,
    NOT,
    OR,
    PASS,
    RAISE,
    RETURN,
    TRY,
    WHILE,
    WITH,
    YIELD,

    // Dragon-specific keywords
    CATCH,   // Alternative to 'except' for brace syntax
    CONST,   // Immutable binding (Dragon extension)
    STATIC,  // Static field/method (Dragon extension)
    EXTERN,  // C FFI: extern "C" declarations
    THREAD,  // Scoped OS thread: thread { block }
    ENUM,    // 4.2: enum Name { A, B, C } - int-backed enum (Dragon extension)

    // Operators
    PLUS,           // +
    MINUS,          // -
    STAR,           // *
    SLASH,          // /
    DOUBLE_SLASH,   // //
    PERCENT,        // %
    POWER,          // **
    AT,             // @
    AMPERSAND,      // &
    PIPE,           // |
    CARET,          // ^
    TILDE,          // ~
    LEFT_SHIFT,     // <<
    RIGHT_SHIFT,    // >>

    // Comparison
    LESS,           // <
    GREATER,        // >
    LESS_EQUAL,     // <=
    GREATER_EQUAL,  // >=
    EQUAL_EQUAL,    // ==
    NOT_EQUAL,      // !=
    NOT_IN,         // not in (synthesized in Parser::comparison from NOT followed by IN)
    IS_NOT,         // is not (synthesized in Parser::comparison from IS followed by NOT)

    // Assignment
    EQUAL,          // =
    PLUS_EQUAL,     // +=
    MINUS_EQUAL,    // -=
    STAR_EQUAL,     // *=
    SLASH_EQUAL,    // /=
    DOUBLE_SLASH_EQUAL, // //=
    PERCENT_EQUAL,  // %=
    POWER_EQUAL,    // **=
    AT_EQUAL,       // @=
    AMPERSAND_EQUAL,// &=
    PIPE_EQUAL,     // |=
    CARET_EQUAL,    // ^=
    LEFT_SHIFT_EQUAL,  // <<=
    RIGHT_SHIFT_EQUAL, // >>=
    WALRUS,         // :=

    // Delimiters
    LEFT_PAREN,     // (
    RIGHT_PAREN,    // )
    LEFT_BRACKET,   // [
    RIGHT_BRACKET,  // ]
    LEFT_BRACE,     // { (Dragon block delimiter)
    RIGHT_BRACE,    // } (Dragon block delimiter)
    COMMA,          // ,
    COLON,          // :
    SEMICOLON,      // ;
    DOT,            // .
    ARROW,          // ->
    ELLIPSIS,       // ...

    // Indentation (for Python compatibility)
    INDENT,
    DEDENT,
    NEWLINE,

    // Template
    TEMPLATE,                // template { ... } body
    TEMPLATE_CONTENT_OPEN,   // :{ ... } - content alias inside !{} block-interp

    // Special
    END_OF_FILE,
    ERROR
};

/// Source location information
struct SourceLocation {
    std::string filename;
    size_t line = 0;
    size_t column = 0;
    size_t offset = 0;
};

/// Represents a single token from the source
class Token {
public:
    Token();
    Token(TokenType type, std::string lexeme, SourceLocation location);

    TokenType type() const;
    const std::string& lexeme() const;
    const SourceLocation& location() const;

    /// Check if token is a specific type
    bool is(TokenType t) const;

    /// Check if token is one of several types
    template<typename... Types>
    bool isOneOf(Types... types) const {
        return (is(types) || ...);
    }

    /// String representation for debugging
    std::string toString() const;

    /// Get the name of a token type
    static const char* tokenTypeName(TokenType type);

private:
    TokenType type_;
    std::string lexeme_;
    SourceLocation location_;
};

/// Check if a string is a Python/Dragon keyword
bool isKeyword(std::string_view name);

/// Get the TokenType for a keyword (or IDENTIFIER if not a keyword)
TokenType keywordType(std::string_view name);

} // namespace dragon

#endif // DRAGON_TOKEN_H
