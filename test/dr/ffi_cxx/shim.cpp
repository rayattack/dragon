// ADR 041 test shim - deliberately uses the C++ standard library (std::string)
// so the linked program needs libstdc++ and C++ static-init/exception support.
// This exercises dragon build's cc->c++ link-driver switch that engages when a
// --cc-source argument names a C++ translation unit (.cpp).
#include <string>

// Length of a NUL-terminated C string, computed via std::string so the symbol
// genuinely depends on the C++ runtime.
extern "C" long shim_strlen(const char* s) {
    std::string str(s ? s : "");
    return static_cast<long>(str.size());
}
