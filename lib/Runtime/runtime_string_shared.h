#ifndef DRAGON_RUNTIME_STRING_SHARED_H
#define DRAGON_RUNTIME_STRING_SHARED_H

#include "runtime_internal.h"

static inline DragonString* dragon_string_alloc_ascii(int64_t cp_count) {
    if (cp_count < 0 || cp_count > INT64_MAX - (int64_t)sizeof(DragonString) - 1) {
        dragon_raise_exc_cstr(43, "MemoryError: string too large");
    }
    DragonString* s = (DragonString*)dragon_xmalloc(sizeof(DragonString) + (size_t)cp_count + 1);
    dragon_obj_init(&s->header, DRAGON_TAG_STR);
    s->len = cp_count;
    s->kind = 1;
    s->cap = dragon_cap_clamp(cp_count);
    s->data[cp_count] = '\0';
    return s;
}

static inline DragonString* dragon_string_alloc_ucs4(int64_t cp_count) {
    if (cp_count < 0 || cp_count > INT64_MAX / 4) {
        dragon_raise_exc_cstr(43, "MemoryError: string too large");
    }
    int64_t bytes = cp_count * 4;
    DragonString* s = (DragonString*)dragon_xmalloc(sizeof(DragonString) + bytes + 1);
    dragon_obj_init(&s->header, DRAGON_TAG_STR);
    s->len = cp_count;
    s->kind = 4;
    s->cap = dragon_cap_clamp(bytes);
    s->data[bytes] = '\0';
    return s;
}

static inline uint32_t dragon_cp_simple_upper(uint32_t cp) {
    if ((cp >= 0x00E0 && cp <= 0x00F6) || (cp >= 0x00F8 && cp <= 0x00FE)) return cp - 0x20;
    if (cp == 0x00FF) return 0x0178;
    if (cp == 0x00B5) return 0x039C;
    if ((cp >= 0x0100 && cp <= 0x012F) || (cp >= 0x0132 && cp <= 0x0137) ||
        (cp >= 0x014A && cp <= 0x0177))
        return (cp & 1) ? cp - 1 : cp;
    if ((cp >= 0x0139 && cp <= 0x0148) || (cp >= 0x0179 && cp <= 0x017E))
        return (cp & 1) ? cp : cp - 1;
    if (cp == 0x03C2) return 0x03A3;
    if ((cp >= 0x03B1 && cp <= 0x03C1) || (cp >= 0x03C3 && cp <= 0x03CB)) return cp - 0x20;
    if (cp >= 0x0430 && cp <= 0x044F) return cp - 0x20;
    if (cp >= 0x0450 && cp <= 0x045F) return cp - 0x50;
    return cp;
}
static inline uint32_t dragon_cp_simple_lower(uint32_t cp) {
    if ((cp >= 0x00C0 && cp <= 0x00D6) || (cp >= 0x00D8 && cp <= 0x00DE)) return cp + 0x20;
    if (cp == 0x0178) return 0x00FF;
    if ((cp >= 0x0100 && cp <= 0x012F) || (cp >= 0x0132 && cp <= 0x0137) ||
        (cp >= 0x014A && cp <= 0x0177))
        return (cp & 1) ? cp : cp + 1;
    if ((cp >= 0x0139 && cp <= 0x0148) || (cp >= 0x0179 && cp <= 0x017E))
        return (cp & 1) ? cp + 1 : cp;
    if ((cp >= 0x0391 && cp <= 0x03A1) || (cp >= 0x03A3 && cp <= 0x03AB)) return cp + 0x20;
    if (cp >= 0x0410 && cp <= 0x042F) return cp + 0x20;
    if (cp >= 0x0400 && cp <= 0x040F) return cp + 0x50;
    return cp;
}

static inline bool dragon_is_heap_string(const char* s) {
    return dragon_str_is_heap(s) != 0;
}

static inline uint32_t dragon_str_cp_at(const char* s, DragonString* ds, int64_t i) {
    if (!ds || ds->kind == 1) return (uint32_t)(unsigned char)s[i];
    return ((const uint32_t*)ds->data)[i];
}

static inline int dragon_cp_is_upper(uint32_t cp) {
    if (cp < 128) return cp >= 'A' && cp <= 'Z';
    return dragon_cp_simple_lower(cp) != cp;
}

static inline int dragon_cp_is_lower(uint32_t cp) {
    if (cp < 128) return cp >= 'a' && cp <= 'z';
    return dragon_cp_simple_upper(cp) != cp;
}

static inline int dragon_cp_is_alpha(uint32_t cp) {
    if (cp < 128) return (cp >= 'a' && cp <= 'z') || (cp >= 'A' && cp <= 'Z');
    if (dragon_cp_simple_upper(cp) != cp || dragon_cp_simple_lower(cp) != cp) return 1;
    if (cp >= 0x05D0 && cp <= 0x05EA) return 1;
    if (cp >= 0x0621 && cp <= 0x064A) return 1;
    if (cp >= 0x3041 && cp <= 0x30FF) return 1;
    if (cp >= 0x4E00 && cp <= 0x9FFF) return 1;
    if (cp >= 0xAC00 && cp <= 0xD7A3) return 1;
    return 0;
}

static inline int dragon_cp_is_digit(uint32_t cp) {
    return cp >= '0' && cp <= '9';
}

static inline int dragon_cp_is_space(uint32_t cp) {
    if (cp < 128) {
        return cp == ' ' || cp == '\t' || cp == '\n' || cp == '\r' ||
               cp == '\f' || cp == '\v';
    }
    return cp == 0x0085 || cp == 0x00A0 || cp == 0x1680 ||
           (cp >= 0x2000 && cp <= 0x200A) || cp == 0x2028 || cp == 0x2029 ||
           cp == 0x202F || cp == 0x205F || cp == 0x3000;
}

#endif
