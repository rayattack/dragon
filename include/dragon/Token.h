#ifndef DRAGON_TOKEN_H
#define DRAGON_TOKEN_H

#include <string>
#include <string_view>

namespace dragon {

enum class TokenType {
    INTEGER,
    FLOAT,
    STRING,
    BYTES,
    TRUE,
    FALSE,
    NONE,

    IDENTIFIER,

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

    CATCH,
    CONST,
    STATIC,
    EXTERN,
    THREAD,
    ENUM,

    PLUS,
    MINUS,
    STAR,
    SLASH,
    DOUBLE_SLASH,
    PERCENT,
    POWER,
    AT,
    AMPERSAND,
    PIPE,
    CARET,
    TILDE,
    LEFT_SHIFT,
    RIGHT_SHIFT,

    LESS,
    GREATER,
    LESS_EQUAL,
    GREATER_EQUAL,
    EQUAL_EQUAL,
    NOT_EQUAL,
    NOT_IN,
    IS_NOT,

    EQUAL,
    PLUS_EQUAL,
    MINUS_EQUAL,
    STAR_EQUAL,
    SLASH_EQUAL,
    DOUBLE_SLASH_EQUAL,
    PERCENT_EQUAL,
    POWER_EQUAL,
    AT_EQUAL,
    AMPERSAND_EQUAL,
    PIPE_EQUAL,
    CARET_EQUAL,
    LEFT_SHIFT_EQUAL,
    RIGHT_SHIFT_EQUAL,
    WALRUS,

    LEFT_PAREN,
    RIGHT_PAREN,
    LEFT_BRACKET,
    RIGHT_BRACKET,
    LEFT_BRACE,
    RIGHT_BRACE,
    COMMA,
    COLON,
    SEMICOLON,
    DOT,
    ARROW,
    ELLIPSIS,

    INDENT,
    DEDENT,
    NEWLINE,

    TEMPLATE,
    TEMPLATE_CONTENT_OPEN,

    END_OF_FILE,
    ERROR
};

struct SourceLocation {
    std::string filename;
    size_t line = 0;
    size_t column = 0;
    size_t offset = 0;
};

class Token {
public:
    Token();
    Token(TokenType type, std::string lexeme, SourceLocation location);

    TokenType type() const;
    const std::string& lexeme() const;
    const SourceLocation& location() const;

    bool is(TokenType t) const;

    template<typename... Types>
    bool isOneOf(Types... types) const {
        return (is(types) || ...);
    }

    std::string toString() const;

    static const char* tokenTypeName(TokenType type);

private:
    TokenType type_;
    std::string lexeme_;
    SourceLocation location_;
};

bool isKeyword(std::string_view name);

TokenType keywordType(std::string_view name);

}

#endif
