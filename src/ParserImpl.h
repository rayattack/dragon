/// Parser::Impl pimpl struct and small literal/docstring helpers, shared so
/// Parser.cpp/ParserStmts.cpp split without duplicating state (helpers are `inline`).
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
    // Extra stmts from multi-decl constructs (extern "C" from "lib" { })
    std::vector<std::unique_ptr<Stmt>> pendingStmts;
    // D052 - the ffi/json imports a process extern needs, injected once per module.
    bool ffiProcessImportsInjected = false;
    // Recursion depth cap - prevents stack overflow on `(((...)))`-style
    // attacks. Compiler is exposed to user input via dragonlang.org.
    int recursionDepth = 0;
    static constexpr int kMaxRecursionDepth = 500;
};

// Guards numeric literal conversion: an oversized digit run would otherwise
// throw std::out_of_range from stoll/stod and terminate the process.
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

// RAII guard for the recursion-depth counter: increments on entry, decrements
// on scope exit so error-recovery early returns can't leak depth.
struct ParserRecursionGuard {
    int& depth;
    explicit ParserRecursionGuard(int& d) : depth(d) { ++depth; }
    ~ParserRecursionGuard() { --depth; }
};

// A body's first bare string literal (not f-string, not bytes) is the
// docstring, left in the body too (matches CPython's `Module.body[0]`).
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

}  // namespace dragon

#endif  // DRAGON_PARSER_IMPL_H
