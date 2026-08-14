#ifndef DRAGON_LEXER_H
#define DRAGON_LEXER_H

#include "dragon/Token.h"
#include <string>
#include <string_view>
#include <vector>
#include <memory>

namespace dragon {

/// Diagnostic message from the lexer
struct LexerDiagnostic {
    enum class Level { Warning, Error };
    Level level;
    SourceLocation location;
    std::string message;
};

/// Configuration options for the lexer.
struct LexerOptions {
    bool useBraceBlocks = true;  // braces vs indentation for blocks
    int tabWidth = 4;
    std::string filename = "<stdin>";

    // True inside a `!{ ... }` body, where `:{` means TEMPLATE_CONTENT_OPEN
    // (D017 Phase 4) instead of a parse error. Set by CodeGen when re-lexing it.
    bool inTemplateInterpolation = false;
};

/// Lexer for Dragon (.dr, brace blocks) and Python (.py, indentation blocks)
/// source, producing the token stream the parser consumes.
class Lexer {
public:
    explicit Lexer(std::string_view source, LexerOptions options = {});
    ~Lexer();

    Lexer(const Lexer&) = delete;
    Lexer& operator=(const Lexer&) = delete;

    std::vector<Token> tokenize();
    Token nextToken();
    Token peek();

    /// Peeks token N positions ahead (0 = next token).
    Token peekAhead(int n);

    bool isAtEnd() const;
    const std::vector<LexerDiagnostic>& diagnostics() const;
    bool hasErrors() const;

private:
    char advance();
    char current() const;
    char peekChar() const;
    char peekNext() const;
    bool match(char expected);
    bool isAtEnd(size_t offset) const;

    Token makeToken(TokenType type);
    Token makeToken(TokenType type, std::string_view lexeme);
    Token errorToken(const std::string& message);

    Token scanToken();
    Token scanString(char quote);
    Token scanNumber();
    Token scanIdentifier();
    Token scanTemplateBody(const std::string& contentType = "");
    // D017 Phase 4: scans a `:{ ... }` content fragment inside `!{}` (caller
    // already consumed `:`), brace-depth matched like scanTemplateBody.
    Token scanTemplateContentBody();
    void skipWhitespaceAndComments();
    void handleIndentation();

    void pushIndent(int level);
    void popIndent();
    int currentIndent() const;

    void addDiagnostic(LexerDiagnostic::Level level, const std::string& message);

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace dragon

#endif // DRAGON_LEXER_H
