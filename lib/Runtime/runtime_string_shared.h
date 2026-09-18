#ifndef DRAGON_RUNTIME_STRING_SHARED_H
#define DRAGON_RUNTIME_STRING_SHARED_H

#include "runtime_internal.h"

#define DRAGON_STR_TABLE_STRIDE 64
#define DRAGON_STR_MAX_BYTES (INT32_MAX - 16)

static inline int dragon_utf8_next(const unsigned char* p, const unsigned char* end,
                                   uint32_t* out_cp) {
    unsigned char b0 = p[0];
    if (b0 < 0x80) { *out_cp = b0; return 1; }
    int64_t remaining = end - p;
    if ((b0 & 0xE0) == 0xC0 && remaining >= 2 && (p[1] & 0xC0) == 0x80) {
        uint32_t cp = ((uint32_t)(b0 & 0x1F) << 6) | (p[1] & 0x3F);
        if (cp >= 0x80) { *out_cp = cp; return 2; }
    } else if ((b0 & 0xF0) == 0xE0 && remaining >= 3 &&
               (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80) {
        uint32_t cp = ((uint32_t)(b0 & 0x0F) << 12) |
                      ((uint32_t)(p[1] & 0x3F) << 6) | (p[2] & 0x3F);
        if (cp >= 0x800 && !(cp >= 0xD800 && cp <= 0xDFFF)) { *out_cp = cp; return 3; }
    } else if ((b0 & 0xF8) == 0xF0 && remaining >= 4 &&
               (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80 && (p[3] & 0xC0) == 0x80) {
        uint32_t cp = ((uint32_t)(b0 & 0x07) << 18) |
                      ((uint32_t)(p[1] & 0x3F) << 12) |
                      ((uint32_t)(p[2] & 0x3F) << 6) | (p[3] & 0x3F);
        if (cp >= 0x10000 && cp <= 0x10FFFF) { *out_cp = cp; return 4; }
    }
    *out_cp = b0;
    return 1;
}

static inline int dragon_utf8_valid_next(const unsigned char* p, const unsigned char* end,
                                         uint32_t* out_cp) {
    unsigned char b0 = p[0];
    if (b0 < 0x80) { *out_cp = b0; return 1; }
    int adv = dragon_utf8_next(p, end, out_cp);
    return adv == 1 ? 0 : adv;
}

static inline int dragon_utf8_encode(uint32_t cp, char* out) {
    if (cp < 0x80) {
        out[0] = (char)cp;
        return 1;
    }
    if (cp < 0x800) {
        out[0] = (char)(0xC0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp < 0x10000) {
        out[0] = (char)(0xE0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    out[0] = (char)(0xF0 | (cp >> 18));
    out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
    out[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

static inline int dragon_utf8_width(uint32_t cp) {
    return cp < 0x80 ? 1 : cp < 0x800 ? 2 : cp < 0x10000 ? 3 : 4;
}

static inline int64_t dragon_ascii_prefix(const unsigned char* p, int64_t n) {
    static const uint64_t kHighBits = 0x8080808080808080ULL;
    int64_t i = 0;
    for (; i + 64 <= n; i += 64) {
        uint64_t acc = 0;
        for (int64_t j = 0; j < 64; j += 8) {
            uint64_t word;
            memcpy(&word, p + i + j, sizeof(word));
            acc |= word;
        }
        if (acc & kHighBits) break;
    }
    for (; i + 8 <= n; i += 8) {
        uint64_t word;
        memcpy(&word, p + i, sizeof(word));
        if (word & kHighBits) break;
    }
    for (; i < n; ++i) {
        if (p[i] >= 0x80) break;
    }
    return i;
}

static inline void dragon_str_check_bytes(int64_t nbytes) {
    if (nbytes < 0 || nbytes > DRAGON_STR_MAX_BYTES)
        dragon_raise_exc_cstr(43, "MemoryError: string exceeds 2GB of utf-8");
}

static inline size_t dragon_str_table_offset(int64_t nbytes) {
    return ((size_t)nbytes + 1 + 3) & ~(size_t)3;
}

static inline size_t dragon_str_table_slots(int64_t nbytes) {
    return (size_t)nbytes / DRAGON_STR_TABLE_STRIDE;
}

static inline uint32_t* dragon_str_table(DragonString* s) {
    return (uint32_t*)(s->data + dragon_str_table_offset(s->cap));
}

static inline int64_t dragon_ascii_word_run(const unsigned char* q, const unsigned char* end,
                                            int64_t count) {
    static const uint64_t kHighBits = 0x8080808080808080ULL;
    if (end - q < 8 || (count & (DRAGON_STR_TABLE_STRIDE - 1)) + 8 > DRAGON_STR_TABLE_STRIDE)
        return 0;
    uint64_t word;
    memcpy(&word, q, sizeof(word));
    return (word & kHighBits) ? 0 : 8;
}

typedef struct {
    uint32_t* table;
    int64_t count;
} DragonStrTableFill;

static inline void dragon_str_table_note(DragonStrTableFill* f, const unsigned char* base,
                                         const unsigned char* q) {
    if ((f->count & (DRAGON_STR_TABLE_STRIDE - 1)) == 0)
        f->table[f->count / DRAGON_STR_TABLE_STRIDE - 1] = (uint32_t)(q - base);
}

static inline DragonString* dragon_string_alloc_ascii(int64_t n) {
    dragon_str_check_bytes(n);
    DragonString* s = (DragonString*)dragon_xmalloc(sizeof(DragonString) + (size_t)n + 1);
    dragon_obj_init(&s->header, DRAGON_TAG_STR);
    s->header.gc_flags |= GC_FLAG_STR_ASCII;
    s->len = n;
    s->nbytes = (int32_t)n;
    s->cap = (int32_t)n;
    s->data[n] = '\0';
    return s;
}

static inline DragonString* dragon_string_alloc_utf8_raw(int64_t max_nbytes) {
    dragon_str_check_bytes(max_nbytes);
    size_t bytes = sizeof(DragonString) + dragon_str_table_offset(max_nbytes) +
                   dragon_str_table_slots(max_nbytes) * sizeof(uint32_t);
    DragonString* s = (DragonString*)dragon_xmalloc(bytes);
    dragon_obj_init(&s->header, DRAGON_TAG_STR);
    s->len = 0;
    s->nbytes = (int32_t)max_nbytes;
    s->cap = (int32_t)max_nbytes;
    s->data[max_nbytes] = '\0';
    return s;
}

static inline void dragon_string_finish_utf8(DragonString* s, int64_t nbytes) {
    s->nbytes = (int32_t)nbytes;
    s->data[nbytes] = '\0';
    const unsigned char* p = (const unsigned char*)s->data;
    if (dragon_ascii_prefix(p, nbytes) == nbytes) {
        s->header.gc_flags |= GC_FLAG_STR_ASCII;
        s->len = nbytes;
        return;
    }
    s->header.gc_flags &= (uint8_t)~GC_FLAG_STR_ASCII;
    DragonStrTableFill fill = {dragon_str_table(s), 0};
    const unsigned char* end = p + nbytes;
    const unsigned char* q = p;
    while (q < end) {
        int64_t run = dragon_ascii_word_run(q, end, fill.count);
        if (run) {
            q += run;
            fill.count += run;
            dragon_str_table_note(&fill, p, q);
            continue;
        }
        uint32_t cp;
        q += dragon_utf8_next(q, end, &cp);
        fill.count++;
        dragon_str_table_note(&fill, p, q);
    }
    s->len = fill.count;
}

static inline const char* dragon_string_from_utf8(const char* src, int64_t nbytes) {
    if (nbytes <= 0) return dragon_string_alloc_ascii(0)->data;
    const unsigned char* p = (const unsigned char*)src;
    if (dragon_ascii_prefix(p, nbytes) == nbytes) {
        DragonString* s = dragon_string_alloc_ascii(nbytes);
        memcpy(s->data, src, (size_t)nbytes);
        return s->data;
    }
    DragonString* s = dragon_string_alloc_utf8_raw(nbytes);
    memcpy(s->data, src, (size_t)nbytes);
    dragon_string_finish_utf8(s, nbytes);
    return s->data;
}

static inline bool dragon_is_heap_string(const char* s) {
    return dragon_str_is_heap(s) != 0;
}

typedef struct {
    const unsigned char* p;
    const unsigned char* end;
} DragonStrIter;

static inline DragonStrIter dragon_str_iter(const char* s, DragonString* ds) {
    DragonStrIter it;
    it.p = (const unsigned char*)s;
    it.end = it.p + (ds ? ds->nbytes : (s ? (int64_t)strlen(s) : 0));
    return it;
}

static inline int dragon_str_iter_next(DragonStrIter* it, uint32_t* cp) {
    if (it->p >= it->end) return 0;
    it->p += dragon_utf8_next(it->p, it->end, cp);
    return 1;
}

static inline int64_t dragon_str_cp_count(const char* s, DragonString* ds) {
    if (ds) return ds->len;
    if (!s) return 0;
    int64_t n = 0;
    uint32_t cp;
    DragonStrIter it = dragon_str_iter(s, NULL);
    while (dragon_str_iter_next(&it, &cp)) n++;
    return n;
}

static inline int64_t dragon_str_cp_offset(const char* s, DragonString* ds, int64_t i) {
    if (ds && dragon_str_ascii_flag(ds)) return i;
    const unsigned char* base = (const unsigned char*)s;
    const unsigned char* end = base + (ds ? ds->nbytes : (int64_t)strlen(s));
    const unsigned char* q = base;
    int64_t rem = i;
    if (ds && i >= DRAGON_STR_TABLE_STRIDE) {
        int64_t k = i / DRAGON_STR_TABLE_STRIDE;
        q = base + dragon_str_table(ds)[k - 1];
        rem = i - k * DRAGON_STR_TABLE_STRIDE;
    }
    uint32_t cp;
    while (rem > 0 && q < end) {
        q += dragon_utf8_next(q, end, &cp);
        rem--;
    }
    return q - base;
}

static inline uint32_t dragon_str_cp_at(const char* s, DragonString* ds, int64_t i) {
    if (ds && dragon_str_ascii_flag(ds)) return (uint32_t)(unsigned char)s[i];
    const unsigned char* base = (const unsigned char*)s;
    const unsigned char* end = base + (ds ? ds->nbytes : (int64_t)strlen(s));
    const unsigned char* q = base + dragon_str_cp_offset(s, ds, i);
    uint32_t cp = 0;
    if (q < end) dragon_utf8_next(q, end, &cp);
    return cp;
}

static inline int64_t dragon_str_cp_index_of_offset(const char* s, DragonString* ds,
                                                    int64_t byte_off) {
    if (ds && dragon_str_ascii_flag(ds)) return byte_off;
    const unsigned char* base = (const unsigned char*)s;
    const unsigned char* end = base + (ds ? ds->nbytes : (int64_t)strlen(s));
    const unsigned char* q = base;
    int64_t index = 0;
    if (ds) {
        uint32_t* table = dragon_str_table(ds);
        int64_t slots = ds->len / DRAGON_STR_TABLE_STRIDE;
        int64_t lo = 0, hi = slots;
        while (lo < hi) {
            int64_t mid = (lo + hi) / 2;
            if ((int64_t)table[mid] <= byte_off) lo = mid + 1;
            else hi = mid;
        }
        if (lo > 0) {
            q = base + table[lo - 1];
            index = lo * DRAGON_STR_TABLE_STRIDE;
        }
    }
    uint32_t cp;
    while (q < end && (int64_t)(q - base) < byte_off) {
        q += dragon_utf8_next(q, end, &cp);
        index++;
    }
    return index;
}

static inline int dragon_str_offset_is_boundary(const char* s, DragonString* ds,
                                                int64_t byte_off) {
    if (ds && dragon_str_ascii_flag(ds)) return 1;
    int64_t index = dragon_str_cp_index_of_offset(s, ds, byte_off);
    return dragon_str_cp_offset(s, ds, index) == byte_off;
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
