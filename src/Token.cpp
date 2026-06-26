#include "dragon/Token.h"
#include <unordered_map>

namespace dragon {

//===----------------------------------------------------------------------===//
// Token Implementation
//===----------------------------------------------------------------------===//

Token::Token() : type_(TokenType::ERROR), lexeme_(""), location_{} {}

Token::Token(TokenType type, std::string lexeme, SourceLocation location)
    : type_(type), lexeme_(std::move(lexeme)), location_(std::move(location)) {}

TokenType Token::type() const { return type_; }
const std::string& Token::lexeme() const { return lexeme_; }
const SourceLocation& Token::location() const { return location_; }

bool Token::is(TokenType t) const { return type_ == t; }

std::string Token::toString() const {
    std::string result = tokenTypeName(type_);
    if (!lexeme_.empty() && type_ != TokenType::END_OF_FILE) {
        result += "(";
        result += lexeme_;
        result += ")";
    }
    return result;
}

const char* Token::tokenTypeName(TokenType type) {
    switch (type) {
        // Literals
        case TokenType::INTEGER: return "INTEGER";
        case TokenType::FLOAT: return "FLOAT";
        case TokenType::STRING: return "STRING";
        case TokenType::BYTES: return "BYTES";
        case TokenType::TRUE: return "TRUE";
        case TokenType::FALSE: return "FALSE";
        case TokenType::NONE: return "NONE";

        // Identifiers
        case TokenType::IDENTIFIER: return "IDENTIFIER";

        // Keywords
        case TokenType::AND: return "AND";
        case TokenType::AS: return "AS";
        case TokenType::ASSERT: return "ASSERT";
        case TokenType::ASYNC: return "ASYNC";
        case TokenType::AWAIT: return "AWAIT";
        case TokenType::BREAK: return "BREAK";
        case TokenType::CLASS: return "CLASS";
        case TokenType::CONTINUE: return "CONTINUE";
        case TokenType::DEF: return "DEF";
        case TokenType::DEL: return "DEL";
        case TokenType::ELIF: return "ELIF";
        case TokenType::ELSE: return "ELSE";
        case TokenType::EXCEPT: return "EXCEPT";
        case TokenType::FINALLY: return "FINALLY";
        case TokenType::FIRE: return "FIRE";
        case TokenType::FOR: return "FOR";
        case TokenType::FROM: return "FROM";
        case TokenType::GLOBAL: return "GLOBAL";
        case TokenType::IF: return "IF";
        case TokenType::IMPORT: return "IMPORT";
        case TokenType::IN: return "IN";
        case TokenType::IS: return "IS";
        case TokenType::LAMBDA: return "LAMBDA";
        case TokenType::NONLOCAL: return "NONLOCAL";
        case TokenType::NOT: return "NOT";
        case TokenType::OR: return "OR";
        case TokenType::PASS: return "PASS";
        case TokenType::RAISE: return "RAISE";
        case TokenType::RETURN: return "RETURN";
        case TokenType::TRY: return "TRY";
        case TokenType::WHILE: return "WHILE";
        case TokenType::WITH: return "WITH";
        case TokenType::YIELD: return "YIELD";
        case TokenType::CATCH: return "CATCH";
        case TokenType::CONST: return "CONST";
        case TokenType::STATIC: return "STATIC";
        case TokenType::EXTERN: return "EXTERN";
        case TokenType::THREAD: return "THREAD";
        case TokenType::ENUM: return "ENUM";

        // Operators
        case TokenType::PLUS: return "PLUS";
        case TokenType::MINUS: return "MINUS";
        case TokenType::STAR: return "STAR";
        case TokenType::SLASH: return "SLASH";
        case TokenType::DOUBLE_SLASH: return "DOUBLE_SLASH";
        case TokenType::PERCENT: return "PERCENT";
        case TokenType::POWER: return "POWER";
        case TokenType::AT: return "AT";
        case TokenType::AMPERSAND: return "AMPERSAND";
        case TokenType::PIPE: return "PIPE";
        case TokenType::CARET: return "CARET";
        case TokenType::TILDE: return "TILDE";
        case TokenType::LEFT_SHIFT: return "LEFT_SHIFT";
        case TokenType::RIGHT_SHIFT: return "RIGHT_SHIFT";

        // Comparison
        case TokenType::LESS: return "LESS";
        case TokenType::GREATER: return "GREATER";
        case TokenType::LESS_EQUAL: return "LESS_EQUAL";
        case TokenType::GREATER_EQUAL: return "GREATER_EQUAL";
        case TokenType::EQUAL_EQUAL: return "EQUAL_EQUAL";
        case TokenType::NOT_EQUAL: return "NOT_EQUAL";
        case TokenType::NOT_IN: return "NOT_IN";
        case TokenType::IS_NOT: return "IS_NOT";

        // Assignment
        case TokenType::EQUAL: return "EQUAL";
        case TokenType::PLUS_EQUAL: return "PLUS_EQUAL";
        case TokenType::MINUS_EQUAL: return "MINUS_EQUAL";
        case TokenType::STAR_EQUAL: return "STAR_EQUAL";
        case TokenType::SLASH_EQUAL: return "SLASH_EQUAL";
        case TokenType::DOUBLE_SLASH_EQUAL: return "DOUBLE_SLASH_EQUAL";
        case TokenType::PERCENT_EQUAL: return "PERCENT_EQUAL";
        case TokenType::POWER_EQUAL: return "POWER_EQUAL";
        case TokenType::AT_EQUAL: return "AT_EQUAL";
        case TokenType::AMPERSAND_EQUAL: return "AMPERSAND_EQUAL";
        case TokenType::PIPE_EQUAL: return "PIPE_EQUAL";
        case TokenType::CARET_EQUAL: return "CARET_EQUAL";
        case TokenType::LEFT_SHIFT_EQUAL: return "LEFT_SHIFT_EQUAL";
        case TokenType::RIGHT_SHIFT_EQUAL: return "RIGHT_SHIFT_EQUAL";
        case TokenType::WALRUS: return "WALRUS";

        // Delimiters
        case TokenType::LEFT_PAREN: return "LEFT_PAREN";
        case TokenType::RIGHT_PAREN: return "RIGHT_PAREN";
        case TokenType::LEFT_BRACKET: return "LEFT_BRACKET";
        case TokenType::RIGHT_BRACKET: return "RIGHT_BRACKET";
        case TokenType::LEFT_BRACE: return "LEFT_BRACE";
        case TokenType::RIGHT_BRACE: return "RIGHT_BRACE";
        case TokenType::COMMA: return "COMMA";
        case TokenType::COLON: return "COLON";
        case TokenType::SEMICOLON: return "SEMICOLON";
        case TokenType::DOT: return "DOT";
        case TokenType::ARROW: return "ARROW";
        case TokenType::ELLIPSIS: return "ELLIPSIS";

        // Indentation
        case TokenType::INDENT: return "INDENT";
        case TokenType::DEDENT: return "DEDENT";
        case TokenType::NEWLINE: return "NEWLINE";

        // Template
        case TokenType::TEMPLATE: return "TEMPLATE";
        case TokenType::TEMPLATE_CONTENT_OPEN: return "TEMPLATE_CONTENT_OPEN";

        // Special
        case TokenType::END_OF_FILE: return "EOF";
        case TokenType::ERROR: return "ERROR";
    }
    return "UNKNOWN";
}

//===----------------------------------------------------------------------===//
// Keyword Handling
//===----------------------------------------------------------------------===//

static const std::unordered_map<std::string_view, TokenType> keywords = {
    {"and", TokenType::AND},
    {"as", TokenType::AS},
    {"assert", TokenType::ASSERT},
    {"async", TokenType::ASYNC},
    {"await", TokenType::AWAIT},
    {"break", TokenType::BREAK},
    {"class", TokenType::CLASS},
    {"continue", TokenType::CONTINUE},
    {"def", TokenType::DEF},
    {"del", TokenType::DEL},
    {"elif", TokenType::ELIF},
    {"else", TokenType::ELSE},
    {"except", TokenType::EXCEPT},
    {"finally", TokenType::FINALLY},
    {"fire", TokenType::FIRE},
    {"for", TokenType::FOR},
    {"from", TokenType::FROM},
    {"global", TokenType::GLOBAL},
    {"if", TokenType::IF},
    {"import", TokenType::IMPORT},
    {"in", TokenType::IN},
    {"is", TokenType::IS},
    {"lambda", TokenType::LAMBDA},
    {"nonlocal", TokenType::NONLOCAL},
    {"not", TokenType::NOT},
    {"or", TokenType::OR},
    {"pass", TokenType::PASS},
    {"raise", TokenType::RAISE},
    {"return", TokenType::RETURN},
    {"try", TokenType::TRY},
    {"while", TokenType::WHILE},
    {"with", TokenType::WITH},
    {"yield", TokenType::YIELD},
    {"True", TokenType::TRUE},
    {"False", TokenType::FALSE},
    {"None", TokenType::NONE},
    {"true", TokenType::TRUE},      // .dr mode alias
    {"false", TokenType::FALSE},    // .dr mode alias
    {"none", TokenType::NONE},      // .dr mode alias
    {"catch", TokenType::CATCH},    // Dragon extension
    {"const", TokenType::CONST},    // Dragon extension
    {"static", TokenType::STATIC},  // Dragon extension
    {"extern", TokenType::EXTERN},  // Dragon extension (C FFI)
    // NOTE: "thread" is a contextual keyword - resolved in Parser::statement(),
    // NOT in the lexer. This allows "thread" as an identifier in all other positions
    // (parameter names, variable names, etc.).
};

bool isKeyword(std::string_view name) {
    return keywords.find(name) != keywords.end();
}

TokenType keywordType(std::string_view name) {
    auto it = keywords.find(name);
    if (it != keywords.end()) {
        return it->second;
    }
    return TokenType::IDENTIFIER;
}

} // namespace dragon
