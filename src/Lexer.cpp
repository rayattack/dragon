#include "dragon/Lexer.h"
#include <vector>
#include <stack>
#include <cctype>

namespace dragon {

struct Lexer::Impl {
    std::string_view source;
    LexerOptions options;

    size_t start = 0;
    size_t current = 0;
    size_t line = 1;
    size_t column = 1;
    size_t startColumn = 1;

    std::vector<Token> peekedTokens;
    std::vector<LexerDiagnostic> diagnostics;
    std::stack<int> indentStack;
    int pendingDedents = 0;
    bool pendingIndent = false;
    bool atLineStart = true;
    int nestingDepth = 0;
};

Lexer::Lexer(std::string_view source, LexerOptions options)
    : impl_(std::make_unique<Impl>()) {
    impl_->source = source;
    impl_->options = options;
    impl_->indentStack.push(0);
}

Lexer::~Lexer() = default;

std::vector<Token> Lexer::tokenize() {
    std::vector<Token> tokens;
    while (true) {
        Token tok = nextToken();
        tokens.push_back(tok);
        if (tok.type() == TokenType::END_OF_FILE) break;
    }
    return tokens;
}

Token Lexer::nextToken() {
    if (!impl_->peekedTokens.empty()) {
        Token tok = impl_->peekedTokens.front();
        impl_->peekedTokens.erase(impl_->peekedTokens.begin());
        return tok;
    }

    if (impl_->pendingDedents > 0) {
        impl_->pendingDedents--;
        return makeToken(TokenType::DEDENT, "");
    }

    if (impl_->pendingIndent) {
        impl_->pendingIndent = false;
        impl_->start = impl_->current;
        impl_->startColumn = impl_->column;
        return makeToken(TokenType::INDENT, "");
    }

    if (!impl_->options.useBraceBlocks && impl_->atLineStart) {
        handleIndentation();
        if (impl_->pendingIndent) {
            impl_->pendingIndent = false;
            impl_->start = impl_->current;
            impl_->startColumn = impl_->column;
            return makeToken(TokenType::INDENT, "");
        }
        if (impl_->pendingDedents > 0) {
            impl_->pendingDedents--;
            return makeToken(TokenType::DEDENT, "");
        }
    }

    skipWhitespaceAndComments();

    if (isAtEnd()) {
        if (!impl_->options.useBraceBlocks && impl_->indentStack.size() > 1) {
            impl_->indentStack.pop();
            impl_->pendingDedents = static_cast<int>(impl_->indentStack.size()) - 1;
            while (impl_->indentStack.size() > 1) impl_->indentStack.pop();
            return makeToken(TokenType::DEDENT, "");
        }
        return makeToken(TokenType::END_OF_FILE);
    }

    impl_->start = impl_->current;
    impl_->startColumn = impl_->column;
    return scanToken();
}

Token Lexer::peek() {
    if (impl_->peekedTokens.empty()) {
        impl_->peekedTokens.push_back(nextToken());
    }
    return impl_->peekedTokens.front();
}

Token Lexer::peekAhead(int n) {
    while (static_cast<int>(impl_->peekedTokens.size()) <= n) {
        auto saved = std::move(impl_->peekedTokens);
        impl_->peekedTokens.clear();
        Token tok = nextToken();
        impl_->peekedTokens = std::move(saved);
        impl_->peekedTokens.push_back(tok);
    }
    return impl_->peekedTokens[n];
}

bool Lexer::isAtEnd() const {
    return impl_->current >= impl_->source.length();
}

const std::vector<LexerDiagnostic>& Lexer::diagnostics() const {
    return impl_->diagnostics;
}

bool Lexer::hasErrors() const {
    for (const auto& diag : impl_->diagnostics) {
        if (diag.level == LexerDiagnostic::Level::Error) return true;
    }
    return false;
}

char Lexer::advance() {
    char c = impl_->source[impl_->current++];
    if (c == '\n') {
        impl_->line++;
        impl_->column = 1;
    } else {
        impl_->column++;
    }
    return c;
}

char Lexer::current() const {
    if (isAtEnd()) return '\0';
    return impl_->source[impl_->current];
}

char Lexer::peekChar() const {
    if (isAtEnd()) return '\0';
    return impl_->source[impl_->current];
}

char Lexer::peekNext() const {
    if (impl_->current + 1 >= impl_->source.length()) return '\0';
    return impl_->source[impl_->current + 1];
}

bool Lexer::match(char expected) {
    if (isAtEnd()) return false;
    if (impl_->source[impl_->current] != expected) return false;
    impl_->current++;
    impl_->column++;
    return true;
}

bool Lexer::isAtEnd(size_t offset) const {
    return (impl_->current + offset) >= impl_->source.length();
}

Token Lexer::makeToken(TokenType type) {
    SourceLocation loc{impl_->options.filename, impl_->line,
                       impl_->startColumn, impl_->start};
    std::string lexeme(impl_->source.substr(
        impl_->start, impl_->current - impl_->start));
    return Token(type, std::move(lexeme), loc);
}

Token Lexer::makeToken(TokenType type, std::string_view lexeme) {
    SourceLocation loc{impl_->options.filename, impl_->line,
                       impl_->startColumn, impl_->start};
    return Token(type, std::string(lexeme), loc);
}

Token Lexer::errorToken(const std::string& message) {
    addDiagnostic(LexerDiagnostic::Level::Error, message);
    return makeToken(TokenType::ERROR);
}

Token Lexer::scanToken() {
    char c = advance();

    if (std::isalpha(c) || c == '_') {
        if ((c == 'f' || c == 'r' || c == 'b' || c == 'F' || c == 'R' || c == 'B') &&
            !isAtEnd()) {
            char next = peekChar();
            if (next == '"' || next == '\'') {
                advance();
                return scanString(next);
            }
            if ((c == 'r' || c == 'R') && (next == 'b' || next == 'B') && !isAtEnd(1)) {
                char next2 = impl_->source[impl_->current + 1];
                if (next2 == '"' || next2 == '\'') {
                    advance();
                    advance();
                    return scanString(next2);
                }
            }
            if ((c == 'b' || c == 'B') && (next == 'r' || next == 'R') && !isAtEnd(1)) {
                char next2 = impl_->source[impl_->current + 1];
                if (next2 == '"' || next2 == '\'') {
                    advance();
                    advance();
                    return scanString(next2);
                }
            }
        }
        return scanIdentifier();
    }

    if (std::isdigit(c)) {
        return scanNumber();
    }

    if (c == '"' || c == '\'') {
        return scanString(c);
    }

    switch (c) {
        case '(':
            impl_->nestingDepth++;
            return makeToken(TokenType::LEFT_PAREN);
        case ')':
            if (impl_->nestingDepth > 0) impl_->nestingDepth--;
            return makeToken(TokenType::RIGHT_PAREN);
        case '[':
            impl_->nestingDepth++;
            return makeToken(TokenType::LEFT_BRACKET);
        case ']':
            if (impl_->nestingDepth > 0) impl_->nestingDepth--;
            return makeToken(TokenType::RIGHT_BRACKET);
        case '{':
            if (!impl_->options.useBraceBlocks) impl_->nestingDepth++;
            return makeToken(TokenType::LEFT_BRACE);
        case '}':
            if (!impl_->options.useBraceBlocks && impl_->nestingDepth > 0) impl_->nestingDepth--;
            return makeToken(TokenType::RIGHT_BRACE);

        case ',': return makeToken(TokenType::COMMA);
        case ';': return makeToken(TokenType::SEMICOLON);

        case ':':
            if (match('=')) return makeToken(TokenType::WALRUS);
            if (impl_->options.inTemplateInterpolation && peekChar() == '{') {
                return scanTemplateContentBody();
            }
            return makeToken(TokenType::COLON);

        case '.':
            if (peekChar() == '.' && peekNext() == '.') {
                advance();
                advance();
                return makeToken(TokenType::ELLIPSIS);
            }
            return makeToken(TokenType::DOT);

        case '+':
            if (match('=')) return makeToken(TokenType::PLUS_EQUAL);
            return makeToken(TokenType::PLUS);

        case '-':
            if (match('=')) return makeToken(TokenType::MINUS_EQUAL);
            if (match('>')) return makeToken(TokenType::ARROW);
            return makeToken(TokenType::MINUS);

        case '*':
            if (match('*')) {
                if (match('=')) return makeToken(TokenType::POWER_EQUAL);
                return makeToken(TokenType::POWER);
            }
            if (match('=')) return makeToken(TokenType::STAR_EQUAL);
            return makeToken(TokenType::STAR);

        case '/':
            if (match('/')) {
                if (match('=')) return makeToken(TokenType::DOUBLE_SLASH_EQUAL);
                return makeToken(TokenType::DOUBLE_SLASH);
            }
            if (match('=')) return makeToken(TokenType::SLASH_EQUAL);
            return makeToken(TokenType::SLASH);

        case '%':
            if (match('=')) return makeToken(TokenType::PERCENT_EQUAL);
            return makeToken(TokenType::PERCENT);

        case '@':
            if (match('=')) return makeToken(TokenType::AT_EQUAL);
            return makeToken(TokenType::AT);

        case '&':
            if (match('=')) return makeToken(TokenType::AMPERSAND_EQUAL);
            return makeToken(TokenType::AMPERSAND);

        case '|':
            if (match('=')) return makeToken(TokenType::PIPE_EQUAL);
            return makeToken(TokenType::PIPE);

        case '^':
            if (match('=')) return makeToken(TokenType::CARET_EQUAL);
            return makeToken(TokenType::CARET);

        case '~':
            return makeToken(TokenType::TILDE);

        case '=':
            if (match('=')) return makeToken(TokenType::EQUAL_EQUAL);
            return makeToken(TokenType::EQUAL);

        case '!':
            if (match('=')) return makeToken(TokenType::NOT_EQUAL);
            return errorToken("Unexpected character '!'. Use 'not' for boolean negation.");

        case '<':
            if (match('<')) {
                if (match('=')) return makeToken(TokenType::LEFT_SHIFT_EQUAL);
                return makeToken(TokenType::LEFT_SHIFT);
            }
            if (match('=')) return makeToken(TokenType::LESS_EQUAL);
            return makeToken(TokenType::LESS);

        case '>':
            if (match('>')) {
                if (match('=')) return makeToken(TokenType::RIGHT_SHIFT_EQUAL);
                return makeToken(TokenType::RIGHT_SHIFT);
            }
            if (match('=')) return makeToken(TokenType::GREATER_EQUAL);
            return makeToken(TokenType::GREATER);

        case '\n':
            if (impl_->nestingDepth == 0) {
                impl_->atLineStart = true;
                return makeToken(TokenType::NEWLINE);
            }
            impl_->start = impl_->current;
            impl_->startColumn = impl_->column;
            skipWhitespaceAndComments();
            if (isAtEnd()) return makeToken(TokenType::END_OF_FILE);
            impl_->start = impl_->current;
            impl_->startColumn = impl_->column;
            return scanToken();
    }

    return errorToken(std::string("Unexpected character '") + c + "'");
}

Token Lexer::scanString(char quote) {
    bool isTriple = false;
    if (peekChar() == quote && peekNext() == quote) {
        advance();
        advance();
        isTriple = true;
    }

    while (!isAtEnd()) {
        char c = peekChar();

        if (c == '\\') {
            advance();
            if (!isAtEnd()) advance();
            continue;
        }

        if (c == quote) {
            if (isTriple) {
                if (peekNext() == quote && !isAtEnd(2) &&
                    impl_->source[impl_->current + 2] == quote) {
                    advance();
                    advance();
                    advance();
                    return makeToken(TokenType::STRING);
                }
                advance();
                continue;
            } else {
                advance();
                return makeToken(TokenType::STRING);
            }
        }

        if (c == '\n') {
            if (!isTriple) {
                return errorToken("Unterminated string literal");
            }
        }

        advance();
    }

    return errorToken("Unterminated string literal");
}

Token Lexer::scanNumber() {
    char first = impl_->source[impl_->start];

    if (first == '0' && !isAtEnd()) {
        char prefix = peekChar();
        if (prefix == 'x' || prefix == 'X') {
            advance();
            if (!isAtEnd() && (std::isxdigit(peekChar()) || peekChar() == '_')) {
                while (!isAtEnd() && (std::isxdigit(peekChar()) || peekChar() == '_'))
                    advance();
                return makeToken(TokenType::INTEGER);
            }
            return errorToken("Invalid hexadecimal literal");
        }
        if (prefix == 'b' || prefix == 'B') {
            advance();
            if (!isAtEnd() && (peekChar() == '0' || peekChar() == '1' || peekChar() == '_')) {
                while (!isAtEnd() && (peekChar() == '0' || peekChar() == '1' || peekChar() == '_'))
                    advance();
                return makeToken(TokenType::INTEGER);
            }
            return errorToken("Invalid binary literal");
        }
        if (prefix == 'o' || prefix == 'O') {
            advance();
            if (!isAtEnd() && ((peekChar() >= '0' && peekChar() <= '7') || peekChar() == '_')) {
                while (!isAtEnd() && ((peekChar() >= '0' && peekChar() <= '7') || peekChar() == '_'))
                    advance();
                return makeToken(TokenType::INTEGER);
            }
            return errorToken("Invalid octal literal");
        }
    }

    while (!isAtEnd() && (std::isdigit(peekChar()) || peekChar() == '_'))
        advance();

    bool isFloat = false;

    if (!isAtEnd() && peekChar() == '.' && !isAtEnd(1) && std::isdigit(peekNext())) {
        isFloat = true;
        advance();
        while (!isAtEnd() && (std::isdigit(peekChar()) || peekChar() == '_'))
            advance();
    }

    if (!isAtEnd() && (peekChar() == 'e' || peekChar() == 'E')) {
        isFloat = true;
        advance();
        if (!isAtEnd() && (peekChar() == '+' || peekChar() == '-'))
            advance();
        if (!isAtEnd() && std::isdigit(peekChar())) {
            while (!isAtEnd() && (std::isdigit(peekChar()) || peekChar() == '_'))
                advance();
        } else {
            return errorToken("Invalid numeric literal: expected digits after exponent");
        }
    }

    return makeToken(isFloat ? TokenType::FLOAT : TokenType::INTEGER);
}

Token Lexer::scanIdentifier() {
    while (!isAtEnd() && (std::isalnum(peekChar()) || peekChar() == '_')) {
        advance();
    }

    std::string_view text = impl_->source.substr(
        impl_->start, impl_->current - impl_->start);

    if (text == "template") {
        size_t saved = impl_->current;
        while (!isAtEnd() && (peekChar() == ' ' || peekChar() == '\t')) {
            advance();
        }
        if (!isAtEnd() && peekChar() == '{') {
            return scanTemplateBody();
        }
        if (!isAtEnd() && peekChar() == '[') {
            advance();
            std::string contentType;
            while (!isAtEnd() && peekChar() != ']') {
                contentType += peekChar();
                advance();
            }
            if (isAtEnd()) {
                return errorToken("Unterminated template content type bracket");
            }
            advance();
            while (!isAtEnd() && (peekChar() == ' ' || peekChar() == '\t')) {
                advance();
            }
            if (!isAtEnd() && peekChar() == '{') {
                return scanTemplateBody(contentType);
            }
            if (!isAtEnd() && peekChar() == '(') {
                std::string lexeme = contentType;
                lexeme += '\0';
                SourceLocation loc{impl_->options.filename, impl_->line,
                                   impl_->startColumn, impl_->start};
                return Token(TokenType::TEMPLATE, std::move(lexeme), loc);
            }
            return errorToken("Expected '{' or '(' after template[" + contentType + "]");
        }
        impl_->current = saved;
    }

    TokenType type = keywordType(text);
    return makeToken(type);
}

Token Lexer::scanTemplateContentBody() {
    advance();

    int depth = 1;
    std::string body;
    while (!isAtEnd() && depth > 0) {
        char c = peekChar();
        if (c == '{') {
            depth++;
            body += c;
            advance();
        } else if (c == '}') {
            depth--;
            if (depth > 0) body += c;
            advance();
        } else {
            body += c;
            advance();
        }
    }
    if (depth != 0) {
        return errorToken("Unterminated :{ ... } content block");
    }
    SourceLocation loc{impl_->options.filename, impl_->line,
                       impl_->startColumn, impl_->start};
    return Token(TokenType::TEMPLATE_CONTENT_OPEN, std::move(body), loc);
}

Token Lexer::scanTemplateBody(const std::string& contentType) {
    advance();

    int depth = 1;
    std::string body;

    while (!isAtEnd() && depth > 0) {
        char c = peekChar();
        if (c == '{') {
            depth++;
            body += c;
            advance();
        } else if (c == '}') {
            depth--;
            if (depth > 0) {
                body += c;
            }
            advance();
        } else {
            body += c;
            advance();
        }
    }

    if (depth != 0) {
        return errorToken("Unterminated template block");
    }

    std::string lexeme;
    if (!contentType.empty()) {
        lexeme = contentType;
        lexeme += '\0';
        lexeme += body;
    } else {
        lexeme = std::move(body);
    }

    SourceLocation loc{impl_->options.filename, impl_->line,
                       impl_->startColumn, impl_->start};
    return Token(TokenType::TEMPLATE, std::move(lexeme), loc);
}

void Lexer::skipWhitespaceAndComments() {
    while (!isAtEnd()) {
        char c = peekChar();
        switch (c) {
            case ' ':
            case '\t':
            case '\r':
                advance();
                break;
            case '\n':
                if (impl_->options.useBraceBlocks) {
                    if (impl_->nestingDepth > 0) {
                        advance();
                    } else {
                        return;
                    }
                } else {
                    return;
                }
                break;
            case '#':
                while (!isAtEnd() && peekChar() != '\n')
                    advance();
                break;
            default:
                return;
        }
    }
}

void Lexer::handleIndentation() {
    int indent = 0;
    while (!isAtEnd()) {
        if (peekChar() == ' ') {
            indent++;
            advance();
        } else if (peekChar() == '\t') {
            indent += impl_->options.tabWidth;
            advance();
        } else {
            break;
        }
    }

    if (isAtEnd() || peekChar() == '\n' || peekChar() == '#') {
        return;
    }

    int currentLevel = currentIndent();
    if (indent > currentLevel) {
        pushIndent(indent);
        impl_->pendingIndent = true;
    } else if (indent < currentLevel) {
        while (indent < currentIndent()) {
            popIndent();
            impl_->pendingDedents++;
        }
        if (indent != currentIndent()) {
            addDiagnostic(LexerDiagnostic::Level::Error,
                          "Indentation does not match any outer level");
        }
    }
    impl_->atLineStart = false;
}

void Lexer::pushIndent(int level) {
    impl_->indentStack.push(level);
}

void Lexer::popIndent() {
    if (impl_->indentStack.size() > 1)
        impl_->indentStack.pop();
}

int Lexer::currentIndent() const {
    return impl_->indentStack.top();
}

void Lexer::addDiagnostic(LexerDiagnostic::Level level, const std::string& message) {
    impl_->diagnostics.push_back({
        level,
        {impl_->options.filename, impl_->line, impl_->column, impl_->current},
        message
    });
}

}
