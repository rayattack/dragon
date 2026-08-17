#include "runtime_internal.h"
#include "runtime_string_shared.h"

extern "C" {

static int64_t dragon_str_find_cp(const char* haystack, const char* needle, int64_t start);

const char* dragon_str_slice(const char* s, int64_t start, int64_t stop, int64_t step);

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

static const char* dragon_str_strip_ws_impl(const char* s, int do_left, int do_right) {
    if (!s) return dragon_string_alloc("", 0);
    DragonString* ds = dragon_is_heap_string(s) ? dragon_string_from_data(s) : NULL;
    int64_t n = ds ? ds->len : (int64_t)strlen(s);
    int64_t start = 0, stop = n;
    if (do_left)  while (start < stop && dragon_cp_is_strip_ws(dragon_str_cp_at(s, ds, start))) start++;
    if (do_right) while (stop > start && dragon_cp_is_strip_ws(dragon_str_cp_at(s, ds, stop - 1))) stop--;
    return dragon_str_slice(s, start, stop, 1);
}

static const char* dragon_str_map_cp(const char* s, uint32_t (*xform)(uint32_t)) {
    if (!s) return dragon_string_alloc("", 0);
    DragonString* ds = dragon_is_heap_string(s) ? dragon_string_from_data(s) : NULL;
    int64_t n = ds ? ds->len : (int64_t)strlen(s);
    if (!ds || ds->kind == 1) {
        DragonString* out = dragon_string_alloc_raw(n);
        for (int64_t i = 0; i < n; ++i)
            out->data[i] = (char)xform((uint32_t)(unsigned char)s[i]);
        out->data[n] = '\0';
        return out->data;
    }
    const uint32_t* src = (const uint32_t*)ds->data;
    uint32_t max_cp = 0;
    DragonString* out = dragon_string_alloc_ucs4(n);
    uint32_t* dst = (uint32_t*)out->data;
    for (int64_t i = 0; i < n; ++i) {
        uint32_t cp = xform(src[i]);
        dst[i] = cp;
        if (cp > max_cp) max_cp = cp;
    }
    if (max_cp < 0x80) {
        DragonString* out1 = dragon_string_alloc_ascii(n);
        for (int64_t i = 0; i < n; ++i) out1->data[i] = (char)dst[i];
        free(out);
        return out1->data;
    }
    return out->data;
}

const char* dragon_str_upper(const char* s) { return dragon_str_map_cp(s, cp_ascii_upper); }
const char* dragon_str_lower(const char* s) { return dragon_str_map_cp(s, cp_ascii_lower); }

const char* dragon_str_strip(const char* s)  { return dragon_str_strip_ws_impl(s, 1, 1); }
const char* dragon_str_lstrip(const char* s) { return dragon_str_strip_ws_impl(s, 1, 0); }
const char* dragon_str_rstrip(const char* s) { return dragon_str_strip_ws_impl(s, 0, 1); }

static inline int dragon_cp_in_set(uint32_t cp, const char* set, DragonString* set_ds) {
    int64_t n = set_ds ? set_ds->len : (int64_t)strlen(set);
    for (int64_t i = 0; i < n; ++i)
        if (dragon_str_cp_at(set, set_ds, i) == cp) return 1;
    return 0;
}

static const char* dragon_str_strip_chars_impl(const char* s, const char* chars,
                                               int do_left, int do_right) {
    if (!s) return dragon_string_alloc("", 0);
    DragonString* ds = dragon_is_heap_string(s) ? dragon_string_from_data(s) : NULL;
    DragonString* cds = dragon_is_heap_string(chars) ? dragon_string_from_data(chars) : NULL;
    int64_t n = ds ? ds->len : (int64_t)strlen(s);
    int64_t start = 0, stop = n;
    if (do_left)  while (start < stop && dragon_cp_in_set(dragon_str_cp_at(s, ds, start), chars, cds)) start++;
    if (do_right) while (stop > start && dragon_cp_in_set(dragon_str_cp_at(s, ds, stop - 1), chars, cds)) stop--;
    return dragon_str_slice(s, start, stop, 1);
}

const char* dragon_str_strip_chars(const char* s, const char* chars) {
    if (!s) return dragon_string_alloc("", 0);
    if (!chars) return dragon_str_strip(s);
    return dragon_str_strip_chars_impl(s, chars, 1, 1);
}

const char* dragon_str_lstrip_chars(const char* s, const char* chars) {
    if (!s) return dragon_string_alloc("", 0);
    if (!chars) return dragon_str_lstrip(s);
    return dragon_str_strip_chars_impl(s, chars, 1, 0);
}

const char* dragon_str_rstrip_chars(const char* s, const char* chars) {
    if (!s) return dragon_string_alloc("", 0);
    if (!chars) return dragon_str_rstrip(s);
    return dragon_str_strip_chars_impl(s, chars, 0, 1);
}

static const char* dragon_str_finish_cps(DragonString* out, uint32_t* dst,
                                         int64_t n, uint32_t max_cp) {
    if (max_cp < 0x80) {
        DragonString* o1 = dragon_string_alloc_ascii(n);
        for (int64_t i = 0; i < n; ++i) o1->data[i] = (char)dst[i];
        free(out);
        return o1->data;
    }
    return out->data;
}

const char* dragon_str_title(const char* s) {
    if (!s) return dragon_string_alloc("", 0);
    DragonString* ds = dragon_is_heap_string(s) ? dragon_string_from_data(s) : NULL;
    int64_t n = ds ? ds->len : (int64_t)strlen(s);
    if (!ds || ds->kind == 1) {
        DragonString* out = dragon_string_alloc_raw(n);
        int cap = 1;
        for (int64_t i = 0; i < n; i++) {
            unsigned char c = (unsigned char)s[i];
            if (c == ' ' || c == '\t' || c == '\n') { out->data[i] = (char)c; cap = 1; }
            else if (cap) { out->data[i] = (char)((c >= 'a' && c <= 'z') ? c - 32 : c); cap = 0; }
            else { out->data[i] = (char)((c >= 'A' && c <= 'Z') ? c + 32 : c); }
        }
        out->data[n] = '\0';
        return out->data;
    }
    const uint32_t* src = (const uint32_t*)ds->data;
    DragonString* out = dragon_string_alloc_ucs4(n);
    uint32_t* dst = (uint32_t*)out->data, max_cp = 0;
    int cap = 1;
    for (int64_t i = 0; i < n; i++) {
        uint32_t cp = src[i], o;
        if (cp == ' ' || cp == '\t' || cp == '\n') { o = cp; cap = 1; }
        else if (cap) { o = cp_ascii_upper(cp); cap = 0; }
        else { o = cp_ascii_lower(cp); }
        dst[i] = o; if (o > max_cp) max_cp = o;
    }
    return dragon_str_finish_cps(out, dst, n, max_cp);
}

const char* dragon_str_capitalize(const char* s) {
    if (!s) return dragon_string_alloc("", 0);
    DragonString* ds = dragon_is_heap_string(s) ? dragon_string_from_data(s) : NULL;
    int64_t n = ds ? ds->len : (int64_t)strlen(s);
    if (!ds || ds->kind == 1) {
        DragonString* out = dragon_string_alloc_raw(n);
        for (int64_t i = 0; i < n; i++) {
            unsigned char c = (unsigned char)s[i];
            out->data[i] = (char)((c >= 'A' && c <= 'Z') ? c + 32 : c);
        }
        if (n > 0 && out->data[0] >= 'a' && out->data[0] <= 'z') out->data[0] -= 32;
        out->data[n] = '\0';
        return out->data;
    }
    const uint32_t* src = (const uint32_t*)ds->data;
    DragonString* out = dragon_string_alloc_ucs4(n);
    uint32_t* dst = (uint32_t*)out->data, max_cp = 0;
    for (int64_t i = 0; i < n; i++) {
        uint32_t o = (i == 0) ? cp_ascii_upper(src[i]) : cp_ascii_lower(src[i]);
        dst[i] = o; if (o > max_cp) max_cp = o;
    }
    return dragon_str_finish_cps(out, dst, n, max_cp);
}

const char* dragon_str_swapcase(const char* s) {
    return dragon_str_map_cp(s, cp_ascii_swapcase);
}

const char* dragon_str_casefold(const char* s) {
    return dragon_str_lower(s);
}

const char* dragon_str_replace_n(const char* s, const char* old_s, const char* new_s, int64_t max_count) {
    if (!s) return dragon_string_alloc("", 0);
    DragonString* ds = dragon_is_heap_string(s) ? dragon_string_from_data(s) : NULL;
    DragonString* dold = dragon_is_heap_string(old_s) ? dragon_string_from_data(old_s) : NULL;
    DragonString* dnew = dragon_is_heap_string(new_s) ? dragon_string_from_data(new_s) : NULL;
    int64_t slen = ds ? ds->len : (int64_t)strlen(s);
    int64_t olen = dold ? dold->len : (old_s ? (int64_t)strlen(old_s) : 0);
    int64_t nlen = dnew ? dnew->len : (new_s ? (int64_t)strlen(new_s) : 0);
    if (!old_s) old_s = "";
    if (!new_s) new_s = "";
    if (olen == 0 || max_count == 0) return dragon_str_slice(s, 0, slen, 1);

    bool s_k1 = (!ds || ds->kind == 1);
    bool o_k1 = (!dold || dold->kind == 1);
    bool n_k1 = (!dnew || dnew->kind == 1);

    if (s_k1 && o_k1 && n_k1) {
        int64_t count = 0;
        int64_t i = 0;
        while (i + olen <= slen) {
            const void* hit = memchr(s + i, (unsigned char)old_s[0], (size_t)(slen - i - olen + 1));
            if (!hit) break;
            int64_t j = (const char*)hit - s;
            if (memcmp(s + j, old_s, (size_t)olen) == 0) {
                count++;
                i = j + olen;
                if (max_count >= 0 && count >= max_count) break;
            } else {
                i = j + 1;
            }
        }
        if (nlen > olen && count > 0 && count > (INT64_MAX - slen) / (nlen - olen)) {
            dragon_raise_exc_cstr(43, "MemoryError: string too large");
        }
        int64_t rlen = slen + count * (nlen - olen);
        DragonString* out = dragon_string_alloc_raw(rlen);
        char* w = out->data;
        int64_t pos = 0;
        int64_t done = 0;
        while (pos <= slen) {
            int64_t f = -1;
            if (!(max_count >= 0 && done >= max_count)) {
                int64_t k = pos;
                while (k + olen <= slen) {
                    const void* hit = memchr(s + k, (unsigned char)old_s[0], (size_t)(slen - k - olen + 1));
                    if (!hit) break;
                    int64_t j = (const char*)hit - s;
                    if (memcmp(s + j, old_s, (size_t)olen) == 0) { f = j; break; }
                    k = j + 1;
                }
            }
            if (f < 0) {
                memcpy(w, s + pos, (size_t)(slen - pos)); w += slen - pos;
                break;
            }
            memcpy(w, s + pos, (size_t)(f - pos)); w += f - pos;
            memcpy(w, new_s, (size_t)nlen); w += nlen;
            pos = f + olen;
            done++;
        }
        *w = '\0';
        out->len = w - out->data;
        return out->data;
    }

    int64_t count = 0;
    int64_t pos = 0;
    while (true) {
        if (max_count >= 0 && count >= max_count) break;
        int64_t r = dragon_str_find_cp(s, old_s, pos);
        if (r < 0) break;
        count++;
        pos = r + olen;
    }
    if (nlen > olen && count > 0 && count > (INT64_MAX - slen) / (nlen - olen)) {
        dragon_raise_exc_cstr(43, "MemoryError: string too large");
    }
    int64_t rlen = slen + count * (nlen - olen);
    if (rlen < 0) rlen = 0;
    DragonString* out = dragon_string_alloc_ucs4(rlen);
    uint32_t* dst = (uint32_t*)out->data;
    int64_t w = 0;
    pos = 0;
    int64_t done = 0;
    uint32_t max_cp = 0;
    while (true) {
        int64_t r = (max_count >= 0 && done >= max_count) ? -1 : dragon_str_find_cp(s, old_s, pos);
        if (r < 0) {
            for (int64_t i = pos; i < slen; ++i) {
                uint32_t cp = dragon_str_cp_at(s, ds, i);
                dst[w++] = cp;
                if (cp > max_cp) max_cp = cp;
            }
            break;
        }
        for (int64_t i = pos; i < r; ++i) {
            uint32_t cp = dragon_str_cp_at(s, ds, i);
            dst[w++] = cp;
            if (cp > max_cp) max_cp = cp;
        }
        for (int64_t j = 0; j < nlen; ++j) {
            uint32_t cp = dragon_str_cp_at(new_s, dnew, j);
            dst[w++] = cp;
            if (cp > max_cp) max_cp = cp;
        }
        pos = r + olen;
        done++;
    }
    if (max_cp < 0x80) {
        DragonString* out1 = dragon_string_alloc_ascii(w);
        for (int64_t i = 0; i < w; ++i) out1->data[i] = (char)dst[i];
        free(out);
        return out1->data;
    }
    return out->data;
}

const char* dragon_str_replace(const char* s, const char* old_s, const char* new_s) {
    return dragon_str_replace_n(s, old_s, new_s, -1);
}

const char* dragon_str_repeat(const char* s, int64_t n) {
    if (!s || n <= 0) return dragon_string_alloc("", 0);
    DragonString* in = dragon_is_heap_string(s) ? dragon_string_from_data(s) : NULL;
    if (!in || in->kind == 1) {
        int64_t len = in ? in->len : (int64_t)strlen(s);
        if (len > 0 && (uint64_t)n > (uint64_t)INT64_MAX / (uint64_t)len) {
            dragon_raise_exc_cstr(43, "MemoryError: string repeat too large");
        }
        int64_t total = len * n;
        DragonString* ds = dragon_string_alloc_raw(total);
        for (int64_t i = 0; i < n; i++) memcpy(ds->data + i * len, s, (size_t)len);
        ds->data[total] = '\0';
        ds->len = total;
        return ds->data;
    }
    int64_t len = in->len;
    if (len > 0 && (uint64_t)n > (uint64_t)INT64_MAX / (uint64_t)len) {
        dragon_raise_exc_cstr(43, "MemoryError: string repeat too large");
    }
    int64_t total = len * n;
    DragonString* ds = dragon_string_alloc_ucs4(total);
    uint32_t* dst = (uint32_t*)ds->data;
    const uint32_t* src = (const uint32_t*)in->data;
    int64_t w = 0;
    for (int64_t i = 0; i < n; i++)
        for (int64_t j = 0; j < len; j++) dst[w++] = src[j];
    return ds->data;
}

const char* dragon_str_removeprefix(const char* s, const char* prefix) {
    if (!s) return dragon_string_alloc("", 0);
    DragonString* ds = dragon_is_heap_string(s) ? dragon_string_from_data(s) : NULL;
    int64_t n = ds ? ds->len : (int64_t)strlen(s);
    if (!prefix) return dragon_str_slice(s, 0, n, 1);
    DragonString* pds = dragon_is_heap_string(prefix) ? dragon_string_from_data(prefix) : NULL;
    int64_t pn = pds ? pds->len : (int64_t)strlen(prefix);
    if (pn <= n) {
        int64_t i = 0;
        for (; i < pn; i++)
            if (dragon_str_cp_at(s, ds, i) != dragon_str_cp_at(prefix, pds, i)) break;
        if (i == pn) return dragon_str_slice(s, pn, n, 1);
    }
    return dragon_str_slice(s, 0, n, 1);
}

const char* dragon_str_removesuffix(const char* s, const char* suffix) {
    if (!s) return dragon_string_alloc("", 0);
    DragonString* ds = dragon_is_heap_string(s) ? dragon_string_from_data(s) : NULL;
    int64_t n = ds ? ds->len : (int64_t)strlen(s);
    if (!suffix) return dragon_str_slice(s, 0, n, 1);
    DragonString* xds = dragon_is_heap_string(suffix) ? dragon_string_from_data(suffix) : NULL;
    int64_t xn = xds ? xds->len : (int64_t)strlen(suffix);
    if (xn <= n) {
        int64_t i = 0;
        for (; i < xn; i++)
            if (dragon_str_cp_at(s, ds, n - xn + i) != dragon_str_cp_at(suffix, xds, i)) break;
        if (i == xn) return dragon_str_slice(s, 0, n - xn, 1);
    }
    return dragon_str_slice(s, 0, n, 1);
}

static constexpr int64_t DRAGON_STR_MAX_WIDTH = 1LL << 28;

const char* dragon_str_center(const char* s, int64_t w, char fill) {
    if (!s) return dragon_string_alloc("", 0);
    if (w > DRAGON_STR_MAX_WIDTH) {
        dragon_raise_exc_cstr(22, "OverflowError: width too large");
        return dragon_string_alloc("", 0);
    }
    DragonString* in = dragon_is_heap_string(s) ? dragon_string_from_data(s) : NULL;
    int64_t len = in ? in->len : (int64_t)strlen(s);
    if (len >= w) return dragon_str_slice(s, 0, len, 1);
    int64_t pad = w - len, left = pad / 2, right = pad - left;
    if (!in || in->kind == 1) {
        DragonString* ds = dragon_string_alloc_raw(w);
        memset(ds->data, fill, (size_t)left);
        memcpy(ds->data + left, s, (size_t)len);
        memset(ds->data + left + len, fill, (size_t)right);
        ds->data[w] = '\0';
        ds->len = w;
        return ds->data;
    }
    DragonString* ds = dragon_string_alloc_ucs4(w);
    uint32_t* dst = (uint32_t*)ds->data;
    const uint32_t* src = (const uint32_t*)in->data;
    uint32_t fcp = (uint32_t)(unsigned char)fill;
    int64_t k = 0;
    for (int64_t i = 0; i < left; i++)  dst[k++] = fcp;
    for (int64_t i = 0; i < len; i++)   dst[k++] = src[i];
    for (int64_t i = 0; i < right; i++) dst[k++] = fcp;
    return ds->data;
}

const char* dragon_str_ljust(const char* s, int64_t w, char fill) {
    if (!s) return dragon_string_alloc("", 0);
    if (w > DRAGON_STR_MAX_WIDTH) {
        dragon_raise_exc_cstr(22, "OverflowError: width too large");
        return dragon_string_alloc("", 0);
    }
    DragonString* in = dragon_is_heap_string(s) ? dragon_string_from_data(s) : NULL;
    int64_t len = in ? in->len : (int64_t)strlen(s);
    if (len >= w) return dragon_str_slice(s, 0, len, 1);
    if (!in || in->kind == 1) {
        DragonString* ds = dragon_string_alloc_raw(w);
        memcpy(ds->data, s, (size_t)len);
        memset(ds->data + len, fill, (size_t)(w - len));
        ds->data[w] = '\0';
        ds->len = w;
        return ds->data;
    }
    DragonString* ds = dragon_string_alloc_ucs4(w);
    uint32_t* dst = (uint32_t*)ds->data;
    const uint32_t* src = (const uint32_t*)in->data;
    uint32_t fcp = (uint32_t)(unsigned char)fill;
    int64_t k = 0;
    for (int64_t i = 0; i < len; i++)     dst[k++] = src[i];
    for (int64_t i = 0; i < w - len; i++) dst[k++] = fcp;
    return ds->data;
}

const char* dragon_str_rjust(const char* s, int64_t w, char fill) {
    if (!s) return dragon_string_alloc("", 0);
    if (w > DRAGON_STR_MAX_WIDTH) {
        dragon_raise_exc_cstr(22, "OverflowError: width too large");
        return dragon_string_alloc("", 0);
    }
    DragonString* in = dragon_is_heap_string(s) ? dragon_string_from_data(s) : NULL;
    int64_t len = in ? in->len : (int64_t)strlen(s);
    if (len >= w) return dragon_str_slice(s, 0, len, 1);
    if (!in || in->kind == 1) {
        DragonString* ds = dragon_string_alloc_raw(w);
        memset(ds->data, fill, (size_t)(w - len));
        memcpy(ds->data + w - len, s, (size_t)len);
        ds->data[w] = '\0';
        ds->len = w;
        return ds->data;
    }
    DragonString* ds = dragon_string_alloc_ucs4(w);
    uint32_t* dst = (uint32_t*)ds->data;
    const uint32_t* src = (const uint32_t*)in->data;
    uint32_t fcp = (uint32_t)(unsigned char)fill;
    int64_t k = 0;
    for (int64_t i = 0; i < w - len; i++) dst[k++] = fcp;
    for (int64_t i = 0; i < len; i++)     dst[k++] = src[i];
    return ds->data;
}

const char* dragon_str_zfill(const char* s, int64_t w) {
    if (!s) return dragon_string_alloc("", 0);
    if (w > DRAGON_STR_MAX_WIDTH) {
        dragon_raise_exc_cstr(22, "OverflowError: width too large");
        return dragon_string_alloc("", 0);
    }
    DragonString* ds = dragon_is_heap_string(s) ? dragon_string_from_data(s) : NULL;
    int64_t n = ds ? ds->len : (int64_t)strlen(s);
    if (n >= w) return dragon_str_slice(s, 0, n, 1);
    if (!ds || ds->kind == 1) {
        DragonString* out = dragon_string_alloc_raw(w);
        int64_t off = 0;
        if (n > 0 && (s[0] == '+' || s[0] == '-')) { out->data[0] = s[0]; off = 1; }
        memset(out->data + off, '0', (size_t)(w - n));
        memcpy(out->data + (w - n) + off, s + off, (size_t)(n - off));
        out->data[w] = '\0';
        return out->data;
    }
    const uint32_t* src = (const uint32_t*)ds->data;
    DragonString* out = dragon_string_alloc_ucs4(w);
    uint32_t* dst = (uint32_t*)out->data, max_cp = 0;
    int64_t off = 0;
    if (n > 0 && (src[0] == '+' || src[0] == '-')) { dst[0] = src[0]; off = 1; }
    for (int64_t i = 0; i < w - n; i++) dst[off + i] = '0';
    for (int64_t i = off; i < n; i++) {
        uint32_t cp = src[i]; dst[(w - n) + i] = cp; if (cp > max_cp) max_cp = cp;
    }
    return dragon_str_finish_cps(out, dst, w, max_cp);
}

const char* dragon_str_expandtabs(const char* s, int64_t tabsize) {
    if (!s) return dragon_string_alloc("", 0);
    if (tabsize <= 0) tabsize = 8;
    DragonString* ds = dragon_is_heap_string(s) ? dragon_string_from_data(s) : NULL;
    int64_t n = ds ? ds->len : (int64_t)strlen(s);
    int64_t count = 0;
    for (int64_t i = 0; i < n; i++) if (dragon_str_cp_at(s, ds, i) == '\t') count++;
    int64_t budget = DRAGON_STR_MAX_WIDTH - n;
    if (budget < 0 || (count != 0 && tabsize > budget / count)) {
        dragon_raise_exc_cstr(22, "OverflowError: expandtabs result too large");
        return dragon_string_alloc("", 0);
    }
    int64_t maxLen = n + count * tabsize;
    if (!ds || ds->kind == 1) {
        DragonString* out = dragon_string_alloc_raw(maxLen);
        int64_t w = 0, col = 0;
        for (int64_t i = 0; i < n; i++) {
            unsigned char c = (unsigned char)s[i];
            if (c == '\t') {
                int64_t spaces = tabsize - (col % tabsize);
                if (spaces <= 0) spaces = tabsize;
                for (int64_t j = 0; j < spaces; j++) { out->data[w++] = ' '; col++; }
            } else if (c == '\n') { out->data[w++] = '\n'; col = 0; }
            else { out->data[w++] = (char)c; col++; }
        }
        out->data[w] = '\0';
        out->len = w;
        return out->data;
    }
    const uint32_t* src = (const uint32_t*)ds->data;
    DragonString* out = dragon_string_alloc_ucs4(maxLen);
    uint32_t* dst = (uint32_t*)out->data, max_cp = 0;
    int64_t w = 0, col = 0;
    for (int64_t i = 0; i < n; i++) {
        uint32_t cp = src[i];
        if (cp == '\t') {
            int64_t spaces = tabsize - (col % tabsize);
            if (spaces <= 0) spaces = tabsize;
            for (int64_t j = 0; j < spaces; j++) { dst[w++] = ' '; col++; }
        } else if (cp == '\n') { dst[w++] = '\n'; col = 0; }
        else { dst[w++] = cp; if (cp > max_cp) max_cp = cp; col++; }
    }
    out->len = w;
    return dragon_str_finish_cps(out, dst, w, max_cp);
}

static int64_t dragon_str_find_cp_se(const char* haystack, const char* needle,
                                     int64_t start, int64_t end) {
    if (!haystack || !needle) return -1;
    DragonString* dh = dragon_is_heap_string(haystack)
        ? dragon_string_from_data(haystack) : NULL;
    DragonString* dn = dragon_is_heap_string(needle)
        ? dragon_string_from_data(needle) : NULL;
    int64_t hlen = dh ? dh->len : (int64_t)strlen(haystack);
    int64_t nlen = dn ? dn->len : (int64_t)strlen(needle);
    if (start < 0) start = 0;
    if (end < 0 || end > hlen) end = hlen;
    if (start > end) return -1;
    if (nlen == 0) return start <= end ? start : -1;
    if (end - start < nlen) return -1;
    bool h_kind1 = (!dh || dh->kind == 1);
    bool n_kind1 = (!dn || dn->kind == 1);
    if (h_kind1 && n_kind1) {
        const char* base = haystack + start;
        int64_t window = end - start;
        for (int64_t i = 0; i + nlen <= window; ++i) {
            if (memcmp(base + i, needle, (size_t)nlen) == 0) {
                return start + i;
            }
        }
        return -1;
    }
    for (int64_t i = start; i + nlen <= end; ++i) {
        bool match = true;
        for (int64_t j = 0; j < nlen; ++j) {
            if (dragon_str_cp_at(haystack, dh, i + j) !=
                dragon_str_cp_at(needle, dn, j)) {
                match = false;
                break;
            }
        }
        if (match) return i;
    }
    return -1;
}

static int64_t dragon_str_find_cp(const char* haystack, const char* needle,
                                  int64_t start) {
    return dragon_str_find_cp_se(haystack, needle, start, -1);
}

int64_t dragon_str_find(const char* s, const char* sub) {
    return dragon_str_find_cp_se(s, sub, 0, -1);
}

int64_t dragon_str_find_se(const char* s, const char* sub, int64_t start, int64_t end) {
    return dragon_str_find_cp_se(s, sub, start, end);
}

int64_t dragon_str_rfind(const char* s, const char* sub) {
    int64_t last = -1;
    int64_t pos = 0;
    DragonString* dn = (sub && dragon_is_heap_string(sub))
        ? dragon_string_from_data(sub) : NULL;
    int64_t nlen = dn ? dn->len : (sub ? (int64_t)strlen(sub) : 0);
    int64_t step = nlen > 0 ? nlen : 1;
    for (;;) {
        int64_t r = dragon_str_find_cp_se(s, sub, pos, -1);
        if (r < 0) break;
        last = r;
        pos = r + step;
    }
    return last;
}

int64_t dragon_str_rfind_se(const char* s, const char* sub, int64_t start, int64_t end) {
    int64_t last = -1;
    int64_t pos = start < 0 ? 0 : start;
    DragonString* dn = (sub && dragon_is_heap_string(sub))
        ? dragon_string_from_data(sub) : NULL;
    int64_t nlen = dn ? dn->len : (sub ? (int64_t)strlen(sub) : 0);
    int64_t step = nlen > 0 ? nlen : 1;
    for (;;) {
        int64_t r = dragon_str_find_cp_se(s, sub, pos, end);
        if (r < 0) break;
        last = r;
        pos = r + step;
    }
    return last;
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

int64_t dragon_str_count(const char* s, const char* sub) {
    if (!s || !sub) return 0;
    DragonString* dn = dragon_is_heap_string(sub) ? dragon_string_from_data(sub) : NULL;
    int64_t nlen = dn ? dn->len : (int64_t)strlen(sub);
    if (nlen == 0) return 0;
    int64_t c = 0, pos = 0;
    for (;;) {
        int64_t r = dragon_str_find_cp_se(s, sub, pos, -1);
        if (r < 0) break;
        c++;
        pos = r + nlen;
    }
    return c;
}

int64_t dragon_str_count_se(const char* s, const char* sub, int64_t start, int64_t end) {
    if (!s || !sub) return 0;
    DragonString* dn = dragon_is_heap_string(sub) ? dragon_string_from_data(sub) : NULL;
    int64_t nlen = dn ? dn->len : (int64_t)strlen(sub);
    if (nlen == 0) return 0;
    int64_t c = 0;
    int64_t pos = start < 0 ? 0 : start;
    for (;;) {
        int64_t r = dragon_str_find_cp_se(s, sub, pos, end);
        if (r < 0) break;
        c++;
        pos = r + nlen;
    }
    return c;
}

int64_t dragon_str_startswith(const char* s, const char* prefix) {
    if (!s || !prefix) return 0;
    DragonString* ds = dragon_is_heap_string(s) ? dragon_string_from_data(s) : NULL;
    DragonString* dp = dragon_is_heap_string(prefix) ? dragon_string_from_data(prefix) : NULL;
    int64_t slen = ds ? ds->len : (int64_t)strlen(s);
    int64_t plen = dp ? dp->len : (int64_t)strlen(prefix);
    if (plen > slen) return 0;
    bool s_k1 = (!ds || ds->kind == 1);
    bool p_k1 = (!dp || dp->kind == 1);
    if (s_k1 && p_k1) return strncmp(s, prefix, (size_t)plen) == 0 ? 1 : 0;
    for (int64_t i = 0; i < plen; ++i) {
        if (dragon_str_cp_at(s, ds, i) != dragon_str_cp_at(prefix, dp, i)) return 0;
    }
    return 1;
}

int64_t dragon_str_endswith(const char* s, const char* suffix) {
    if (!s || !suffix) return 0;
    DragonString* ds = dragon_is_heap_string(s) ? dragon_string_from_data(s) : NULL;
    DragonString* du = dragon_is_heap_string(suffix) ? dragon_string_from_data(suffix) : NULL;
    int64_t slen = ds ? ds->len : (int64_t)strlen(s);
    int64_t ulen = du ? du->len : (int64_t)strlen(suffix);
    if (ulen > slen) return 0;
    bool s_k1 = (!ds || ds->kind == 1);
    bool u_k1 = (!du || du->kind == 1);
    if (s_k1 && u_k1) return strcmp(s + slen - ulen, suffix) == 0 ? 1 : 0;
    int64_t off = slen - ulen;
    for (int64_t i = 0; i < ulen; ++i) {
        if (dragon_str_cp_at(s, ds, off + i) != dragon_str_cp_at(suffix, du, i)) return 0;
    }
    return 1;
}

int64_t dragon_str_contains(const char* s, const char* sub) {
    return dragon_str_find_cp(s, sub, 0) >= 0 ? 1 : 0;
}

static inline int64_t dragon_pred_len(const char* s, DragonString** out_ds) {
    *out_ds = NULL;
    if (!s) return -1;
    if (dragon_is_heap_string(s)) {
        *out_ds = dragon_string_from_data(s);
        return (*out_ds)->len;
    }
    return (int64_t)strlen(s);
}

int64_t dragon_str_isdigit(const char* s) {
    DragonString* ds;
    int64_t n = dragon_pred_len(s, &ds);
    if (n <= 0) return 0;
    for (int64_t i = 0; i < n; i++)
        if (!dragon_cp_is_digit(dragon_str_cp_at(s, ds, i))) return 0;
    return 1;
}

int64_t dragon_str_isalpha(const char* s) {
    DragonString* ds;
    int64_t n = dragon_pred_len(s, &ds);
    if (n <= 0) return 0;
    for (int64_t i = 0; i < n; i++)
        if (!dragon_cp_is_alpha(dragon_str_cp_at(s, ds, i))) return 0;
    return 1;
}

int64_t dragon_str_isalnum(const char* s) {
    DragonString* ds;
    int64_t n = dragon_pred_len(s, &ds);
    if (n <= 0) return 0;
    for (int64_t i = 0; i < n; i++) {
        uint32_t cp = dragon_str_cp_at(s, ds, i);
        if (!dragon_cp_is_alpha(cp) && !dragon_cp_is_digit(cp)) return 0;
    }
    return 1;
}

int64_t dragon_str_isspace(const char* s) {
    DragonString* ds;
    int64_t n = dragon_pred_len(s, &ds);
    if (n <= 0) return 0;
    for (int64_t i = 0; i < n; i++)
        if (!dragon_cp_is_space(dragon_str_cp_at(s, ds, i))) return 0;
    return 1;
}

int64_t dragon_str_isupper(const char* s) {
    DragonString* ds;
    int64_t n = dragon_pred_len(s, &ds);
    if (n <= 0) return 0;
    int has = 0;
    for (int64_t i = 0; i < n; i++) {
        uint32_t cp = dragon_str_cp_at(s, ds, i);
        if (dragon_cp_is_lower(cp)) return 0;
        if (dragon_cp_is_upper(cp)) has = 1;
    }
    return has;
}

int64_t dragon_str_islower(const char* s) {
    DragonString* ds;
    int64_t n = dragon_pred_len(s, &ds);
    if (n <= 0) return 0;
    int has = 0;
    for (int64_t i = 0; i < n; i++) {
        uint32_t cp = dragon_str_cp_at(s, ds, i);
        if (dragon_cp_is_upper(cp)) return 0;
        if (dragon_cp_is_lower(cp)) has = 1;
    }
    return has;
}

int64_t dragon_str_istitle(const char* s) {
    DragonString* ds;
    int64_t n = dragon_pred_len(s, &ds);
    if (n <= 0) return 0;
    int prev_cased = 0, has_cased = 0;
    for (int64_t i = 0; i < n; i++) {
        uint32_t cp = dragon_str_cp_at(s, ds, i);
        if (dragon_cp_is_upper(cp)) {
            if (prev_cased) return 0;
            prev_cased = 1;
            has_cased = 1;
        } else if (dragon_cp_is_lower(cp)) {
            if (!prev_cased) return 0;
            prev_cased = 1;
            has_cased = 1;
        } else {
            prev_cased = 0;
        }
    }
    return has_cased;
}

int64_t dragon_str_isascii(const char* s) {
    if (!s) return 1;
    DragonString* ds = dragon_is_heap_string(s) ? dragon_string_from_data(s) : NULL;
    if (ds && ds->kind == 4) {
        int64_t n = ds->len;
        const uint32_t* cps = (const uint32_t*)ds->data;
        for (int64_t i = 0; i < n; i++) if (cps[i] > 127) return 0;
        return 1;
    }
    for (; *s; s++) if ((unsigned char)*s > 127) return 0;
    return 1;
}

int64_t dragon_str_isdecimal(const char* s) { return dragon_str_isdigit(s); }
int64_t dragon_str_isnumeric(const char* s) { return dragon_str_isdigit(s); }

int64_t dragon_str_isprintable(const char* s) {
    if (!s) return 1;
    DragonString* ds = dragon_is_heap_string(s) ? dragon_string_from_data(s) : NULL;
    if (ds && ds->kind == 4) {
        int64_t n = ds->len;
        const uint32_t* cps = (const uint32_t*)ds->data;
        for (int64_t i = 0; i < n; i++) if (cps[i] < 32 || cps[i] == 127) return 0;
        return 1;
    }
    if (!*s) return 1;
    for (; *s; s++) if ((unsigned char)*s < 32 || (unsigned char)*s == 127) return 0;
    return 1;
}

int64_t dragon_str_isidentifier(const char* s) {
    DragonString* ds;
    int64_t n = dragon_pred_len(s, &ds);
    if (n <= 0) return 0;
    for (int64_t i = 0; i < n; i++) {
        uint32_t cp = dragon_str_cp_at(s, ds, i);
        if (cp == '_') continue;
        if ((cp >= 'a' && cp <= 'z') || (cp >= 'A' && cp <= 'Z')) continue;
        if (i > 0 && cp >= '0' && cp <= '9') continue;
        return 0;
    }
    return 1;
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

const char* dragon_str_slice(const char* s, int64_t start, int64_t stop, int64_t step) {
    if (!s) return dragon_string_alloc("", 0);
    if (step == 0) {
        dragon_raise_exc_cstr(90, "ValueError: slice step cannot be zero");
    }
    DragonString* in = dragon_is_heap_string(s) ? dragon_string_from_data(s) : NULL;
    int64_t cp_count = in ? in->len : (int64_t)strlen(s);
    dragon_slice_indices(cp_count, &start, &stop, step);
    if (!in || in->kind == 1) {
        int64_t out_count = 0;
        if (step > 0) { for (int64_t i = start; i < stop; i += step) out_count++; }
        else          { for (int64_t i = start; i > stop; i += step) out_count++; }
        DragonString* ds = dragon_string_alloc_raw(out_count);
        int64_t w = 0;
        if (step > 0) { for (int64_t i = start; i < stop; i += step) ds->data[w++] = s[i]; }
        else { for (int64_t i = start; i > stop; i += step) ds->data[w++] = s[i]; }
        ds->data[w] = '\0';
        ds->len = w;
        return ds->data;
    }
    const uint32_t* src = (const uint32_t*)in->data;
    int64_t out_count = 0;
    if (step > 0) { for (int64_t i = start; i < stop; i += step) out_count++; }
    else          { for (int64_t i = start; i > stop; i += step) out_count++; }
    if (out_count == 0) return dragon_string_alloc("", 0);
    uint32_t max_cp = 0;
    if (step > 0) {
        for (int64_t i = start; i < stop; i += step) {
            if (src[i] > max_cp) max_cp = src[i];
        }
    } else {
        for (int64_t i = start; i > stop; i += step) {
            if (src[i] > max_cp) max_cp = src[i];
        }
    }
    if (max_cp < 0x80) {
        DragonString* ds = dragon_string_alloc_ascii(out_count);
        int64_t w = 0;
        if (step > 0) { for (int64_t i = start; i < stop; i += step) ds->data[w++] = (char)src[i]; }
        else          { for (int64_t i = start; i > stop; i += step) ds->data[w++] = (char)src[i]; }
        return ds->data;
    }
    DragonString* ds = dragon_string_alloc_ucs4(out_count);
    uint32_t* dst = (uint32_t*)ds->data;
    int64_t w = 0;
    if (step > 0) { for (int64_t i = start; i < stop; i += step) dst[w++] = src[i]; }
    else          { for (int64_t i = start; i > stop; i += step) dst[w++] = src[i]; }
    return ds->data;
}

DragonList* dragon_list_slice(DragonList* l, int64_t start, int64_t stop, int64_t step) {
    if (!l) return dragon_list_new(0);
    int64_t len = l->size;
    if (step == 0) {
        dragon_raise_exc_cstr(90, "ValueError: slice step cannot be zero");
    }
    dragon_slice_indices(len, &start, &stop, step);
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

DragonList* dragon_str_split_max(const char* s, const char* sep, int64_t maxsplit) {
    DragonList* l = dragon_list_new_tagged(8, TAG_STR);
    if (!s) return l;
    DragonString* ds = dragon_is_heap_string(s) ? dragon_string_from_data(s) : NULL;
    int64_t slen = ds ? ds->len : (int64_t)strlen(s);
    DragonString* dsep = (sep && dragon_is_heap_string(sep))
        ? dragon_string_from_data(sep) : NULL;
    int64_t seplen = sep
        ? (dsep ? dsep->len : (int64_t)strlen(sep))
        : 0;

    auto is_ws_cp = [](uint32_t cp) {
        return cp == ' ' || cp == '\t' || cp == '\n' || cp == '\r';
    };

    int64_t nsplits = 0;
    if (!sep || seplen == 0) {
        int64_t i = 0;
        while (i < slen) {
            while (i < slen && is_ws_cp(dragon_str_cp_at(s, ds, i))) i++;
            if (i >= slen) break;
            int64_t start = i;
            if (maxsplit >= 0 && nsplits >= maxsplit) {
                dragon_list_append(l, (int64_t)dragon_str_slice(s, start, slen, 1));
                break;
            }
            while (i < slen && !is_ws_cp(dragon_str_cp_at(s, ds, i))) i++;
            const char* w = dragon_str_slice(s, start, i, 1);
            dragon_list_append(l, (int64_t)w);
            nsplits++;
        }
    } else {
        int64_t pos = 0;
        for (;;) {
            if (maxsplit >= 0 && nsplits >= maxsplit) {
                dragon_list_append(l, (int64_t)dragon_str_slice(s, pos, slen, 1));
                break;
            }
            int64_t f = dragon_str_find_cp_se(s, sep, pos, -1);
            if (f < 0) {
                const char* w = dragon_str_slice(s, pos, slen, 1);
                dragon_list_append(l, (int64_t)w);
                break;
            }
            const char* w = dragon_str_slice(s, pos, f, 1);
            dragon_list_append(l, (int64_t)w);
            pos = f + seplen;
            nsplits++;
        }
    }
    return l;
}

DragonList* dragon_str_split(const char* s, const char* sep) {
    return dragon_str_split_max(s, sep, -1);
}

static const char* dragon_join_utf8(const char* sep, const char** items, int64_t n) {
    if (n <= 0) return dragon_string_alloc("", 0);
    int64_t sep_blen = 0;
    char* sep_enc = sep ? dragon_str_to_utf8_alloc(sep, &sep_blen) : NULL;
    const char* sep_bytes = sep_enc ? sep_enc : sep;

    char** owned = (char**)dragon_calloc_nullable((size_t)n, sizeof(char*));
    int64_t* blens = owned ? (int64_t*)dragon_calloc_nullable((size_t)n, sizeof(int64_t)) : NULL;
    if (!owned || !blens) {
        free(owned);
        if (sep_enc) free(sep_enc);
        dragon_raise_oom();
    }
    int64_t total = 0;
    bool overflow = false;
    for (int64_t i = 0; i < n; ++i) {
        const char* elem = items[i];
        int64_t bl = 0;
        owned[i] = elem ? dragon_str_to_utf8_alloc(elem, &bl) : NULL;
        blens[i] = elem ? bl : 0;
        int64_t add = blens[i] + (i > 0 ? sep_blen : 0);
        if (add > INT64_MAX - total) { overflow = true; break; }
        total += add;
    }
    char* buf = overflow ? NULL : (char*)dragon_malloc_nullable(total > 0 ? (size_t)total : 1);
    if (!buf) {
        for (int64_t i = 0; i < n; ++i) free(owned[i]);
        free(owned);
        free(blens);
        if (sep_enc) free(sep_enc);
        if (overflow)
            dragon_raise_exc_cstr(43, "MemoryError: allocation size overflow");
        dragon_raise_oom();
    }
    char* w = buf;
    for (int64_t i = 0; i < n; ++i) {
        if (i > 0 && sep_blen > 0) { memcpy(w, sep_bytes, (size_t)sep_blen); w += sep_blen; }
        if (blens[i] > 0) {
            const char* eb = owned[i] ? owned[i] : items[i];
            memcpy(w, eb, (size_t)blens[i]);
            w += blens[i];
        }
        if (owned[i]) free(owned[i]);
    }
    int32_t clbase = dragon_cleanup_depth();
    dragon_cleanup_push((int64_t)(uintptr_t)buf, DCLEAN_FREE, 0);
    dragon_cleanup_push((int64_t)(uintptr_t)owned, DCLEAN_FREE, 0);
    dragon_cleanup_push((int64_t)(uintptr_t)blens, DCLEAN_FREE, 0);
    if (sep_enc) dragon_cleanup_push((int64_t)(uintptr_t)sep_enc, DCLEAN_FREE, 0);
    const char* result = dragon_string_alloc(buf, total);
    dragon_cleanup_reset(clbase);
    free(buf);
    free(owned);
    free(blens);
    if (sep_enc) free(sep_enc);
    return result;
}

const char* dragon_str_join_ptr(const char* sep, DragonListPtr* l) {
    if (!l || l->size == 0) return dragon_string_alloc("", 0);
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
    DragonString* in = dragon_is_heap_string(s) ? dragon_string_from_data(s) : NULL;
    int64_t n = in ? in->len : (int64_t)strlen(s);
    int64_t start = 0;
    int64_t i = 0;
    while (i < n) {
        uint32_t cp = dragon_str_cp_at(s, in, i);
        if (cp == '\n' || cp == '\r') {
            const char* w = dragon_str_slice(s, start, i, 1);
            dragon_list_append(r, (int64_t)w);
            if (cp == '\r' && i + 1 < n && dragon_str_cp_at(s, in, i + 1) == '\n') i++;
            i++;
            start = i;
        } else { i++; }
    }
    if (start < n) {
        const char* w = dragon_str_slice(s, start, n, 1);
        dragon_list_append(r, (int64_t)w);
    }
    return r;
}

DragonTuple* dragon_str_partition(const char* s, const char* sep) {
    DragonTuple* r = dragon_tuple_new(3);
    if (!s || !sep) {
        DragonString* si = s && dragon_is_heap_string(s) ? dragon_string_from_data(s) : NULL;
        int64_t sn = s ? (si ? si->len : (int64_t)strlen(s)) : 0;
        dragon_tuple_set_tagged(r, 0, (int64_t)(s ? dragon_str_slice(s, 0, sn, 1) : dragon_string_alloc("", 0)), TAG_STR);
        dragon_tuple_set_tagged(r, 1, (int64_t)dragon_string_alloc("", 0), TAG_STR);
        dragon_tuple_set_tagged(r, 2, (int64_t)dragon_string_alloc("", 0), TAG_STR);
        return r;
    }
    DragonString* si = dragon_is_heap_string(s) ? dragon_string_from_data(s) : NULL;
    DragonString* pi = dragon_is_heap_string(sep) ? dragon_string_from_data(sep) : NULL;
    int64_t sn = si ? si->len : (int64_t)strlen(s);
    int64_t sl = pi ? pi->len : (int64_t)strlen(sep);
    int64_t at = (sl == 0) ? -1 : dragon_str_find_cp(s, sep, 0);
    if (at < 0) {
        dragon_tuple_set_tagged(r, 0, (int64_t)dragon_str_slice(s, 0, sn, 1), TAG_STR);
        dragon_tuple_set_tagged(r, 1, (int64_t)dragon_string_alloc("", 0), TAG_STR);
        dragon_tuple_set_tagged(r, 2, (int64_t)dragon_string_alloc("", 0), TAG_STR);
        return r;
    }
    dragon_tuple_set_tagged(r, 0, (int64_t)dragon_str_slice(s, 0, at, 1), TAG_STR);
    dragon_tuple_set_tagged(r, 1, (int64_t)dragon_str_slice(sep, 0, sl, 1), TAG_STR);
    dragon_tuple_set_tagged(r, 2, (int64_t)dragon_str_slice(s, at + sl, sn, 1), TAG_STR);
    return r;
}

DragonTuple* dragon_str_rpartition(const char* s, const char* sep) {
    DragonTuple* r = dragon_tuple_new(3);
    if (!s || !sep) {
        DragonString* si = s && dragon_is_heap_string(s) ? dragon_string_from_data(s) : NULL;
        int64_t sn = s ? (si ? si->len : (int64_t)strlen(s)) : 0;
        dragon_tuple_set_tagged(r, 0, (int64_t)dragon_string_alloc("", 0), TAG_STR);
        dragon_tuple_set_tagged(r, 1, (int64_t)dragon_string_alloc("", 0), TAG_STR);
        dragon_tuple_set_tagged(r, 2, (int64_t)(s ? dragon_str_slice(s, 0, sn, 1) : dragon_string_alloc("", 0)), TAG_STR);
        return r;
    }
    DragonString* si = dragon_is_heap_string(s) ? dragon_string_from_data(s) : NULL;
    DragonString* pi = dragon_is_heap_string(sep) ? dragon_string_from_data(sep) : NULL;
    int64_t sn = si ? si->len : (int64_t)strlen(s);
    int64_t sl = pi ? pi->len : (int64_t)strlen(sep);
    int64_t last = -1;
    if (sl > 0) {
        int64_t pos = 0;
        for (;;) {
            int64_t at = dragon_str_find_cp(s, sep, pos);
            if (at < 0) break;
            last = at;
            pos = at + sl;
        }
    }
    if (last < 0) {
        dragon_tuple_set_tagged(r, 0, (int64_t)dragon_string_alloc("", 0), TAG_STR);
        dragon_tuple_set_tagged(r, 1, (int64_t)dragon_string_alloc("", 0), TAG_STR);
        dragon_tuple_set_tagged(r, 2, (int64_t)dragon_str_slice(s, 0, sn, 1), TAG_STR);
        return r;
    }
    dragon_tuple_set_tagged(r, 0, (int64_t)dragon_str_slice(s, 0, last, 1), TAG_STR);
    dragon_tuple_set_tagged(r, 1, (int64_t)dragon_str_slice(sep, 0, sl, 1), TAG_STR);
    dragon_tuple_set_tagged(r, 2, (int64_t)dragon_str_slice(s, last + sl, sn, 1), TAG_STR);
    return r;
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
