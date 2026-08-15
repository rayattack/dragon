// runtime_sqltemplate.cpp - Decision 032 `template[SQL]` runtime helpers.
// Backs the runtime paths the compiler can't constant-fold (SQL.build
// fallback, dynamic composition); MUST hash identically to
// CodeGen::Impl::sqlCanonicalHash so both land in the same cache bucket.

#include "runtime_internal.h"

#include <cstdint>
#include <cstring>
#include <cstdlib>

extern "C" {

// 64-bit FNV-1a over a Dragon str's NUL-terminated bytes. SQL canonical text
// is always ASCII ($$N + literal SQL), so strlen-bounded hashing is exact.
int64_t dragon_str_fnv1a(const char* s) {
    uint64_t h = 0xcbf29ce484222325ULL;  // FNV offset basis
    if (s) {
        for (const unsigned char* p = (const unsigned char*)s; *p; ++p) {
            h ^= (uint64_t)*p;
            h *= 0x100000001b3ULL;       // FNV prime
        }
    }
    return (int64_t)h;
}

} // extern "C"
