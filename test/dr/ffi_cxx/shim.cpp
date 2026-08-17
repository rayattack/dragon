#include <string>

extern "C" long shim_strlen(const char* s) {
    std::string str(s ? s : "");
    return static_cast<long>(str.size());
}
