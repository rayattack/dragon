/// Dragon Runtime - Bytes-aware whole-file IO
///
/// Split out of the compression TU so file IO can be linked without dragging
/// libz / libzstd. Used by stdlib/gzip.dr, stdlib/zstandard.dr, stdlib/tarfile.dr,
/// and any future bytes-mode reader. The legacy `dragon_file_read_bytes`
/// returns a DragonString masquerading as bytes; these helpers operate
/// exclusively on DragonBytes - no codec ever touches the buffer.

#include "runtime_internal.h"

#include <cstdio>
#include <cstdlib>

extern "C" {

/// Read the entire file at `path` into a fresh DragonBytes. Raises OSError
/// on open/read failure (matching CPython's `Path.read_bytes()` contract).
DragonBytes* dragon_read_file_bytes(const char* path) {
    if (!path) {
        dragon_raise_exc_cstr(50 /* OSError */, "read_file_bytes: null path");
        return nullptr;
    }
    FILE* f = std::fopen(path, "rb");
    if (!f) {
        dragon_raise_exc_cstr(50, "read_file_bytes: cannot open file");
        return nullptr;
    }
    if (std::fseek(f, 0, SEEK_END) != 0) {
        std::fclose(f);
        dragon_raise_exc_cstr(50, "read_file_bytes: fseek failed");
        return nullptr;
    }
    long size = std::ftell(f);
    if (size < 0) {
        std::fclose(f);
        dragon_raise_exc_cstr(50, "read_file_bytes: ftell failed");
        return nullptr;
    }
    std::rewind(f);
    uint8_t* buf = (uint8_t*)std::malloc(size > 0 ? (size_t)size : 1);
    size_t n = std::fread(buf, 1, (size_t)size, f);
    std::fclose(f);
    if ((long)n != size) {
        std::free(buf);
        dragon_raise_exc_cstr(50, "read_file_bytes: short read");
        return nullptr;
    }
    DragonBytes* out = dragon_bytes_new(buf, (int64_t)size);
    std::free(buf);
    return out;
}

/// Write `data` to `path`, replacing any existing file. Returns the byte
/// count written.
int64_t dragon_write_file_bytes(const char* path, DragonBytes* data) {
    if (!path) {
        dragon_raise_exc_cstr(50, "write_file_bytes: null path");
        return 0;
    }
    FILE* f = std::fopen(path, "wb");
    if (!f) {
        dragon_raise_exc_cstr(50, "write_file_bytes: cannot open file");
        return 0;
    }
    int64_t n = 0;
    if (data && data->len > 0) {
        size_t w = std::fwrite(data->data, 1, (size_t)data->len, f);
        n = (int64_t)w;
    }
    std::fclose(f);
    return n;
}

/// Write `data` (raw bytes, no codec) to an open FILE*. Returns bytes written.
/// Backs Writer.write(bytes): the str overload encodes UTF-8 via
/// dragon_file_write_text, but binary data must land verbatim, so it routes
/// here instead.
int64_t dragon_file_write_bytes(void* handle, DragonBytes* data) {
    FILE* f = (FILE*)handle;
    if (!f || !data || data->len <= 0) return 0;
    size_t w = std::fwrite(data->data, 1, (size_t)data->len, f);
    return (int64_t)w;
}

/// Read up to `size` BYTES of text from an open FILE* and decode as UTF-8 to a
/// str. Unlike a bare fread+decode, this never returns a broken code point: if
/// the requested byte count lands in the middle of a multibyte UTF-8 sequence,
/// the trailing continuation bytes are read so the final code point is whole.
/// (A few extra bytes - at most 3 - may be consumed to finish that character.)
/// This is the root fix for `File.read(size)` splitting a char mid-sequence.
const char* dragon_file_read_text(void* handle, int64_t size) {
    FILE* f = (FILE*)handle;
    if (!f || size <= 0) return dragon_string_alloc("", 0);
    // +4 headroom for a top-up that completes a 4-byte sequence.
    uint8_t* buf = (uint8_t*)std::malloc((size_t)size + 4);
    size_t n = std::fread(buf, 1, (size_t)size, f);
    if (n > 0) {
        // Walk back over up to 3 continuation bytes (10xxxxxx) to the lead byte.
        size_t i = n, cont = 0;
        while (i > 0 && (buf[i - 1] & 0xC0) == 0x80 && cont < 3) { i--; cont++; }
        if (i > 0) {
            uint8_t lead = buf[i - 1];
            size_t need = 0;
            if ((lead & 0x80) == 0x00) need = 1;
            else if ((lead & 0xE0) == 0xC0) need = 2;
            else if ((lead & 0xF0) == 0xE0) need = 3;
            else if ((lead & 0xF8) == 0xF0) need = 4;
            size_t have = n - (i - 1);
            while (need > 1 && have < need) {
                int c = std::fgetc(f);
                if (c == EOF) break;
                buf[n++] = (uint8_t)c;
                have++;
            }
        }
    }
    const char* result = dragon_string_alloc((const char*)buf, (int64_t)n);
    std::free(buf);
    return result;
}

/// Write a str to an open FILE* as UTF-8, returning the byte count written.
/// The bare `fwrite(data, 1, len(data), h)` path in .dr was wrong for non-ASCII
/// text: `len` is the code-point count, but a non-ASCII str's buffer is UCS-4
/// (4 bytes/cp), so it emitted truncated garbage. Encoding to UTF-8 first is the
/// root fix and makes File.write correct for all text.
int64_t dragon_file_write_text(void* handle, const char* s) {
    FILE* f = (FILE*)handle;
    if (!f || !s) return 0;
    int64_t blen = 0;
    char* enc = dragon_str_to_utf8_alloc(s, &blen);
    const char* src = enc ? enc : s;
    size_t w = std::fwrite(src, 1, (size_t)blen, f);
    if (enc) std::free(enc);
    return (int64_t)w;
}

} // extern "C"
