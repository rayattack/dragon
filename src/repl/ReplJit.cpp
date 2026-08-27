#include "ReplJit.h"

#include "llvm/ExecutionEngine/Orc/ExecutionUtils.h"
#include "llvm/ExecutionEngine/Orc/JITTargetMachineBuilder.h"
#include "llvm/Support/CodeGen.h"

namespace dragon {

using namespace llvm;
using namespace llvm::orc;

namespace {

Error attachProcessSymbols(JITDylib& jd, const DataLayout& dl) {
    auto gen = DynamicLibrarySearchGenerator::GetForCurrentProcess(dl.getGlobalPrefix());
    if (!gen) return gen.takeError();
    jd.addGenerator(std::move(*gen));
    return Error::success();
}

Error attachArchive(JITDylib& jd, ObjectLayer& layer, const std::string& path) {
    if (path.empty()) return Error::success();
    auto gen = StaticLibraryDefinitionGenerator::Load(layer, path.c_str());
    if (!gen) return gen.takeError();
    jd.addGenerator(std::move(*gen));
    return Error::success();
}

}

ReplJit::ReplJit(std::unique_ptr<LLJIT> jit,
                 JITDylib& runtime,
                 JITDylib& editor,
                 JITDylib& session)
    : jit_(std::move(jit)), runtime_(&runtime), editor_(&editor), session_(&session) {}

ReplJit::~ReplJit() = default;

Expected<std::unique_ptr<ReplJit>> ReplJit::create(const ReplJitOptions& options) {
    auto jtmb = JITTargetMachineBuilder::detectHost();
    if (!jtmb) return jtmb.takeError();

    jtmb->setRelocationModel(Reloc::PIC_);
    jtmb->setCodeGenOptLevel(options.optimizationLevel > 0 ? CodeGenOptLevel::Default
                                                           : CodeGenOptLevel::None);
    jtmb->getOptions().EmulatedTLS = false;

    auto jit = LLJITBuilder()
                   .setJITTargetMachineBuilder(std::move(*jtmb))
                   .setNumCompileThreads(0)
                   .create();
    if (!jit) return jit.takeError();

    auto& runtime = (*jit)->getMainJITDylib();
    if (auto err = attachProcessSymbols(runtime, (*jit)->getDataLayout()))
        return std::move(err);
    if (auto err = attachArchive(runtime, (*jit)->getObjLinkingLayer(), options.pcre2LibPath))
        return std::move(err);
    if (auto err = attachArchive(runtime, (*jit)->getObjLinkingLayer(), options.sqlite3LibPath))
        return std::move(err);

    auto editor = (*jit)->createJITDylib("dragon.editor");
    if (!editor) return editor.takeError();
    editor->addToLinkOrder(runtime);

    auto session = (*jit)->createJITDylib("dragon.session");
    if (!session) return session.takeError();
    session->addToLinkOrder(runtime);

    return std::unique_ptr<ReplJit>(
        new ReplJit(std::move(*jit), runtime, *editor, *session));
}

ResourceTrackerSP ReplJit::newTurnTracker() {
    return session_->createResourceTracker();
}

Error ReplJit::addEditorModule(ThreadSafeModule module) {
    return jit_->addIRModule(*editor_, std::move(module));
}

Error ReplJit::addTurnModule(ResourceTrackerSP tracker, ThreadSafeModule module) {
    return jit_->addIRModule(std::move(tracker), std::move(module));
}

Expected<ExecutorAddr> ReplJit::lookupInEditor(StringRef name) {
    return jit_->lookup(*editor_, name);
}

Expected<ExecutorAddr> ReplJit::lookupInSession(StringRef name) {
    return jit_->lookup(*session_, name);
}

Error ReplJit::clearSession() {
    return session_->clear();
}

}
