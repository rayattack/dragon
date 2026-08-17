#pragma once

#include "dragon/Lexer.h"
#include "dragon/Parser.h"
#include "dragon/Sema.h"
#include "dragon/TypeChecker.h"
#include <string>
#include <vector>
#include <memory>

namespace dragon::test {

inline std::vector<Token> lex(const std::string& source, bool isDragon = true) {
    LexerOptions opts;
    opts.useBraceBlocks = isDragon;
    opts.filename = "<test>";
    Lexer lexer(source, opts);
    return lexer.tokenize();
}

inline std::unique_ptr<Module> parse(const std::string& source, bool isDragon = true) {
    auto tokens = lex(source, isDragon);
    ParserOptions opts;
    opts.isDragonFile = isDragon;
    opts.requireTypes = isDragon;
    opts.filename = "<test>";
    Parser parser(std::move(tokens), opts);
    return parser.parseModule();
}

inline std::vector<ParserDiagnostic> parseErrors(const std::string& source, bool isDragon = true) {
    auto tokens = lex(source, isDragon);
    ParserOptions opts;
    opts.isDragonFile = isDragon;
    opts.requireTypes = isDragon;
    opts.filename = "<test>";
    Parser parser(std::move(tokens), opts);
    parser.parseModule();
    return parser.diagnostics();
}

inline std::vector<LexerDiagnostic> lexErrors(const std::string& source, bool isDragon = true) {
    LexerOptions opts;
    opts.useBraceBlocks = isDragon;
    opts.filename = "<test>";
    Lexer lexer(source, opts);
    lexer.tokenize();
    return lexer.diagnostics();
}

}
