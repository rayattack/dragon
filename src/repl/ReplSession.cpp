#include "dragon/Repl.h"

#include <algorithm>
#include <fstream>
#include <sstream>

#include "ReplFrontend.h"
#include "ReplJit.h"
#include "ReplTurn.h"

#include "dragon/AstClone.h"
#include "dragon/CodeGen.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/raw_ostream.h"

extern "C" int64_t dragon_vthread_live_count();

namespace dragon {

namespace {

std::string turnFilenameFor(int turnIndex) {
    return "<repl:" + std::to_string(turnIndex) + ">";
}

std::string directoryOf(const std::string& path) {
    const auto slash = path.find_last_of('/');
    if (slash == std::string::npos) return "./";
    return path.substr(0, slash + 1);
}

}

struct ReplSession::Impl {
    ReplOptions options;
    std::unique_ptr<ReplJit> jit;

    std::vector<std::unique_ptr<Module>> committedTurns;
    std::vector<std::string> committedSource;
    std::vector<llvm::orc::ResourceTrackerSP> turnTrackers;
    std::vector<std::string> turnIRText;
    std::vector<std::string> residentModules;

    int nextTurn = 1;

    explicit Impl(ReplOptions opts) : options(std::move(opts)) {}

    size_t residentStatementCount() const {
        size_t total = 0;
        for (const auto& turn : committedTurns) total += turn->body.size();
        return total;
    }

    std::unique_ptr<Module> buildAnalysisModule(const Module& pending) const {
        auto analysis = std::make_unique<Module>();
        for (const auto& turn : committedTurns)
            for (const auto& stmt : turn->body)
                analysis->body.push_back(cloneStmt(stmt.get()));
        for (const auto& stmt : pending.body)
            analysis->body.push_back(cloneStmt(stmt.get()));
        return analysis;
    }
};

ReplSession::ReplSession(ReplOptions options)
    : impl_(std::make_unique<Impl>(std::move(options))) {}

ReplSession::~ReplSession() {
    std::string ignored;
    releaseSessionGlobals(ignored);
}

bool ReplSession::bringUp(std::string& errorOut) {
    ReplJitOptions jitOptions;
    jitOptions.optimizationLevel = impl_->options.optimizationLevel;
    jitOptions.pcre2LibPath = impl_->options.pcre2LibPath;
    jitOptions.sqlite3LibPath = impl_->options.sqlite3LibPath;

    auto jit = ReplJit::create(jitOptions);
    if (!jit) {
        errorOut = toString(jit.takeError());
        return false;
    }
    impl_->jit = std::move(*jit);
    return true;
}

bool ReplSession::runEditor(const std::string& editorPath,
                            int& exitCode,
                            std::string& errorOut) {
    if (!impl_->jit) {
        errorOut = "the session was not brought up";
        return false;
    }

    std::ifstream in(editorPath);
    if (!in) {
        errorOut = "cannot read " + editorPath;
        return false;
    }
    std::ostringstream buffer;
    buffer << in.rdbuf();

    TurnResult parseResult;
    auto editor = replParseCell(buffer.str(), editorPath, true, parseResult);
    if (!editor) {
        errorOut = parseResult.diagnostics.empty()
                       ? "the shell source did not parse"
                       : parseResult.diagnostics.front().message;
        return false;
    }

    std::vector<std::string> searchPaths = impl_->options.searchPaths;
    searchPaths.insert(searchPaths.begin(), directoryOf(editorPath));

    auto frontend = replAnalyze(*editor, editorPath, true, searchPaths, {});
    if (!frontend.ok) {
        errorOut = frontend.diagnostics.empty()
                       ? "the shell source did not type check"
                       : frontend.diagnostics.front().message;
        return false;
    }

    CodeGenOptions codegenOpts;
    codegenOpts.optimizationLevel = impl_->options.optimizationLevel;
    codegenOpts.jitTarget = true;
    codegenOpts.includePaths = searchPaths;

    CodeGen codegen(codegenOpts);
    if (!codegen.generate(*editor, frontend.depModules)) {
        errorOut = codegen.diagnostics().empty()
                       ? "the shell source did not compile"
                       : codegen.diagnostics().front().message;
        return false;
    }

    auto module = codegen.takeModule();
    auto context = codegen.takeContext();
    module->setDataLayout(impl_->jit->dataLayout());

    if (auto err = impl_->jit->addEditorModule(
            llvm::orc::ThreadSafeModule(std::move(module), std::move(context)))) {
        errorOut = toString(std::move(err));
        return false;
    }

    auto addr = impl_->jit->lookupInEditor("main");
    if (!addr) {
        errorOut = toString(addr.takeError());
        return false;
    }

    auto entry = addr->toPtr<int (*)(int, char**)>();
    exitCode = entry(0, nullptr);
    return true;
}

TurnResult ReplSession::evaluate(const std::string& cellSource) {
    TurnResult result;
    result.turnIndex = impl_->nextTurn;

    const std::string turnFilename = turnFilenameFor(impl_->nextTurn);
    const bool isDragonSurface = !impl_->options.pythonSurface;

    if (!impl_->jit) {
        result.status = TurnStatus::JitError;
        replAddError(result, turnFilename, "dragon repl: session was not brought up");
        return result;
    }

    auto pending = replParseCell(cellSource, turnFilename, isDragonSurface, result);
    if (!pending) {
        result.status = TurnStatus::FrontendError;
        return result;
    }

    auto analysis = impl_->buildAnalysisModule(*pending);
    const size_t residentCount = impl_->residentStatementCount();

    auto frontend = replAnalyze(*analysis, turnFilename, isDragonSurface,
                                impl_->options.searchPaths, impl_->residentModules);
    if (!frontend.ok) {
        result.status = TurnStatus::FrontendError;
        result.diagnostics = std::move(frontend.diagnostics);
        replFilterToTurn(result, turnFilename);
        return result;
    }

    CodeGenOptions codegenOpts;
    codegenOpts.optimizationLevel = impl_->options.optimizationLevel;
    codegenOpts.jitTarget = true;
    codegenOpts.replMode = true;
    codegenOpts.replTurnIndex = impl_->nextTurn;
    codegenOpts.replResidentStmtCount = residentCount;
    codegenOpts.replResidentModules = impl_->residentModules;
    codegenOpts.includePaths = impl_->options.searchPaths;

    CodeGen codegen(codegenOpts);
    if (!codegen.generate(*analysis, frontend.depModules)) {
        result.status = TurnStatus::CodeGenError;
        replAddCodeGenErrors(result, turnFilename, codegen.diagnostics());
        return result;
    }

    std::string irText;
    if (impl_->options.retainIR) {
        llvm::raw_string_ostream stream(irText);
        codegen.getLLVMModule()->print(stream, nullptr);
    }

    auto module = codegen.takeModule();
    auto context = codegen.takeContext();
    module->setDataLayout(impl_->jit->dataLayout());

    auto tracker = impl_->jit->newTurnTracker();
    if (auto err = impl_->jit->addTurnModule(
            tracker, llvm::orc::ThreadSafeModule(std::move(module), std::move(context)))) {
        result.status = TurnStatus::JitError;
        replAddError(result, turnFilename, toString(std::move(err)));
        return result;
    }

    const std::string entryName = "repl.turn." + std::to_string(impl_->nextTurn);
    auto addr = impl_->jit->lookupInSession(entryName);
    if (!addr) {
        result.status = TurnStatus::JitError;
        replAddError(result, turnFilename, toString(addr.takeError()));
        if (auto err = tracker->remove()) consumeError(std::move(err));
        return result;
    }

    std::string raiseText;
    if (!replCallTurnEntry(addr->toPtr<void*>(), raiseText)) {
        result.status = TurnStatus::Raised;
        result.exceptionText = raiseText;
        if (auto err = tracker->remove()) consumeError(std::move(err));
        return result;
    }

    impl_->committedTurns.push_back(std::move(pending));
    impl_->committedSource.push_back(cellSource);
    impl_->turnTrackers.push_back(tracker);
    if (impl_->options.retainIR) impl_->turnIRText.push_back(std::move(irText));
    for (auto& name : frontend.residentModuleNames)
        impl_->residentModules.push_back(name);
    ++impl_->nextTurn;

    result.status = TurnStatus::Ok;
    return result;
}

bool ReplSession::reset(std::string& refusalOut) {
    if (hasLiveVThreads()) {
        refusalOut = "cannot reset while fired vthreads are still running";
        return false;
    }
    if (!releaseSessionGlobals(refusalOut)) return false;
    if (impl_->jit) {
        if (auto err = impl_->jit->clearSession()) {
            refusalOut = toString(std::move(err));
            return false;
        }
    }
    impl_->committedTurns.clear();
    impl_->committedSource.clear();
    impl_->turnTrackers.clear();
    impl_->turnIRText.clear();
    impl_->residentModules.clear();
    impl_->nextTurn = 1;
    return true;
}

bool ReplSession::releaseSessionGlobals(std::string& errorOut) {
    if (!impl_->jit || impl_->committedTurns.empty()) return true;

    Module empty;
    auto analysis = impl_->buildAnalysisModule(empty);

    auto frontend = replAnalyze(*analysis, "<repl:teardown>",
                                !impl_->options.pythonSurface,
                                impl_->options.searchPaths, {});
    if (!frontend.ok) {
        errorOut = "could not analyze the session for teardown";
        return false;
    }

    CodeGenOptions codegenOpts;
    codegenOpts.jitTarget = true;
    codegenOpts.replMode = true;
    codegenOpts.replTeardown = true;
    codegenOpts.replTurnIndex = impl_->nextTurn;
    codegenOpts.replResidentStmtCount = analysis->body.size();
    codegenOpts.replResidentModules = impl_->residentModules;
    codegenOpts.includePaths = impl_->options.searchPaths;

    CodeGen codegen(codegenOpts);
    if (!codegen.generate(*analysis, frontend.depModules)) {
        errorOut = codegen.diagnostics().empty()
                       ? "could not generate the session teardown"
                       : codegen.diagnostics().front().message;
        return false;
    }

    auto module = codegen.takeModule();
    auto context = codegen.takeContext();
    module->setDataLayout(impl_->jit->dataLayout());

    auto tracker = impl_->jit->newTurnTracker();
    if (auto err = impl_->jit->addTurnModule(
            tracker, llvm::orc::ThreadSafeModule(std::move(module), std::move(context)))) {
        errorOut = toString(std::move(err));
        return false;
    }

    const std::string entryName = "repl.teardown." + std::to_string(impl_->nextTurn);
    auto addr = impl_->jit->lookupInSession(entryName);
    if (!addr) {
        errorOut = toString(addr.takeError());
        return false;
    }

    std::string raiseText;
    if (!replCallTurnEntry(addr->toPtr<void*>(), raiseText)) {
        errorOut = "session teardown raised: " + raiseText;
        return false;
    }
    return true;
}

bool ReplSession::hasLiveVThreads() const {
    return dragon_vthread_live_count() > 0;
}

int ReplSession::committedTurnCount() const {
    return static_cast<int>(impl_->committedTurns.size());
}

std::vector<std::pair<std::string, std::string>> ReplSession::boundNames() const {
    return {};
}

std::vector<std::string> ReplSession::definedFunctions() const {
    return {};
}

std::string ReplSession::turnIR(int turnIndex) const {
    const size_t index = static_cast<size_t>(turnIndex - 1);
    if (index >= impl_->turnIRText.size()) return {};
    return impl_->turnIRText[index];
}

std::string ReplSession::sessionSource() const {
    std::string out;
    for (const auto& source : impl_->committedSource) {
        out += source;
        if (!source.empty() && source.back() != '\n') out += '\n';
    }
    return out;
}

}
