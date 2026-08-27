#ifndef DRAGON_REPL_H
#define DRAGON_REPL_H

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "dragon/Token.h"

namespace dragon {

struct ReplOptions {
    bool pythonSurface = false;
    int optimizationLevel = 0;
    std::vector<std::string> searchPaths;
    std::string pcre2LibPath;
    std::string sqlite3LibPath;
    bool retainIR = false;
};

struct ReplDiagnostic {
    enum class Level { Warning, Error };

    Level level = Level::Error;
    SourceLocation location;
    std::string message;
};

enum class TurnStatus {
    Ok,
    FrontendError,
    CodeGenError,
    JitError,
    Raised
};

struct TurnResult {
    TurnStatus status = TurnStatus::Ok;
    std::vector<ReplDiagnostic> diagnostics;
    std::string exceptionText;
    int turnIndex = 0;

    bool ok() const { return status == TurnStatus::Ok; }
};

class ReplSession {
public:
    explicit ReplSession(ReplOptions options = {});
    ~ReplSession();

    ReplSession(const ReplSession&) = delete;
    ReplSession& operator=(const ReplSession&) = delete;

    bool bringUp(std::string& errorOut);

    bool runEditor(const std::string& editorPath, int& exitCode, std::string& errorOut);

    TurnResult evaluate(const std::string& cellSource);

    bool reset(std::string& refusalOut);
    bool hasLiveVThreads() const;

    int committedTurnCount() const;
    std::vector<std::pair<std::string, std::string>> boundNames() const;
    std::vector<std::string> definedFunctions() const;
    std::string turnIR(int turnIndex) const;
    std::string sessionSource() const;

    struct Impl;

private:
    bool releaseSessionGlobals(std::string& errorOut);

    std::unique_ptr<Impl> impl_;
};

}

#endif
