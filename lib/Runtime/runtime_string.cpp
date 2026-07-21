/// Dragon Runtime - String Operations
#include "runtime_internal.h"
#include "runtime_string_shared.h"

extern "C" {



/// Decode one UTF-8 code point starting at `p` (length `remaining` bytes
/// available). Writes the decoded code point into `*out_cp` and returns the
/// number of bytes consumed (1..4). Returns 0 on invalid encoding.
static int dragon_utf8_decode_one(const unsigned char* p, int64_t remaining,
                                  uint32_t* out_cp) {
    if (remaining <= 0) return 0;
    unsigned char b0 = p[0];
    if (b0 < 0x80) { *out_cp = b0; return 1; }
    if ((b0 & 0xE0) == 0xC0 && remaining >= 2 && (p[1] & 0xC0) == 0x80) {
        uint32_t cp = ((uint32_t)(b0 & 0x1F) << 6) | (p[1] & 0x3F);
        if (cp < 0x80) return 0; // overlong
        *out_cp = cp; return 2;
    }
    if ((b0 & 0xF0) == 0xE0 && remaining >= 3 &&
        (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80) {
        uint32_t cp = ((uint32_t)(b0 & 0x0F) << 12) |
                      ((uint32_t)(p[1] & 0x3F) << 6) | (p[2] & 0x3F);
        if (cp < 0x800) return 0; // overlong
        if (cp >= 0xD800 && cp <= 0xDFFF) return 0; // surrogate
        *out_cp = cp; return 3;
    }
    if ((b0 & 0xF8) == 0xF0 && remaining >= 4 &&
        (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80 && (p[3] & 0xC0) == 0x80) {
        uint32_t cp = ((uint32_t)(b0 & 0x07) << 18) |
                      ((uint32_t)(p[1] & 0x3F) << 12) |
                      ((uint32_t)(p[2] & 0x3F) << 6) | (p[3] & 0x3F);
        if (cp < 0x10000 || cp > 0x10FFFF) return 0;
        *out_cp = cp; return 4;
    }
    return 0;
}

/// Allocate a new refcounted string from a UTF-8 byte buffer of length
/// `byte_len`. Scans the bytes: if pure ASCII, allocates kind=1 with a
/// byte-for-byte copy. Otherwise decodes UTF-8 to UCS-4 and allocates kind=4.
/// Returns the public data pointer.
const char* dragon_string_alloc(const char* src, int64_t byte_len) {
    if (byte_len <= 0) {
        DragonString* s = dragon_string_alloc_ascii(0);
        return s->data;
    }
    // Scan for any non-ASCII byte.
    const unsigned char* p = (const unsigned char*)src;
    int has_high = 0;
    for (int64_t i = 0; i < byte_len; ++i) {
        if (p[i] >= 0x80) { has_high = 1; break; }
    }
    if (!has_high) {
        // ASCII fast path: byte_len == cp_count, raw byte copy.
        DragonString* s = dragon_string_alloc_ascii(byte_len);
        memcpy(s->data, src, (size_t)byte_len);
        return s->data;
    }
    // UTF-8 decode path: count code points, decode into kind=4 buffer.
    int64_t cp_count = 0;
    for (int64_t i = 0; i < byte_len; ) {
        uint32_t cp;
        int n = dragon_utf8_decode_one(p + i, byte_len - i, &cp);
        if (n <= 0) {
            // Invalid UTF-8: treat the byte as Latin-1 (replacement-style),
            // counting it as one code point. Keeps the caller alive even when
            // they hand us non-UTF-8 byte sequences.
            n = 1;
        }
        cp_count++;
        i += n;
    }
    DragonString* s = dragon_string_alloc_ucs4(cp_count);
    uint32_t* dst = (uint32_t*)s->data;
    int64_t out = 0;
    for (int64_t i = 0; i < byte_len; ) {
        uint32_t cp;
        int n = dragon_utf8_decode_one(p + i, byte_len - i, &cp);
        if (n <= 0) {
            cp = (uint32_t)p[i];
            n = 1;
        }
        dst[out++] = cp;
        i += n;
    }
    return s->data;
}

// --- Codec helpers for str.encode / bytes.decode ---
//
// Dragon ships exactly the UTF-8 + ASCII codecs and the strict/replace error
// handlers (no pluggable codec registry - that dynamic machinery has no place
// in a statically typed language). `str` is already decoded code points, so
// these operate purely at the bytes<->str boundary. Unknown encoding or error
// handler -> LookupError(40). Strict decode of invalid input -> UnicodeDecode
// Error(92) (Python parity; the old silent Latin-1 fallback was a correctness
// bug). Strict ascii-encode of a non-ASCII char -> UnicodeEncodeError(93).

// Case-insensitive match of an encoding/handler name against `lc_target`
// (which must be lowercase). Avoids a locale-coupled strcasecmp.
static int dragon_enc_is(const char* enc, const char* lc_target) {
    if (!enc) return 0;
    const char* a = enc;
    const char* b = lc_target;
    while (*a && *b) {
        char ca = *a;
        if (ca >= 'A' && ca <= 'Z') ca = (char)(ca - 'A' + 'a');
        if (ca != *b) return 0;
        ++a; ++b;
    }
    return *a == 0 && *b == 0;
}

// errors policy: 0 = strict (raise), 1 = replace (U+FFFD / '?'), -1 = unknown.
static int dragon_errors_policy(const char* errors) {
    if (!errors || dragon_enc_is(errors, "strict")) return 0;
    if (dragon_enc_is(errors, "replace")) return 1;
    return -1;
}

// Core decode: `ascii_only` selects the ASCII codec (every byte must be < 0x80)
// vs UTF-8; `pol` is strict(0)/replace(1). On a strict failure this raises and
// returns "" (the raise longjmps away in practice). On replace, each invalid
// unit becomes U+FFFD. Mirrors dragon_string_alloc's two-pass build but with
// defined error behavior instead of a silent Latin-1 reinterpretation.
static const char* dragon_decode_checked(const unsigned char* p, int64_t n,
                                         int ascii_only, int pol) {
    int64_t cp_count = 0;
    int has_high = 0;
    for (int64_t i = 0; i < n; ) {
        uint32_t cp;
        int adv;
        if (ascii_only) {
            if (p[i] < 0x80) { cp = p[i]; adv = 1; }
            else if (pol == 1) { cp = 0xFFFD; adv = 1; }
            else {
                dragon_raise_exc_cstr(92, "'ascii' codec can't decode byte");
                return dragon_string_alloc("", 0);
            }
        } else {
            adv = dragon_utf8_decode_one(p + i, n - i, &cp);
            if (adv <= 0) {
                if (pol == 1) { cp = 0xFFFD; adv = 1; }
                else {
                    dragon_raise_exc_cstr(92, "'utf-8' codec can't decode byte");
                    return dragon_string_alloc("", 0);
                }
            }
        }
        if (cp >= 0x80) has_high = 1;
        cp_count++;
        i += adv;
    }
    if (!has_high) {
        // Pure-ASCII output: every accepted unit was a single byte < 0x80.
        DragonString* s = dragon_string_alloc_ascii(cp_count);
        int64_t out = 0;
        for (int64_t i = 0; i < n; ++i) s->data[out++] = (char)p[i];
        return s->data;
    }
    DragonString* s = dragon_string_alloc_ucs4(cp_count);
    uint32_t* dst = (uint32_t*)s->data;
    int64_t out = 0;
    for (int64_t i = 0; i < n; ) {
        uint32_t cp;
        int adv;
        if (ascii_only) {
            if (p[i] < 0x80) { cp = p[i]; adv = 1; }
            else { cp = 0xFFFD; adv = 1; }   // pol==replace (strict raised in pass 1)
        } else {
            adv = dragon_utf8_decode_one(p + i, n - i, &cp);
            if (adv <= 0) { cp = 0xFFFD; adv = 1; }
        }
        dst[out++] = cp;
        i += adv;
    }
    return s->data;
}

// bytes.decode(encoding="utf-8", errors="strict"). Honors the arguments that
// the bare dragon_bytes_decode silently ignored.
const char* dragon_bytes_decode_ex(DragonBytes* b, const char* encoding,
                                   const char* errors) {
    int pol = dragon_errors_policy(errors);
    if (pol < 0) {
        dragon_raise_exc_cstr(40, "unknown error handler name");
        return dragon_string_alloc("", 0);
    }
    int is_ascii = dragon_enc_is(encoding, "ascii") || dragon_enc_is(encoding, "us-ascii");
    int is_utf8  = dragon_enc_is(encoding, "utf-8") || dragon_enc_is(encoding, "utf8") ||
                   dragon_enc_is(encoding, "u8");
    if (!is_ascii && !is_utf8) {
        dragon_raise_exc_cstr(40, "unknown encoding");
        return dragon_string_alloc("", 0);
    }
    if (!b || b->len == 0) return dragon_string_alloc("", 0);
    return dragon_decode_checked(b->data, b->len, is_ascii ? 1 : 0, pol);
}

// str.encode(encoding="utf-8", errors="strict"). UTF-8 always succeeds (every
// code point encodes); ASCII raises/replaces on a non-ASCII code point.
DragonBytes* dragon_str_encode_ex(const char* s, const char* encoding,
                                  const char* errors) {
    int pol = dragon_errors_policy(errors);
    if (pol < 0) {
        dragon_raise_exc_cstr(40, "unknown error handler name");
        return dragon_bytes_new(nullptr, 0);
    }
    int is_ascii = dragon_enc_is(encoding, "ascii") || dragon_enc_is(encoding, "us-ascii");
    int is_utf8  = dragon_enc_is(encoding, "utf-8") || dragon_enc_is(encoding, "utf8") ||
                   dragon_enc_is(encoding, "u8");
    if (!is_ascii && !is_utf8) {
        dragon_raise_exc_cstr(40, "unknown encoding");
        return dragon_bytes_new(nullptr, 0);
    }
    if (!s) return dragon_bytes_new(nullptr, 0);
    int64_t blen = 0;
    char* enc = dragon_str_to_utf8_alloc(s, &blen);
    const char* src = enc ? enc : s;
    if (is_utf8) {
        DragonBytes* bts = dragon_bytes_new((const uint8_t*)src, blen);
        if (enc) free(enc);
        return bts;
    }
    // ASCII: any code point >= 0x80 shows up as a byte >= 0x80 in the UTF-8 form.
    int has_high = 0;
    for (int64_t i = 0; i < blen; ++i) {
        if ((unsigned char)src[i] >= 0x80) { has_high = 1; break; }
    }
    if (!has_high) {
        DragonBytes* bts = dragon_bytes_new((const uint8_t*)src, blen);
        if (enc) free(enc);
        return bts;
    }
    if (pol == 0) {
        if (enc) free(enc);
        dragon_raise_exc_cstr(93, "'ascii' codec can't encode character");
        return dragon_bytes_new(nullptr, 0);
    }
    // replace: each non-ASCII code point becomes '?'.
    const unsigned char* p = (const unsigned char*)src;
    int64_t outn = 0;
    for (int64_t i = 0; i < blen; ) {
        uint32_t cp;
        int adv = dragon_utf8_decode_one(p + i, blen - i, &cp);
        if (adv <= 0) adv = 1;
        outn++; i += adv;
    }
    uint8_t* buf = (uint8_t*)malloc(outn > 0 ? (size_t)outn : 1);
    int64_t w = 0;
    for (int64_t i = 0; i < blen; ) {
        uint32_t cp;
        int adv = dragon_utf8_decode_one(p + i, blen - i, &cp);
        if (adv <= 0) { cp = p[i]; adv = 1; }
        buf[w++] = (cp < 0x80) ? (uint8_t)cp : (uint8_t)'?';
        i += adv;
    }
    DragonBytes* bts = dragon_bytes_new(buf, outn);
    free(buf);
    if (enc) free(enc);
    return bts;
}

/// Allocate a DragonString but return the raw struct pointer (for in-place
/// building). Always allocates a kind=1 (byte-oriented) buffer of `byte_len`
/// bytes. Callers that need UCS-4 must use the kind-aware helpers.
DragonString* dragon_string_alloc_raw(int64_t byte_len) {
    // Defense in depth for all callers: a negative/overflowed size must never
    // under-allocate. `sizeof + negative` wraps to a huge size_t -> malloc
    // returns NULL -> the unconditional deref below would SIGSEGV. Callers that
    // derive sizes from user input bound them up front (the real fix); this is
    // the backstop. One predictable branch - no hot-path cost.
    if (byte_len < 0) byte_len = 0;
    // dragon_xmalloc raises MemoryError on OOM. (Previously raised OverflowError,
    // conflating a genuine out-of-memory with an overflowing size; the negative/
    // overflow case is the clamp above, OOM is a MemoryError.)
    DragonString* s = (DragonString*)dragon_xmalloc(sizeof(DragonString) + (size_t)byte_len + 1);
    dragon_obj_init(&s->header, DRAGON_TAG_STR);
    s->len = byte_len;   // for kind=1 ASCII, cp_count == byte count
    s->kind = 1;
    s->cap = dragon_cap_clamp(byte_len);
    s->data[byte_len] = '\0';
    return s;
}

/// Duplicate a string (possibly a literal) into a heap-allocated DragonString.
/// Safe to call on any const char* - always returns a refcounted copy.
/// Preserves the source kind: kind=4 strings round-trip as kind=4 (no UTF-8
/// re-decode), kind=1 / borrowed literals copy bytes verbatim.
/// Snapshot a message string for re-raising, but ONLY when it could be freed
/// out from under the in-flight exception. A re-raise site dups the in-flight
/// exc_msg before running scope cleanup, because cleanup may decref a local that
/// owns the message buffer (UAF otherwise). But unconditionally dup'ing leaks:
/// the dup is set as the borrowed exc_msg and never freed. The vast majority of
/// messages are string literals / interned immortals that scope cleanup can
/// NEVER free - return those unchanged (no dup, no leak). Only a MORTAL heap
/// string needs the protective copy. (That copy is itself the broader
/// exception-message-ownership leak - exc_msg has no owner to free it - but it
/// is rare and UAF-avoidance dominates.)
const char* dragon_exc_msg_preserve(const char* s) {
    if (!s) return s;
    if (!dragon_str_is_heap(s)) return s;  // C-string literal - never freed
    DragonString* ds = dragon_string_from_data(s);
    if (ds->header.refcount == DRAGON_IMMORTAL_REFCOUNT) return s;  // interned literal
    return dragon_string_dup(s);  // mortal heap - must snapshot before cleanup
}

/// Dup a KNOWN plain C string (stack snprintf buffer, errno text, ...) into a
/// fresh heap DragonString. Unlike dragon_string_dup it never probes for a
/// DragonString header - reading header bytes BEFORE an arbitrary stack/heap
/// buffer is out-of-bounds (ASan stack-buffer-underflow) even when it
/// happens to work. Use this whenever the input is by construction not a
/// DragonString data pointer.
const char* dragon_string_dup_cstr(const char* s) {
    if (!s) return dragon_string_alloc("", 0);
    return dragon_string_alloc(s, (int64_t)strlen(s));
}

const char* dragon_string_dup(const char* s) {
    if (!s) return dragon_string_alloc("", 0);
    if (dragon_str_is_heap(s)) {
        DragonString* src = dragon_string_from_data(s);
        if (src->kind == 4) {
            DragonString* out = dragon_string_alloc_ucs4(src->len);
            memcpy(out->data, src->data, (size_t)src->len * 4);
            return out->data;
        }
        // kind=1: copy bytes verbatim (cp_count == byte count).
        DragonString* out = dragon_string_alloc_ascii(src->len);
        memcpy(out->data, src->data, (size_t)src->len);
        return out->data;
    }
    // Borrowed C-string literal - re-scan via dragon_string_alloc so that
    // non-ASCII UTF-8 bytes get decoded to UCS-4 (canonical kind).
    return dragon_string_alloc(s, (int64_t)strlen(s));
}

/// One-shot interning: allocate from a UTF-8 byte buffer and mark the result
/// immortal. Used by codegen for non-ASCII string literals so that subsequent
/// loads at use sites are zero-cost (just a pointer load) and refcount
/// machinery skips the literal as it does for plain C-string literals today.
const char* dragon_str_intern(const char* utf8_bytes, int64_t byte_len) {
    const char* data = dragon_string_alloc(utf8_bytes, byte_len);
    if (data) {
        DragonString* ds = dragon_string_from_data(data);
        ds->header.refcount = DRAGON_IMMORTAL_REFCOUNT;
    }
    return data;
}

/// PRomote a heap string to immortal (refcount staturated), so incref/decref
/// becomes no-ops on it forever. Codegen calls this on a module-global whose
/// value is a program-lifetime constant. The HTTP server multiplexes handlers
/// across a POOL of OS worker threads, and Dragon's default refcount is
/// not atomic, so a shared global with a live refcount races (concurrent
/// incref/decref -> torn count -> premature free -> use-after-free). An immortal
/// never touches its refcount, so concurrent reads are safe with zero synchronization.
/// No-op on a string literal (no header) or an already-immortal STRING. This does not
/// make a MUTABLE shared object thread-safe - only its refcount; use a pool/lock for
/// objects whose contents mutate.
void dragon_str_make_immortal(const char* s) {
    if (!s || !dragon_is_heap_string(s)) return;
    DragonString* ds = dragon_string_from_data(s);
    ds->header.refcount = DRAGON_IMMORTAL_REFCOUNT;
}

// Forward decl - defined further below alongside the refcount helpers.
// Forward decl for the kind-aware substring scan (used by replace).
static int64_t dragon_str_find_cp(const char* haystack, const char* needle, int64_t start);

/// Encode a single Unicode code point as UTF-8 into `out`. `out` must have at
/// least 4 bytes. Returns the number of bytes written (1..4).
static int dragon_utf8_encode_one(uint32_t cp, char* out) {
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

/// Encode a kind=4 DragonString as a freshly-malloc'd UTF-8 byte buffer.
/// Sets `*out_byte_len` to the byte length (excluding NUL). Caller frees.
/// For kind=1 strings the public data pointer is already valid UTF-8 - use
/// `dragon_str_byte_view` (a header inline) and avoid this allocation.
char* dragon_str_to_utf8_alloc(const char* s, int64_t* out_byte_len) {
    if (!s) { if (out_byte_len) *out_byte_len = 0; return NULL; }
    DragonString* ds = dragon_is_heap_string(s)
        ? dragon_string_from_data(s) : NULL;
    if (!ds || ds->kind == 1) {
        // Borrow path: caller must use the raw pointer instead. Returning NULL
        // signals "no allocation needed; use s directly".
        if (out_byte_len) *out_byte_len = ds ? ds->len : (int64_t)strlen(s);
        return NULL;
    }
    // kind=4: bound the worst case at 4 bytes per code point. Guard the
    // multiply - ds->len is a code-point count that, while not directly
    // attacker-sized here, could overflow int64 on a pathological string and
    // wrap to a tiny malloc followed by a gigabyte-scale encode loop.
    if (ds->len < 0 || ds->len > INT64_MAX / 4) {
        dragon_raise_exc_cstr(43, "MemoryError: string too large to encode");
    }
    int64_t max_bytes = ds->len * 4;
    char* buf = (char*)malloc((size_t)max_bytes + 1);
    if (!buf) dragon_raise_exc_cstr(43, "MemoryError: out of memory encoding string");
    const uint32_t* cps = (const uint32_t*)ds->data;
    int64_t w = 0;
    for (int64_t i = 0; i < ds->len; ++i) {
        w += dragon_utf8_encode_one(cps[i], buf + w);
    }
    buf[w] = '\0';
    if (out_byte_len) *out_byte_len = w;
    return buf;
}

/// Public wire-byte-length helper - returns the UTF-8-encoded byte count.
/// kind=1 strings are already UTF-8 bytes (ds->len == byte count). kind=4
/// strings store one cp per 4-byte slot; the UTF-8 wire length is the sum
/// of utf8_encode_one widths over each cp. String literals (no header) fall
/// back to strlen - they're plain bytes by construction.
///
/// This is the right input to wire-protocol code (HTTP content-length,
/// nb_send length, fwrite count) where the contract is bytes. Returning
/// 4×cp_count for kind=4 (the storage byte count) advertises a wire size
/// that overshoots the actual UTF-8 payload by ~3-4×, leaving the client
/// waiting for bytes that never arrive (curl: `transfer closed with N
/// bytes remaining`). The cp-count from `len()` undershoots in the
/// opposite direction.
int64_t dragon_str_byte_len_pub(const char* s) {
    if (!s) return 0;
    if (!dragon_is_heap_string(s)) return (int64_t)strlen(s);
    DragonString* ds = dragon_string_from_data(s);
    if (ds->kind == 1) return ds->len;
    // kind=4: sum the UTF-8 width per code point. Each cp encodes to 1..4
    // bytes; ASCII (the common case for HTML/JSON wrapping a single
    // multibyte char) stays at 1 byte.
    const uint32_t* cps = (const uint32_t*)ds->data;
    int64_t bytes = 0;
    for (int64_t i = 0; i < ds->len; ++i) {
        uint32_t cp = cps[i];
        if (cp < 0x80) bytes += 1;
        else if (cp < 0x800) bytes += 2;
        else if (cp < 0x10000) bytes += 3;
        else bytes += 4;
    }
    return bytes;
}

/// Identity retain: incref (no-op for literals / immortals) and return `s`.
/// Codegen routes `str(s)`-of-a-str and single-part `f"{s}"` through this so
/// the result is an owned +1 CallInst, matching the calls-return-owned
/// convention (isBorrowedHeapExpr) that assignment / arg-temp / raise
/// consumers assume. Without it those identity paths handed out a borrow
/// that consumers released anyway - over-release, then use-after-free on
/// the next reader (e.g. the exception slot after `msg = str(e)`).
const char* dragon_str_retain(const char* s) {
    dragon_incref_str(s);
    return s;
}

/// Increment refcount of a heap-allocated DragonString.
/// Safely skips string literals (no DragonObjectHeader) via heap validation.
void dragon_incref_str(const char* s) {
    if (!s) return;
    if (!dragon_is_heap_string(s)) return;  // string literal - skip
    DragonString* ds = dragon_string_from_data(s);
    if (dragon_refcount_load(&ds->header) >= DRAGON_IMMORTAL_REFCOUNT) return;  // immortal guard
    // SHARED strings must use atomic ops - see d018-shared-refcount.md.
    if (dragon_gc_flags_load(&ds->header) & GC_FLAG_SHARED) {
        __atomic_fetch_add(&ds->header.refcount, 1, __ATOMIC_RELAXED);
        return;
    }
    ds->header.refcount++;
}

/// Decrement refcount of a heap-allocated DragonString.
/// Safely skips string literals (no DragonObjectHeader) via heap validation.
/// Frees the entire DragonString allocation when refcount reaches 0.
void dragon_decref_str(const char* s) {
    if (!s) return;
    if (!dragon_is_heap_string(s)) return;  // string literal - skip
    DragonString* ds = dragon_string_from_data(s);
    if (dragon_refcount_load(&ds->header) >= DRAGON_IMMORTAL_REFCOUNT) return;  // immortal guard
    // SHARED strings route through the atomic decref path so concurrent
    // mutators on different OS threads don't tear the refcount.
    if (dragon_gc_flags_load(&ds->header) & GC_FLAG_SHARED) {
        if (__atomic_sub_fetch(&ds->header.refcount, 1, __ATOMIC_ACQ_REL) == 0) {
            int collecting = __atomic_load_n(&gc_collecting, __ATOMIC_ACQUIRE);
            if (!collecting || ds->header.gc_track_idx < 0) free(ds);
        }
        return;
    }
    if (--ds->header.refcount == 0) {
        int collecting = __atomic_load_n(&gc_collecting, __ATOMIC_ACQUIRE);
        if (!collecting || ds->header.gc_track_idx < 0) free(ds);
    }
}

// --- Thread-safe atomic variants for strings (Decision 018 Phase 4) ---

void dragon_incref_str_atomic(const char* s) {
    if (!s) return;
    if (!dragon_is_heap_string(s)) return;  // string literal - skip
    DragonString* ds = dragon_string_from_data(s);
    if (dragon_refcount_load(&ds->header) >= DRAGON_IMMORTAL_REFCOUNT) return;  // immortal guard
    // Atomic op implies the string has escaped to another OS thread; mark
    // SHARED so subsequent plain dragon_*_str calls also use atomic ops.
    if (!(dragon_gc_flags_load(&ds->header) & GC_FLAG_SHARED))
        __atomic_fetch_or(&ds->header.gc_flags, GC_FLAG_SHARED, __ATOMIC_RELAXED);
    __atomic_fetch_add(&ds->header.refcount, 1, __ATOMIC_RELAXED);
}

void dragon_decref_str_atomic(const char* s) {
    if (!s) return;
    if (!dragon_is_heap_string(s)) return;  // string literal - skip
    DragonString* ds = dragon_string_from_data(s);
    if (dragon_refcount_load(&ds->header) >= DRAGON_IMMORTAL_REFCOUNT) return;  // immortal guard
    if (!(dragon_gc_flags_load(&ds->header) & GC_FLAG_SHARED))
        __atomic_fetch_or(&ds->header.gc_flags, GC_FLAG_SHARED, __ATOMIC_RELAXED);
    if (__atomic_sub_fetch(&ds->header.refcount, 1, __ATOMIC_ACQ_REL) == 0)
        free(ds);
}

/// Cycle-collector helper. Decrement a string's refcount and free
/// it directly if it hits zero, bypassing `dragon_decref_str`'s
/// `gc_collecting` guard. Used by `dragon_*_clear_refs` while we're tearing
/// down an unreachable cycle: the string is owned exclusively by the
/// container being cleared, so no other thread can resurrect it. Honors
/// the immortal sentinel and string-literal pointers.
void dragon_str_force_free_if_zero(const char* s) {
    if (!s) return;
    if (!dragon_is_heap_string(s)) return;          // string literal - skip
    DragonString* ds = dragon_string_from_data(s);
    if (ds->header.refcount >= DRAGON_IMMORTAL_REFCOUNT) return;   // immortal - never free
    if (--ds->header.refcount == 0) free(ds);
}


//===----------------------------------------------------------------------===//
// String Operations
//===----------------------------------------------------------------------===//

/// Concatenate two strings. Result kind is the minimum that fits all code
/// points (canonical storage so memcmp comparison + identical hashing work).
const char* dragon_str_concat(const char* a, const char* b) {
    // Probe BEFORE substituting "" for a NULL operand: dragon_is_heap_string("")
    // would read the header of a header-less rodata literal out of bounds. The
    // probe is NULL-safe, so feed it the original pointer, then materialize ""
    // for the byte-copy paths (where the corresponding length is already 0).
    DragonString* da = dragon_is_heap_string(a) ? dragon_string_from_data(a) : NULL;
    DragonString* db = dragon_is_heap_string(b) ? dragon_string_from_data(b) : NULL;
    int64_t na = da ? da->len : (a ? (int64_t)strlen(a) : 0);
    int64_t nb = db ? db->len : (b ? (int64_t)strlen(b) : 0);
    if (!a) a = "";
    if (!b) b = "";
    // Guard the addition: na/nb come from user-controlled string lengths.
    // Mirrors str_repeat's hard-fail; callers have no recoverable path.
    if (na < 0 || nb < 0 || na > INT64_MAX - nb) {
        dragon_raise_exc_cstr(43, "MemoryError: string concat too large");
    }
    int64_t total = na + nb;

    // Fast path: both inputs kind=1 (ASCII / Latin-1 bytes). Pure byte copy.
    bool a_kind1 = (!da || da->kind == 1);
    bool b_kind1 = (!db || db->kind == 1);
    if (a_kind1 && b_kind1) {
        DragonString* ds = dragon_string_alloc_raw(total);
        memcpy(ds->data, a, (size_t)na);
        memcpy(ds->data + na, b, (size_t)nb);
        ds->data[total] = '\0';
        return ds->data;
    }
    // At least one operand is kind=4. Build a kind=4 result, then downgrade
    // to kind=1 if the combined max code point fits in 0x7F.
    DragonString* ds = dragon_string_alloc_ucs4(total);
    uint32_t* dst = (uint32_t*)ds->data;
    uint32_t max_cp = 0;
    for (int64_t i = 0; i < na; ++i) {
        uint32_t cp = dragon_str_cp_at(a, da, i);
        dst[i] = cp;
        if (cp > max_cp) max_cp = cp;
    }
    for (int64_t i = 0; i < nb; ++i) {
        uint32_t cp = dragon_str_cp_at(b, db, i);
        dst[na + i] = cp;
        if (cp > max_cp) max_cp = cp;
    }
    if (max_cp < 0x80) {
        // Downgrade to kind=1 for canonical storage.
        DragonString* ds1 = dragon_string_alloc_ascii(total);
        for (int64_t i = 0; i < total; ++i) ds1->data[i] = (char)dst[i];
        // Free the kind=4 buffer eagerly - we never published its pointer.
        free(ds);
        return ds1->data;
    }
    return ds->data;
}

/// Append `b` onto `a`, mutating in place when `a` is uniquely owned.
///
/// OWNERSHIP:
///   - Consumes one reference to `a`. After this call the old `a` pointer is
///     dead; the returned pointer is the new authoritative value of the slot.
///   - Borrows `b` (reads only; no incref, no free).
///
/// FAST PATH (no new DragonString allocated): taken iff `a` is a heap kind=1
/// string with refcount==1, not immortal, not SHARED, distinct from `b`, and
/// `b` is kind=1-appendable (heap kind=1, or a borrowed ASCII literal - non-
/// ASCII literals are interned as kind=4 heap strings). The buffer grows with
/// geometric capacity via realloc only when the existing `cap` can't hold the
/// result; refcount stays 1, so a `while` accumulator loop is amortized O(n).
///
/// FALLBACK (any gate fails - refcount>1 aliasing, immortal, SHARED, a kind=4
/// operand, a==b, or a >2 GiB result): returns dragon_str_concat(a, b) and
/// decrefs `a` once, consuming the input reference. Correct under aliasing:
/// `t = s; s = s + x` forces this path (a's refcount is 2), so t's buffer is
/// never mutated. The result kind stays canonical (concat downgrades kind=4→1
/// when all cps < 0x80), preserving the minimum-kind storage invariant.
const char* dragon_str_append_inplace(const char* a, const char* b) {
    // Do not substitute "" for a NULL operand before probing: the probe is
    // NULL-safe, but dragon_is_heap_string("") would read a header-less rodata
    // literal out of bounds, and so would the fallback decref of that "". Keep
    // `a`/`b` original; dragon_str_concat and the decref family are NULL-safe.
    if (a != b && dragon_is_heap_string(a)) {
        DragonString* da = dragon_string_from_data(a);
        bool a_unique = da->header.refcount == 1
                        && !dragon_is_immortal(da)
                        && !(da->header.gc_flags & GC_FLAG_SHARED);
        if (a_unique && da->kind == 1) {
            DragonString* db = dragon_is_heap_string(b)
                ? dragon_string_from_data(b) : NULL;
            if (!db || db->kind == 1) {
                int64_t na = da->len;                     // kind=1: bytes == cps
                int64_t nb = db ? db->len : (b ? (int64_t)strlen(b) : 0);
                int64_t need = na + nb;                   // total bytes after
                if (need <= 0x7fffffff) {
                    bool ok = true;
                    if (need > (int64_t)da->cap) {
                        // Grow geometrically; realloc may relocate the block.
                        int64_t new_cap = (int64_t)da->cap * 2;
                        if (new_cap < need) new_cap = need;
                        if (new_cap > 0x7fffffff) new_cap = 0x7fffffff;
                        // realloc-into-tmp so a NULL return doesn't orphan `da`
                        // (caller still holds `a` == da->data; fall back to the
                        // fresh-alloc path below, which uses dragon_str_concat).
                        DragonString* tmp = (DragonString*)realloc(
                            da, sizeof(DragonString) + (size_t)new_cap + 1);
                        if (tmp) {
                            da = tmp;
                            da->cap = (int32_t)new_cap;
                        } else {
                            ok = false;
                        }
                    }
                    if (ok) {
                        if (nb) memcpy(da->data + na, b, (size_t)nb);
                        da->len = need;
                        da->data[need] = '\0';
                        return da->data;
                    }
                }
            }
        }
    }

    // Fallback: fresh allocation, then consume `a`'s reference. Uses the
    // dispatch decref so a SHARED `a` is torn down atomically.
    const char* r = dragon_str_concat(a, b);
    dragon_decref_str_dispatch(a);
    return r;
}

/// String length - code-point count (Python `len()` semantics).
/// For heap DragonStrings, returns the precomputed `len` field.
/// For string literals (no DragonString header), falls back to `strlen` -
/// safe because literals are emitted as ASCII / valid UTF-8 *byte* sequences
/// today, but NOTE: literal codegen will switch to emit DragonString-header'd
/// globals in Phase E so that non-ASCII literals also report cp count.
int64_t dragon_str_len(const char* s) {
    if (!s) return 0;
    if (dragon_is_heap_string(s)) {
        return dragon_string_from_data(s)->len;
    }
    return (int64_t)strlen(s);
}

/// Int to string
const char* dragon_int_to_str(int64_t value) {
    // Hot path (str(int) shows up in logging, formatting, dict keys, ...).
    // Hand-rolled itoa straight into the string buffer beats snprintf("%ld")
    // ~4× (no format-string parse / locale handling), and we allocate the
    // kind=1 buffer directly so we also skip the non-ASCII rescan that the
    // generic dragon_string_alloc would do - digits and '-' are always ASCII.
    char tmp[20];                 // INT64_MIN = "-9223372036854775808" = 20 chars
    int n = 0;
    // Negate into unsigned so INT64_MIN (no positive counterpart) is handled.
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
    while (n) d[i++] = tmp[--n];  // emit digits most-significant first
    return s->data;
}

/// Format `value` into `buf` exactly as Python's `repr(float)` would: the
/// shortest decimal string that round-trips to the same double, always
/// carrying a fractional part or exponent so it reads back as a float
/// (`1.0`, not `1`). Returns the written length. `buf` must be >= 32 bytes.
///
/// Single source of truth for ALL float formatting (scalar print, str(),
/// f-strings, and container repr) so there is exactly one notion of how a
/// Dragon float prints.
int dragon_format_double_into(double value, char* buf, size_t bufsz) {
    if (std::isnan(value)) return snprintf(buf, bufsz, "nan");
    if (std::isinf(value)) return snprintf(buf, bufsz, value < 0 ? "-inf" : "inf");

    // Shortest significant-digit count P in [1,17] that round-trips. `%e`
    // always yields P-1 fractional digits => exactly P significant digits,
    // free of the trailing zeros %g would invent at a fixed precision.
    char ebuf[40];
    int P = 17;
    for (int p = 1; p <= 17; p++) {
        snprintf(ebuf, sizeof(ebuf), "%.*e", p - 1, value);
        if (strtod(ebuf, nullptr) == value) { P = p; break; }
    }
    // Decimal exponent of the leading digit (parsed from the shortest %e form).
    snprintf(ebuf, sizeof(ebuf), "%.*e", P - 1, value);
    const char* epos = strchr(ebuf, 'e');
    int exp10 = epos ? atoi(epos + 1) : 0;

    int len;
    // Python repr uses fixed notation for -4 <= exp < 16, else exponential.
    if (exp10 >= -4 && exp10 < 16) {
        int frac = (P - 1) - exp10;        // fractional digits to reach P sig figs
        if (frac < 0) frac = 0;
        len = snprintf(buf, bufsz, "%.*f", frac, value);
        if (frac == 0 && (size_t)len + 2 < bufsz) {
            buf[len++] = '.';              // integer-valued => append ".0"
            buf[len++] = '0';
            buf[len] = '\0';
        }
    } else {
        // `%e` already prints lowercase 'e', a sign, and >= 2 exponent digits,
        // matching Python (e.g. 1e+16, 1e-05).
        len = snprintf(buf, bufsz, "%.*e", P - 1, value);
    }
    return len;
}

/// Float to string
const char* dragon_float_to_str(double value) {
    char tmp[64];
    int len = dragon_format_double_into(value, tmp, sizeof(tmp));
    return dragon_string_alloc(tmp, len);
}

//===----------------------------------------------------------------------===//
// Python format mini-language
//
//   [[fill]align][sign][#][0][width][grouping][.precision][type]
//
// Implemented end-to-end (no printf passthrough of the user's spec) so
// alignment (< > ^ =), fill, sign (+ - space), '#' alt-form, '0'-padding,
// ',' / '_' grouping and the '%' percent type all match CPython. Because the
// user's spec text never becomes a printf format string (we build fixed
// directives like "%.*f" and pass the parsed precision as an int argument),
// this is also inherently free of the format-string-injection risk the old
// validator existed to guard against.
//===----------------------------------------------------------------------===//

typedef struct {
    char fill;       // pad character (default ' ')
    char align;      // '<' '>' '^' '=' or 0 (caller picks default)
    char sign;       // '+', '-' (default), or ' '
    int  alt;        // '#'
    long width;      // minimum field width
    char grouping;   // ',' '_' or 0
    long precision;  // -1 == unset
    char type;       // conversion char, 0 == unset
} DragonFmtSpec;

static int dragon_fmt_is_align(char c) {
    return c == '<' || c == '>' || c == '^' || c == '=';
}

// Parse `spec` (length `slen`) into `o`. Returns 1 on success, 0 if malformed.
static int dragon_parse_fmt_spec(const char* spec, size_t slen, DragonFmtSpec* o) {
    o->fill = ' '; o->align = 0; o->sign = '-'; o->alt = 0;
    o->width = 0; o->grouping = 0; o->precision = -1; o->type = 0;
    size_t i = 0;
    // [[fill]align] - a fill char is only consumed when followed by an align.
    if (slen - i >= 2 && dragon_fmt_is_align(spec[i + 1])) {
        o->fill = spec[i]; o->align = spec[i + 1]; i += 2;
    } else if (slen - i >= 1 && dragon_fmt_is_align(spec[i])) {
        o->align = spec[i]; i += 1;
    }
    // [sign]
    if (i < slen && (spec[i] == '+' || spec[i] == '-' || spec[i] == ' ')) { o->sign = spec[i]; i++; }
    // [#]
    if (i < slen && spec[i] == '#') { o->alt = 1; i++; }
    // [0] - leading zero implies '=' align with '0' fill unless align given.
    if (i < slen && spec[i] == '0') {
        if (!o->align) { o->align = '='; o->fill = '0'; }
        i++;
    }
    // [width]
    while (i < slen && spec[i] >= '0' && spec[i] <= '9') {
        o->width = o->width * 10 + (spec[i] - '0');
        if (o->width > 1000000) return 0;  // absurd width - reject
        i++;
    }
    // [grouping]
    if (i < slen && (spec[i] == ',' || spec[i] == '_')) { o->grouping = spec[i]; i++; }
    // [.precision]
    if (i < slen && spec[i] == '.') {
        i++;
        if (i >= slen || spec[i] < '0' || spec[i] > '9') return 0;  // '.' requires digits
        o->precision = 0;
        while (i < slen && spec[i] >= '0' && spec[i] <= '9') {
            o->precision = o->precision * 10 + (spec[i] - '0');
            if (o->precision > 1000000) return 0;
            i++;
        }
    }
    // [type]
    if (i < slen) {
        if (!strchr("bcdoxXeEfFgGn%", spec[i])) return 0;
        o->type = spec[i];
        i++;
    }
    return i == slen;  // must consume the entire spec
}

// Insert `sep` every `group` chars from the right of digit-string `in` into
// `out` (caller sizes `out`). `in` is pure digits (no sign/prefix).
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

// Pad `prefix`+`body` to `o->width` honouring align/fill. `prefix` holds the
// sign and any alt marker (0x/0o/0b); `body` holds the magnitude digits so the
// '=' alignment can slot the fill between them. Returns a fresh Dragon string.
static const char* dragon_fmt_pad(const DragonFmtSpec* o, const char* prefix, const char* body) {
    size_t plen = strlen(prefix), blen = strlen(body);
    size_t total = plen + blen;
    long width = o->width;
    size_t pad = (long)total >= width ? 0 : (size_t)(width - (long)total);
    char* buf = (char*)malloc(total + pad + 1);
    char align = o->align ? o->align : '>';   // numbers default to right-align
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
    } else { // '>'
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

/// Format a float using the full Python format mini-language (".2f", "e",
/// ">10.3g", "+,.2f", ".1%", ...).
const char* dragon_float_format(double value, const char* spec) {
    if (!spec || !*spec) return dragon_float_to_str(value);
    size_t slen = strlen(spec);
    DragonFmtSpec o;
    if (!dragon_parse_fmt_spec(spec, slen, &o)) return dragon_fmt_error();

    char type = o.type;
    if (type == 'n') type = 'g';        // locale 'n' ≈ 'g' (no locale grouping)
    int percent = 0;
    double v = value;
    if (type == '%') { percent = 1; v = value * 100.0; type = 'f'; }

    int neg = std::signbit(v);
    double av = neg ? -v : v;
    int nan_inf = std::isnan(av) || std::isinf(av);
    // nan/inf never zero-pad - Python space-pads and right-aligns them.
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
        heapmag = (char*)malloc(magcap);
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

    // Grouping on the integer part (decimal notation only - never with e/E).
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
            grpbuf = (char*)malloc(cap2);
            snprintf(grpbuf, cap2, "%s%s", grouped, mag + ilen);
            finalmag = grpbuf;
        }
    }

    char* withpct = NULL;
    if (percent) {
        size_t cap3 = strlen(finalmag) + 2;
        withpct = (char*)malloc(cap3);
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

/// Format an int using the full Python format mini-language ("d", "x", "08b",
/// ">6", "+,d", "#06x", ...). Float-presentation types (e/f/g/%) convert to
/// float and delegate.
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
    if (type != 'c' && o.precision >= 0) return dragon_fmt_error();  // int + precision invalid

    // Magnitude via unsigned to handle INT64_MIN without overflow.
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

    // Grouping: ',' / '_' every 3 (decimal); '_' every 4 (x/X/o/b).
    char grouped[160];
    if (o.grouping && (type == 'd' || type == 'n')) {
        dragon_fmt_group(digits, o.grouping, 3, grouped);
    } else if (o.grouping == '_' && (type == 'x' || type == 'X' || type == 'o' || type == 'b')) {
        dragon_fmt_group(digits, '_', 4, grouped);
    } else if (o.grouping == ',') {
        return dragon_fmt_error();  // ',' only valid for decimal
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

/// String equality. For canonical-kind storage (every alloc picks min kind)
/// memcmp on equal-kind strings would suffice, but we may have transient
/// non-canonical strings during concat/replace etc. - compare code points.
int64_t dragon_str_eq(const char* a, const char* b) {
    if (a == b) return 1;
    if (!a || !b) return 0;
    DragonString* da = dragon_is_heap_string(a) ? dragon_string_from_data(a) : NULL;
    DragonString* db = dragon_is_heap_string(b) ? dragon_string_from_data(b) : NULL;
    int64_t la = da ? da->len : (int64_t)strlen(a);
    int64_t lb = db ? db->len : (int64_t)strlen(b);
    if (la != lb) return 0;
    bool a_k1 = (!da || da->kind == 1);
    bool b_k1 = (!db || db->kind == 1);
    // Both kind=1: compare all `la` bytes with memcmp, NOT strcmp. strcmp stops
    // at the first NUL, so two equal-length kind=1 strings that share a prefix
    // up to an embedded NUL but differ afterwards - e.g. "ab\0cd" vs "ab\0ce" -
    // wrongly compared equal (and comparing a token/hash byte string this way
    // could accept a wrong secret). Lengths already match, and each buffer holds
    // la bytes + a terminator, so memcmp neither under- nor over-reads.
    if (a_k1 && b_k1) return memcmp(a, b, (size_t)la) == 0 ? 1 : 0;
    for (int64_t i = 0; i < la; ++i) {
        if (dragon_str_cp_at(a, da, i) != dragon_str_cp_at(b, db, i)) return 0;
    }
    return 1;
}

/// Constant-time string equality for security-sensitive comparisons (auth
/// tokens, HMAC/MAC tags, password hashes). Unlike dragon_str_eq (which uses
/// strcmp and early-exits on the first differing byte), this never branches on
/// content and never early-exits, so an attacker timing the response cannot
/// learn the shared-prefix length of a secret. Compares the UTF-8 wire bytes
/// (kind-safe) in time proportional to `b`'s length, folding in any length
/// mismatch - the established compare_digest pattern. Backs hmac.compare_digest.
int64_t dragon_str_eq_const(const char* a, const char* b) {
    if (!a || !b) return (a == b) ? 1 : 0;
    int64_t la = 0, lb = 0;
    char* oa = dragon_str_to_utf8_alloc(a, &la);
    char* ob = dragon_str_to_utf8_alloc(b, &lb);
    const char* ba = oa ? oa : a;
    const char* bb = ob ? ob : b;
    // result starts nonzero iff lengths differ; the loop then ORs in every
    // per-byte difference. No data-dependent branch or early return.
    volatile unsigned int result = (la == lb) ? 0u : 1u;
    for (int64_t i = 0; i < lb; i++) {
        // Index into `a` modulo its length so a shorter/empty `a` never reads
        // out of bounds while the loop count stays a function of `b` alone.
        unsigned char ca = (la > 0) ? (unsigned char)ba[i % la] : 0;
        result |= (unsigned int)(ca ^ (unsigned char)bb[i]);
    }
    if (oa) free(oa);
    if (ob) free(ob);
    return result == 0u ? 1 : 0;
}

int64_t dragon_str_cmp(const char* a, const char* b) {
    if (!a && !b) return 0;
    if (!a) return -1;
    if (!b) return 1;
    DragonString* da = dragon_is_heap_string(a) ? dragon_string_from_data(a) : NULL;
    DragonString* db = dragon_is_heap_string(b) ? dragon_string_from_data(b) : NULL;
    int64_t la = da ? da->len : (int64_t)strlen(a);
    int64_t lb = db ? db->len : (int64_t)strlen(b);
    bool a_k1 = (!da || da->kind == 1);
    bool b_k1 = (!db || db->kind == 1);
    // Both kind=1: memcmp over the shorter length + length tiebreak, NOT strcmp.
    // strcmp would stop ordering at an embedded NUL (same class as the eq bug
    // above). memcmp compares raw bytes lexicographically, which for kind=1
    // (byte==code point) is the correct code-point ordering.
    if (a_k1 && b_k1) {
        int64_t n0 = la < lb ? la : lb;
        int c = memcmp(a, b, (size_t)n0);
        if (c != 0) return c < 0 ? -1 : 1;
        return la == lb ? 0 : (la < lb ? -1 : 1);
    }
    int64_t n = la < lb ? la : lb;
    for (int64_t i = 0; i < n; ++i) {
        uint32_t ca = dragon_str_cp_at(a, da, i);
        uint32_t cb = dragon_str_cp_at(b, db, i);
        if (ca < cb) return -1;
        if (ca > cb) return 1;
    }
    return la == lb ? 0 : (la < lb ? -1 : 1);
}


//===----------------------------------------------------------------------===//
// Type Conversions
//===----------------------------------------------------------------------===//

/// String to int
/// int(str) - Python-parity strict parse (base 10).
///
/// Accepts: optional surrounding ASCII whitespace, an optional +/- sign, one
/// or more decimal digits, with single underscores permitted BETWEEN digits
/// (`1_000` → 1000) - never leading, trailing, or doubled. Anything else
/// raises ValueError (code 90), matching CPython's
/// "invalid literal for int() with base 10: '...'". Out-of-i64-range input
/// raises OverflowError (code 22) - Dragon ints are i64, not bignums.
///
/// Replaces the old lenient `atol` (which silently returned 0 on garbage).
/// All internal stdlib callers (json/tomllib/drs lexers, http param_int via
/// _is_int_str) pre-validate, so the stricter contract doesn't break them.
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
    // Accumulate magnitude in u64; bound against the signed i64 range,
    // allowing exactly -2^63 (whose magnitude is 2^63).
    const uint64_t kMagLimit = (sign < 0) ? 9223372036854775808ULL   // 2^63
                                          : 9223372036854775807ULL;  // 2^63 - 1
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
            // Single underscore allowed only between digits.
            if (!any_digit || prev_underscore) return fail(s);
            prev_underscore = true;
            p++;
        } else {
            break;
        }
    }
    if (prev_underscore) return fail(s);   // trailing underscore
    while (is_ws(*p)) p++;
    if (*p != '\0' || !any_digit) return fail(s);
    if (overflow) {
        snprintf(tls_msg, sizeof(tls_msg),
                 "OverflowError: int('%s') exceeds the 64-bit integer range", s);
        dragon_raise_exc_cstr(22, tls_msg);
        return 0;
    }
    // Negate via unsigned wraparound so acc == 2^63 yields INT64_MIN cleanly
    // (a signed `-(int64_t)acc` would be UB at the boundary).
    if (sign < 0) return (int64_t)(0ULL - acc);
    return (int64_t)acc;
}

/// String to float
double dragon_str_to_float(const char* s) {
    return s ? atof(s) : 0.0;
}


//===----------------------------------------------------------------------===//
// String Indexing
//===----------------------------------------------------------------------===//

/// Get character at index as a new string (supports negative indexing)
const char* dragon_str_index(const char* s, int64_t index) {
    if (!s) {
        dragon_raise_exc_cstr(41, "IndexError: string index out of range");
    }
    DragonString* ds = dragon_is_heap_string(s) ? dragon_string_from_data(s) : NULL;
    int64_t cp_count = ds ? ds->len : (int64_t)strlen(s);
    if (index < 0) index += cp_count;
    if (index < 0 || index >= cp_count) {
        dragon_raise_exc_cstr(41, "IndexError: string index out of range");
    }
    if (!ds || ds->kind == 1) {
        // ASCII fast path: byte index == cp index, single-byte result.
        char ch = s[index];
        return dragon_string_alloc(&ch, 1);
    }
    // kind=4: pull the code point and build a 1-cp result with min kind.
    uint32_t cp = ((const uint32_t*)ds->data)[index];
    if (cp < 0x80) {
        char ch = (char)cp;
        return dragon_string_alloc(&ch, 1);
    }
    DragonString* out = dragon_string_alloc_ucs4(1);
    ((uint32_t*)out->data)[0] = cp;
    return out->data;
}

//===----------------------------------------------------------------------===//
// Bool to String (for f-strings)
//===----------------------------------------------------------------------===//

const char* dragon_bool_to_str(int64_t value) {
    return value ? dragon_string_alloc("True", 4) : dragon_string_alloc("False", 5);
}


} // extern "C"
