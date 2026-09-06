#if defined(__x86_64__)
#define DRAGON_CRC32C_HARDWARE 1
#include <immintrin.h>
#elif defined(__aarch64__) && defined(__ARM_FEATURE_CRC32)
#define DRAGON_CRC32C_HARDWARE 1
#include <arm_acle.h>
#endif

#include "runtime_internal.h"

#include <zlib.h>

#include <cstdlib>
#include <cstring>

static constexpr uint32_t DRAGON_CRC32C_POLY = 0x82F63B78u;
static constexpr int64_t DRAGON_CRC32_CHUNK = 1 << 30;

namespace {

struct Crc32cTable {
    uint32_t entry[256];
};

constexpr Crc32cTable crc32c_build_table() {
    Crc32cTable t = {};
    for (uint32_t n = 0; n < 256; n++) {
        uint32_t c = n;
        for (int k = 0; k < 8; k++) {
            c = (c & 1) ? ((c >> 1) ^ DRAGON_CRC32C_POLY) : (c >> 1);
        }
        t.entry[n] = c;
    }
    return t;
}

constexpr Crc32cTable kCrc32cTable = crc32c_build_table();

uint32_t crc32c_table_loop(uint32_t crc, const uint8_t* p, int64_t n) {
    for (int64_t i = 0; i < n; i++) {
        crc = kCrc32cTable.entry[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
    }
    return crc;
}

#ifdef DRAGON_CRC32C_HARDWARE

#if defined(__x86_64__)
__attribute__((target("sse4.2")))
uint32_t crc32c_intrinsic(uint32_t crc, const uint8_t* p, int64_t n) {
    uint64_t wide = crc;
    while (n >= 8) {
        uint64_t word;
        memcpy(&word, p, 8);
        wide = _mm_crc32_u64(wide, word);
        p += 8;
        n -= 8;
    }
    uint32_t narrow = (uint32_t)wide;
    while (n > 0) {
        narrow = _mm_crc32_u8(narrow, *p);
        p++;
        n--;
    }
    return narrow;
}
#else
uint32_t crc32c_intrinsic(uint32_t crc, const uint8_t* p, int64_t n) {
    while (n >= 8) {
        uint64_t word;
        memcpy(&word, p, 8);
        crc = __crc32cd(crc, word);
        p += 8;
        n -= 8;
    }
    while (n > 0) {
        crc = __crc32cb(crc, *p);
        p++;
        n--;
    }
    return crc;
}
#endif

bool crc32c_probe_hardware() {
    const char* forced = getenv("DRAGON_CRC32C_TABLE");
    if (forced && forced[0] == '1') return false;
#if defined(__x86_64__)
    return __builtin_cpu_supports("sse4.2") != 0;
#else
    return true;
#endif
}

int crc32c_hardware_cache = -1;

bool crc32c_hardware() {
    int cached = __atomic_load_n(&crc32c_hardware_cache, __ATOMIC_RELAXED);
    if (cached < 0) {
        cached = crc32c_probe_hardware() ? 1 : 0;
        __atomic_store_n(&crc32c_hardware_cache, cached, __ATOMIC_RELAXED);
    }
    return cached == 1;
}

#endif

uint32_t crc32c_span(uint32_t crc, const uint8_t* p, int64_t n) {
#ifdef DRAGON_CRC32C_HARDWARE
    if (crc32c_hardware()) return crc32c_intrinsic(crc, p, n);
#endif
    return crc32c_table_loop(crc, p, n);
}

void check_span(DragonBytes* b, int64_t start, int64_t length) {
    int64_t len = b ? b->len : 0;
    if (start < 0 || length < 0 || start > len - length) {
        dragon_raise_exc_cstr(41, "IndexError: bytes range out of range");
    }
}

}

extern "C" {

int64_t dragon_crc32c_range(DragonBytes* b, int64_t start, int64_t length, int64_t value) {
    check_span(b, start, length);
    if (length == 0) return value & 0xFFFFFFFF;
    uint32_t crc = crc32c_span(~(uint32_t)value, b->data + start, length);
    return (int64_t)(~crc & 0xFFFFFFFFu);
}

int64_t dragon_crc32_range(DragonBytes* b, int64_t start, int64_t length, int64_t value) {
    check_span(b, start, length);
    uLong crc = (uLong)((uint32_t)value);
    const uint8_t* p = b ? b->data + start : nullptr;
    while (length > 0) {
        int64_t chunk = length < DRAGON_CRC32_CHUNK ? length : DRAGON_CRC32_CHUNK;
        crc = crc32(crc, (const Bytef*)p, (uInt)chunk);
        p += chunk;
        length -= chunk;
    }
    return (int64_t)((uint32_t)crc);
}

}
