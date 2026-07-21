/// Dragon Runtime - shared low-level string helpers
///
/// Used by both runtime_string.cpp (alloc / encode / RC / concat / format)
/// and runtime_string_methods.cpp (case ops, strip, find, slice, split, ...).
/// `static inline` so each TU gets a local copy - pure code motion from
/// runtime_string.cpp, no behavior change.
#ifndef DRAGON_RUNTIME_STRING_SHARED_H
#define DRAGON_RUNTIME_STRING_SHARED_H

#include "runtime_internal.h"

/// Allocate an ASCII/Latin-1 (kind=1) DragonString of `cp_count` bytes.
/// Caller fills `data`.
static inline DragonString* dragon_string_alloc_ascii(int64_t cp_count) {
    // Guard negative / overflowing lengths (_ascii lacked the guard
    // _ucs4 has). A negative cp_count wraps sizeof+cp_count to a huge size_t and
    // the data[cp_count] write would be out of bounds.
    if (cp_count < 0 || cp_count > INT64_MAX - (int64_t)sizeof(DragonString) - 1) {
        dragon_raise_exc_cstr(43, "MemoryError: string too large");
    }
    // dragon_xmalloc raises MemoryError on NULL instead of letting the deref
    // below SIGSEGV on OOM.
    DragonString* s = (DragonString*)dragon_xmalloc(sizeof(DragonString) + (size_t)cp_count + 1);
    dragon_obj_init(&s->header, DRAGON_TAG_STR);
    s->len = cp_count;
    s->kind = 1;
    s->cap = dragon_cap_clamp(cp_count);  // kind=1: byte count == cp count
    s->data[cp_count] = '\0';
    return s;
}

/// Allocate a UCS-4 (kind=4) DragonString of `cp_count` code points.
/// Caller fills `data` (treated as a uint32_t[cp_count]).
static inline DragonString* dragon_string_alloc_ucs4(int64_t cp_count) {
    // Guard the cp_count * 4 multiplication: attacker-controlled lengths
    // (e.g. via huge string concats) must not wrap. Mirror str_repeat's
    // canonical hard-fail; the caller has no recoverable path.
    if (cp_count < 0 || cp_count > INT64_MAX / 4) {
        dragon_raise_exc_cstr(43, "MemoryError: string too large");
    }
    int64_t bytes = cp_count * 4;
    // +1 byte tail to allow consistent NUL probing; not a valid C string.
    // dragon_xmalloc raises MemoryError on NULL (was an unchecked deref -> SEGV
    // on OOM).
    DragonString* s = (DragonString*)dragon_xmalloc(sizeof(DragonString) + bytes + 1);
    dragon_obj_init(&s->header, DRAGON_TAG_STR);
    s->len = cp_count;
    s->kind = 4;
    s->cap = dragon_cap_clamp(bytes);  // kind=4: 4 bytes per cp
    s->data[bytes] = '\0';
    return s;
}

/// Simple (length-preserving, locale-independent) Unicode case mapping for the
/// common BMP letter ranges, computed algorithmically - no tables. Covers
/// Latin-1 Supplement, Latin Extended-A, Greek, and Cyrillic. Code points with
/// no simple mapping, or whose mapping is length-changing / locale-dependent,
/// are returned unchanged. The Turkish dotted/dotless I pair (U+0130/U+0131) is
/// deliberately excluded: auto-folding it the ASCII way is exactly the
/// cross-locale account-confusion hazard, and its correct mapping is
/// length-changing. ASCII A-Z/a-z is handled by the callers before this runs.
static inline uint32_t dragon_cp_simple_upper(uint32_t cp) {
    // Latin-1 Supplement: à-þ (skip ÷ at 0xF7) -> À-Þ
    if ((cp >= 0x00E0 && cp <= 0x00F6) || (cp >= 0x00F8 && cp <= 0x00FE)) return cp - 0x20;
    if (cp == 0x00FF) return 0x0178;                       // ÿ -> Ÿ
    if (cp == 0x00B5) return 0x039C;                       // µ -> Greek capital Mu
    // Latin Extended-A: even=upper/odd=lower sub-blocks
    if ((cp >= 0x0100 && cp <= 0x012F) || (cp >= 0x0132 && cp <= 0x0137) ||
        (cp >= 0x014A && cp <= 0x0177))
        return (cp & 1) ? cp - 1 : cp;
    // Latin Extended-A: odd=upper/even=lower sub-blocks
    if ((cp >= 0x0139 && cp <= 0x0148) || (cp >= 0x0179 && cp <= 0x017E))
        return (cp & 1) ? cp : cp - 1;
    // Greek: α-ω (skip reserved) -> Α-Ω
    if (cp == 0x03C2) return 0x03A3;                       // final sigma ς -> Σ
    if ((cp >= 0x03B1 && cp <= 0x03C1) || (cp >= 0x03C3 && cp <= 0x03CB)) return cp - 0x20;
    // Cyrillic
    if (cp >= 0x0430 && cp <= 0x044F) return cp - 0x20;
    if (cp >= 0x0450 && cp <= 0x045F) return cp - 0x50;
    return cp;
}
static inline uint32_t dragon_cp_simple_lower(uint32_t cp) {
    // Latin-1 Supplement: À-Þ (skip × at 0xD7) -> à-þ
    if ((cp >= 0x00C0 && cp <= 0x00D6) || (cp >= 0x00D8 && cp <= 0x00DE)) return cp + 0x20;
    if (cp == 0x0178) return 0x00FF;                       // Ÿ -> ÿ
    // Latin Extended-A: even=upper/odd=lower sub-blocks
    if ((cp >= 0x0100 && cp <= 0x012F) || (cp >= 0x0132 && cp <= 0x0137) ||
        (cp >= 0x014A && cp <= 0x0177))
        return (cp & 1) ? cp : cp + 1;
    // Latin Extended-A: odd=upper/even=lower sub-blocks
    if ((cp >= 0x0139 && cp <= 0x0148) || (cp >= 0x0179 && cp <= 0x017E))
        return (cp & 1) ? cp + 1 : cp;
    // Greek: Α-Ω -> α-ω (skip reserved 0x03A2)
    if ((cp >= 0x0391 && cp <= 0x03A1) || (cp >= 0x03A3 && cp <= 0x03AB)) return cp + 0x20;
    // Cyrillic
    if (cp >= 0x0410 && cp <= 0x042F) return cp + 0x20;
    if (cp >= 0x0400 && cp <= 0x040F) return cp + 0x50;
    return cp;
}

/// Check if a string pointer is a valid heap-allocated DragonString.
/// Uses the GC_FLAG_HEAP_OBJ sentinel set by dragon_obj_init.
///
/// Delegates to the single, NULL-guarded implementation in runtime_internal.h
/// (`dragon_str_is_heap`). Keeping two copies let this one drift without the
/// `if (!s)` guard, so a NULL slot (e.g. an uninitialized / raced green-thread
/// string) computed `s - 32` and read the header out of bounds near NULL. One
/// source of truth now; never probe a pointer that isn't NULL or a real
/// DragonString `data` pointer (a header-less C literal would read OOB).
static inline bool dragon_is_heap_string(const char* s) {
    return dragon_str_is_heap(s) != 0;
}

/// Get the i-th code point of a string. Handles kind=1 (byte) / kind=4 / and
/// borrowed string-literal pointers (treated as kind=1).
static inline uint32_t dragon_str_cp_at(const char* s, DragonString* ds, int64_t i) {
    if (!ds || ds->kind == 1) return (uint32_t)(unsigned char)s[i];
    return ((const uint32_t*)ds->data)[i];
}

#endif  // DRAGON_RUNTIME_STRING_SHARED_H
