#include <cstdio>
#include <string>

extern "C" long shim_strlen(const char* s) {
    std::string str(s ? s : "");
    return static_cast<long>(str.size());
}

extern "C" void dragon_raise_exc_cstr(long type, const char* msg);

extern "C" void shim_raise_value_error(const char* msg) {
    char text[128];
    snprintf(text, sizeof(text), "ValueError: %s", msg ? msg : "");
    dragon_raise_exc_cstr(90, text);
}
