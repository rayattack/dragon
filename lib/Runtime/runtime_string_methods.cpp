#include "runtime_internal.h"
#include "runtime_string_shared.h"

extern "C" {

const char* dragon_str_slice(const char* s, int64_t start, int64_t stop, int64_t step);
const char* dragon_str_from_cp(uint32_t cp);

typedef struct {
    const char* s;
    DragonString* ds;
    int64_t nbytes;
    int64_t cps;
} DragonStrView;

static inline DragonStrView dragon_str_view(const char* s) {
    DragonStrView v;
    v.s = s ? s : "";
    v.ds = (s && dragon_is_heap_string(s)) ? dragon_string_from_data(s) : NULL;
    v.nbytes = v.ds ? v.ds->nbytes : (int64_t)strlen(v.s);
    v.cps = v.ds ? v.ds->len : -1;
    return v;
}

static inline int64_t dragon_view_cps(DragonStrView* v) {
    if (v->cps < 0) v->cps = dragon_str_cp_count(v->s, v->ds);
    return v->cps;
}

static inline int dragon_view_is_ascii(const DragonStrView* v) {
    if (v->ds) return dragon_str_ascii_flag(v->ds);
    return dragon_ascii_prefix((const unsigned char*)v->s, v->nbytes) == v->nbytes;
}

static inline const char* dragon_str_from_range(DragonStrView* v, int64_t byte_start,
                                                int64_t byte_stop) {
    if (byte_stop <= byte_start) return dragon_string_alloc_ascii(0)->data;
    return dragon_string_from_utf8(v->s + byte_start, byte_stop - byte_start);
}

static inline const char* dragon_str_copy_view(DragonStrView* v) {
    return dragon_str_from_range(v, 0, v->nbytes);
}

typedef struct {
    DragonString* s;
    char* w;
} DragonStrOut;

static inline DragonStrOut dragon_str_out_begin(int64_t max_nbytes) {
    DragonStrOut o;
    o.s = dragon_string_alloc_utf8_raw(max_nbytes);
    o.w = o.s->data;
    return o;
}

static inline void dragon_str_out_cp(DragonStrOut* o, uint32_t cp) {
    o->w += dragon_utf8_encode(cp, o->w);
}

static inline void dragon_str_out_bytes(DragonStrOut* o, const char* p, int64_t n) {
    memcpy(o->w, p, (size_t)n);
    o->w += n;
}

static inline void dragon_str_out_fill(DragonStrOut* o, char c, int64_t n) {
    memset(o->w, c, (size_t)n);
    o->w += n;
}

static inline const char* dragon_str_out_finish(DragonStrOut* o) {
    dragon_string_finish_utf8(o->s, o->w - o->s->data);
    return o->s->data;
}

static inline uint32_t cp_ascii_upper(uint32_t cp) {
    if (cp < 0x80) return (cp >= 'a' && cp <= 'z') ? cp - 32 : cp;
    return dragon_cp_simple_upper(cp);
}
static inline uint32_t cp_ascii_lower(uint32_t cp) {
    if (cp < 0x80) return (cp >= 'A' && cp <= 'Z') ? cp + 32 : cp;
    return dragon_cp_simple_lower(cp);
}
static inline uint32_t cp_ascii_swapcase(uint32_t cp) {
    uint32_t up = cp_ascii_upper(cp);
    if (up != cp) return up;
    return cp_ascii_lower(cp);
}

static inline int dragon_cp_is_strip_ws(uint32_t cp) {
    return cp == ' ' || cp == '\t' || cp == '\n' || cp == '\r';
}

static const char* dragon_str_fold_ascii(DragonStrView* v, int upper) {
    DragonString* out = dragon_string_alloc_ascii(v->nbytes);
    const unsigned char* src = (const unsigned char*)v->s;
    unsigned char* dst = (unsigned char*)out->data;
    for (int64_t i = 0; i < v->nbytes; ++i) {
        unsigned char c = src[i];
        unsigned char lower = (unsigned char)(c + (unsigned char)(32 * (c >= 'A' && c <= 'Z')));
        unsigned char upperc = (unsigned char)(c - (unsigned char)(32 * (c >= 'a' && c <= 'z')));
        dst[i] = upper ? upperc : lower;
    }
    return out->data;
}

static const char* dragon_str_map_cp(const char* s, uint32_t (*xform)(uint32_t)) {
    DragonStrView v = dragon_str_view(s);
    if (dragon_view_is_ascii(&v)) {
        if (xform == cp_ascii_lower) return dragon_str_fold_ascii(&v, 0);
        if (xform == cp_ascii_upper) return dragon_str_fold_ascii(&v, 1);
    }
    DragonStrOut o = dragon_str_out_begin(v.nbytes * 2);
    DragonStrIter it = dragon_str_iter(v.s, v.ds);
    uint32_t cp;
    while (dragon_str_iter_next(&it, &cp)) dragon_str_out_cp(&o, xform(cp));
    return dragon_str_out_finish(&o);
}

const char* dragon_str_upper(const char* s) { return dragon_str_map_cp(s, cp_ascii_upper); }
const char* dragon_str_lower(const char* s) { return dragon_str_map_cp(s, cp_ascii_lower); }

static int64_t dragon_str_leading_bytes(DragonStrView* v, int (*pred)(uint32_t, void*), void* ctx) {
    const unsigned char* base = (const unsigned char*)v->s;
    const unsigned char* end = base + v->nbytes;
    const unsigned char* q = base;
    while (q < end) {
        uint32_t cp;
        int adv = dragon_utf8_next(q, end, &cp);
        if (!pred(cp, ctx)) break;
        q += adv;
    }
    return q - base;
}

static int64_t dragon_str_trailing_start(DragonStrView* v, int64_t from_byte,
                                         int (*pred)(uint32_t, void*), void* ctx) {
    const unsigned char* base = (const unsigned char*)v->s;
    const unsigned char* end = base + v->nbytes;
    const unsigned char* q = base + from_byte;
    int64_t keep_end = from_byte;
    while (q < end) {
        uint32_t cp;
        int adv = dragon_utf8_next(q, end, &cp);
        q += adv;
        if (!pred(cp, ctx)) keep_end = q - base;
    }
    return keep_end;
}

static int dragon_pred_strip_ws(uint32_t cp, void*) { return dragon_cp_is_strip_ws(cp); }

static const char* dragon_str_strip_impl(const char* s, int do_left, int do_right,
                                         int (*pred)(uint32_t, void*), void* ctx) {
    if (!s) return dragon_string_alloc("", 0);
    DragonStrView v = dragon_str_view(s);
    int64_t start = do_left ? dragon_str_leading_bytes(&v, pred, ctx) : 0;
    int64_t stop = do_right ? dragon_str_trailing_start(&v, start, pred, ctx) : v.nbytes;
    return dragon_str_from_range(&v, start, stop);
}

const char* dragon_str_strip(const char* s)  { return dragon_str_strip_impl(s, 1, 1, dragon_pred_strip_ws, NULL); }
const char* dragon_str_lstrip(const char* s) { return dragon_str_strip_impl(s, 1, 0, dragon_pred_strip_ws, NULL); }
const char* dragon_str_rstrip(const char* s) { return dragon_str_strip_impl(s, 0, 1, dragon_pred_strip_ws, NULL); }

static int dragon_pred_in_set(uint32_t cp, void* ctx) {
    DragonStrView* set = (DragonStrView*)ctx;
    DragonStrIter it = dragon_str_iter(set->s, set->ds);
    uint32_t c;
    while (dragon_str_iter_next(&it, &c))
        if (c == cp) return 1;
    return 0;
}

const char* dragon_str_strip_chars(const char* s, const char* chars) {
    if (!s) return dragon_string_alloc("", 0);
    if (!chars) return dragon_str_strip(s);
    DragonStrView set = dragon_str_view(chars);
    return dragon_str_strip_impl(s, 1, 1, dragon_pred_in_set, &set);
}

const char* dragon_str_lstrip_chars(const char* s, const char* chars) {
    if (!s) return dragon_string_alloc("", 0);
    if (!chars) return dragon_str_lstrip(s);
    DragonStrView set = dragon_str_view(chars);
    return dragon_str_strip_impl(s, 1, 0, dragon_pred_in_set, &set);
}

const char* dragon_str_rstrip_chars(const char* s, const char* chars) {
    if (!s) return dragon_string_alloc("", 0);
    if (!chars) return dragon_str_rstrip(s);
    DragonStrView set = dragon_str_view(chars);
    return dragon_str_strip_impl(s, 0, 1, dragon_pred_in_set, &set);
}

const char* dragon_str_title(const char* s) {
    if (!s) return dragon_string_alloc("", 0);
    DragonStrView v = dragon_str_view(s);
    DragonStrOut o = dragon_str_out_begin(v.nbytes * 2);
    DragonStrIter it = dragon_str_iter(v.s, v.ds);
    uint32_t cp;
    int cap = 1;
    while (dragon_str_iter_next(&it, &cp)) {
        int boundary = cp == ' ' || cp == '\t' || cp == '\n';
        uint32_t out = boundary ? cp : (cap ? cp_ascii_upper(cp) : cp_ascii_lower(cp));
        cap = boundary;
        dragon_str_out_cp(&o, out);
    }
    return dragon_str_out_finish(&o);
}

const char* dragon_str_capitalize(const char* s) {
    if (!s) return dragon_string_alloc("", 0);
    DragonStrView v = dragon_str_view(s);
    DragonStrOut o = dragon_str_out_begin(v.nbytes * 2);
    DragonStrIter it = dragon_str_iter(v.s, v.ds);
    uint32_t cp;
    int first = 1;
    while (dragon_str_iter_next(&it, &cp)) {
        dragon_str_out_cp(&o, first ? cp_ascii_upper(cp) : cp_ascii_lower(cp));
        first = 0;
    }
    return dragon_str_out_finish(&o);
}

const char* dragon_str_swapcase(const char* s) {
    return dragon_str_map_cp(s, cp_ascii_swapcase);
}

const char* dragon_str_casefold(const char* s) {
    return dragon_str_lower(s);
}

static const char* dragon_memmem(const char* hay, size_t hlen,
                                 const char* needle, size_t nlen) {
    if (nlen == 0) return hay;
    if (nlen > hlen) return NULL;
#if defined(__linux__) || defined(__APPLE__)
    return (const char*)memmem(hay, hlen, needle, nlen);
#else
    const char first = needle[0];
    const char* p = hay;
    size_t remaining = hlen;
    while (remaining >= nlen) {
        const char* c = (const char*)memchr(p, first, remaining - nlen + 1);
        if (!c) return NULL;
        if (memcmp(c, needle, nlen) == 0) return c;
        remaining -= (size_t)(c - p) + 1;
        p = c + 1;
    }
    return NULL;
#endif
}

static int64_t dragon_find_bytes(DragonStrView* h, DragonStrView* n,
                                 int64_t from_byte, int64_t end_byte) {
    int64_t pos = from_byte;
    while (pos + n->nbytes <= end_byte) {
        const char* hit = dragon_memmem(h->s + pos, (size_t)(end_byte - pos),
                                        n->s, (size_t)n->nbytes);
        if (!hit) return -1;
        int64_t off = hit - h->s;
        if (dragon_str_offset_is_boundary(h->s, h->ds, off)) return off;
        pos = off + 1;
    }
    return -1;
}

static int64_t dragon_str_find_impl(const char* haystack, const char* needle,
                                    int64_t start, int64_t end, int reverse) {
    if (!haystack || !needle) return -1;
    DragonStrView h = dragon_str_view(haystack);
    DragonStrView n = dragon_str_view(needle);
    int64_t hlen = dragon_view_cps(&h);
    if (start < 0) start = 0;
    if (end < 0 || end > hlen) end = hlen;
    if (start > end) return -1;
    int64_t nlen = dragon_view_cps(&n);
    if (nlen == 0) return reverse ? end : start;
    if (end - start < nlen) return -1;
    int64_t from_byte = dragon_str_cp_offset(h.s, h.ds, start);
    int64_t end_byte = dragon_str_cp_offset(h.s, h.ds, end);
    int64_t found = -1;
    int64_t pos = from_byte;
    for (;;) {
        int64_t off = dragon_find_bytes(&h, &n, pos, end_byte);
        if (off < 0) break;
        found = off;
        if (!reverse) break;
        pos = off + 1;
    }
    return found < 0 ? -1 : dragon_str_cp_index_of_offset(h.s, h.ds, found);
}

int64_t dragon_str_find(const char* s, const char* sub) {
    return dragon_str_find_impl(s, sub, 0, -1, 0);
}

int64_t dragon_str_find_se(const char* s, const char* sub, int64_t start, int64_t end) {
    return dragon_str_find_impl(s, sub, start, end, 0);
}

int64_t dragon_str_rfind(const char* s, const char* sub) {
    return dragon_str_find_impl(s, sub, 0, -1, 1);
}

int64_t dragon_str_rfind_se(const char* s, const char* sub, int64_t start, int64_t end) {
    return dragon_str_find_impl(s, sub, start, end, 1);
}

int64_t dragon_str_index_of(const char* s, const char* sub) {
    int64_t r = dragon_str_find(s, sub);
    if (r < 0) {
        dragon_raise_exc_cstr(90, "ValueError: substring not found");
    }
    return r;
}

int64_t dragon_str_rindex(const char* s, const char* sub) {
    int64_t r = dragon_str_rfind(s, sub);
    if (r < 0) {
        dragon_raise_exc_cstr(90, "ValueError: substring not found");
    }
    return r;
}

static int64_t dragon_str_count_impl(const char* s, const char* sub, int64_t start, int64_t end) {
    if (!s || !sub) return 0;
    DragonStrView h = dragon_str_view(s);
    DragonStrView n = dragon_str_view(sub);
    if (n.nbytes == 0) return 0;
    int64_t hlen = dragon_view_cps(&h);
    if (start < 0) start = 0;
    if (end < 0 || end > hlen) end = hlen;
    if (start > end) return 0;
    int64_t pos = dragon_str_cp_offset(h.s, h.ds, start);
    int64_t end_byte = dragon_str_cp_offset(h.s, h.ds, end);
    int64_t c = 0;
    for (;;) {
        int64_t off = dragon_find_bytes(&h, &n, pos, end_byte);
        if (off < 0) break;
        c++;
        pos = off + n.nbytes;
    }
    return c;
}

int64_t dragon_str_count(const char* s, const char* sub) {
    return dragon_str_count_impl(s, sub, 0, -1);
}

int64_t dragon_str_count_se(const char* s, const char* sub, int64_t start, int64_t end) {
    return dragon_str_count_impl(s, sub, start, end);
}

const char* dragon_str_replace_n(const char* s, const char* old_s, const char* new_s, int64_t max_count) {
    if (!s) return dragon_string_alloc("", 0);
    DragonStrView h = dragon_str_view(s);
    DragonStrView o = dragon_str_view(old_s);
    DragonStrView n = dragon_str_view(new_s);
    if (o.nbytes == 0 || max_count == 0) return dragon_str_copy_view(&h);
    int64_t count = 0;
    int64_t pos = 0;
    while (max_count < 0 || count < max_count) {
        int64_t off = dragon_find_bytes(&h, &o, pos, h.nbytes);
        if (off < 0) break;
        count++;
        pos = off + o.nbytes;
    }
    if (count == 0) return dragon_str_copy_view(&h);
    if (n.nbytes > o.nbytes && count > (DRAGON_STR_MAX_BYTES - h.nbytes) / (n.nbytes - o.nbytes)) {
        dragon_raise_exc_cstr(43, "MemoryError: string too large");
    }
    int64_t rlen = h.nbytes + count * (n.nbytes - o.nbytes);
    DragonStrOut out = dragon_str_out_begin(rlen);
    pos = 0;
    int64_t done = 0;
    while (done < count) {
        int64_t off = dragon_find_bytes(&h, &o, pos, h.nbytes);
        dragon_str_out_bytes(&out, h.s + pos, off - pos);
        dragon_str_out_bytes(&out, n.s, n.nbytes);
        pos = off + o.nbytes;
        done++;
    }
    dragon_str_out_bytes(&out, h.s + pos, h.nbytes - pos);
    return dragon_str_out_finish(&out);
}

const char* dragon_str_replace(const char* s, const char* old_s, const char* new_s) {
    return dragon_str_replace_n(s, old_s, new_s, -1);
}

const char* dragon_str_repeat(const char* s, int64_t n) {
    if (!s || n <= 0) return dragon_string_alloc("", 0);
    DragonStrView v = dragon_str_view(s);
    if (v.nbytes > 0 && (uint64_t)n > (uint64_t)DRAGON_STR_MAX_BYTES / (uint64_t)v.nbytes) {
        dragon_raise_exc_cstr(43, "MemoryError: string repeat too large");
    }
    int64_t total = v.nbytes * n;
    DragonStrOut out = dragon_str_out_begin(total);
    for (int64_t i = 0; i < n; i++) dragon_str_out_bytes(&out, v.s, v.nbytes);
    return dragon_str_out_finish(&out);
}

const char* dragon_str_removeprefix(const char* s, const char* prefix) {
    if (!s) return dragon_string_alloc("", 0);
    DragonStrView v = dragon_str_view(s);
    if (!prefix) return dragon_str_copy_view(&v);
    DragonStrView p = dragon_str_view(prefix);
    bool has = p.nbytes <= v.nbytes && memcmp(v.s, p.s, (size_t)p.nbytes) == 0 &&
               dragon_str_offset_is_boundary(v.s, v.ds, p.nbytes);
    return dragon_str_from_range(&v, has ? p.nbytes : 0, v.nbytes);
}

const char* dragon_str_removesuffix(const char* s, const char* suffix) {
    if (!s) return dragon_string_alloc("", 0);
    DragonStrView v = dragon_str_view(s);
    if (!suffix) return dragon_str_copy_view(&v);
    DragonStrView x = dragon_str_view(suffix);
    int64_t off = v.nbytes - x.nbytes;
    bool has = x.nbytes <= v.nbytes && memcmp(v.s + off, x.s, (size_t)x.nbytes) == 0 &&
               dragon_str_offset_is_boundary(v.s, v.ds, off);
    return dragon_str_from_range(&v, 0, has ? off : v.nbytes);
}

static constexpr int64_t DRAGON_STR_MAX_WIDTH = 1LL << 28;

static const char* dragon_str_justify(const char* s, int64_t w, char fill, int mode) {
    if (!s) return dragon_string_alloc("", 0);
    if (w > DRAGON_STR_MAX_WIDTH) {
        dragon_raise_exc_cstr(22, "OverflowError: width too large");
        return dragon_string_alloc("", 0);
    }
    DragonStrView v = dragon_str_view(s);
    int64_t len = dragon_view_cps(&v);
    if (len >= w) return dragon_str_copy_view(&v);
    int64_t pad = w - len;
    int64_t left = mode == 0 ? pad / 2 : (mode == 1 ? 0 : pad);
    int64_t right = pad - left;
    DragonStrOut out = dragon_str_out_begin(v.nbytes + pad);
    dragon_str_out_fill(&out, fill, left);
    dragon_str_out_bytes(&out, v.s, v.nbytes);
    dragon_str_out_fill(&out, fill, right);
    return dragon_str_out_finish(&out);
}

const char* dragon_str_center(const char* s, int64_t w, char fill) { return dragon_str_justify(s, w, fill, 0); }
const char* dragon_str_ljust(const char* s, int64_t w, char fill)  { return dragon_str_justify(s, w, fill, 1); }
const char* dragon_str_rjust(const char* s, int64_t w, char fill)  { return dragon_str_justify(s, w, fill, 2); }

const char* dragon_str_zfill(const char* s, int64_t w) {
    if (!s) return dragon_string_alloc("", 0);
    if (w > DRAGON_STR_MAX_WIDTH) {
        dragon_raise_exc_cstr(22, "OverflowError: width too large");
        return dragon_string_alloc("", 0);
    }
    DragonStrView v = dragon_str_view(s);
    int64_t n = dragon_view_cps(&v);
    if (n >= w) return dragon_str_copy_view(&v);
    int64_t sign = (v.nbytes > 0 && (v.s[0] == '+' || v.s[0] == '-')) ? 1 : 0;
    DragonStrOut out = dragon_str_out_begin(v.nbytes + (w - n));
    dragon_str_out_bytes(&out, v.s, sign);
    dragon_str_out_fill(&out, '0', w - n);
    dragon_str_out_bytes(&out, v.s + sign, v.nbytes - sign);
    return dragon_str_out_finish(&out);
}

const char* dragon_str_expandtabs(const char* s, int64_t tabsize) {
    if (!s) return dragon_string_alloc("", 0);
    if (tabsize <= 0) tabsize = 8;
    DragonStrView v = dragon_str_view(s);
    int64_t count = 0;
    for (int64_t i = 0; i < v.nbytes; i++) count += v.s[i] == '\t';
    int64_t budget = DRAGON_STR_MAX_WIDTH - v.nbytes;
    if (budget < 0 || (count != 0 && tabsize > budget / count)) {
        dragon_raise_exc_cstr(22, "OverflowError: expandtabs result too large");
        return dragon_string_alloc("", 0);
    }
    DragonStrOut out = dragon_str_out_begin(v.nbytes + count * tabsize);
    DragonStrIter it = dragon_str_iter(v.s, v.ds);
    uint32_t cp;
    int64_t col = 0;
    while (dragon_str_iter_next(&it, &cp)) {
        if (cp == '\t') {
            int64_t spaces = tabsize - (col % tabsize);
            dragon_str_out_fill(&out, ' ', spaces);
            col += spaces;
            continue;
        }
        dragon_str_out_cp(&out, cp);
        col = cp == '\n' ? 0 : col + 1;
    }
    return dragon_str_out_finish(&out);
}

int64_t dragon_str_startswith(const char* s, const char* prefix) {
    if (!s || !prefix) return 0;
    DragonStrView v = dragon_str_view(s);
    DragonStrView p = dragon_str_view(prefix);
    if (p.nbytes > v.nbytes) return 0;
    return memcmp(v.s, p.s, (size_t)p.nbytes) == 0 &&
           dragon_str_offset_is_boundary(v.s, v.ds, p.nbytes);
}

int64_t dragon_str_endswith(const char* s, const char* suffix) {
    if (!s || !suffix) return 0;
    DragonStrView v = dragon_str_view(s);
    DragonStrView x = dragon_str_view(suffix);
    if (x.nbytes > v.nbytes) return 0;
    int64_t off = v.nbytes - x.nbytes;
    return memcmp(v.s + off, x.s, (size_t)x.nbytes) == 0 &&
           dragon_str_offset_is_boundary(v.s, v.ds, off);
}

int64_t dragon_str_contains(const char* s, const char* sub) {
    return dragon_str_find(s, sub) >= 0 ? 1 : 0;
}

int64_t dragon_str_kind(const char* s) {
    return dragon_str_is_ascii(s) ? 1 : 4;
}

void dragon_str_index_error(void) {
    dragon_raise_exc_cstr(41, "IndexError: string index out of range");
}

int64_t dragon_cp_isdigit(int64_t cp) {
    return dragon_cp_is_digit((uint32_t)cp) ? 1 : 0;
}

int64_t dragon_cp_isalpha(int64_t cp) {
    return dragon_cp_is_alpha((uint32_t)cp) ? 1 : 0;
}

int64_t dragon_cp_isalnum(int64_t cp) {
    return (dragon_cp_is_alpha((uint32_t)cp) || dragon_cp_is_digit((uint32_t)cp)) ? 1 : 0;
}

int64_t dragon_cp_isspace(int64_t cp) {
    return dragon_cp_is_space((uint32_t)cp) ? 1 : 0;
}

static int64_t dragon_str_all_cps(const char* s, int (*pred)(uint32_t)) {
    if (!s) return 0;
    DragonStrView v = dragon_str_view(s);
    if (v.nbytes == 0) return 0;
    DragonStrIter it = dragon_str_iter(v.s, v.ds);
    uint32_t cp;
    while (dragon_str_iter_next(&it, &cp))
        if (!pred(cp)) return 0;
    return 1;
}

static int dragon_cp_is_alnum(uint32_t cp) {
    return dragon_cp_is_alpha(cp) || dragon_cp_is_digit(cp);
}

int64_t dragon_str_isdigit(const char* s) { return dragon_str_all_cps(s, dragon_cp_is_digit); }
int64_t dragon_str_isalpha(const char* s) { return dragon_str_all_cps(s, dragon_cp_is_alpha); }
int64_t dragon_str_isalnum(const char* s) { return dragon_str_all_cps(s, dragon_cp_is_alnum); }
int64_t dragon_str_isspace(const char* s) { return dragon_str_all_cps(s, dragon_cp_is_space); }

static int64_t dragon_str_cased_all(const char* s, int (*is_wanted)(uint32_t),
                                    int (*is_other)(uint32_t)) {
    if (!s) return 0;
    DragonStrView v = dragon_str_view(s);
    if (v.nbytes == 0) return 0;
    DragonStrIter it = dragon_str_iter(v.s, v.ds);
    uint32_t cp;
    int has = 0;
    while (dragon_str_iter_next(&it, &cp)) {
        if (is_other(cp)) return 0;
        if (is_wanted(cp)) has = 1;
    }
    return has;
}

int64_t dragon_str_isupper(const char* s) { return dragon_str_cased_all(s, dragon_cp_is_upper, dragon_cp_is_lower); }
int64_t dragon_str_islower(const char* s) { return dragon_str_cased_all(s, dragon_cp_is_lower, dragon_cp_is_upper); }

int64_t dragon_str_istitle(const char* s) {
    if (!s) return 0;
    DragonStrView v = dragon_str_view(s);
    if (v.nbytes == 0) return 0;
    DragonStrIter it = dragon_str_iter(v.s, v.ds);
    uint32_t cp;
    int prev_cased = 0, has_cased = 0;
    while (dragon_str_iter_next(&it, &cp)) {
        int upper = dragon_cp_is_upper(cp);
        int lower = dragon_cp_is_lower(cp);
        if ((upper && prev_cased) || (lower && !prev_cased)) return 0;
        prev_cased = upper || lower;
        has_cased |= prev_cased;
    }
    return has_cased;
}

int64_t dragon_str_isascii(const char* s) {
    return dragon_str_is_ascii(s);
}

int64_t dragon_str_isdecimal(const char* s) { return dragon_str_isdigit(s); }
int64_t dragon_str_isnumeric(const char* s) { return dragon_str_isdigit(s); }

static int dragon_cp_is_printable(uint32_t cp) {
    return cp >= 32 && cp != 127;
}

int64_t dragon_str_isprintable(const char* s) {
    if (!s) return 1;
    DragonStrView v = dragon_str_view(s);
    if (v.nbytes == 0) return 1;
    return dragon_str_all_cps(s, dragon_cp_is_printable);
}

static int dragon_cp_is_ident_tail(uint32_t cp) {
    return cp == '_' || (cp >= 'a' && cp <= 'z') || (cp >= 'A' && cp <= 'Z') ||
           (cp >= '0' && cp <= '9');
}

int64_t dragon_str_isidentifier(const char* s) {
    if (!s) return 0;
    DragonStrView v = dragon_str_view(s);
    if (v.nbytes == 0) return 0;
    if (v.s[0] >= '0' && v.s[0] <= '9') return 0;
    return dragon_str_all_cps(s, dragon_cp_is_ident_tail);
}

#define DRAGON_SLICE_NONE (-9223372036854775807LL - 1)

void dragon_slice_indices(int64_t len, int64_t* start, int64_t* stop, int64_t step) {
    if (*start == DRAGON_SLICE_NONE) *start = (step < 0) ? len - 1 : 0;
    else if (*start < 0) { *start += len; if (*start < 0) *start = (step < 0) ? -1 : 0; }
    else if (*start >= len) *start = (step < 0) ? len - 1 : len;
    if (*stop == DRAGON_SLICE_NONE) *stop = (step < 0) ? -1 : len;
    else if (*stop < 0) { *stop += len; if (*stop < 0) *stop = (step < 0) ? -1 : 0; }
    else if (*stop >= len) *stop = (step < 0) ? len : len;
}

static inline int64_t dragon_slice_count(int64_t start, int64_t stop, int64_t step) {
    if (step > 0) return stop > start ? (stop - start + step - 1) / step : 0;
    return start > stop ? (start - stop - step - 1) / (-step) : 0;
}

const char* dragon_str_slice(const char* s, int64_t start, int64_t stop, int64_t step) {
    if (!s) return dragon_string_alloc("", 0);
    if (step == 0) {
        dragon_raise_exc_cstr(90, "ValueError: slice step cannot be zero");
    }
    DragonStrView v = dragon_str_view(s);
    int64_t cp_count = dragon_view_cps(&v);
    dragon_slice_indices(cp_count, &start, &stop, step);
    if (step == 1) {
        if (stop <= start) return dragon_string_alloc_ascii(0)->data;
        return dragon_str_from_range(&v, dragon_str_cp_offset(v.s, v.ds, start),
                                     dragon_str_cp_offset(v.s, v.ds, stop));
    }
    int64_t out_count = dragon_slice_count(start, stop, step);
    if (out_count == 0) return dragon_string_alloc_ascii(0)->data;
    DragonStrOut out = dragon_str_out_begin(out_count * 4);
    for (int64_t k = 0; k < out_count; k++)
        dragon_str_out_cp(&out, dragon_str_cp_at(v.s, v.ds, start + k * step));
    return dragon_str_out_finish(&out);
}

DragonList* dragon_list_slice(DragonList* l, int64_t start, int64_t stop, int64_t step) {
    if (!l) return dragon_list_new(0);
    int64_t len = l->size;
    if (step == 0) {
        dragon_raise_exc_cstr(90, "ValueError: slice step cannot be zero");
    }
    dragon_slice_indices(len, &start, &stop, step);
    if (l->header.type_tag == DRAGON_TAG_LIST_BOX) {
        auto* src = (DragonListBox*)(void*)l;
        DragonListBox* boxed = dragon_list_box_new(8);
        auto take = [&](int64_t i) {
            DragonListBoxElem e = src->data[i];
            dragon_incref_tagged(e.payload, (uint8_t)e.tag);
            dragon_list_box_append(boxed, e.tag, e.payload);
        };
        if (step > 0) {
            for (int64_t i = start; i < stop; i += step) take(i);
        } else {
            for (int64_t i = start; i > stop; i += step) take(i);
        }
        return (DragonList*)(void*)boxed;
    }
    DragonList* r = dragon_list_new_tagged(8, l->elem_tag);
    if (step > 0) {
        for (int64_t i = start; i < stop; i += step) {
            int64_t v = dragon_list_load(l, i);
            dragon_incref_tagged(v, l->elem_tag);
            dragon_list_append(r, v);
        }
    } else {
        for (int64_t i = start; i > stop; i += step) {
            int64_t v = dragon_list_load(l, i);
            dragon_incref_tagged(v, l->elem_tag);
            dragon_list_append(r, v);
        }
    }
    return r;
}

static int64_t dragon_ws_run_end(DragonStrView* v, int64_t from, int want_ws) {
    const unsigned char* base = (const unsigned char*)v->s;
    const unsigned char* end = base + v->nbytes;
    const unsigned char* q = base + from;
    while (q < end) {
        uint32_t cp;
        int adv = dragon_utf8_next(q, end, &cp);
        if (dragon_cp_is_strip_ws(cp) != want_ws) break;
        q += adv;
    }
    return q - base;
}

static DragonList* dragon_str_split_ws(DragonStrView* v, int64_t maxsplit) {
    DragonList* l = dragon_list_new_tagged(8, TAG_STR);
    int64_t nsplits = 0;
    int64_t i = 0;
    while (i < v->nbytes) {
        i = dragon_ws_run_end(v, i, 1);
        if (i >= v->nbytes) break;
        int64_t word_end = (maxsplit >= 0 && nsplits >= maxsplit)
            ? v->nbytes : dragon_ws_run_end(v, i, 0);
        dragon_list_append(l, (int64_t)dragon_str_from_range(v, i, word_end));
        nsplits++;
        i = word_end;
    }
    return l;
}

DragonList* dragon_str_split_max(const char* s, const char* sep, int64_t maxsplit) {
    if (!s) return dragon_list_new_tagged(8, TAG_STR);
    DragonStrView v = dragon_str_view(s);
    DragonStrView p = dragon_str_view(sep);
    if (!sep || p.nbytes == 0) return dragon_str_split_ws(&v, maxsplit);
    DragonList* l = dragon_list_new_tagged(8, TAG_STR);
    int64_t pos = 0;
    int64_t nsplits = 0;
    for (;;) {
        int64_t f = (maxsplit >= 0 && nsplits >= maxsplit) ? -1
                  : dragon_find_bytes(&v, &p, pos, v.nbytes);
        if (f < 0) {
            dragon_list_append(l, (int64_t)dragon_str_from_range(&v, pos, v.nbytes));
            break;
        }
        dragon_list_append(l, (int64_t)dragon_str_from_range(&v, pos, f));
        pos = f + p.nbytes;
        nsplits++;
    }
    return l;
}

DragonList* dragon_str_split(const char* s, const char* sep) {
    return dragon_str_split_max(s, sep, -1);
}

static const char* dragon_join_utf8(const char* sep, const char** items, int64_t n) {
    if (n <= 0) return dragon_string_alloc("", 0);
    DragonStrView sv = dragon_str_view(sep);
    int64_t total = 0;
    for (int64_t i = 0; i < n; ++i) {
        int64_t add = dragon_str_total_bytes(items[i]) + (i > 0 ? sv.nbytes : 0);
        if (add > DRAGON_STR_MAX_BYTES - total)
            dragon_raise_exc_cstr(43, "MemoryError: string too large");
        total += add;
    }
    DragonStrOut out = dragon_str_out_begin(total);
    for (int64_t i = 0; i < n; ++i) {
        if (i > 0) dragon_str_out_bytes(&out, sv.s, sv.nbytes);
        if (items[i]) dragon_str_out_bytes(&out, items[i], dragon_str_total_bytes(items[i]));
    }
    return dragon_str_out_finish(&out);
}

const char* dragon_str_join_ptr(const char* sep, DragonListPtr* l) {
    if (!l || l->size == 0) return dragon_string_alloc("", 0);
    if (l->elem_tag != TAG_STR)
        dragon_raise_exc_cstr(80, "TypeError: join expects a list of str");
    int64_t n = l->size;
    const char** items = (const char**)dragon_xmalloc_n(n, sizeof(char*));
    for (int64_t i = 0; i < n; ++i) items[i] = (const char*)l->data[i];
    int32_t clbase = dragon_cleanup_depth();
    dragon_cleanup_push((int64_t)(uintptr_t)items, DCLEAN_FREE, 0);
    const char* r = dragon_join_utf8(sep, items, n);
    dragon_cleanup_reset(clbase);
    free((void*)items);
    return r;
}

const char* dragon_str_join(const char* sep, DragonList* l) {
    if (!l || l->size == 0) return dragon_string_alloc("", 0);
    int64_t n = l->size;
    const char** items = (const char**)dragon_xmalloc_n(n, sizeof(char*));
    for (int64_t i = 0; i < n; ++i) items[i] = (const char*)(uintptr_t)dragon_list_load(l, i);
    int32_t clbase = dragon_cleanup_depth();
    dragon_cleanup_push((int64_t)(uintptr_t)items, DCLEAN_FREE, 0);
    const char* r = dragon_join_utf8(sep, items, n);
    dragon_cleanup_reset(clbase);
    free((void*)items);
    return r;
}

DragonList* dragon_str_splitlines(const char* s) {
    DragonList* r = dragon_list_new_tagged(8, TAG_STR);
    if (!s) return r;
    DragonStrView v = dragon_str_view(s);
    int64_t start = 0;
    int64_t i = 0;
    while (i < v.nbytes) {
        char c = v.s[i];
        if (c != '\n' && c != '\r') { i++; continue; }
        dragon_list_append(r, (int64_t)dragon_str_from_range(&v, start, i));
        if (c == '\r' && i + 1 < v.nbytes && v.s[i + 1] == '\n') i++;
        i++;
        start = i;
    }
    if (start < v.nbytes)
        dragon_list_append(r, (int64_t)dragon_str_from_range(&v, start, v.nbytes));
    return r;
}

static DragonTuple* dragon_partition_result(DragonStrView* v, DragonStrView* p,
                                            int64_t at, int missing_left) {
    DragonTuple* r = dragon_tuple_new(3);
    if (at < 0) {
        const char* whole = dragon_str_copy_view(v);
        const char* empty_a = dragon_string_alloc("", 0);
        const char* empty_b = dragon_string_alloc("", 0);
        dragon_tuple_set_tagged(r, 0, (int64_t)(missing_left ? empty_a : whole), TAG_STR);
        dragon_tuple_set_tagged(r, 1, (int64_t)empty_b, TAG_STR);
        dragon_tuple_set_tagged(r, 2, (int64_t)(missing_left ? whole : empty_a), TAG_STR);
        return r;
    }
    dragon_tuple_set_tagged(r, 0, (int64_t)dragon_str_from_range(v, 0, at), TAG_STR);
    dragon_tuple_set_tagged(r, 1, (int64_t)dragon_str_copy_view(p), TAG_STR);
    dragon_tuple_set_tagged(r, 2, (int64_t)dragon_str_from_range(v, at + p->nbytes, v->nbytes), TAG_STR);
    return r;
}

DragonTuple* dragon_str_partition(const char* s, const char* sep) {
    DragonStrView v = dragon_str_view(s);
    DragonStrView p = dragon_str_view(sep);
    int64_t at = (!s || !sep || p.nbytes == 0) ? -1 : dragon_find_bytes(&v, &p, 0, v.nbytes);
    return dragon_partition_result(&v, &p, at, 0);
}

DragonTuple* dragon_str_rpartition(const char* s, const char* sep) {
    DragonStrView v = dragon_str_view(s);
    DragonStrView p = dragon_str_view(sep);
    int64_t last = -1;
    int64_t pos = 0;
    while (s && sep && p.nbytes > 0) {
        int64_t at = dragon_find_bytes(&v, &p, pos, v.nbytes);
        if (at < 0) break;
        last = at;
        pos = at + p.nbytes;
    }
    return dragon_partition_result(&v, &p, last, 1);
}

DragonList* dragon_str_rsplit(const char* s, const char* sep, int64_t maxsplit) {
    if (maxsplit < 0) return dragon_str_split(s, sep);
    DragonList* all = dragon_str_split(s, sep);
    if (maxsplit >= all->size - 1) return all;
    DragonList* result = dragon_list_new_tagged(maxsplit + 1, TAG_STR);
    DragonList* head = dragon_list_new_tagged(all->size - maxsplit, TAG_STR);
    for (int64_t i = 0; i < all->size - maxsplit; i++) {
        int64_t v = dragon_list_load(all, i);
        dragon_incref_str((const char*)(uintptr_t)v);
        dragon_list_append(head, v);
    }
    const char* joined = dragon_str_join(sep ? sep : " ", head);
    dragon_list_append(result, (int64_t)joined);
    for (int64_t i = all->size - maxsplit; i < all->size; i++) {
        int64_t v = dragon_list_load(all, i);
        dragon_incref_str((const char*)(uintptr_t)v);
        dragon_list_append(result, v);
    }
    dragon_decref(head);
    dragon_decref(all);
    return result;
}

}
