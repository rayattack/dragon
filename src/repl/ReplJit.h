#ifndef DRAGON_REPL_JIT_H
#define DRAGON_REPL_JIT_H

#include <memory>
#include <string>

#include "llvm/ExecutionEngine/Orc/Core.h"
#include "llvm/ExecutionEngine/Orc/LLJIT.h"
#include "llvm/ExecutionEngine/Orc/ThreadSafeModule.h"
#include "llvm/Support/Error.h"

namespace dragon {

struct ReplJitOptions {
    int optimizationLevel = 0;
    std::string pcre2LibPath;
    std::string sqlite3LibPath;
};

class ReplJit {
public:
    static llvm::Expected<std::unique_ptr<ReplJit>> create(const ReplJitOptions& options);
    ~ReplJit();

    llvm::orc::JITDylib& editorDylib() { return *editor_; }
    llvm::orc::JITDylib& sessionDylib() { return *session_; }

    llvm::orc::ResourceTrackerSP newTurnTracker();

    llvm::Error addEditorModule(llvm::orc::ThreadSafeModule module);
    llvm::Error addTurnModule(llvm::orc::ResourceTrackerSP tracker,
                              llvm::orc::ThreadSafeModule module);

    llvm::Expected<llvm::orc::ExecutorAddr> lookupInEditor(llvm::StringRef name);
    llvm::Expected<llvm::orc::ExecutorAddr> lookupInSession(llvm::StringRef name);

    const llvm::DataLayout& dataLayout() const { return jit_->getDataLayout(); }

    llvm::Error clearSession();

private:
    ReplJit(std::unique_ptr<llvm::orc::LLJIT> jit,
            llvm::orc::JITDylib& runtime,
            llvm::orc::JITDylib& editor,
            llvm::orc::JITDylib& session);

    std::unique_ptr<llvm::orc::LLJIT> jit_;
    llvm::orc::JITDylib* runtime_;
    llvm::orc::JITDylib* editor_;
    llvm::orc::JITDylib* session_;
};

}

#endif
