#include "dragon.h"

#include "llvm/Support/TargetSelect.h"

namespace dragon {

void initialize() {
    llvm::InitializeNativeTarget();
    llvm::InitializeNativeTargetAsmPrinter();
    llvm::InitializeNativeTargetAsmParser();
}

void shutdown() {
    // LLVM handles cleanup automatically via static destructors
}

} // namespace dragon
