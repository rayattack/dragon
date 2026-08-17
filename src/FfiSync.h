#ifndef DRAGON_FFI_SYNC_H
#define DRAGON_FFI_SYNC_H

#include <string>

namespace dragon {

class Module;

int runFfiSync(const std::string& filename, bool checkOnly);

int verifyFfiStubSignatures(const Module& module);

}

#endif
