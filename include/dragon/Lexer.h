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

/// Configuration options for the lexer
struct LexerOptions {
    /// If true, use curly braces for blocks; if false, use indentation
    bool useBraceBlocks = true;

    /// Tab width for indentation calculation
    int tabWidth = 4;

    /// Filename for error reporting
    std::string filename = "<stdin>";

    /// When true, the lexer is scanning the body of a `!{ ... }` template
    /// block-interpolation. In this mode `:{` is recognized as
    /// TEMPLATE_CONTENT_OPEN - the terse content-mode alias from D017
    /// Phase 4 §"Block Interpolation and the :{} Content Alias". Outside of
    /// `!{}` (i.e., in regular source), `:` and `{` keep their normal
    /// meanings (slice colon / block brace) and `:{` is a parse error.
    /// Caller-set by CodeGen when re-lexing the body of each `!{...}`.
    bool inTemplateInterpolation = false;
};

/// Lexer for Dragon and Python source code
/// 
/// Handles both brace-delimited blocks (.dr) and indentation-based blocks (.py).
/// Produces a stream of tokens that can be consumed by the parser.
class Lexer {
public:
    explicit Lexer(std::string_view source, LexerOptions options = {});
    ~Lexer();

    // Disable copy
    Lexer(const Lexer&) = delete;
    Lexer& operator=(const Lexer&) = delete;

    /// Tokenize the entire source and return all tokens
    std::vector<Token> tokenize();

    /// Get the next token from the source
    Token nextToken();

    /// Peek at the next token without consuming it
    Token peek();

    /// Peek at token N positions ahead (0 = next token)
    Token peekAhead(int n);

    /// Check if we've reached end of file
    bool isAtEnd() const;

    /// Get all diagnostics generated during lexing
    const std::vector<LexerDiagnostic>& diagnostics() const;

    /// Check if any errors occurred
    bool hasErrors() const;

private:
    // Scanning helpers
    char advance();
    char current() const;
    char peekChar() const;
    char peekNext() const;
    bool match(char expected);
    bool isAtEnd(size_t offset) const;

    // Token creation
    Token makeToken(TokenType type);
    Token makeToken(TokenType type, std::string_view lexeme);
    Token errorToken(const std::string& message);

    // Scanning methods
    Token scanToken();
    Token scanString(char quote);
    Token scanNumber();
    Token scanIdentifier();
    Token scanTemplateBody(const std::string& contentType = "");
    // D017 Phase 4 - scan a `:{ ... }` content fragment inside a `!{}`
    // block-interpolation. The caller has already consumed the `:` and the
    // next character is `{`. Brace-depth scan with the same rules as
    // scanTemplateBody; emits TEMPLATE_CONTENT_OPEN whose lexeme is the
    // raw body (without outer braces). `!{...}` interpolations inside the
    // body are preserved as-is for the recursive CodeGen pass to process.
    Token scanTemplateContentBody();
    void skipWhitespaceAndComments();
    void handleIndentation();

    // Indentation tracking (for Python mode)
    void pushIndent(int level);
    void popIndent();
    int currentIndent() const;

    // Error reporting
    void addDiagnostic(LexerDiagnostic::Level level, const std::string& message);

    // State
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace dragon

#endif // DRAGON_LEXER_H
