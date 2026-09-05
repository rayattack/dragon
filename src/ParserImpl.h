#ifndef DRAGON_PARSER_IMPL_H
#define DRAGON_PARSER_IMPL_H

#include "dragon/Parser.h"
#include "dragon/AST.h"
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace dragon {

struct Parser::Impl {
    std::vector<Token> tokens;
    ParserOptions options;
    size_t current = 0;
    bool inClassBody = false;
    std::vector<ParserDiagnostic> diagnostics;
    std::vector<std::unique_ptr<Stmt>> pendingStmts;
    bool ffiProcessImportsInjected = false;
    int recursionDepth = 0;
    static constexpr int kMaxRecursionDepth = 500;
};

inline std::string reservedWordAsNameMessage(std::string_view word) {
    return "'" + std::string(word) +
           "' is a reserved word and cannot be used as a name; choose a different name";
}

inline bool keywordMayBeFollowedByColon(TokenType type) {
    switch (type) {
        case TokenType::TRY:
        case TokenType::ELSE:
        case TokenType::ELIF:
        case TokenType::EXCEPT:
        case TokenType::FINALLY:
        case TokenType::CATCH:
        case TokenType::LAMBDA:
            return true;
        default:
            return false;
    }
}

inline bool isAssignmentOperator(TokenType type) {
    switch (type) {
        case TokenType::EQUAL:
        case TokenType::PLUS_EQUAL:
        case TokenType::MINUS_EQUAL:
        case TokenType::STAR_EQUAL:
        case TokenType::SLASH_EQUAL:
        case TokenType::DOUBLE_SLASH_EQUAL:
        case TokenType::PERCENT_EQUAL:
        case TokenType::POWER_EQUAL:
        case TokenType::AT_EQUAL:
        case TokenType::AMPERSAND_EQUAL:
        case TokenType::PIPE_EQUAL:
        case TokenType::CARET_EQUAL:
        case TokenType::LEFT_SHIFT_EQUAL:
        case TokenType::RIGHT_SHIFT_EQUAL:
            return true;
        default:
            return false;
    }
}

inline bool parseIntLiteralChecked(const std::string& s, int base, int64_t& out) {
    try {
        size_t pos = 0;
        out = static_cast<int64_t>(std::stoll(s, &pos, base));
        return pos == s.size();
    } catch (const std::exception&) {
        return false;
    }
}

inline bool parseFloatLiteralChecked(const std::string& s, double& out) {
    try {
        size_t pos = 0;
        out = std::stod(s, &pos);
        return pos == s.size();
    } catch (const std::exception&) {
        return false;
    }
}

struct ParserRecursionGuard {
    int& depth;
    explicit ParserRecursionGuard(int& d) : depth(d) { ++depth; }
    ~ParserRecursionGuard() { --depth; }
};

inline std::optional<std::string> extractDocstring(
    const std::vector<std::unique_ptr<Stmt>>& body) {
    if (body.empty()) return std::nullopt;
    auto* first = dynamic_cast<ExprStmt*>(body.front().get());
    if (!first) return std::nullopt;
    auto* lit = dynamic_cast<StringLiteral*>(first->expr.get());
    if (!lit) return std::nullopt;
    if (lit->isFString || lit->isBytes) return std::nullopt;
    return lit->value;
}

}

#endif
