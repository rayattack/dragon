#include "ReplBridge.h"

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>

#include "dragon.h"
#include "dragon/DiagnosticFormatter.h"
#include "dragon/Repl.h"

extern "C" {
const char* dragon_string_alloc(const char* src, int64_t len);
char* dragon_str_to_utf8_alloc(const char* s, int64_t* out_byte_len);
}

namespace dragon {

namespace {

ReplSession* g_session = nullptr;
DiagnosticFormatter* g_formatter = nullptr;

std::string fromDragonString(const char* s) {
    if (!s) return {};
    int64_t len = 0;
    char* encoded = dragon_str_to_utf8_alloc(s, &len);
    std::string out(encoded ? encoded : s, static_cast<size_t>(len));
    if (encoded) std::free(encoded);
    return out;
}

const char* toDragonString(const std::string& s) {
    return dragon_string_alloc(s.data(), static_cast<int64_t>(s.size()));
}

void reportTurn(const TurnResult& result) {
    for (const auto& diag : result.diagnostics) {
        std::cerr << g_formatter->format(
            diag.location.filename.empty() ? "<repl>" : diag.location.filename,
            diag.location.line, diag.location.column, "error", diag.message);
    }
    if (!result.exceptionText.empty())
        std::cerr << g_formatter->format("<repl>", 0, 0, "error",
                                         result.exceptionText);
    std::cerr.flush();
}

}

void replBridgeBind(ReplSession* session, DiagnosticFormatter* formatter) {
    g_session = session;
    g_formatter = formatter;
}

}

extern "C" {

int32_t dragon_repl_eval(const char* cell) {
    using namespace dragon;
    if (!g_session) return static_cast<int32_t>(ReplBridgeStatus::Failed);

    const TurnResult result = g_session->evaluate(fromDragonString(cell));
    std::cout.flush();
    if (result.ok()) return static_cast<int32_t>(ReplBridgeStatus::Ok);

    reportTurn(result);
    return static_cast<int32_t>(ReplBridgeStatus::Failed);
}

int32_t dragon_repl_command(const char* name, const char* arg) {
    using namespace dragon;
    if (!g_session) return static_cast<int32_t>(ReplBridgeStatus::Failed);

    const std::string command = fromDragonString(name);
    (void)fromDragonString(arg);

    if (command == "reset") {
        std::string refusal;
        if (g_session->reset(refusal)) {
            return static_cast<int32_t>(ReplBridgeStatus::Ok);
        }
        std::cerr << "dragon repl: " << refusal << "\n";
        std::cerr.flush();
        return static_cast<int32_t>(ReplBridgeStatus::Refused);
    }

    return static_cast<int32_t>(ReplBridgeStatus::UnknownCommand);
}

const char* dragon_repl_version(void) {
    return dragon::toDragonString(dragon::VERSION);
}

int32_t dragon_repl_turn_count(void) {
    using namespace dragon;
    if (!g_session) return 0;
    return g_session->committedTurnCount();
}

}
