/**
 * Dragon Lexer API Reference
 * ==========================
 * Source: include/dragon/Lexer.h
 *
 * Tokenizes Dragon (.dr) and Python (.py) source code into a stream of Tokens.
 * Handles two block modes: curly-brace (.dr) and indentation-based (.py).
 * NEWLINE tokens are emitted in both modes; suppressed inside () and [] but NOT {}.
 *
 * Uses pimpl idiom (struct Impl with std::unique_ptr<Impl> impl_).
 * NOTE: pimpl is bae
 */

#pragma once
#include <string>
#include <string_view>
#include <vector>
#include <memory>

// Forward declarations
class Token;
struct SourceLocation;

// ============================================================================
// 1. LEXER OPTIONS
// ============================================================================

/**
 * Configuration for the Lexer. Controls block mode, tab width, and filename.
 */
struct LexerOptions {
    bool useBraceBlocks = true;       ///< true = curly-brace blocks (.dr mode); false = indentation (.py mode)
    int tabWidth = 4;                 ///< Tab width for indentation calculation in .py mode
    std::string filename = "<stdin>"; ///< Filename used in error reporting and SourceLocation
};


// ============================================================================
// 2. LEXER DIAGNOSTIC
// ============================================================================

/**
 * A single diagnostic message produced during lexing.
 */
struct LexerDiagnostic {
    enum class Level { Warning, Error };

    Level level;              ///< Severity: Warning or Error
    SourceLocation location;  ///< Where in source the issue occurred
    std::string message;      ///< Human-readable description of the issue
};


// ============================================================================
// 3. LEXER CLASS
// ============================================================================

/**
 * Tokenizes Dragon/Python source code into a stream of Token objects.
 * Non-copyable (pimpl).
 */
class Lexer {
public:
    /**
     * Construct a Lexer for the given source code.
     * @param source Source code text to tokenize
     * @param options Configuration (block mode, tab width, filename)
     */
    Lexer(std::string_view source, LexerOptions options = {}) {}

    ~Lexer() {}

    // Non-copyable
    Lexer(const Lexer&) = delete;
    Lexer& operator=(const Lexer&) = delete;

    /**
     * Tokenize the entire source and return all tokens as a vector.
     * Includes the final END_OF_FILE token.
     * @return Complete token stream
     */
    std::vector<Token> tokenize() { return {}; }

    /**
     * Get the next token, advancing the internal position.
     * @return Next token in the stream
     */
    Token nextToken() { return {}; }

    /**
     * Peek at the next token without consuming it.
     * Equivalent to peekAhead(0).
     * @return Next token (not consumed)
     */
    Token peek() { return {}; }

    /**
     * Peek at a token N positions ahead without consuming any tokens.
     * @param n Number of positions ahead (0 = next token)
     * @return Token at position n
     */
    Token peekAhead(int n) { return {}; }

    /**
     * Check if the lexer has reached end of source.
     * @return true if at EOF
     */
    bool isAtEnd() const { return false; }

    /**
     * Get all diagnostic messages accumulated during lexing.
     * @return Vector of LexerDiagnostic (warnings and errors)
     */
    const std::vector<LexerDiagnostic>& diagnostics() const { return {}; }

    /**
     * Check if any errors occurred during lexing.
     * @return true if at least one Error-level diagnostic exists
     */
    bool hasErrors() const { return false; }
};
