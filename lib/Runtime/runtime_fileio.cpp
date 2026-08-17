#include "runtime_internal.h"

#include <cstdio>
#include <cstdlib>

extern "C" {

DragonBytes* dragon_read_file_bytes(const char* path) {
    if (!path) {
        dragon_raise_exc_cstr(50 , "read_file_bytes: null path");
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
    uint8_t* buf = (uint8_t*)dragon_malloc_nullable(size > 0 ? (size_t)size : 1);
    if (!buf) { std::fclose(f); dragon_raise_oom(); }
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
        if ((int64_t)w != data->len) {
            std::fclose(f);
            dragon_raise_exc_cstr(50, "write_file_bytes: short write (disk full?)");
            return 0;
        }
    }
    if (std::fclose(f) != 0) {
        dragon_raise_exc_cstr(50, "write_file_bytes: close failed (write not flushed)");
        return 0;
    }
    return n;
}

int64_t dragon_file_write_bytes(void* handle, DragonBytes* data) {
    FILE* f = (FILE*)handle;
    if (!f || !data || data->len <= 0) return 0;
    size_t w = std::fwrite(data->data, 1, (size_t)data->len, f);
    return (int64_t)w;
}

const char* dragon_file_read_text(void* handle, int64_t size) {
    FILE* f = (FILE*)handle;
    if (!f || size <= 0) return dragon_string_alloc("", 0);
    uint8_t* buf = (uint8_t*)dragon_xmalloc((size_t)size + 4);
    size_t n = std::fread(buf, 1, (size_t)size, f);
    if (n > 0) {
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

}
