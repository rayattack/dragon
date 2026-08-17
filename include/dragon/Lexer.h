#ifndef DRAGON_LEXER_H
#define DRAGON_LEXER_H

#include "dragon/Token.h"
#include <string>
#include <string_view>
#include <vector>
#include <memory>

namespace dragon {

struct LexerDiagnostic {
    enum class Level { Warning, Error };
    Level level;
    SourceLocation location;
    std::string message;
};

struct LexerOptions {
    bool useBraceBlocks = true;
    int tabWidth = 4;
    std::string filename = "<stdin>";

    bool inTemplateInterpolation = false;
};

class Lexer {
public:
    explicit Lexer(std::string_view source, LexerOptions options = {});
    ~Lexer();

    Lexer(const Lexer&) = delete;
    Lexer& operator=(const Lexer&) = delete;

    std::vector<Token> tokenize();
    Token nextToken();
    Token peek();

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

}

#endif
