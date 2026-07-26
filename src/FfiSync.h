#ifndef DRAGON_FFI_SYNC_H
#define DRAGON_FFI_SYNC_H

#include <string>

namespace dragon {

class Module;

// D052 `dragon ffi sync`: regenerate foreign-side stubs for the process externs
// declared in `filename`. checkOnly writes nothing, exits nonzero when stale
int runFfiSync(const std::string& filename, bool checkOnly);

// D052: every process extern whose stub file EXISTS must carry a signature line
// matching the declaration; returns the stale count (absent stubs are fine)
int verifyFfiStubSignatures(const Module& module);

}  // namespace dragon

#endif  // DRAGON_FFI_SYNC_H
