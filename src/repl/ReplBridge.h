#ifndef DRAGON_REPL_BRIDGE_H
#define DRAGON_REPL_BRIDGE_H

#include <cstdint>

namespace dragon {

class DiagnosticFormatter;
class ReplSession;

enum class ReplBridgeStatus : int32_t {
    Ok = 0,
    Failed = 1,
    Quit = 2,
    UnknownCommand = 3,
    Refused = 4
};

void replBridgeBind(ReplSession* session, DiagnosticFormatter* formatter);

}

extern "C" {

int32_t dragon_repl_eval(const char* cell);
int32_t dragon_repl_command(const char* name, const char* arg);
const char* dragon_repl_version(void);
int32_t dragon_repl_turn_count(void);

}

#endif
