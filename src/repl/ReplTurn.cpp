#include "ReplTurn.h"

#include <csetjmp>

#include "dragon/AstClone.h"
#include "dragon/CodeGen.h"
#include "dragon/Lexer.h"
#include "dragon/Parser.h"
#include "llvm/IR/Module.h"

extern "C" {
void* dragon_exc_push_frame();
void dragon_exc_pop_frame();
void dragon_exc_cleanup_unwind();
int64_t dragon_exc_get_type();
const char* dragon_exc_get_msg();
const char* dragon_exc_name_for_code(int code);
}

namespace dragon {

namespace {

void addError(TurnResult& result, const std::string& filename, const std::string& message) {
    result.diagnostics.push_back({ReplDiagnostic::Level::Error, {filename, 0, 0, 0}, message});
}

template <typename Diagnostics>
void addStageErrors(TurnResult& result,
                    const std::string& filename,
                    const Diagnostics& diagnostics) {
    for (const auto& diag : diagnostics) {
        if (diag.level != std::decay_t<decltype(diag)>::Level::Error) continue;
        SourceLocation loc = diag.location;
        if (loc.filename.empty()) loc.filename = filename;
        result.diagnostics.push_back({ReplDiagnostic::Level::Error, loc, diag.message});
    }
}

std::string formatRaise() {
    const int64_t code = dragon_exc_get_type();
    const char* name = dragon_exc_name_for_code(static_cast<int>(code));
    const char* msg = dragon_exc_get_msg();

    std::string out = name ? name : "Exception";
    if (msg && *msg) {
        const std::string text(msg);
        if (text.rfind(out + ":", 0) == 0) return text;
        out += ": " + text;
    }
    return out;
}

}

std::unique_ptr<Module> replParseCell(const std::string& source,
                                      const std::string& turnFilename,
                                      bool isDragonSurface,
                                      TurnResult& result) {
    LexerOptions lexOpts;
    lexOpts.useBraceBlocks = isDragonSurface;
    lexOpts.filename = turnFilename;

    Lexer lexer(source, lexOpts);
    auto tokens = lexer.tokenize();
    if (lexer.hasErrors()) {
        addStageErrors(result, turnFilename, lexer.diagnostics());
        return nullptr;
    }

    ParserOptions parseOpts;
    parseOpts.isDragonFile = isDragonSurface;
    parseOpts.requireTypes = isDragonSurface;
    parseOpts.filename = turnFilename;

    Parser parser(std::move(tokens), parseOpts);
    auto module = parser.parseModule();
    if (parser.hasErrors()) {
        addStageErrors(result, turnFilename, parser.diagnostics());
        return nullptr;
    }
    return module;
}

void replFilterToTurn(TurnResult& result, const std::string& turnFilename) {
    std::vector<ReplDiagnostic> mine;
    for (auto& diag : result.diagnostics) {
        if (diag.location.filename == turnFilename || diag.location.filename.empty())
            mine.push_back(diag);
    }
    if (!mine.empty()) {
        result.diagnostics = std::move(mine);
        return;
    }
}

bool replCallTurnEntry(void* entry, std::string& raiseText) {
    auto* frame = static_cast<jmp_buf*>(dragon_exc_push_frame());
    if (setjmp(*frame) == 0) {
        reinterpret_cast<void (*)()>(entry)();
        dragon_exc_pop_frame();
        return true;
    }
    dragon_exc_cleanup_unwind();
    dragon_exc_pop_frame();
    raiseText = formatRaise();
    return false;
}

void replAddError(TurnResult& result, const std::string& filename, const std::string& message) {
    addError(result, filename, message);
}

void replAddCodeGenErrors(TurnResult& result,
                          const std::string& filename,
                          const std::vector<CodeGenDiagnostic>& diagnostics) {
    addStageErrors(result, filename, diagnostics);
}

}
