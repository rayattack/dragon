#include "runtime_internal.h"
#include "runtime_string_shared.h"

extern "C" {

const char* dragon_string_alloc(const char* src, int64_t byte_len) {
    if (byte_len <= 0 || !src) return dragon_string_alloc_ascii(0)->data;
    return dragon_string_from_utf8(src, byte_len);
}

static inline __attribute__((always_inline))
int dragon_name_is(const char* name, const char* lc_target, size_t n) {
    for (size_t i = 0; i < n; ++i) {
        char c = name[i];
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        if (c != lc_target[i]) return 0;
    }
    return name[n] == 0;
}

#define dragon_enc_is(name, lit) dragon_name_is((name), (lit), sizeof(lit) - 1)

static int dragon_errors_policy(const char* errors) {
    if (!errors) return 0;
    char first = errors[0];
    if (first == 's' || first == 'S') return dragon_enc_is(errors, "strict") ? 0 : -1;
    if (first == 'r' || first == 'R') return dragon_enc_is(errors, "replace") ? 1 : -1;
    return -1;
}

enum {
    DRAGON_ENCODING_UNKNOWN = 0,
    DRAGON_ENCODING_ASCII = 1,
    DRAGON_ENCODING_UTF8 = 2
};

static int dragon_encoding_kind(const char* encoding) {
    if (!encoding) return DRAGON_ENCODING_UNKNOWN;
    char first = encoding[0];
    if (first == 'u' || first == 'U') {
        if (dragon_enc_is(encoding, "utf-8") || dragon_enc_is(encoding, "utf8") ||
            dragon_enc_is(encoding, "u8"))
            return DRAGON_ENCODING_UTF8;
        return dragon_enc_is(encoding, "us-ascii") ? DRAGON_ENCODING_ASCII
                                                   : DRAGON_ENCODING_UNKNOWN;
    }
    if (first == 'a' || first == 'A')
        return dragon_enc_is(encoding, "ascii") ? DRAGON_ENCODING_ASCII
                                                : DRAGON_ENCODING_UNKNOWN;
    return DRAGON_ENCODING_UNKNOWN;
}

static const char kReplacementUtf8[3] = {(char)0xEF, (char)0xBF, (char)0xBD};

static const char* dragon_decode_checked(const unsigned char* p, int64_t n,
                                         int ascii_only, int pol) {
    if (dragon_ascii_prefix(p, n) == n) {
        DragonString* s = dragon_string_alloc_ascii(n);
        memcpy(s->data, p, (size_t)n);
        return s->data;
    }
    if (ascii_only && pol == 0) {
        dragon_raise_exc_cstr(92, "'ascii' codec can't decode byte");
        return dragon_string_alloc_ascii(0)->data;
    }
    dragon_str_check_bytes(n > DRAGON_STR_MAX_BYTES / 3 ? DRAGON_STR_MAX_BYTES + 1 : n * 3);
    DragonString* s = dragon_string_alloc_utf8_raw(n * 3);
    char* w = s->data;
    const unsigned char* base = (const unsigned char*)s->data;
    DragonStrTableFill fill = {dragon_str_table(s), 0};
    const unsigned char* end = p + n;
    const unsigned char* q = p;
    while (q < end) {
        int64_t run = dragon_ascii_word_run(q, end, fill.count);
        if (run) {
            memcpy(w, q, (size_t)run);
            w += run;
            q += run;
            fill.count += run;
            dragon_str_table_note(&fill, base, (const unsigned char*)w);
            continue;
        }
        uint32_t cp;
        int adv = *q < 0x80 ? 1 : (ascii_only ? 0 : dragon_utf8_valid_next(q, end, &cp));
        if (adv <= 0 && pol == 0) {
            free(s);
            dragon_raise_exc_cstr(92, ascii_only ? "'ascii' codec can't decode byte"
                                                 : "'utf-8' codec can't decode byte");
            return dragon_string_alloc_ascii(0)->data;
        }
        if (adv <= 0) {
            memcpy(w, kReplacementUtf8, 3);
            w += 3;
            q += 1;
        } else if (adv == 1) {
            *w++ = (char)*q++;
        } else {
            for (int k = 0; k < adv; ++k) *w++ = (char)*q++;
        }
        fill.count++;
        dragon_str_table_note(&fill, base, (const unsigned char*)w);
    }
    s->nbytes = (int32_t)(w - s->data);
    s->data[s->nbytes] = '\0';
    s->len = fill.count;
    s->header.gc_flags |= GC_FLAG_STR_UTF8;
    return s->data;
}

const char* dragon_bytes_decode_ex(DragonBytes* b, const char* encoding,
                                   const char* errors) {
    int pol = dragon_errors_policy(errors);
    if (pol < 0) {
        dragon_raise_exc_cstr(40, "unknown error handler name");
        return dragon_string_alloc_ascii(0)->data;
    }
    int kind = dragon_encoding_kind(encoding);
    if (kind == DRAGON_ENCODING_UNKNOWN) {
        dragon_raise_exc_cstr(40, "unknown encoding");
        return dragon_string_alloc_ascii(0)->data;
    }
    if (!b || b->len == 0) return dragon_string_alloc_ascii(0)->data;
    return dragon_decode_checked(b->data, b->len, kind == DRAGON_ENCODING_ASCII ? 1 : 0, pol);
}

static_assert(sizeof(DragonBytes) == sizeof(DragonString),
              "a bytes object must be relabelable as a string in place");
static_assert(offsetof(DragonBytes, len) == offsetof(DragonString, len),
              "a bytes object must be relabelable as a string in place");
static_assert(sizeof(DragonBytes) == offsetof(DragonString, data),
              "an inline bytes buffer must start where string characters start");

static bool dragon_bytes_adoptable_as_str(DragonBytes* b) {
    if (!b || b->len <= 0 || b->len > DRAGON_STR_MAX_BYTES) return false;
    if (b->header.type_tag != DRAGON_TAG_BYTES) return false;
    if (dragon_refcount_load(&b->header) != 1) return false;
    if (dragon_gc_flags_load(&b->header) &
        (GC_FLAG_SHARED | GC_FLAG_BORROWED_BUFFER | GC_FLAG_TRACKED)) return false;
    if (b->data != (uint8_t*)(b + 1)) return false;
    return dragon_ascii_prefix(b->data, b->len) == b->len;
}

static const char* dragon_bytes_adopt_as_str(DragonBytes* b) {
    DragonString* s = (DragonString*)b;
    int64_t n = b->len;
    s->header.type_tag = DRAGON_TAG_STR;
    s->header.gc_flags |= GC_FLAG_STR_ASCII | GC_FLAG_STR_UTF8;
    s->nbytes = (int32_t)n;
    s->cap = (int32_t)n;
    return s->data;
}

const char* dragon_bytes_decode_owned_ex(DragonBytes* b, const char* encoding,
                                         const char* errors) {
    if (dragon_errors_policy(errors) >= 0 &&
        dragon_encoding_kind(encoding) != DRAGON_ENCODING_UNKNOWN &&
        dragon_bytes_adoptable_as_str(b))
        return dragon_bytes_adopt_as_str(b);
    const char* out = dragon_bytes_decode_ex(b, encoding, errors);
    dragon_decref(b);
    return out;
}

DragonBytes* dragon_str_encode_ex(const char* s, const char* encoding,
                                  const char* errors) {
    int pol = dragon_errors_policy(errors);
    if (pol < 0) {
        dragon_raise_exc_cstr(40, "unknown error handler name");
        return dragon_bytes_new(nullptr, 0);
    }
    int kind = dragon_encoding_kind(encoding);
    if (kind == DRAGON_ENCODING_UNKNOWN) {
        dragon_raise_exc_cstr(40, "unknown encoding");
        return dragon_bytes_new(nullptr, 0);
    }
    if (!s) return dragon_bytes_new(nullptr, 0);
    if (kind == DRAGON_ENCODING_UTF8) return dragon_bytes_of_utf8_str(s);
    int64_t blen = dragon_str_total_bytes(s);
    if (dragon_ascii_prefix((const unsigned char*)s, blen) == blen)
        return dragon_bytes_of_utf8_str(s);
    if (pol == 0) {
        dragon_raise_exc_cstr(93, "'ascii' codec can't encode character");
        return dragon_bytes_new(nullptr, 0);
    }
    DragonString* ds = dragon_is_heap_string(s) ? dragon_string_from_data(s) : NULL;
    int64_t outn = dragon_str_cp_count(s, ds);
    DragonBytes* bts = dragon_bytes_alloc_raw(outn);
    DragonStrIter it = dragon_str_iter(s, ds);
    uint32_t cp;
    int64_t w = 0;
    while (dragon_str_iter_next(&it, &cp))
        bts->data[w++] = (cp < 0x80) ? (uint8_t)cp : (uint8_t)'?';
    return bts;
}

DragonString* dragon_string_alloc_raw(int64_t byte_len) {
    if (byte_len < 0) byte_len = 0;
    return dragon_string_alloc_utf8_raw(byte_len);
}

void dragon_string_raw_finish(DragonString* s, int64_t nbytes) {
    dragon_string_finish_utf8(s, nbytes);
}

/// Dup a message string before re-raising only when it's a MORTAL heap string
/// scope cleanup could free out from under the exception (UAF); literals/immortals return unchanged (no leak, cleanup never frees them).
const char* dragon_exc_msg_preserve(const char* s) {
    if (!s) return s;
    if (!dragon_str_is_heap(s)) return s;
    DragonString* ds = dragon_string_from_data(s);
    if (ds->header.refcount == DRAGON_IMMORTAL_REFCOUNT) return s;
    return dragon_string_dup(s);
}

const char* dragon_string_dup_cstr(const char* s) {
    if (!s) return dragon_string_alloc_ascii(0)->data;
    return dragon_string_from_utf8(s, (int64_t)strlen(s));
}

const char* dragon_string_dup(const char* s) {
    if (!s) return dragon_string_alloc_ascii(0)->data;
    return dragon_string_from_utf8(s, dragon_str_total_bytes(s));
}

const char* dragon_str_intern(const char* utf8_bytes, int64_t byte_len) {
    const char* data = dragon_string_alloc(utf8_bytes, byte_len);
    if (data) {
        DragonString* ds = dragon_string_from_data(data);
        ds->header.refcount = DRAGON_IMMORTAL_REFCOUNT;
    }
    return data;
}

/// Promote a heap string to immortal (refcount saturated) so incref/decref no-op.
/// Used on module-globals shared across HTTP threads to avoid a racing non-atomic refcount (torn count -> UAF).
void dragon_str_make_immortal(const char* s) {
    if (!s || !dragon_is_heap_string(s)) return;
    DragonString* ds = dragon_string_from_data(s);
    ds->header.refcount = DRAGON_IMMORTAL_REFCOUNT;
}

char* dragon_str_to_utf8_alloc(const char* s, int64_t* out_byte_len) {
    if (out_byte_len) *out_byte_len = dragon_str_total_bytes(s);
    return NULL;
}

int64_t dragon_str_byte_len_pub(const char* s) {
    return dragon_str_total_bytes(s);
}

int64_t dragon_str_is_ascii(const char* s) {
    if (!s) return 1;
    if (dragon_is_heap_string(s)) return dragon_str_ascii_flag(dragon_string_from_data(s));
    int64_t n = (int64_t)strlen(s);
    return dragon_ascii_prefix((const unsigned char*)s, n) == n ? 1 : 0;
}

int64_t dragon_str_is_utf8(const char* s) {
    if (!s || !dragon_is_heap_string(s)) return 0;
    return (dragon_string_from_data(s)->header.gc_flags & GC_FLAG_STR_UTF8) ? 1 : 0;
}

int64_t dragon_str_cp_at_index(const char* s, int64_t index) {
    DragonString* ds = dragon_is_heap_string(s) ? dragon_string_from_data(s) : NULL;
    return (int64_t)dragon_str_cp_at(s, ds, index);
}

int64_t dragon_str_decode_at(const char* s, int64_t byte_off) {
    DragonString* ds = dragon_is_heap_string(s) ? dragon_string_from_data(s) : NULL;
    const unsigned char* base = (const unsigned char*)s;
    const unsigned char* end = base + (ds ? ds->nbytes : (int64_t)strlen(s));
    const unsigned char* q = base + byte_off;
    uint32_t cp = 0;
    int64_t adv = q < end ? dragon_utf8_next(q, end, &cp) : 0;
    return (int64_t)cp | (adv << 32);
}

int64_t dragon_str_cp_byte_offset(const char* s, int64_t index) {
    DragonString* ds = dragon_is_heap_string(s) ? dragon_string_from_data(s) : NULL;
    return dragon_str_cp_offset(s, ds, index);
}

int64_t dragon_str_next_cp(const char* s, int64_t* byte_cursor) {
    DragonString* ds = dragon_is_heap_string(s) ? dragon_string_from_data(s) : NULL;
    const unsigned char* base = (const unsigned char*)s;
    const unsigned char* end = base + (ds ? ds->nbytes : (int64_t)strlen(s));
    const unsigned char* q = base + *byte_cursor;
    uint32_t cp = 0;
    if (q < end) *byte_cursor += dragon_utf8_next(q, end, &cp);
    return (int64_t)cp;
}

/// Identity retain: incref (no-op for literals/immortals), return `s`. Codegen
/// routes str(s)/f"{s}" through this for an owned result; without it, consumers over-released a borrow, causing UAF (e.g. `msg = str(e)`).
const char* dragon_str_retain(const char* s) {
    dragon_incref_str(s);
    return s;
}

void dragon_incref_str(const char* s) {
    if (!s) return;
    if (!dragon_is_heap_string(s)) return;
    DragonString* ds = dragon_string_from_data(s);
    if (dragon_refcount_load(&ds->header) >= DRAGON_IMMORTAL_REFCOUNT) return;
    if (dragon_gc_flags_load(&ds->header) & GC_FLAG_SHARED) {
        __atomic_fetch_add(&ds->header.refcount, 1, __ATOMIC_RELAXED);
        return;
    }
    ds->header.refcount++;
}

static void dragon_str_free_now(DragonString* ds) {
    int collecting = __atomic_load_n(&gc_collecting, __ATOMIC_ACQUIRE);
    if (!collecting || ds->header.gc_track_idx < 0) free(ds);
}

static __attribute__((noinline)) void dragon_str_free_at_safepoint(DragonString* ds) {
    dragon_gc_safepoint();
    dragon_str_free_now(ds);
}

void dragon_decref_str(const char* s) {
    if (!s) return;
    if (!dragon_is_heap_string(s)) return;
    DragonString* ds = dragon_string_from_data(s);
    if (dragon_refcount_load(&ds->header) >= DRAGON_IMMORTAL_REFCOUNT) return;
    if (dragon_gc_flags_load(&ds->header) & GC_FLAG_SHARED) {
        if (__atomic_sub_fetch(&ds->header.refcount, 1, __ATOMIC_ACQ_REL) != 0) return;
        if (dragon_gc_stop_pending()) dragon_str_free_at_safepoint(ds);
        else dragon_str_free_now(ds);
        return;
    }
    if (--ds->header.refcount != 0) return;
    if (dragon_gc_stop_pending()) dragon_str_free_at_safepoint(ds);
    else dragon_str_free_now(ds);
}

void dragon_incref_str_atomic(const char* s) {
    if (!s) return;
    if (!dragon_is_heap_string(s)) return;
    DragonString* ds = dragon_string_from_data(s);
    if (dragon_refcount_load(&ds->header) >= DRAGON_IMMORTAL_REFCOUNT) return;
    if (!(dragon_gc_flags_load(&ds->header) & GC_FLAG_SHARED))
        __atomic_fetch_or(&ds->header.gc_flags, GC_FLAG_SHARED, __ATOMIC_RELAXED);
    __atomic_fetch_add(&ds->header.refcount, 1, __ATOMIC_RELAXED);
}

void dragon_decref_str_atomic(const char* s) {
    if (!s) return;
    if (!dragon_is_heap_string(s)) return;
    DragonString* ds = dragon_string_from_data(s);
    if (dragon_refcount_load(&ds->header) >= DRAGON_IMMORTAL_REFCOUNT) return;
    if (!(dragon_gc_flags_load(&ds->header) & GC_FLAG_SHARED))
        __atomic_fetch_or(&ds->header.gc_flags, GC_FLAG_SHARED, __ATOMIC_RELAXED);
    if (__atomic_sub_fetch(&ds->header.refcount, 1, __ATOMIC_ACQ_REL) == 0)
        free(ds);
}

void dragon_str_force_free_if_zero(const char* s) {
    if (!s) return;
    if (!dragon_is_heap_string(s)) return;
    DragonString* ds = dragon_string_from_data(s);
    if (ds->header.refcount >= DRAGON_IMMORTAL_REFCOUNT) return;
    if (--ds->header.refcount == 0) free(ds);
}

static inline bool dragon_str_is_ascii_text(const char* s, DragonString* ds, int64_t nbytes) {
    if (ds) return dragon_str_ascii_flag(ds) != 0;
    return dragon_ascii_prefix((const unsigned char*)s, nbytes) == nbytes;
}

const char* dragon_str_concat(const char* a, const char* b) {
    if (!a) a = "";
    if (!b) b = "";
    DragonString* da = dragon_is_heap_string(a) ? dragon_string_from_data(a) : NULL;
    DragonString* db = dragon_is_heap_string(b) ? dragon_string_from_data(b) : NULL;
    int64_t na = da ? da->nbytes : (int64_t)strlen(a);
    int64_t nb = db ? db->nbytes : (int64_t)strlen(b);
    if (na > DRAGON_STR_MAX_BYTES - nb) {
        dragon_raise_exc_cstr(43, "MemoryError: string concat too large");
    }
    int64_t total = na + nb;
    if (dragon_str_is_ascii_text(a, da, na) && dragon_str_is_ascii_text(b, db, nb)) {
        DragonString* ds = dragon_string_alloc_ascii(total);
        memcpy(ds->data, a, (size_t)na);
        memcpy(ds->data + na, b, (size_t)nb);
        return ds->data;
    }
    DragonString* ds = dragon_string_alloc_utf8_raw(total);
    memcpy(ds->data, a, (size_t)na);
    memcpy(ds->data + na, b, (size_t)nb);
    dragon_string_finish_utf8(ds, total);
    return ds->data;
}

static DragonString* dragon_str_unique_ascii_owner(const char* a, const char* b) {
    if (a == b || !dragon_is_heap_string(a)) return NULL;
    DragonString* da = dragon_string_from_data(a);
    bool unique = da->header.refcount == 1 && !dragon_is_immortal(da) &&
                  !(da->header.gc_flags & GC_FLAG_SHARED) && dragon_str_ascii_flag(da);
    return unique ? da : NULL;
}

static DragonString* dragon_str_grow_for_append(DragonString* da, int64_t need) {
    if (need <= (int64_t)da->cap) return da;
    int64_t new_cap = (int64_t)da->cap * 2;
    if (new_cap < need) new_cap = need;
    if (new_cap > DRAGON_STR_MAX_BYTES) new_cap = DRAGON_STR_MAX_BYTES;
    DragonString* grown = (DragonString*)dragon_realloc_nullable(
        da, sizeof(DragonString) + (size_t)new_cap + 1);
    if (!grown) return NULL;
    grown->cap = (int32_t)new_cap;
    return grown;
}

const char* dragon_str_append_inplace(const char* a, const char* b) {
    DragonString* da = dragon_str_unique_ascii_owner(a, b);
    const char* bb = b ? b : "";
    DragonString* db = dragon_is_heap_string(bb) ? dragon_string_from_data(bb) : NULL;
    int64_t nb = db ? db->nbytes : (int64_t)strlen(bb);
    bool appendable = da && dragon_str_is_ascii_text(bb, db, nb) &&
                      (int64_t)da->nbytes + nb <= DRAGON_STR_MAX_BYTES;
    DragonString* grown = appendable ? dragon_str_grow_for_append(da, da->nbytes + nb) : NULL;
    if (!grown) {
        const char* r = dragon_str_concat(a, b);
        dragon_decref_str_dispatch(a);
        return r;
    }
    int64_t na = grown->nbytes;
    int64_t need = na + nb;
    if (nb) memcpy(grown->data + na, bb, (size_t)nb);
    grown->len = need;
    grown->nbytes = (int32_t)need;
    grown->data[need] = '\0';
    return grown->data;
}

int64_t dragon_str_len(const char* s) {
    if (!s) return 0;
    if (dragon_is_heap_string(s)) return dragon_string_from_data(s)->len;
    return dragon_str_cp_count(s, NULL);
}

const char* dragon_int_to_str(int64_t value) {
    char tmp[20];
    int n = 0;
    uint64_t u = (value < 0) ? (uint64_t)(-(value + 1)) + 1 : (uint64_t)value;
    do {
        tmp[n++] = (char)('0' + (int)(u % 10));
        u /= 10;
    } while (u);
    int neg = value < 0;
    DragonString* s = dragon_string_alloc_ascii(n + neg);
    char* d = s->data;
    int i = 0;
    if (neg) d[i++] = '-';
    while (n) d[i++] = tmp[--n];
    return s->data;
}

int dragon_format_double_into(double value, char* buf, size_t bufsz) {
    if (std::isnan(value)) return snprintf(buf, bufsz, "nan");
    if (std::isinf(value)) return snprintf(buf, bufsz, value < 0 ? "-inf" : "inf");

    char ebuf[40];
    int P = 17;
    for (int p = 1; p <= 17; p++) {
        snprintf(ebuf, sizeof(ebuf), "%.*e", p - 1, value);
        if (strtod(ebuf, nullptr) == value) { P = p; break; }
    }
    snprintf(ebuf, sizeof(ebuf), "%.*e", P - 1, value);
    const char* epos = strchr(ebuf, 'e');
    int exp10 = epos ? atoi(epos + 1) : 0;

    int len;
    if (exp10 >= -4 && exp10 < 16) {
        int frac = (P - 1) - exp10;
        if (frac < 0) frac = 0;
        len = snprintf(buf, bufsz, "%.*f", frac, value);
        if (frac == 0 && (size_t)len + 2 < bufsz) {
            buf[len++] = '.';
            buf[len++] = '0';
            buf[len] = '\0';
        }
    } else {
        len = snprintf(buf, bufsz, "%.*e", P - 1, value);
    }
    return len;
}

const char* dragon_float_to_str(double value) {
    char tmp[64];
    int len = dragon_format_double_into(value, tmp, sizeof(tmp));
    return dragon_string_alloc(tmp, len);
}

typedef struct {
    char fill;
    char align;
    char sign;
    int  alt;
    long width;
    char grouping;
    long precision;
    char type;
} DragonFmtSpec;

static int dragon_fmt_is_align(char c) {
    return c == '<' || c == '>' || c == '^' || c == '=';
}

static int dragon_parse_fmt_spec(const char* spec, size_t slen, DragonFmtSpec* o) {
    o->fill = ' '; o->align = 0; o->sign = '-'; o->alt = 0;
    o->width = 0; o->grouping = 0; o->precision = -1; o->type = 0;
    size_t i = 0;
    if (slen - i >= 2 && dragon_fmt_is_align(spec[i + 1])) {
        o->fill = spec[i]; o->align = spec[i + 1]; i += 2;
    } else if (slen - i >= 1 && dragon_fmt_is_align(spec[i])) {
        o->align = spec[i]; i += 1;
    }
    if (i < slen && (spec[i] == '+' || spec[i] == '-' || spec[i] == ' ')) { o->sign = spec[i]; i++; }
    if (i < slen && spec[i] == '#') { o->alt = 1; i++; }
    if (i < slen && spec[i] == '0') {
        if (!o->align) { o->align = '='; o->fill = '0'; }
        i++;
    }
    while (i < slen && spec[i] >= '0' && spec[i] <= '9') {
        o->width = o->width * 10 + (spec[i] - '0');
        if (o->width > 1000000) return 0;
        i++;
    }
    if (i < slen && (spec[i] == ',' || spec[i] == '_')) { o->grouping = spec[i]; i++; }
    if (i < slen && spec[i] == '.') {
        i++;
        if (i >= slen || spec[i] < '0' || spec[i] > '9') return 0;
        o->precision = 0;
        while (i < slen && spec[i] >= '0' && spec[i] <= '9') {
            o->precision = o->precision * 10 + (spec[i] - '0');
            if (o->precision > 1000000) return 0;
            i++;
        }
    }
    if (i < slen) {
        if (!strchr("bcdoxXeEfFgGn%", spec[i])) return 0;
        o->type = spec[i];
        i++;
    }
    return i == slen;
}

static void dragon_fmt_group(const char* in, char sep, int group, char* out) {
    int n = (int)strlen(in);
    int first = n % group;
    if (first == 0) first = group;
    int oi = 0, di = 0;
    for (int k = 0; k < first && di < n; k++) out[oi++] = in[di++];
    while (di < n) {
        out[oi++] = sep;
        for (int k = 0; k < group && di < n; k++) out[oi++] = in[di++];
    }
    out[oi] = '\0';
}

static const char* dragon_fmt_pad(const DragonFmtSpec* o, const char* prefix, const char* body) {
    size_t plen = strlen(prefix), blen = strlen(body);
    size_t total = plen + blen;
    long width = o->width;
    size_t pad = (long)total >= width ? 0 : (size_t)(width - (long)total);
    char* buf = (char*)dragon_xmalloc(total + pad + 1);
    char align = o->align ? o->align : '>';
    char fill = o->fill;
    size_t pos = 0;
    if (pad == 0 || align == '<') {
        memcpy(buf + pos, prefix, plen); pos += plen;
        memcpy(buf + pos, body, blen);   pos += blen;
        for (size_t k = 0; k < pad; k++) buf[pos++] = fill;
    } else if (align == '^') {
        size_t left = pad / 2, right = pad - left;
        for (size_t k = 0; k < left; k++) buf[pos++] = fill;
        memcpy(buf + pos, prefix, plen); pos += plen;
        memcpy(buf + pos, body, blen);   pos += blen;
        for (size_t k = 0; k < right; k++) buf[pos++] = fill;
    } else if (align == '=') {
        memcpy(buf + pos, prefix, plen); pos += plen;
        for (size_t k = 0; k < pad; k++) buf[pos++] = fill;
        memcpy(buf + pos, body, blen);   pos += blen;
    } else {
        for (size_t k = 0; k < pad; k++) buf[pos++] = fill;
        memcpy(buf + pos, prefix, plen); pos += plen;
        memcpy(buf + pos, body, blen);   pos += blen;
    }
    buf[pos] = '\0';
    const char* r = dragon_string_alloc(buf, (int64_t)pos);
    free(buf);
    return r;
}

static const char* dragon_fmt_error(void) {
    dragon_raise_exc_cstr(90, "ValueError: invalid format specifier");
    return dragon_string_alloc("", 0);
}

const char* dragon_float_format(double value, const char* spec) {
    if (!spec || !*spec) return dragon_float_to_str(value);
    size_t slen = strlen(spec);
    DragonFmtSpec o;
    if (!dragon_parse_fmt_spec(spec, slen, &o)) return dragon_fmt_error();

    char type = o.type;
    if (type == 'n') type = 'g';
    int percent = 0;
    double v = value;
    if (type == '%') { percent = 1; v = value * 100.0; type = 'f'; }

    int neg = std::signbit(v);
    double av = neg ? -v : v;
    int nan_inf = std::isnan(av) || std::isinf(av);
    if (nan_inf) {
        if (o.fill == '0') o.fill = ' ';
        if (o.align == '=') o.align = '>';
    }

    long prec = o.precision;
    char nanbuf[8];
    char* heapmag = NULL;
    char* mag;
    if (nan_inf) {
        snprintf(nanbuf, sizeof(nanbuf), "%s", std::isnan(av) ? "nan" : "inf");
        mag = nanbuf;
    } else {
        long bufprec = (prec < 0 ? 17 : prec);
        size_t magcap = 360 + (size_t)bufprec + 8;
        heapmag = (char*)dragon_xmalloc(magcap);
        if (type == 'f' || type == 'F') {
            snprintf(heapmag, magcap, "%.*f", (int)(prec < 0 ? 6 : prec), av);
        } else if (type == 'e' || type == 'E') {
            snprintf(heapmag, magcap, type == 'e' ? "%.*e" : "%.*E", (int)(prec < 0 ? 6 : prec), av);
        } else if (type == 'g' || type == 'G') {
            int gp = (int)(prec < 0 ? 6 : (prec == 0 ? 1 : prec));
            snprintf(heapmag, magcap, type == 'g' ? "%.*g" : "%.*G", gp, av);
        } else if (type == 0) {
            if (prec < 0) dragon_format_double_into(av, heapmag, magcap);
            else snprintf(heapmag, magcap, "%.*g", (int)(prec == 0 ? 1 : prec), av);
        } else {
            free(heapmag);
            return dragon_fmt_error();
        }
        mag = heapmag;
    }

    char* finalmag = mag;
    char* grpbuf = NULL;
    if (!nan_inf && (o.grouping == ',' || o.grouping == '_') &&
        !strchr(mag, 'e') && !strchr(mag, 'E')) {
        size_t ilen = 0;
        while (mag[ilen] >= '0' && mag[ilen] <= '9') ilen++;
        char intpart[400];
        if (ilen < sizeof(intpart)) {
            memcpy(intpart, mag, ilen); intpart[ilen] = '\0';
            char grouped[540];
            dragon_fmt_group(intpart, o.grouping, 3, grouped);
            size_t cap2 = strlen(grouped) + strlen(mag + ilen) + 2;
            grpbuf = (char*)dragon_malloc_nullable(cap2);
            if (!grpbuf) { free(heapmag); dragon_raise_oom(); }
            snprintf(grpbuf, cap2, "%s%s", grouped, mag + ilen);
            finalmag = grpbuf;
        }
    }

    char* withpct = NULL;
    if (percent) {
        size_t cap3 = strlen(finalmag) + 2;
        withpct = (char*)dragon_malloc_nullable(cap3);
        if (!withpct) { free(grpbuf); free(heapmag); dragon_raise_oom(); }
        snprintf(withpct, cap3, "%s%%", finalmag);
        finalmag = withpct;
    }

    char prefix[4]; int pp = 0;
    if (neg) prefix[pp++] = '-';
    else if (o.sign == '+') prefix[pp++] = '+';
    else if (o.sign == ' ') prefix[pp++] = ' ';
    prefix[pp] = '\0';

    const char* result = dragon_fmt_pad(&o, prefix, finalmag);
    if (heapmag) free(heapmag);
    if (grpbuf) free(grpbuf);
    if (withpct) free(withpct);
    return result;
}

const char* dragon_int_format(int64_t value, const char* spec) {
    if (!spec || !*spec) return dragon_int_to_str(value);
    size_t slen = strlen(spec);
    DragonFmtSpec o;
    if (!dragon_parse_fmt_spec(spec, slen, &o)) return dragon_fmt_error();

    char type = o.type ? o.type : 'd';
    if (type == 'e' || type == 'E' || type == 'f' || type == 'F' ||
        type == 'g' || type == 'G' || type == '%') {
        return dragon_float_format((double)value, spec);
    }
    if (type != 'c' && o.precision >= 0) return dragon_fmt_error();

    int neg = value < 0;
    unsigned long long mag = neg ? (unsigned long long)(-(value + 1)) + 1ULL
                                 : (unsigned long long)value;
    char digits[80];
    const char* altpfx = "";
    switch (type) {
        case 'd': case 'n': snprintf(digits, sizeof(digits), "%llu", mag); break;
        case 'x': snprintf(digits, sizeof(digits), "%llx", mag); if (o.alt) altpfx = "0x"; break;
        case 'X': snprintf(digits, sizeof(digits), "%llX", mag); if (o.alt) altpfx = "0X"; break;
        case 'o': snprintf(digits, sizeof(digits), "%llo", mag); if (o.alt) altpfx = "0o"; break;
        case 'b': {
            char bits[80]; int bp = 0; unsigned long long t = mag;
            if (t == 0) bits[bp++] = '0'; else while (t) { bits[bp++] = '0' + (int)(t & 1); t >>= 1; }
            int di = 0; for (int k = bp - 1; k >= 0; k--) digits[di++] = bits[k]; digits[di] = '\0';
            if (o.alt) altpfx = "0b"; break;
        }
        case 'c': digits[0] = (char)value; digits[1] = '\0'; break;
        default: return dragon_fmt_error();
    }

    char grouped[160];
    if (o.grouping && (type == 'd' || type == 'n')) {
        dragon_fmt_group(digits, o.grouping, 3, grouped);
    } else if (o.grouping == '_' && (type == 'x' || type == 'X' || type == 'o' || type == 'b')) {
        dragon_fmt_group(digits, '_', 4, grouped);
    } else if (o.grouping == ',') {
        return dragon_fmt_error();
    } else {
        strncpy(grouped, digits, sizeof(grouped) - 1); grouped[sizeof(grouped) - 1] = '\0';
    }

    char prefix[8]; int pp = 0;
    if (neg) prefix[pp++] = '-';
    else if (o.sign == '+') prefix[pp++] = '+';
    else if (o.sign == ' ') prefix[pp++] = ' ';
    for (const char* a = altpfx; *a; a++) prefix[pp++] = *a;
    prefix[pp] = '\0';

    return dragon_fmt_pad(&o, prefix, grouped);
}

int64_t dragon_str_eq(const char* a, const char* b) {
    return dragon_str_bytes_equal(a, b);
}

int64_t dragon_str_eq_const(const char* a, const char* b) {
    if (!a || !b) return (a == b) ? 1 : 0;
    int64_t la = dragon_str_total_bytes(a);
    int64_t lb = dragon_str_total_bytes(b);
    volatile unsigned int result = (la == lb) ? 0u : 1u;
    for (int64_t i = 0; i < lb; i++) {
        unsigned char ca = (la > 0) ? (unsigned char)a[i % la] : 0;
        result |= (unsigned int)(ca ^ (unsigned char)b[i]);
    }
    return result == 0u ? 1 : 0;
}

int64_t dragon_str_cmp(const char* a, const char* b) {
    if (!a && !b) return 0;
    if (!a) return -1;
    if (!b) return 1;
    int64_t la = dragon_str_total_bytes(a);
    int64_t lb = dragon_str_total_bytes(b);
    int64_t n0 = la < lb ? la : lb;
    int c = memcmp(a, b, (size_t)n0);
    if (c != 0) return c < 0 ? -1 : 1;
    return la == lb ? 0 : (la < lb ? -1 : 1);
}

int64_t dragon_str_to_int(const char* s) {
    char tls_msg[320];
    auto is_ws = [](char c) {
        return c == ' ' || c == '\t' || c == '\n' ||
               c == '\r' || c == '\f' || c == '\v';
    };
    auto fail = [&](const char* str) -> int64_t {
        snprintf(tls_msg, sizeof(tls_msg),
                 "ValueError: invalid literal for int() with base 10: '%s'",
                 str ? str : "");
        dragon_raise_exc_cstr(90, tls_msg);
        return 0;
    };
    if (!s) {
        dragon_raise_exc_cstr(90,
            "ValueError: int() argument must be a string, not NoneType");
        return 0;
    }
    const char* p = s;
    while (is_ws(*p)) p++;
    int sign = 1;
    if (*p == '+') { p++; }
    else if (*p == '-') { sign = -1; p++; }

    bool any_digit = false;
    bool prev_underscore = false;
    bool overflow = false;
    const uint64_t kMagLimit = (sign < 0) ? 9223372036854775808ULL
                                          : 9223372036854775807ULL;
    uint64_t acc = 0;
    for (;;) {
        char c = *p;
        if (c >= '0' && c <= '9') {
            any_digit = true;
            prev_underscore = false;
            uint64_t d = (uint64_t)(c - '0');
            if (acc > (UINT64_MAX - d) / 10ULL) {
                overflow = true;
            } else {
                acc = acc * 10ULL + d;
                if (acc > kMagLimit) overflow = true;
            }
            p++;
        } else if (c == '_') {
            if (!any_digit || prev_underscore) return fail(s);
            prev_underscore = true;
            p++;
        } else {
            break;
        }
    }
    if (prev_underscore) return fail(s);
    while (is_ws(*p)) p++;
    if (*p != '\0' || !any_digit) return fail(s);
    if (overflow) {
        snprintf(tls_msg, sizeof(tls_msg),
                 "OverflowError: int('%s') exceeds the 64-bit integer range", s);
        dragon_raise_exc_cstr(22, tls_msg);
        return 0;
    }
    if (sign < 0) return (int64_t)(0ULL - acc);
    return (int64_t)acc;
}

double dragon_str_to_float(const char* s) {
    if (!s || !dragon_str_is_ascii(s)) {
        dragon_raise_exc_cstr(90, "ValueError: could not convert string to float");
    }
    const char* p = s;
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r' ||
           *p == '\f' || *p == '\v') p++;
    char* end = nullptr;
    double v = strtod(p, &end);
    while (end && (*end == ' ' || *end == '\t' || *end == '\n' || *end == '\r' ||
                   *end == '\f' || *end == '\v')) end++;
    if (end == p || (end && *end)) {
        char msg[128];
        snprintf(msg, sizeof(msg),
                 "ValueError: could not convert string to float: '%s'", s);
        dragon_raise_exc_cstr(90, msg);
    }
    return v;
}

struct DragonCharSingleton {
    DragonObjectHeader header;
    int64_t len;
    int32_t nbytes;
    int32_t cap;
    char    data[8];
};
static_assert(offsetof(DragonCharSingleton, data) == offsetof(DragonString, data),
              "a one-character singleton must present the string layout");
static_assert(offsetof(DragonCharSingleton, nbytes) == offsetof(DragonString, nbytes),
              "a one-character singleton must present the string layout");

static DragonCharSingleton dragon_ascii_chars[128];

__attribute__((constructor))
static void dragon_ascii_chars_init(void) {
    for (int c = 1; c < 128; ++c) {
        DragonCharSingleton& s = dragon_ascii_chars[c];
        s.header.refcount = DRAGON_IMMORTAL_REFCOUNT;
        s.header.type_tag = DRAGON_TAG_STR;
        s.header.gc_flags = GC_FLAG_HEAP_OBJ | GC_FLAG_STR_ASCII | GC_FLAG_STR_UTF8;
        s.header.class_id = 0;
        s.header.gc_track_idx = -1;
        s.len = 1;
        s.nbytes = 1;
        s.cap = 1;
        s.data[0] = (char)c;
        s.data[1] = '\0';
    }
}

const char* dragon_str_ascii_char(uint32_t cp) {
    return dragon_ascii_chars[cp].data;
}

static inline bool dragon_cp_has_singleton(uint32_t cp) {
    return cp != 0 && cp < 0x80;
}

const char* dragon_str_from_cp(uint32_t cp) {
    if (dragon_cp_has_singleton(cp)) return dragon_str_ascii_char(cp);
    char buf[4];
    int n = dragon_utf8_encode(cp, buf);
    if (cp < 0x80) {
        DragonString* s = dragon_string_alloc_ascii(1);
        s->data[0] = buf[0];
        return s->data;
    }
    return dragon_string_from_utf8(buf, n);
}

const char* dragon_str_index(const char* s, int64_t index) {
    if (!s) {
        dragon_raise_exc_cstr(41, "IndexError: string index out of range");
    }
    DragonString* ds = dragon_is_heap_string(s) ? dragon_string_from_data(s) : NULL;
    int64_t cp_count = dragon_str_cp_count(s, ds);
    if (index < 0) index += cp_count;
    if (index < 0 || index >= cp_count) {
        dragon_raise_exc_cstr(41, "IndexError: string index out of range");
    }
    return dragon_str_from_cp(dragon_str_cp_at(s, ds, index));
}

const char* dragon_bool_to_str(int64_t value) {
    return value ? dragon_string_alloc("True", 4) : dragon_string_alloc("False", 5);
}

}
