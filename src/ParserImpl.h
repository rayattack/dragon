/// Dragon Parser - shared internal definitions
///
/// Holds the `Parser::Impl` pimpl struct and the small literal/docstring
/// helpers, so the parser can be split across translation units (Parser.cpp =
/// expressions + plumbing, ParserStmts.cpp = statement & declaration parsing)
/// without duplicating state. Pure code motion - no behavior change. The
/// helpers are `inline` so both TUs may include this header without an ODR
/// clash.
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
    // Recursion depth cap - prevents stack overflow on `(((...)))`-style
    // attacks. Compiler is exposed to user input via dragonlang.org.
    int recursionDepth = 0;
    static constexpr int kMaxRecursionDepth = 500;
};

// Numeric literal conversion guarded against out-of-range/malformed input.
// The lexer accepts any-length digit runs, so an oversized literal would
// otherwise propagate std::out_of_range from stoll/stod and terminate the
// process (compiler is exposed to user input via dragonlang.org).
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

// RAII guard for the parser's recursion-depth counter. Increments on entry to
// a deep recursion sink (expression / statement) and decrements on scope exit,
// so even error-recovery early returns can't leak depth. Shared by both parser
// TUs. `inline` namespace-scope (not anonymous) so the two TUs reference one
// type, not two ODR-distinct copies.
struct ParserRecursionGuard {
    int& depth;
    explicit ParserRecursionGuard(int& d) : depth(d) { ++depth; }
    ~ParserRecursionGuard() { --depth; }
};

// A function / class / module body's first statement, if it's a bare string
// literal (not f-string, not bytes), is the docstring. Python keeps it in the
// body too - the literal still evaluates at init time and the AST visitor sees
// it, matching CPython's `Module.body[0]` shape. Net runtime cost is zero (a
// StrLiteral, no refcount; LLVM DCE removes the dead load).
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
