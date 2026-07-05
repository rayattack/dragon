/// Dragon Runtime - Zstandard wrappers
///
/// One-shot bytes-in / bytes-out wrappers around the system libzstd. The
/// streaming decompress path (used when a frame omits the content-size
/// hint) keeps this TU dependent on libzstd only - symbol references stay
/// inside this object, so the linker pulls libzstd just when `needsZstd`.

#include "runtime_internal.h"

#include <zstd.h>

#include <cstdlib>

// Hard ceiling on decompressed output (matches DRAGON_ZLIB_MAX_OUTPUT). A
// crafted zstd frame can lie about its content-size or stream forever; we cap
// both paths to keep the decompression-bomb attack from OOMing the process.
static constexpr int64_t DRAGON_ZSTD_MAX_OUTPUT = 1LL << 28;

extern "C" {

/// Compress `src` with Zstandard at `level` (1..22; 3 is the canonical
/// default that python-zstandard and the `zstd` CLI both use).
DragonBytes* dragon_zstd_compress(DragonBytes* src, int64_t level) {
    if (!src || src->len == 0) return dragon_bytes_new(nullptr, 0);
    size_t cap = ZSTD_compressBound((size_t)src->len);
    uint8_t* buf = (uint8_t*)std::malloc(cap);
    size_t outLen = ZSTD_compress(buf, cap, src->data,
                                   (size_t)src->len, (int)level);
    if (ZSTD_isError(outLen)) {
        const char* msg = ZSTD_getErrorName(outLen);
        std::free(buf);
        dragon_raise_exc_cstr(50, msg ? msg : "zstd: compress failed");
        return nullptr;
    }
    DragonBytes* out = dragon_bytes_new(buf, (int64_t)outLen);
    std::free(buf);
    return out;
}

/// Decompress a Zstandard frame. Uses the embedded content-size field for
/// a single-shot allocation when present, falls back to streaming when the
/// frame was produced without size hints (e.g. piped through `zstd -`).
DragonBytes* dragon_zstd_decompress(DragonBytes* src) {
    if (!src || src->len == 0) return dragon_bytes_new(nullptr, 0);
    unsigned long long expected =
        ZSTD_getFrameContentSize(src->data, (size_t)src->len);
    if (expected != ZSTD_CONTENTSIZE_ERROR &&
        expected != ZSTD_CONTENTSIZE_UNKNOWN) {
        // Content-size is attacker-controlled - refuse before allocating to
        // block the "tiny frame claims a TB of output" allocator-DoS path.
        if ((int64_t)expected > DRAGON_ZSTD_MAX_OUTPUT) {
            dragon_raise_exc_cstr(50, "zstd: decompressed output exceeds maximum size");
            return nullptr;
        }
        uint8_t* buf = (uint8_t*)std::malloc(expected > 0 ? expected : 1);
        if (!buf) {
            dragon_raise_exc_cstr(50, "zstd: out of memory");
            return nullptr;
        }
        size_t actual = ZSTD_decompress(buf, (size_t)expected,
                                         src->data, (size_t)src->len);
        if (ZSTD_isError(actual)) {
            const char* msg = ZSTD_getErrorName(actual);
            std::free(buf);
            dragon_raise_exc_cstr(50, msg ? msg : "zstd: decompress failed");
            return nullptr;
        }
        DragonBytes* out = dragon_bytes_new(buf, (int64_t)actual);
        std::free(buf);
        return out;
    }
    // Streaming path - frame produced without content-size hint. Pull
    // chunks through ZSTD_decompressStream and grow the output buffer
    // geometrically. ZSTD_DStreamInSize / OutSize are the algorithm's
    // recommended block sizes (currently 128KB out / 128KB in).
    if (expected == ZSTD_CONTENTSIZE_ERROR) {
        dragon_raise_exc_cstr(50, "zstd: not a valid zstd frame");
        return nullptr;
    }
    ZSTD_DStream* ds = ZSTD_createDStream();
    if (!ds) {
        dragon_raise_exc_cstr(50, "zstd: createDStream failed");
        return nullptr;
    }
    size_t init = ZSTD_initDStream(ds);
    if (ZSTD_isError(init)) {
        const char* msg = ZSTD_getErrorName(init);
        ZSTD_freeDStream(ds);
        dragon_raise_exc_cstr(50, msg ? msg : "zstd: initDStream failed");
        return nullptr;
    }
    size_t outBlock = ZSTD_DStreamOutSize();
    size_t cap = outBlock;
    uint8_t* buf = (uint8_t*)std::malloc(cap);
    if (!buf) {
        ZSTD_freeDStream(ds);
        dragon_raise_exc_cstr(50, "zstd: out of memory");
        return nullptr;
    }
    size_t used = 0;
    // r holds the last ZSTD_decompressStream return. It is 0 only when a
    // frame has been fully decoded; any other value means "more input
    // expected". Start at a nonzero sentinel so an input that never reaches
    // end-of-frame is treated as incomplete. See the post-loop check.
    size_t r = 1;
    ZSTD_inBuffer in = { src->data, (size_t)src->len, 0 };
    while (in.pos < in.size) {
        if (cap - used < outBlock) {
            // Streaming decompression-bomb guard.
            if ((int64_t)cap >= DRAGON_ZSTD_MAX_OUTPUT) {
                ZSTD_freeDStream(ds);
                std::free(buf);
                dragon_raise_exc_cstr(50, "zstd: decompressed output exceeds maximum size");
                return nullptr;
            }
            size_t newCap = cap * 2;
            if ((int64_t)newCap > DRAGON_ZSTD_MAX_OUTPUT)
                newCap = (size_t)DRAGON_ZSTD_MAX_OUTPUT;
            uint8_t* nbuf = (uint8_t*)std::realloc(buf, newCap);
            if (!nbuf) {
                ZSTD_freeDStream(ds);
                std::free(buf);
                dragon_raise_exc_cstr(50, "zstd: out of memory");
                return nullptr;
            }
            buf = nbuf;
            cap = newCap;
        }
        ZSTD_outBuffer out = { buf + used, cap - used, 0 };
        r = ZSTD_decompressStream(ds, &out, &in);
        if (ZSTD_isError(r)) {
            const char* msg = ZSTD_getErrorName(r);
            ZSTD_freeDStream(ds);
            std::free(buf);
            dragon_raise_exc_cstr(50, msg ? msg : "zstd: decompress failed");
            return nullptr;
        }
        used += out.pos;
        if (r == 0) break;          // frame complete
    }
    ZSTD_freeDStream(ds);
    // Truncated frame: the loop drained all input (in.pos == in.size) but
    // ZSTD never signalled end-of-frame (the last r was nonzero, meaning it
    // still expected more bytes). Unlike zlib, zstd's streaming API does not
    // raise on premature end - it just stops producing output - so without
    // this check we would hand the caller a SHORT (often empty) payload and
    // report success. That is silent data loss on a corrupt/clipped archive
    // or a half-received network message. Raise instead.
    if (r != 0) {
        std::free(buf);
        dragon_raise_exc_cstr(50, "zstd: truncated or incomplete frame");
        return nullptr;
    }
    DragonBytes* result = dragon_bytes_new(buf, (int64_t)used);
    std::free(buf);
    return result;
}

} // extern "C"
