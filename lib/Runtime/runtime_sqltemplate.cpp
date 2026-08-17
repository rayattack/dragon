#include "runtime_internal.h"

#include <cstdint>
#include <cstring>
#include <cstdlib>

extern "C" {

int64_t dragon_str_fnv1a(const char* s) {
    uint64_t h = 0xcbf29ce484222325ULL;
    if (s) {
        for (const unsigned char* p = (const unsigned char*)s; *p; ++p) {
            h ^= (uint64_t)*p;
            h *= 0x100000001b3ULL;
        }
    }
    return (int64_t)h;
}

}
