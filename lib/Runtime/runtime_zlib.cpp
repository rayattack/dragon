/// Dragon Runtime - gzip/zlib wrappers. One-shot bytes-in/bytes-out around
/// system zlib (single deflate/inflate pass); stdlib/gzip.dr layers Pythonic
/// streaming on top by chunking. Own TU so `-lz` links only when a program
/// uses `dragon_zlib_*` (gated on `needsZ`), independent of zstd linking.

#include "runtime_internal.h"

#include <zlib.h>

#include <cstdlib>
#include <cstring>

// Hard ceiling on decompressed output: a few kB of crafted zlib can expand to
// GBs (decompression-bomb) and OOM the process. 256 MiB matches our largest
// legitimate payload (vendored stdlib tarballs).
static constexpr int64_t DRAGON_ZLIB_MAX_OUTPUT = 1LL << 28;

extern "C" {

/// Compress `src` to gzip-format bytes (RFC 1952). `level` is 0..9 (zlib's
/// scale: 0=store, 1=fastest, 9=best, -1=default which is 6).
DragonBytes* dragon_zlib_compress(DragonBytes* src, int64_t level) {
    if (!src || src->len == 0) return dragon_bytes_new(nullptr, 0);
    z_stream s;
    std::memset(&s, 0, sizeof(s));
    // windowBits = 15 (max LZ77 window) + 16 (gzip wrapper); MEM_LEVEL=8 default.
    if (deflateInit2(&s, (int)level, Z_DEFLATED, 15 + 16, 8,
                     Z_DEFAULT_STRATEGY) != Z_OK) {
        dragon_raise_exc_cstr(50 /* OSError */, "zlib: deflateInit2 failed");
        return nullptr;
    }
    s.next_in = (Bytef*)src->data;
    s.avail_in = (uInt)src->len;
    // deflateBound is exact for single-call Z_FINISH: allocate once, no realloc.
    uLong cap = deflateBound(&s, (uLong)src->len);
    uint8_t* buf = (uint8_t*)dragon_malloc_nullable(cap > 0 ? cap : 1);
    if (!buf) { deflateEnd(&s); dragon_raise_oom(); }
    s.next_out = buf;
    s.avail_out = (uInt)cap;
    int ret = deflate(&s, Z_FINISH);
    if (ret != Z_STREAM_END) {
        deflateEnd(&s);
        std::free(buf);
        dragon_raise_exc_cstr(50, "zlib: deflate did not finish");
        return nullptr;
    }
    size_t outLen = (size_t)(cap - s.avail_out);
    deflateEnd(&s);
    DragonBytes* out = dragon_bytes_new(buf, (int64_t)outLen);
    std::free(buf);
    return out;
}

/// Decompress gzip- or zlib-format bytes. windowBits = 15 + 32 auto-detects
/// either wrapper (matches Python's `zlib.decompress(..., wbits=47)`).
DragonBytes* dragon_zlib_decompress(DragonBytes* src) {
    if (!src || src->len == 0) return dragon_bytes_new(nullptr, 0);
    z_stream s;
    std::memset(&s, 0, sizeof(s));
    if (inflateInit2(&s, 15 + 32) != Z_OK) {
        dragon_raise_exc_cstr(50, "zlib: inflateInit2 failed");
        return nullptr;
    }
    s.next_in = (Bytef*)src->data;
    s.avail_in = (uInt)src->len;
    // Unknown output size: start at 4x input (typical text ratio), grow
    // geometrically for O(n) total work even on pathological inputs.
    size_t cap = (size_t)src->len * 4;
    if (cap < 4096) cap = 4096;
    if ((int64_t)cap > DRAGON_ZLIB_MAX_OUTPUT) cap = (size_t)DRAGON_ZLIB_MAX_OUTPUT;
    uint8_t* buf = (uint8_t*)dragon_malloc_nullable(cap);
    if (!buf) {
        inflateEnd(&s);
        dragon_raise_exc_cstr(43, "MemoryError: zlib: out of memory while decompressing");
        return nullptr;
    }
    size_t used = 0;
    while (true) {
        s.next_out = buf + used;
        s.avail_out = (uInt)(cap - used);
        int ret = inflate(&s, Z_NO_FLUSH);
        if (ret == Z_STREAM_END) {
            used = cap - s.avail_out;
            break;
        }
        if (ret == Z_OK) {
            used = cap - s.avail_out;
            if (s.avail_out == 0) {
                // Decompression-bomb guard: refuse to grow past the cap.
                if ((int64_t)cap >= DRAGON_ZLIB_MAX_OUTPUT) {
                    inflateEnd(&s);
                    std::free(buf);
                    dragon_raise_exc_cstr(50, "zlib: decompressed output exceeds maximum size");
                    return nullptr;
                }
                size_t newCap = cap * 2;
                if ((int64_t)newCap > DRAGON_ZLIB_MAX_OUTPUT)
                    newCap = (size_t)DRAGON_ZLIB_MAX_OUTPUT;
                uint8_t* nbuf = (uint8_t*)dragon_realloc_nullable(buf, newCap);
                if (!nbuf) {
                    inflateEnd(&s);
                    std::free(buf);
                    dragon_raise_exc_cstr(43, "MemoryError: zlib: out of memory while decompressing");
                    return nullptr;
                }
                buf = nbuf;
                cap = newCap;
            }
            continue;
        }
        // Z_BUF_ERROR is recoverable when we just need more output room;
        // any other error is fatal.
        if (ret == Z_BUF_ERROR && s.avail_out == 0) {
            if ((int64_t)cap >= DRAGON_ZLIB_MAX_OUTPUT) {
                inflateEnd(&s);
                std::free(buf);
                dragon_raise_exc_cstr(50, "zlib: decompressed output exceeds maximum size");
                return nullptr;
            }
            size_t newCap = cap * 2;
            if ((int64_t)newCap > DRAGON_ZLIB_MAX_OUTPUT)
                newCap = (size_t)DRAGON_ZLIB_MAX_OUTPUT;
            uint8_t* nbuf = (uint8_t*)dragon_realloc_nullable(buf, newCap);
            if (!nbuf) {
                inflateEnd(&s);
                std::free(buf);
                dragon_raise_exc_cstr(43, "MemoryError: zlib: out of memory while decompressing");
                return nullptr;
            }
            buf = nbuf;
            cap = newCap;
            continue;
        }
        inflateEnd(&s);
        std::free(buf);
        const char* msg = s.msg ? s.msg : "zlib: inflate failed";
        dragon_raise_exc_cstr(50, msg);
        return nullptr;
    }
    inflateEnd(&s);
    DragonBytes* out = dragon_bytes_new(buf, (int64_t)used);
    std::free(buf);
    return out;
}

/// Compress `src` to RAW DEFLATE bytes (RFC 1951, no zlib/gzip header/trailer) -
/// the exact stream a PKWARE ZIP method-8 entry stores. Used by stdlib/zipfile.dr.
DragonBytes* dragon_zlib_compress_raw(DragonBytes* src, int64_t level) {
    if (!src || src->len == 0) return dragon_bytes_new(nullptr, 0);
    z_stream s;
    std::memset(&s, 0, sizeof(s));
    // windowBits = -15 selects raw deflate (RFC 1951): no zlib header/adler
    // trailer; |15| is the max LZ77 window, MEM_LEVEL=8 is zlib's default.
    if (deflateInit2(&s, (int)level, Z_DEFLATED, -15, 8,
                     Z_DEFAULT_STRATEGY) != Z_OK) {
        dragon_raise_exc_cstr(50 /* OSError */, "zlib: deflateInit2 (raw) failed");
        return nullptr;
    }
    s.next_in = (Bytef*)src->data;
    s.avail_in = (uInt)src->len;
    uLong cap = deflateBound(&s, (uLong)src->len);
    uint8_t* buf = (uint8_t*)dragon_malloc_nullable(cap > 0 ? cap : 1);
    if (!buf) { deflateEnd(&s); dragon_raise_oom(); }
    s.next_out = buf;
    s.avail_out = (uInt)cap;
    int ret = deflate(&s, Z_FINISH);
    if (ret != Z_STREAM_END) {
        deflateEnd(&s);
        std::free(buf);
        dragon_raise_exc_cstr(50, "zlib: deflate (raw) did not finish");
        return nullptr;
    }
    size_t outLen = (size_t)(cap - s.avail_out);
    deflateEnd(&s);
    DragonBytes* out = dragon_bytes_new(buf, (int64_t)outLen);
    std::free(buf);
    return out;
}

/// Decompress RAW DEFLATE bytes (RFC 1951, no header/trailer): the inverse of
/// dragon_zlib_compress_raw, used by stdlib/zipfile.dr for ZIP method-8 entries.
DragonBytes* dragon_zlib_decompress_raw(DragonBytes* src) {
    if (!src || src->len == 0) return dragon_bytes_new(nullptr, 0);
    z_stream s;
    std::memset(&s, 0, sizeof(s));
    if (inflateInit2(&s, -15) != Z_OK) {
        dragon_raise_exc_cstr(50, "zlib: inflateInit2 (raw) failed");
        return nullptr;
    }
    s.next_in = (Bytef*)src->data;
    s.avail_in = (uInt)src->len;
    size_t cap = (size_t)src->len * 4;
    if (cap < 4096) cap = 4096;
    if ((int64_t)cap > DRAGON_ZLIB_MAX_OUTPUT) cap = (size_t)DRAGON_ZLIB_MAX_OUTPUT;
    uint8_t* buf = (uint8_t*)dragon_malloc_nullable(cap);
    if (!buf) {
        inflateEnd(&s);
        dragon_raise_exc_cstr(43, "MemoryError: zlib: out of memory while decompressing");
        return nullptr;
    }
    size_t used = 0;
    while (true) {
        s.next_out = buf + used;
        s.avail_out = (uInt)(cap - used);
        int ret = inflate(&s, Z_NO_FLUSH);
        if (ret == Z_STREAM_END) {
            used = cap - s.avail_out;
            break;
        }
        if (ret == Z_OK) {
            used = cap - s.avail_out;
            if (s.avail_out == 0) {
                if ((int64_t)cap >= DRAGON_ZLIB_MAX_OUTPUT) {
                    inflateEnd(&s);
                    std::free(buf);
                    dragon_raise_exc_cstr(50, "zlib: decompressed output exceeds maximum size");
                    return nullptr;
                }
                size_t newCap = cap * 2;
                if ((int64_t)newCap > DRAGON_ZLIB_MAX_OUTPUT)
                    newCap = (size_t)DRAGON_ZLIB_MAX_OUTPUT;
                uint8_t* nbuf = (uint8_t*)dragon_realloc_nullable(buf, newCap);
                if (!nbuf) {
                    inflateEnd(&s);
                    std::free(buf);
                    dragon_raise_exc_cstr(43, "MemoryError: zlib: out of memory while decompressing");
                    return nullptr;
                }
                buf = nbuf;
                cap = newCap;
            }
            continue;
        }
        if (ret == Z_BUF_ERROR && s.avail_out == 0) {
            if ((int64_t)cap >= DRAGON_ZLIB_MAX_OUTPUT) {
                inflateEnd(&s);
                std::free(buf);
                dragon_raise_exc_cstr(50, "zlib: decompressed output exceeds maximum size");
                return nullptr;
            }
            size_t newCap = cap * 2;
            if ((int64_t)newCap > DRAGON_ZLIB_MAX_OUTPUT)
                newCap = (size_t)DRAGON_ZLIB_MAX_OUTPUT;
            uint8_t* nbuf = (uint8_t*)dragon_realloc_nullable(buf, newCap);
            if (!nbuf) {
                inflateEnd(&s);
                std::free(buf);
                dragon_raise_exc_cstr(43, "MemoryError: zlib: out of memory while decompressing");
                return nullptr;
            }
            buf = nbuf;
            cap = newCap;
            continue;
        }
        inflateEnd(&s);
        std::free(buf);
        const char* msg = s.msg ? s.msg : "zlib: inflate (raw) failed";
        dragon_raise_exc_cstr(50, msg);
        return nullptr;
    }
    inflateEnd(&s);
    DragonBytes* out = dragon_bytes_new(buf, (int64_t)used);
    std::free(buf);
    return out;
}

} // extern "C"
