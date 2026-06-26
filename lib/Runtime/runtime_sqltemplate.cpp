// runtime_sqltemplate.cpp - Decision 032 `template[SQL]` runtime helpers.
//
// `template[SQL] { ... }` is constant-folded by the compiler: the canonical
// `$$N` text is an interned `.rodata` literal and its FNV-1a hash is a
// compile-time constant. These helpers exist for the runtime paths that can't
// be folded - the Dragon-level `SQL.build` fallback and dynamic composition -
// and MUST hash byte-for-byte identically to the compiler (see
// CodeGen::Impl::sqlCanonicalHash) so a folded and a runtime-built canonical
// with the same text land in the same prepared-statement cache bucket.

#include <cstdint>
#include <cstring>
#include <cstdlib>

#include "runtime_internal.h"

extern "C" {

// 64-bit FNV-1a over the NUL-terminated bytes of a Dragon `str`. A Dragon str
// arrives at the FFI boundary as a `const char*` pointing at its data bytes
// (kind=1/ASCII is NUL-terminated), matching dragon_template_escape_sql et al.
// SQL canonical text is always ASCII ($$N + literal SQL), so strlen-bounded
// hashing is exact.
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
