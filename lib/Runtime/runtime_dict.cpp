/// Dragon Runtime - Dict Operations
#include "runtime_internal.h"

extern "C" {
// tag-gated closure decref (runtime_builtins.cpp) - a dict[*, Callable]
// value (entry tag == DRAGON_TAG_CLOSURE) may be a real DragonClosure or a bare
// fn ptr; this frees the former (+ env) and no-ops on the latter.
void dragon_decref_callable(void* p);

// C5 / Any-box: tag-aware printers for container-valued dict entries (a
// dict[str, Any] / dict[*, list|dict] value). Defined in runtime_collections.cpp
// (nested) and runtime_list.cpp (box). Without these the plain dict printers
// fell through to the `%ld` default and rendered a nested list/dict value as a
// raw pointer integer.
void dragon_print_list_nested_raw(DragonList* l);
void dragon_print_dict_nested_raw(DragonDict* d);
void dragon_print_list_box_raw(DragonListBox* l);

// Defined in runtime_builtins.cpp - class-id -> name for `<Name instance>`.
const char* dragon_instance_class_name(void* instance);

// Print one tag-7 dict value. TAG_BYTES == TAG_CLASS (both are generic
// refcounted heap objects), so the tag alone cannot tell a DragonBytes from a
// class instance - gate on the real header exactly like dragon_print_box_raw
// (which fixed the same blind cast: printing an instance as fake bytes read
// hundreds of KB out of bounds).
static void dict_print_bytes_or_instance(int64_t value) {
    DragonObjectHeader* h = (DragonObjectHeader*)(uintptr_t)value;
    if (h && h->type_tag == DRAGON_TAG_BYTES) {
        auto* bv = (DragonBytes*)h;
        printf("b'");
        for (int64_t bi = 0; bi < bv->len; bi++) {
            uint8_t c = bv->data[bi];
            if (c >= 32 && c < 127 && c != '\\' && c != '\'') printf("%c", c);
            else if (c == '\\') printf("\\\\");
            else if (c == '\'') printf("\\'");
            else printf("\\x%02x", c);
        }
        printf("'");
    } else if (!h) {
        printf("None");
    } else {
        const char* nm = dragon_instance_class_name(h);
        if (nm) printf("<%s instance>", nm);
        else    printf("<object at 0x%llx>", (unsigned long long)value);
    }
}

// Print one container-valued dict entry (TAG_LIST / TAG_DICT) tag-aware,
// mirroring dragon_repr_value's header dispatch. Shared by both dict printers.
static void dict_print_container_value(int64_t tag, int64_t value) {
    if (tag == TAG_DICT) {
        dragon_print_dict_nested_raw((DragonDict*)(uintptr_t)value);
        return;
    }
    // TAG_LIST: the payload is a monomorphic DragonList or a DragonListBox.
    DragonObjectHeader* h = (DragonObjectHeader*)(uintptr_t)value;
    if (h && h->type_tag == DRAGON_TAG_LIST_BOX)
        dragon_print_list_box_raw((DragonListBox*)h);
    else
        dragon_print_list_nested_raw((DragonList*)h);
}

//--- Hash table dict with insertion-order preservation (CPython compact design)
// Dense entries array stores {hash, key, value, tag} in insertion order.
// Sparse index table maps hash → entry index for O(1) lookup.
// Empty index slots = -1, tombstone slots = -2 (for deletion).

static const int64_t DICT_EMPTY = -1;
static const int64_t DICT_TOMBSTONE = -2;



// Keyed string hash (SipHash-1-3, per-process random key - HashDoS defense).
// Delegates to the shared dragon_str_content_hash so dict and set use the same
// keyed function. Uses `dragon_str_total_bytes` so kind=4 strings hash all of
// their UCS-4 bytes (not just up to the first internal NUL byte in a code
// point's low byte) and so equal-content strings of the same kind hash
// identically - relying on canonical-kind storage from `dragon_string_alloc` /
// `dragon_str_concat` / `dragon_str_replace`.
static uint64_t dict_hash(const char* key) {
    return dragon_str_content_hash(key);
}

static int64_t next_power_of_2(int64_t n) {
    int64_t p = 8;
    while (p < n) p <<= 1;
    return p;
}

// Find the index-table slot for a key. Returns the slot position.
// If the key exists, indices[slot] is the entry index (>= 0).
// If not found, indices[slot] is DICT_EMPTY or DICT_TOMBSTONE (insertion point).
static int64_t dict_probe(DragonDict* d, const char* key, uint64_t h) {
    int64_t mask = d->index_size - 1;
    int64_t slot = (int64_t)(h & (uint64_t)mask);
    int64_t first_tombstone = -1;
    for (;;) {
        int64_t idx = d->indices[slot];
        if (idx == DICT_EMPTY) {
            return (first_tombstone >= 0) ? first_tombstone : slot;
        }
        if (idx == DICT_TOMBSTONE) {
            if (first_tombstone < 0) first_tombstone = slot;
        } else {
            // Live entry - compare hash first (fast reject), then byte-equal.
            // Kind-aware: canonical storage guarantees same-content keys have
            // the same kind so `total_bytes` + memcmp is a safe equality test
            // (and works for kind=4 strings whose data contains NUL bytes).
            if (d->entries[idx].hash == h) {
                int64_t la = dragon_str_total_bytes(d->entries[idx].key);
                int64_t lb = dragon_str_total_bytes(key);
                if (la == lb && (la == 0 || memcmp(d->entries[idx].key, key, (size_t)la) == 0)) {
                    return slot;
                }
            }
        }
        slot = (slot + 1) & mask;
    }
}

// Forward decl: the int-keyed probe is defined further down, but popitem (which
// handles both key flavors) needs it to invalidate an int key's index slot.
static int64_t dict_probe_i64(DragonDict* d, int64_t key, uint64_t h);

// Rebuild the index table (after resize or compaction). Skips dead dense
// slots so a rebuild that runs while tombstones are still present (e.g. from
// dict_grow) never indexes a removed entry. Clearing + re-probing every live
// entry also drops all index-table tombstones, restoring O(1) probe length.
static void dict_rebuild_index(DragonDict* d) {
    for (int64_t i = 0; i < d->index_size; i++) {
        d->indices[i] = DICT_EMPTY;
    }
    int64_t mask = d->index_size - 1;
    for (int64_t i = 0; i < d->size; i++) {
        if (d->entries[i].dead) continue;
        int64_t slot = (int64_t)(d->entries[i].hash & (uint64_t)mask);
        while (d->indices[slot] != DICT_EMPTY) {
            slot = (slot + 1) & mask;
        }
        d->indices[slot] = i;
    }
}

// Reclaim dead dense slots: slide live entries down to fill the gaps (stable,
// so insertion order is preserved), shrink `size` to `used`, and rebuild the
// index (which also clears its tombstones). O(size), but only triggered after
// enough deletes have accumulated that it is O(1) amortized per delete.
static void dict_compact(DragonDict* d) {
    int64_t w = 0;
    for (int64_t r = 0; r < d->size; r++) {
        if (d->entries[r].dead) continue;
        if (w != r) d->entries[w] = d->entries[r];
        w++;
    }
    d->size = w;  // == d->used
    dict_rebuild_index(d);
}

// Compact when at least half the dense slots are dead. Bounds both the dense
// array (< 2x live) and the index-table tombstone density (< 50%), so a
// delete-heavy workload can't degrade lookups into an O(n) tombstone walk.
// The 16-slot floor avoids churn on tiny dicts.
static inline void dict_maybe_compact(DragonDict* d) {
    if (d->size >= 16 && d->used * 2 < d->size) dict_compact(d);
}

// Grow entries + index when needed
static void dict_grow(DragonDict* d) {
    // Double entries capacity - realloc into a temp first so a NULL return
    // doesn't leak the live buffer or NULL-deref on the next access.
    int64_t new_cap = d->capacity * 2;
    DictEntry* etmp = (DictEntry*)realloc(d->entries, new_cap * sizeof(DictEntry));
    if (!etmp) { fprintf(stderr, "dragon: out of memory\n"); abort(); }
    d->entries = etmp;
    d->capacity = new_cap;
    // Double index size and rebuild
    int64_t new_isz = d->index_size * 2;
    int64_t* itmp = (int64_t*)realloc(d->indices, new_isz * sizeof(int64_t));
    if (!itmp) { fprintf(stderr, "dragon: out of memory\n"); abort(); }
    d->indices = itmp;
    d->index_size = new_isz;
    dict_rebuild_index(d);
}

DragonDict* dragon_dict_new(int64_t cap) {
    if (cap < 4) cap = 4;
    auto* d = (DragonDict*)malloc(sizeof(DragonDict));
    dragon_obj_init(&d->header, DRAGON_TAG_DICT);
    d->size = 0;
    d->used = 0;
    d->keys_are_ptr = 0;     // flipped to 1 by the str setter; int-keyed dicts stay 0
    d->capacity = cap;
    // Index table is 2x entries capacity (load factor ~0.5 for good probe performance)
    d->index_size = next_power_of_2(cap * 2);
    d->entries = (DictEntry*)malloc(d->capacity * sizeof(DictEntry));
    d->indices = (int64_t*)malloc(d->index_size * sizeof(int64_t));
    for (int64_t i = 0; i < d->index_size; i++) d->indices[i] = DICT_EMPTY;
    // Acyclic-skip: created UNTRACKED. The set-tagged paths enroll the dict in
    // cycle tracking on the first traceable value; a leaf-only dict (int/float/
    // bool/str values, with str or int keys) is never tracked - it can't form a
    // cycle. The alloc-counter bump stays UNCONDITIONAL (cadence unchanged).
    if (__atomic_add_fetch(&gc_alloc_counter, 1, __ATOMIC_RELAXED)
        >= __atomic_load_n(&gc_threshold, __ATOMIC_RELAXED)) {
        dragon_gc_collect();
    }
    return d;
}

// Release one owned str key (the dict owns one DragonString ref per key, per
// the codegen contract in src/codegen/Assign.cpp). No-op for: int-keyed dicts
// (keys_are_ptr==0 - the slot is an i64, never touch it), NULL, borrowed
// C-string literals, and interned/immortal keys - all handled by
// dragon_decref_str's heap/immortal guards. Frees only dup'd/incref'd heap keys.
static inline void dragon_dict_release_key(const DragonDict* d, const char* key) {
    if (!d->keys_are_ptr) return;
    if (key) dragon_decref_str_dispatch(key);
}

void dragon_dict_set_tagged(DragonDict* d, const char* key, int64_t value, int64_t tag) {
    // Concurrent-mutation detector (runtime_internal.h): whole op is the
    // window, like Go's hashWriting. No raise can occur below; ends before
    // the old-value decrefs (update branch) / at function exit (insert).
    bool mut_armed = dragon_shared_mut_begin(&d->header, "dict");
    // This is the str-key path: the dict owns one ref per key (see
    // dragon_dict_release_key). Mark the dict str-keyed so destroy/clear/del
    // release keys; int-keyed dicts go through dragon_dict_int_set_tagged and
    // never flip this bit, so their i64 keys are never decref'd as strings.
    d->keys_are_ptr = 1;
    // Acyclic-skip enrollment: enroll the dict in cycle tracking the first time a
    // traceable (list/dict/bytes/instance) VALUE is inserted, before the store.
    // str keys are leaves, so only a traceable value can make the dict cyclic.
    // dragon_value_tag_is_traceable is shared with dragon_dict_traverse so the
    // gate and the collector's follow-set can never diverge.
    if (value && dragon_value_tag_is_traceable((int8_t)tag) &&
        !(d->header.gc_flags & GC_FLAG_TRACKED)) {
        dragon_gc_track(d);
    }
    uint64_t h = dict_hash(key);
    int64_t slot = dict_probe(d, key, h);
    int64_t idx = d->indices[slot];

    // Write barrier: a SHARED dict propagates SHARED to its key (always a
    // str) and to the new value (if heap-typed). Done before either branch
    // so it covers update-in-place and new-insert.
    bool dict_shared = (d->header.gc_flags & GC_FLAG_SHARED) != 0;
    if (dict_shared) {
        if (key) dragon_mark_shared_str(key);
        if (value) {
            if (tag == TAG_STR)
                dragon_mark_shared_str((const char*)(uintptr_t)value);
            else if (tag == TAG_LIST || tag == TAG_DICT || tag == TAG_BYTES)
                dragon_mark_shared_deep((void*)(uintptr_t)value);
        }
    }

    if (idx >= 0) {
        // Update-in-place: the dict keeps its EXISTING stored key and does NOT
        // adopt the incoming one, but the caller (codegen) added exactly one
        // owned ref to `key`. Drop that redundant incoming ref (no-op for
        // borrowed-literal/interned; frees a fresh dup) so re-assigning an
        // existing key doesn't leak a key per assignment.
        dragon_dict_release_key(d, key);
        // Key exists - update the slot in place FIRST, then drop the old
        // value's ref. Reentrancy hardening: the slot must hold the new value
        // before the decref, so if a finalizer ever runs during the drop and
        // re-reads d[key] it observes `value`, never the freed `old_val`.
        // The decref goes through dispatch, which covers atomic-context.
        int8_t old_tag = d->entries[idx].tag;
        int64_t old_val = d->entries[idx].value;
        d->entries[idx].value = value;
        d->entries[idx].tag = (int8_t)tag;
        // Structural work done - close the window before the drop (see the
        // placement rules in runtime_internal.h).
        dragon_shared_mut_end(&d->header, mut_armed);
        if (old_val && old_tag == TAG_STR) {
            dragon_decref_str_dispatch((const char*)(uintptr_t)old_val);
        } else if (old_val && (old_tag == TAG_LIST || old_tag == TAG_DICT || old_tag == TAG_BYTES)) {
            dragon_decref_dispatch((void*)(uintptr_t)old_val);
} else if (old_val && old_tag == DRAGON_TAG_CLOSURE) {
            dragon_decref_callable((void*)(uintptr_t)old_val);
        }
        return;
    }

    // New key - ensure room. The dense array grows on every new insert (even
    // ones that reuse a deleted key's tombstone slot), so first reclaim dead
    // slots via compaction when the array is full; only realloc-grow when it is
    // genuinely full of LIVE entries. Same for the index-load trigger.
    if (d->size >= d->capacity || d->size * 3 >= d->index_size * 2) {
        if (d->used < d->size) dict_compact(d);
        if (d->size >= d->capacity || d->size * 3 >= d->index_size * 2) dict_grow(d);
        // Re-probe: compaction and/or grow rebuilt the index table.
        slot = dict_probe(d, key, h);
    }

    // Append to dense entries array
    int64_t ei = d->size;
    d->entries[ei].hash = h;
    d->entries[ei].key = key;
    d->entries[ei].value = value;
    d->entries[ei].tag = (int8_t)tag;
    d->entries[ei].dead = 0;
    d->indices[slot] = ei;
    d->size++;
    d->used++;
    dragon_shared_mut_end(&d->header, mut_armed);
}

void dragon_dict_set(DragonDict* d, const char* key, int64_t value) {
    dragon_dict_set_tagged(d, key, value, TAG_INT);
}

// Raise a catchable KeyError carrying the missing key. The formatted message
// is heap-dup'd so it survives the longjmp out of this frame.
static void dragon_raise_keyerror(const char* key) {
    char buf[256];
    snprintf(buf, sizeof(buf), "KeyError: '%s'", key ? key : "");
    dragon_raise_exc_cstr(42, buf);
}

// Int-keyed variant (unquoted key, matching Python's KeyError repr for ints).
static void dragon_raise_keyerror_int(int64_t key) {
    char buf[64];
    snprintf(buf, sizeof(buf), "KeyError: %lld", (long long)key);
    dragon_raise_exc_cstr(42, buf);
}

int64_t dragon_dict_get(DragonDict* d, const char* key) {
    uint64_t h = dict_hash(key);
    int64_t slot = dict_probe(d, key, h);
    int64_t idx = d->indices[slot];
    if (idx >= 0) return d->entries[idx].value;
    dragon_raise_keyerror(key);
    return 0;
}

// runtime_string.cpp: copy `byte_len` bytes from a raw pointer into a fresh,
// OWNED DragonString. Declared here for the str-valued getter below.
const char* dragon_string_alloc(const char* src, int64_t byte_len);

// Str-VALUED dict `get(key, default)` that returns an OWNED reference: the stored
// value incref'd (present), or a fresh heap copy of the default (absent). This
// keeps a Dragon `str` local bound to `d.get(k, default)` refcount-balanced. The
// generic dragon_dict_get_default returns a BORROWED value (the dict keeps the
// +1); bound to an owned str local and decref'd at scope exit, it double-frees
// the dict's value (the registry CSRF/login form use-after-free). Returning an
// owned heap copy of an absent default also avoids ever decref'ing a headerless
// string literal.
const char* dragon_dict_get_str_default(DragonDict* d, const char* key, const char* def) {
    uint64_t h = dict_hash(key);
    int64_t slot = dict_probe(d, key, h);
    int64_t idx = d->indices[slot];
    if (idx >= 0) {
        const char* v = (const char*)(uintptr_t)d->entries[idx].value;
        dragon_incref_str(v);
        return v;
    }
    return dragon_string_alloc(def ? def : "", def ? (int64_t)strlen(def) : 0);
}

int64_t dragon_dict_get_tag(DragonDict* d, const char* key) {
    uint64_t h = dict_hash(key);
    int64_t slot = dict_probe(d, key, h);
    int64_t idx = d->indices[slot];
    if (idx >= 0) return d->entries[idx].tag;
    return TAG_INT; // default
}

/// D039 Phase 2: dict[str, Any] read returns a {tag, payload} box by value.
///
/// Contract: returns a BORROW of the entry's payload. For refcounted tags
/// (TAG_STR / TAG_LIST / TAG_DICT / TAG_BYTES / TAG_CLASS), the dict still
/// owns the +1 refcount. Callers that store the result into a longer-lived
/// slot must incref the payload by tag; callers that only read transiently
/// do not. Mirrors the existing dragon_dict_get borrow semantics.
///
/// Returns a DragonBox (%dragon.box = { i64 tag, i64 payload }, defined in
/// runtime_internal.h) so LLVM passes it in two registers on AMD64.
DragonBox dragon_dict_get_box(DragonDict* d, const char* key) {
    uint64_t h = dict_hash(key);
    int64_t slot = dict_probe(d, key, h);
    int64_t idx = d->indices[slot];
    if (idx < 0) {
        dragon_raise_keyerror(key);
        return {};
    }
    DragonBox box;
    box.tag = (int64_t)d->entries[idx].tag;
    box.payload = d->entries[idx].value;
    return box;
}

/// Tag name for error messages
static const char* tag_name(int64_t tag) {
    switch (tag) {
        case TAG_INT:   return "int";
        case TAG_STR:   return "str";
        case TAG_FLOAT: return "float";
        case TAG_BOOL:  return "bool";
        case TAG_NONE:  return "NoneType";
        case TAG_LIST:  return "list";
        case TAG_DICT:  return "dict";
        case TAG_BYTES: return "bytes";
        default:        return "unknown";
    }
}

/// Get dict value with runtime type check. Throws TypeError (code 80) if tag mismatch.
/// Single hash probe - same cost as dragon_dict_get plus one branch.
int64_t dragon_dict_get_checked(DragonDict* d, const char* key, int64_t expected_tag) {
    uint64_t h = dict_hash(key);
    int64_t slot = dict_probe(d, key, h);
    int64_t idx = d->indices[slot];
    if (idx < 0) {
        dragon_raise_keyerror(key);
        return 0;
    }
    int8_t actual_tag = d->entries[idx].tag;
    if (actual_tag != (int8_t)expected_tag) {
        // Per-thread static buffer to avoid the strdup leak - the exception
        // machinery stores the msg by borrowed pointer, so allocating fresh
        // memory here would accumulate forever on repeated TypeErrors.
        char tls_msg[256];
        snprintf(tls_msg, sizeof(tls_msg),
                 "TypeError: value for key '%s' is %s, not %s",
                 key, tag_name(actual_tag), tag_name(expected_tag));
        dragon_raise_exc_cstr(80, tls_msg);
    }
    return d->entries[idx].value;
}

int64_t dragon_dict_len(DragonDict* d) {
    return d ? d->used : 0;  // live entries only (dense `size` includes tombstones)
}

int64_t dragon_dict_has_key(DragonDict* d, const char* key) {
    uint64_t h = dict_hash(key);
    int64_t slot = dict_probe(d, key, h);
    return d->indices[slot] >= 0 ? 1 : 0;
}

/// C9-B `**dict` spread validation: raise TypeError if the dict carries any
/// (str) key that is not one of the callable's bindable parameter names.
/// Mirrors Python's "got an unexpected keyword argument" check. The entries
/// array is dense [0, size) in insertion order, so we report the FIRST stray
/// key in source order. No-op for int-keyed / empty dicts (a `**` spread is by
/// name; an int-keyed dict can't bind named params).
void dragon_dict_reject_unknown_keys(DragonDict* d, const char** allowed,
                                     int64_t n, const char* func_name) {
    if (!d || !d->keys_are_ptr) return;
    for (int64_t i = 0; i < d->size; i++) {
        if (d->entries[i].dead) continue;
        const char* k = d->entries[i].key;
        bool found = false;
        for (int64_t j = 0; j < n; j++) {
            if (allowed[j] && k && strcmp(allowed[j], k) == 0) { found = true; break; }
        }
        if (!found) {
            char tls_msg[256];
            snprintf(tls_msg, sizeof(tls_msg),
                     "TypeError: %s got an unexpected keyword argument '%s'",
                     func_name ? func_name : "function", k ? k : "");
            dragon_raise_exc_cstr(80, tls_msg);
        }
    }
}

/// Fused augmented assignment for a str-keyed dict with an int value:
/// `d[key] = d[key] OP operand` in a SINGLE hash+probe (vs the get-probe +
/// set-probe a naive `d[k] = d[k] OP v` lowering would do). KeyError (+exit,
/// matching `dragon_dict_get`) if the key is absent - Python evaluates `d[k]`
/// first. Returns the new value. op codes:
///   0 +=  1 -=  2 *=  3 //=  4 %=  5 &=  6 |=  7 ^=  8 <<=  9 >>=
int64_t dragon_dict_str_iaug_i64(DragonDict* d, const char* key,
                                 int64_t operand, int64_t op) {
    uint64_t h = dict_hash(key);
    int64_t slot = dict_probe(d, key, h);
    int64_t idx = d->indices[slot];
    if (idx < 0) {
        dragon_raise_keyerror(key);
        return 0;
    }
    int64_t cur = d->entries[idx].value;
    int64_t res;
    switch (op) {
        case 0: res = cur + operand; break;
        case 1: res = cur - operand; break;
        case 2: res = cur * operand; break;
        case 3:  // //= (Python floor division)
        case 4:  // %=  (Python floor modulo)
            if (operand == 0) {
                dragon_raise_exc_cstr(23, "ZeroDivisionError: integer division or modulo by zero");
                return 0;
            }
            if (op == 3) {
                res = cur / operand;
                if ((cur % operand != 0) && ((cur ^ operand) < 0)) res -= 1;
            } else {
                res = cur % operand;
                if (res != 0 && ((res ^ operand) < 0)) res += operand;
            }
            break;
        case 5: res = cur & operand; break;
        case 6: res = cur | operand; break;
        case 7: res = cur ^ operand; break;
        case 8: res = cur << operand; break;
        case 9: res = cur >> operand; break;
        default: res = cur; break;
    }
    d->entries[idx].value = res;
    d->entries[idx].tag = TAG_INT;
    return res;
}

int64_t dragon_dict_get_default(DragonDict* d, const char* key, int64_t def) {
    uint64_t h = dict_hash(key);
    int64_t slot = dict_probe(d, key, h);
    int64_t idx = d->indices[slot];
    if (idx >= 0) return d->entries[idx].value;
    return def;
}

// Owned-returning getters for HEAP-valued dicts (list/dict/set/tuple/bytes/
// instance). The generic i64 getters above return a BORROW of the stored value;
// a binding (`g: list[int] = d.get(k)`) then decrefs it at scope exit and frees
// the dict's value while the dict still holds it -> UAF. These
// incref what they return, so the caller owns its OWN reference and the dict
// keeps its own - the exact contract dragon_dict_get_str_default uses for str.
// dragon_incref is generic (works for any heap object with the RC header);
// str values keep their dedicated *_str_default path.
void* dragon_dict_get_ptr(DragonDict* d, const char* key) {
    uint64_t h = dict_hash(key);
    int64_t slot = dict_probe(d, key, h);
    int64_t idx = d->indices[slot];
    if (idx >= 0) {
        void* v = (void*)(uintptr_t)d->entries[idx].value;
        if (v) dragon_incref(v);
        return v;
    }
    dragon_raise_keyerror(key);  // 1-arg get raises on miss, like dragon_dict_get
    return nullptr;
}

void* dragon_dict_get_ptr_default(DragonDict* d, const char* key, void* def) {
    uint64_t h = dict_hash(key);
    int64_t slot = dict_probe(d, key, h);
    int64_t idx = d->indices[slot];
    void* v = (idx >= 0) ? (void*)(uintptr_t)d->entries[idx].value : def;
    // Incref whatever we return (stored OR default). The caller drains its `def`
    // temp after the call, so this stays balanced both ways: key present -> def
    // unused, freed by the drain; key absent -> def returned, the drain takes one
    // ref and the binding the other.
    if (v) dragon_incref(v);
    return v;
}

DragonList* dragon_dict_keys(DragonDict* d) {
    if (d && d->keys_are_ptr) {
        // str-keyed: each key is an owned heap DragonString. The keys list
        // CO-OWNS them (incref + TAG_STR) so it stays valid if it outlives the
        // dict (e.g. `ks = d.keys(); ... use ks after d dies`); the list's
        // destroy then decrefs them, balancing the incref. Without this the
        // list would hold borrowed keys that dangle once the dict frees them.
        DragonList* l = dragon_list_new_tagged(d->used, TAG_STR);
        for (int64_t i = 0; i < d->size; i++) {
            if (d->entries[i].dead) continue;
            dragon_incref_str(d->entries[i].key);
            dragon_list_append(l, (int64_t)d->entries[i].key);
        }
        return l;
    }
    // int-keyed (or empty): keys are raw i64s in the pointer slot - TAG_INT,
    // never incref'd/decref'd as strings.
    DragonList* l = dragon_list_new(d ? d->used : 0);
    if (d) {
        for (int64_t i = 0; i < d->size; i++) {
            if (d->entries[i].dead) continue;
            dragon_list_append(l, (int64_t)d->entries[i].key);
        }
    }
    return l;
}

void dragon_print_dict_raw(DragonDict* d) {
    printf("{");
    if (d) {
        bool first = true;  // comma logic keys off emitted entries, not index
        for (int64_t i = 0; i < d->size; i++) {
            if (d->entries[i].dead) continue;
            if (!first) printf(", ");
            first = false;
            printf("'%s': ", d->entries[i].key);
            switch (d->entries[i].tag) {
                case TAG_STR:
                    printf("'%s'", (const char*)(uintptr_t)d->entries[i].value);
                    break;
                case TAG_FLOAT: {
                    double fval;
                    memcpy(&fval, &d->entries[i].value, sizeof(double));
                    char ftmp[64];
                    dragon_format_double_into(fval, ftmp, sizeof(ftmp));
                    fputs(ftmp, stdout);
                    break;
                }
                case TAG_BOOL:
                    printf("%s", d->entries[i].value ? "True" : "False");
                    break;
                case TAG_NONE:
                    printf("None");
                    break;
                case TAG_BYTES:
                    dict_print_bytes_or_instance(d->entries[i].value);
                    break;
                case TAG_LIST:
                case TAG_DICT:
                    dict_print_container_value(d->entries[i].tag,
                                               d->entries[i].value);
                    break;
                default:
                    printf("%ld", d->entries[i].value);
                    break;
            }
        }
    }
    printf("}");
}
void dragon_print_dict(DragonDict* d) {
    dragon_print_dict_raw(d);
    putchar('\n');
}

void dragon_print_tagged_raw(int64_t value, int64_t tag) {
    switch (tag) {
        case TAG_STR:
            printf("%s", (const char*)(uintptr_t)value);
            break;
        case TAG_FLOAT: {
            double fval;
            memcpy(&fval, &value, sizeof(double));
            char ftmp[64];
            dragon_format_double_into(fval, ftmp, sizeof(ftmp));
            fputs(ftmp, stdout);
            break;
        }
        case TAG_BOOL:
            printf("%s", value ? "True" : "False");
            break;
        case TAG_NONE:
            printf("None");
            break;
        case TAG_BYTES:
            dict_print_bytes_or_instance(value);
            break;
        default:
            printf("%ld", value);
            break;
    }
}
void dragon_print_tagged(int64_t value, int64_t tag) {
    dragon_print_tagged_raw(value, tag);
    putchar('\n');
}

/// Destroy a dict and free its memory (GC support).
/// Child decrefs go through dispatch helpers (atomic variants when
/// invoked from an atomic-context dealloc).
void dragon_dict_destroy(DragonDict* d) {
    if (!d) return;
    // Phase 5: decref heap-typed values before freeing. Dead slots already had
    // their key/value released at delete time - skip them (re-releasing would
    // double-free).
    for (int64_t i = 0; i < d->size; i++) {
        if (d->entries[i].dead) continue;
        int8_t tag = d->entries[i].tag;
        int64_t val = d->entries[i].value;
        if (val && tag == TAG_STR) {
            dragon_decref_str_dispatch((const char*)(uintptr_t)val);
        } else if (val && (tag == TAG_LIST || tag == TAG_DICT || tag == TAG_BYTES)) {
            dragon_decref_dispatch((void*)(uintptr_t)val);
} else if (val && tag == DRAGON_TAG_CLOSURE) {
            dragon_decref_callable((void*)(uintptr_t)val);
        }
        // Release the owned str key (no-op for int-keyed dicts / immortal keys).
        dragon_dict_release_key(d, d->entries[i].key);
    }
    free(d->entries);
    free(d->indices);
    free(d);
}

//===----------------------------------------------------------------------===//
// Additional Dict Methods (placed here because dict_items needs DragonTuple)
//===----------------------------------------------------------------------===//

/// Get all values as a list (insertion order)
DragonList* dragon_dict_values(DragonDict* d) {
    // Determine elem_tag for the result list. If all entries share one tag we
    // can use it and the result list will properly decref on destroy. If tags
    // are mixed, we fall back to TAG_INT (no decref on destroy) and must NOT
    // incref the values - otherwise the increfs would never be balanced and
    // the values would leak. Callers receiving a mixed-values list are
    // expected to treat it as borrowed for the source dict's lifetime.
    int8_t vtag = TAG_INT;
    bool mixed = false;
    bool seen = false;
    if (d) {
        for (int64_t i = 0; i < d->size; i++) {
            if (d->entries[i].dead) continue;
            if (!seen) { vtag = d->entries[i].tag; seen = true; }
            else if (d->entries[i].tag != vtag) { mixed = true; break; }
        }
        if (mixed) vtag = TAG_INT;
    }
    DragonList* l = dragon_list_new_tagged(d ? d->used : 0, vtag);
    if (d) {
        for (int64_t i = 0; i < d->size; i++) {
            if (d->entries[i].dead) continue;
            if (!mixed) {
                dragon_incref_tagged(d->entries[i].value, d->entries[i].tag);
            }
            dragon_list_append(l, d->entries[i].value);
        }
    }
    return l;
}

/// D039 Phase 9 completion: dict[str, Any].values() - return a DragonListBox
/// preserving each entry's per-element tag. Iterating with box semantics
/// (`for v in cfg.values()`) gives the receiver an Any-typed loop var, so
/// isinstance narrowing and tag-dispatched print all work correctly.
///
/// OWNED contract: each box entry co-owns its payload (incref'd here). It has
/// to - a DragonListBox is a normal refcounted object, and when its refcount
/// hits 0, dragon_list_box_destroy DECREFS every refcounted payload. The old
/// "borrow contract" comment lied: codegen disposes of the values() result as
/// an owned temp (scope cleanup emits dragon_decref on it), so box destroy
/// released references the box never took, dropping the dict's values to
/// refcount 0 while the dict still pointed at them. One `for v in d.values()`
/// over a dict[str, Any] with heap values was a use-after-free in the dict's
/// own destroy (ASan-verified).
DragonListBox* dragon_dict_values_box(DragonDict* d) {
    int64_t n = d ? d->used : 0;
    DragonListBox* lb = dragon_list_box_new(n);
    if (!d) return lb;
    for (int64_t i = 0; i < d->size; i++) {
        if (d->entries[i].dead) continue;
        int64_t tag = (int64_t)d->entries[i].tag;
        int64_t payload = d->entries[i].value;
        // Co-own the payload with an incref that is the EXACT inverse of what
        // dragon_list_box_destroy -> dragon_listbox_decref_elem will drop:
        // STR/LIST/DICT/BYTES only. Deliberately NOT dragon_incref_tagged,
        // which would also incref a TAG_CLOSURE payload that box destroy does
        // not decref - that asymmetry would leak a closure-valued entry. If
        // the box destroy ever learns TAG_CLOSURE, mirror it here too. (A
        // decref arm there is walled by an ASan-proven double-free: see the
        // WALL note in dragon_listbox_decref_elem, runtime_list.cpp - the
        // codegen borrowed-append incref for tag 10 must land first.)
        if (payload) {
            if (tag == TAG_STR)
                dragon_incref_str((const char*)(uintptr_t)payload);
            else if (tag == TAG_LIST || tag == TAG_DICT || tag == TAG_BYTES)
                dragon_incref((void*)(uintptr_t)payload);
        }
        dragon_list_box_append(lb, tag, payload);
    }
    return lb;
}

/// Get all items as list of tuples (key, value) in insertion order
/// Returns DragonList* of DragonTuple* (bitcast to i64)
DragonList* dragon_dict_items(DragonDict* d) {
    // TAG_LIST triggers dragon_decref in list destroy - works for tuples too
    DragonList* l = dragon_list_new_tagged(d ? d->used : 0, TAG_LIST);
    if (d) {
        for (int64_t i = 0; i < d->size; i++) {
            if (d->entries[i].dead) continue;
            DragonTuple* t = dragon_tuple_new(2);
            // The tuple co-owns the key: incref + tag TAG_STR so the tuple's
            // destroy releases it. Without this, once the dict frees its keys on
            // destroy, an items() tuple outliving the dict would dangle. For an
            // int-keyed dict the key is an i64 - store untagged (no ownership).
            if (d->keys_are_ptr) {
                dragon_incref_str(d->entries[i].key);
                dragon_tuple_set_tagged(t, 0, (int64_t)d->entries[i].key, TAG_STR);
            } else {
                dragon_tuple_set(t, 0, (int64_t)d->entries[i].key);
            }
            // Incref value: the tuple now co-owns this value alongside the dict
            dragon_incref_tagged(d->entries[i].value, d->entries[i].tag);
            dragon_tuple_set_tagged(t, 1, d->entries[i].value, d->entries[i].tag);
            dragon_list_append(l, (int64_t)t);
        }
    }
    return l;
}

/// dict.fromkeys(iterable, value=...) - build a new dict where every key
/// from `keys` maps to `value` (with shared tag). Each key is interned so
/// the dict holds a stable, never-freed pointer regardless of how the
/// source DragonString was created.
DragonDict* dragon_dict_fromkeys(DragonList* keys, int64_t value, int64_t tag) {
    int64_t n = keys ? keys->size : 0;
    DragonDict* d = dragon_dict_new(n > 0 ? n : 4);
    if (!keys) return d;
    for (int64_t i = 0; i < n; i++) {
        // List elements for list[str] are DragonString **data** pointers
        // (not struct pointers) - so for ASCII strings the value is also
        // a valid C string. Use dragon_str_to_utf8_alloc to handle both
        // ASCII (NULL return → use the raw pointer) and UCS-4 paths.
        const char* keyData = (const char*)(uintptr_t)dragon_list_load(keys, i);
        if (!keyData) continue;
        // The new-key store ADOPTS the key pointer (dragon_dict_set_tagged sets
        // entries[ei].key = key with no dup), so the dict needs one owned ref.
        // That ref must be a normal mortal +1 (dragon_string_dup), never
        // dragon_str_intern: an interned key is IMMORTAL with no dedup table,
        // so every fromkeys call would leak one unfreeable string per key
        // forever - unbounded RSS for a server calling fromkeys per request.
        // dragon_string_dup gives a +1 the dict's key release path reclaims,
        // and it already handles the ASCII / UCS-4 kinds, so no manual UTF-8
        // round trip is needed.
        const char* ownedKey = dragon_string_dup(keyData);
        // Each entry takes its own reference to the value.
        dragon_incref_tagged(value, tag);
        dragon_dict_set_tagged(d, ownedKey, value, tag);
    }
    return d;
}

/// Remove + return the last inserted (key, value) pair as a 2-tuple.
/// Python 3.7+ semantics: LIFO order. Raises KeyError on empty dict.
/// Returns DragonTuple* (cast to i64) - caller owns the tuple reference.
int64_t dragon_dict_popitem(DragonDict* d) {
    if (!d || d->used == 0) {
        dragon_raise_exc_cstr(42, "KeyError: 'popitem(): dictionary is empty'");
        return 0;
    }
    // Concurrent-mutation detector: armed after the raise-y validation
    // (a longjmp would strand the bit), covers scan + tombstone + compact
    bool mut_armed = dragon_shared_mut_begin(&d->header, "dict");
    // LIFO: the most-recently-inserted LIVE entry. Scan back over any trailing
    // tombstones (a prior pop/del may have left dead slots at the tail).
    int64_t lastIdx = d->size - 1;
    while (lastIdx >= 0 && d->entries[lastIdx].dead) lastIdx--;
    DictEntry e = d->entries[lastIdx];
    DragonTuple* t = dragon_tuple_new(2);
    // Move the dict's owned key ref into the tuple: tag it TAG_STR so the
    // tuple's destroy releases it, and do NOT incref - the dict is dropping
    // this entry, so ownership transfers exactly once. For an int-keyed dict
    // the "key" is an i64 - store it untagged (no ownership).
    if (d->keys_are_ptr) {
        dragon_tuple_set_tagged(t, 0, (int64_t)e.key, TAG_STR);
    } else {
        dragon_tuple_set(t, 0, (int64_t)e.key);
    }
    // Value: the dict was holding a reference; transfer it to the tuple
    // (no incref - net refcount is unchanged across the pop).
    dragon_tuple_set_tagged(t, 1, e.value, e.tag);
    // Invalidate the index slot for this key, then tombstone the dense entry.
    int64_t slot = d->keys_are_ptr ? dict_probe(d, e.key, e.hash)
                                   : dict_probe_i64(d, (int64_t)(uintptr_t)e.key, e.hash);
    d->indices[slot] = DICT_TOMBSTONE;
    d->entries[lastIdx].dead = 1;
    d->entries[lastIdx].key = nullptr;
    d->entries[lastIdx].value = 0;
    d->used--;
    // Trailing tombstone removed: shrink the dense extent so repeated popitem
    // stays O(1) without needing a full compaction.
    if (lastIdx == d->size - 1) {
        while (d->size > 0 && d->entries[d->size - 1].dead) d->size--;
    }
    dict_maybe_compact(d);
    dragon_shared_mut_end(&d->header, mut_armed);
    return (int64_t)t;
}

/// Pop key from dict, return value (KeyError if absent). O(1): tombstone the
/// index slot + mark the dense entry dead in place (no shift, no rebuild);
/// dead slots are reclaimed by lazy compaction. Insertion order of surviving
/// entries is unchanged because their dense positions never move.
int64_t dragon_dict_pop(DragonDict* d, const char* key) {
    // Concurrent-mutation detector: window covers probe + tombstone +
    // compact; released explicitly before the raise (longjmp skips ends).
    bool mut_armed = dragon_shared_mut_begin(&d->header, "dict");
    uint64_t h = dict_hash(key);
    int64_t slot = dict_probe(d, key, h);
    int64_t idx = d->indices[slot];
    if (idx < 0) {
        dragon_shared_mut_end(&d->header, mut_armed);
        dragon_raise_keyerror(key);
        return 0;
    }
    int64_t val = d->entries[idx].value;
    // Caller receives only the VALUE; the dict's owned key is dropped.
    dragon_dict_release_key(d, d->entries[idx].key);
    d->indices[slot] = DICT_TOMBSTONE;
    d->entries[idx].dead = 1;
    d->entries[idx].key = nullptr;
    d->entries[idx].value = 0;
    d->used--;
    dict_maybe_compact(d);
    dragon_shared_mut_end(&d->header, mut_armed);
    return val;
}

/// Pop key with default
int64_t dragon_dict_pop_default(DragonDict* d, const char* key, int64_t def) {
    // Concurrent-mutation detector - see dragon_dict_pop.
    bool mut_armed = dragon_shared_mut_begin(&d->header, "dict");
    uint64_t h = dict_hash(key);
    int64_t slot = dict_probe(d, key, h);
    int64_t idx = d->indices[slot];
    if (idx < 0) {
        dragon_shared_mut_end(&d->header, mut_armed);
        return def;
    }
    int64_t val = d->entries[idx].value;
    // Caller receives only the VALUE; the dict's owned key is dropped.
    dragon_dict_release_key(d, d->entries[idx].key);
    d->indices[slot] = DICT_TOMBSTONE;
    d->entries[idx].dead = 1;
    d->entries[idx].key = nullptr;
    d->entries[idx].value = 0;
    d->used--;
    dict_maybe_compact(d);
    dragon_shared_mut_end(&d->header, mut_armed);
    return val;
}

/// `del d[key]` for str-keyed dicts. Like pop(), but the value is discarded
/// rather than returned, so we decref a heap-typed value here (mirroring
/// dragon_dict_destroy / dragon_dict_clear - values are owned, keys are not).
/// Missing key raises KeyError, matching dragon_dict_get / Python semantics.
void dragon_dict_del(DragonDict* d, const char* key) {
    // Concurrent-mutation detector - see dragon_dict_pop.
    bool mut_armed = dragon_shared_mut_begin(&d->header, "dict");
    uint64_t h = dict_hash(key);
    int64_t slot = dict_probe(d, key, h);
    int64_t idx = d->indices[slot];
    if (idx < 0) {
        dragon_shared_mut_end(&d->header, mut_armed);
        dragon_raise_keyerror(key);
        return;
    }
    int8_t tag = d->entries[idx].tag;
    int64_t val = d->entries[idx].value;
    const char* old_key = d->entries[idx].key;  // capture before clearing
    // Reentrancy hardening: tombstone the entry and restore the dict to a
    // consistent state BEFORE dropping the value's ref, so a finalizer running
    // during the drop can never observe the removed entry. O(1): mark dead in
    // place (no shift, no rebuild).
    d->indices[slot] = DICT_TOMBSTONE;
    d->entries[idx].dead = 1;
    d->entries[idx].key = nullptr;
    d->entries[idx].value = 0;
    d->used--;
    dict_maybe_compact(d);
    // Structural work done - close the window before the drops.
    dragon_shared_mut_end(&d->header, mut_armed);
    if (val && tag == TAG_STR) {
        dragon_decref_str_dispatch((const char*)(uintptr_t)val);
    } else if (val && (tag == TAG_LIST || tag == TAG_DICT || tag == TAG_BYTES)) {
        dragon_decref_dispatch((void*)(uintptr_t)val);
} else if (val && tag == DRAGON_TAG_CLOSURE) {
        dragon_decref_callable((void*)(uintptr_t)val);
    }
    dragon_dict_release_key(d, old_key);
}

/// Clear all entries
void dragon_dict_clear(DragonDict* d) {
    if (d) {
        // Concurrent-mutation detector: whole teardown loop is the window.
        // The per-entry decrefs stay INSIDE it (restructuring them out would
        // need a temp buffer); today no user finalizer can run inside a
        // decref, so self-collision is impossible - revisit if __del__ lands.
        bool mut_armed = dragon_shared_mut_begin(&d->header, "dict");
        // Reentrancy hardening: null each slot's owned value/key and capture
        // them locally BEFORE dropping their refs, so a finalizer that
        // re-reads this dict during a drop observes an emptied slot, never a
        // freed pointer. The count is snapshotted up front so a re-entrant
        // insert appends past it (and is dropped by the size=0 below) rather
        // than corrupting this teardown loop. Nulling the key also stops the
        // reused entries storage from re-releasing a stale key on a later
        // destroy. Dispatch routes to atomic decref under atomic-context.
        int64_t n = d->size;
        for (int64_t i = 0; i < n; i++) {
            if (d->entries[i].dead) continue;  // already released at delete time
            int8_t tag = d->entries[i].tag;
            int64_t val = d->entries[i].value;
            const char* old_key = d->entries[i].key;
            d->entries[i].value = 0;
            d->entries[i].key = nullptr;
            if (val && tag == TAG_STR) {
                dragon_decref_str_dispatch((const char*)(uintptr_t)val);
            } else if (val && (tag == TAG_LIST || tag == TAG_DICT || tag == TAG_BYTES)) {
                dragon_decref_dispatch((void*)(uintptr_t)val);
} else if (val && tag == DRAGON_TAG_CLOSURE) {
                dragon_decref_callable((void*)(uintptr_t)val);
            }
            dragon_dict_release_key(d, old_key);
        }
        d->size = 0;
        d->used = 0;
        for (int64_t i = 0; i < d->index_size; i++) d->indices[i] = DICT_EMPTY;
        dragon_shared_mut_end(&d->header, mut_armed);
    }
}

/// Update dict with entries from another dict (preserves tags)
void dragon_dict_update(DragonDict* d, DragonDict* other) {
    if (!other) return;
    for (int64_t i = 0; i < other->size; i++) {
        if (other->entries[i].dead) continue;
        // Incref the value before inserting - dict_set_tagged does not incref
        // on insert, so the destination dict needs its own reference.
        dragon_incref_tagged(other->entries[i].value, other->entries[i].tag);
        // Key dispatch by the SOURCE dict's key flavor. An int-keyed dict
        // stores a raw i64 in the key slot; routing it through the str-keyed
        // setter made dict_hash() dereference that integer as a char* (wild
        // read -> SEGV) and flipped keys_are_ptr on the destination, so its
        // destroy would decref int keys as strings. The int setter hashes the
        // i64 directly and never touches keys_are_ptr.
        if (other->keys_are_ptr) {
            // d takes its OWN ref to each str key (set_tagged stores the
            // pointer without increfing). Without this, d's keys would be
            // owned by `other` and dangle when `other` dies - and d's destroy
            // would double-free.
            dragon_incref_str(other->entries[i].key);
            dragon_dict_set_tagged(d, other->entries[i].key,
                                   other->entries[i].value, other->entries[i].tag);
        } else {
            dragon_dict_int_set_tagged(d, (int64_t)(uintptr_t)other->entries[i].key,
                                       other->entries[i].value, other->entries[i].tag);
        }
    }
}

/// Get or set default: if key exists return value, else insert default and return it.
/// KEY ownership (#20a): the codegen hands an OWNED key (it increfs a borrowed heap
/// key; literals/owned-temps already carry the right ref state). On insert the
/// store adopts it; on the present branch it is unused, so release it here or it
/// leaks. Guarded - no-op for literal/immortal keys. Shared by the scalar
/// regular-dict path and dragon_syncdict_setdefault (both incref borrowed keys).
int64_t dragon_dict_setdefault(DragonDict* d, const char* key, int64_t def) {
    uint64_t h = dict_hash(key);
    int64_t slot = dict_probe(d, key, h);
    int64_t idx = d->indices[slot];
    if (idx >= 0) {
        dragon_dict_release_key(d, key);
        return d->entries[idx].value;
    }
    dragon_dict_set(d, key, def);
    return def;
}

// Owned-returning setdefault for HEAP-OBJECT-valued dicts (list/dict/set/tuple/
// bytes/instance) - the str-keyed setdefault sibling of the owned-returning get.
// The generic dragon_dict_setdefault above returns the stored value as a BORROW
// and, on insert, stores the default with TAG_INT (so dealloc/cycle-GC never
// see it as heap -> leak + blind collector). This variant fixes both. Contract
// mirrors dragon_dict_get_ptr_default and its call site (which drains the
// default when it is an owned temp):
//   present -> incref the STORED value and return it (the binding owns its own
//              ref); the default is untouched (call site drains an owned temp).
//   absent  -> store the default with its proper heap `tag` (codegen passes
//              inferPtrValueTag), then incref it TWICE - one ref the dict's
//              stored copy keeps, one the binding owns. With the call site's
//              single owned-temp drain this nets exactly two live owners.
// dragon_incref/decref are generic over heap kinds (str has its own path).
void* dragon_dict_setdefault_ptr(DragonDict* d, const char* key, void* def, int64_t tag) {
    uint64_t h = dict_hash(key);
    int64_t slot = dict_probe(d, key, h);
    int64_t idx = d->indices[slot];
    if (idx >= 0) {
        void* v = (void*)(uintptr_t)d->entries[idx].value;
        if (v) dragon_incref(v);
        // Key present: the incoming key is unused. The codegen hands us an OWNED
        // key (it increfs a borrowed heap key; literals/owned-temps already carry
        // the right ref state), so release it here or it leaks. Guarded: no-op for
        // literal/immortal keys (#20-followup: setdefault key-on-insert dangle).
        dragon_dict_release_key(d, key);
        return v;
    }
    // Key absent: store the key+value. dragon_dict_set_tagged ADOPTS one ref of
    // each (stores the key pointer directly), so the codegen's owned key + the
    // owned-or-incref'd default land correctly. (dragon_dict_set_str_ptr is this
    // call but is defined later in the TU.) Tags the value so dealloc/cycle-GC
    // see it as heap, unlike the TAG_INT generic path.
    dragon_dict_set_tagged(d, key, (int64_t)(uintptr_t)def, tag);
    if (def) { dragon_incref(def); dragon_incref(def); }
    return def;
}

/// Shallow copy minus the named keys (the `**dict` → `**kwargs` spread path:
/// regular-param names bind into their call slots straight from the source
/// dict, so only the remaining entries flow into the callee's kwargs dict).
/// Same per-entry ownership rules as dragon_dict_copy. Int-keyed dicts have
/// no excludable names (the TypeChecker only admits dict[str, V] spreads).
DragonDict* dragon_dict_copy_excluding(DragonDict* d, const char** names,
                                       int64_t name_count) {
    DragonDict* copy = dragon_dict_new(d ? d->capacity : 4);
    if (!d) return copy;
    for (int64_t i = 0; i < d->size; i++) {
        if (d->entries[i].dead) continue;
        if (d->keys_are_ptr && name_count > 0) {
            bool excluded = false;
            for (int64_t j = 0; j < name_count; j++) {
                if (names[j] && strcmp(d->entries[i].key, names[j]) == 0) {
                    excluded = true;
                    break;
                }
            }
            if (excluded) continue;
        }
        int64_t val = d->entries[i].value;
        uint8_t tag = d->entries[i].tag;
        // dragon_incref_tagged is the single source of truth for per-tag RC:
        // it covers STR/LIST/DICT/BYTES like the old hand-rolled chain AND
        // DRAGON_TAG_CLOSURE, which the chain missed - a copied dict[str,
        // Callable] had both dicts decref_callable the same closure on
        // destroy while only one ref existed (double free).
        if (val) dragon_incref_tagged(val, tag);
        // Key dispatch by key flavor: an int-keyed dict stores a raw i64 in
        // the key slot; the str setter would dereference it as a char* in
        // dict_hash (SEGV) and poison keys_are_ptr on the copy.
        if (d->keys_are_ptr) {
            dragon_incref_str(d->entries[i].key);
            dragon_dict_set_tagged(copy, d->entries[i].key, val, tag);
        } else {
            dragon_dict_int_set_tagged(copy, (int64_t)(uintptr_t)d->entries[i].key,
                                       val, tag);
        }
    }
    return copy;
}

/// dub (docs/002 2.7): deep copy - mirrors dragon_dict_copy's key handling
/// exactly, but values deep-copy by tag (dragon_deep_copy_tagged returns +1;
/// set_tagged adopts it, so ownership matches the shallow copy's contract).
DragonDict* dragon_dict_deep_copy(DragonDict* d) {
    DragonDict* copy = dragon_dict_new(d ? d->capacity : 4);
    if (d) {
        for (int64_t i = 0; i < d->size; i++) {
            if (d->entries[i].dead) continue;
            int64_t val = dragon_deep_copy_tagged(d->entries[i].value,
                                                  d->entries[i].tag);
            if (d->keys_are_ptr) {
                dragon_incref_str(d->entries[i].key);
                dragon_dict_set_tagged(copy, d->entries[i].key, val,
                                       d->entries[i].tag);
            } else {
                dragon_dict_int_set_tagged(
                    copy, (int64_t)(uintptr_t)d->entries[i].key, val,
                    d->entries[i].tag);
            }
        }
    }
    return copy;
}

/// Shallow copy (preserves tags and insertion order)
DragonDict* dragon_dict_copy(DragonDict* d) {
    DragonDict* copy = dragon_dict_new(d ? d->capacity : 4);
    if (d) {
        for (int64_t i = 0; i < d->size; i++) {
            if (d->entries[i].dead) continue;
            int64_t val = d->entries[i].value;
            uint8_t tag = d->entries[i].tag;
            // Incref copied values: both old and new dict own references.
            // dragon_incref_tagged covers STR/LIST/DICT/BYTES like the old
            // hand-rolled chain AND DRAGON_TAG_CLOSURE, which the chain
            // missed (dict[str, Callable].copy() double-freed closures).
            if (val) dragon_incref_tagged(val, tag);
            // Key dispatch by key flavor: an int-keyed dict stores a raw i64
            // in the key slot; the str setter dereferenced it as a char* in
            // dict_hash (SEGV on the first key) and flipped keys_are_ptr on
            // the copy, so even a surviving copy would decref ints as
            // strings on destroy.
            if (d->keys_are_ptr) {
                // The copy takes its OWN ref to each str key (mirrors the
                // value incref) so both dicts release independently.
                dragon_incref_str(d->entries[i].key);
                dragon_dict_set_tagged(copy, d->entries[i].key, val, tag);
            } else {
                dragon_dict_int_set_tagged(copy,
                    (int64_t)(uintptr_t)d->entries[i].key, val, tag);
            }
        }
    }
    return copy;
}

//===----------------------------------------------------------------------===//
// D030 Phase 3.E - Typed dict ops for str-keyed monomorphic dicts.
//
// Storage stays on the existing DragonDict layout (per-entry tag preserved
// for compatibility with mixed-type dicts and the GC traverse). Codegen
// calls these typed wrappers so the value crosses the runtime/codegen
// boundary at its native LLVM type - no i64 funnel at the call site.
// The value↔i64 reinterpret happens once inside each wrapper and the
// compiler will inline these when the polymorphic core is also inlinable.
//===----------------------------------------------------------------------===//

/// dict[str, float] read - returns native double, asserts TAG_FLOAT.
double dragon_dict_get_str_f64(DragonDict* d, const char* key) {
    int64_t bits = dragon_dict_get_checked(d, key, TAG_FLOAT);
    double f;
    memcpy(&f, &bits, sizeof(f));
    return f;
}

/// dict[str, float] write - accepts native double.
void dragon_dict_set_str_f64(DragonDict* d, const char* key, double value) {
    int64_t bits;
    memcpy(&bits, &value, sizeof(bits));
    dragon_dict_set_tagged(d, key, bits, TAG_FLOAT);
}

/// dict[str, <heap>] read - returns native pointer. Tag is the dict's
/// declared value tag (TAG_STR / TAG_LIST / TAG_DICT / TAG_BYTES) for the
/// runtime type check. No bitcast at the call site.
void* dragon_dict_get_str_ptr(DragonDict* d, const char* key, int64_t expected_tag) {
    int64_t bits = dragon_dict_get_checked(d, key, expected_tag);
    return (void*)(uintptr_t)bits;
}

/// dict[str, <heap>] write - accepts native pointer + value tag for
/// refcount semantics on overwrite.
void dragon_dict_set_str_ptr(DragonDict* d, const char* key, void* value, int64_t tag) {
    dragon_dict_set_tagged(d, key, (int64_t)(uintptr_t)value, tag);
}

//===----------------------------------------------------------------------===//
// D030 Phase 3.G - Typed dict ops for int-keyed monomorphic dicts.
//
// Storage reuses DragonDict - the i64 key fits in the same 8-byte slot as
// the str-keyed `const char*` field. Hashing uses SplitMix64 (branchless,
// ~3 cycles) instead of FNV-1a, and probe compares keys as int64 instead
// of running strlen+memcmp. Refcount semantics on overwrite mirror the
// str-keyed setters.
//
// Codegen picks this family when the static dict type is `dict[int, V]`,
// dispatching by V's LLVM type to the right typed setter/getter so values
// cross the codegen↔runtime boundary at their native type.
//===----------------------------------------------------------------------===//

/// SplitMix64. Branchless, no data-dependent branches, well-distributed for
/// monotonic int sequences. The `| 1` keeps hash 0 from colliding with the
/// "no entry" sentinel pattern used by the index probe.
static inline uint64_t dict_hash_i64(int64_t k) {
    uint64_t z = (uint64_t)k + 0x9E3779B97F4A7C15ULL;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return ((z ^ (z >> 31)) | 1ULL);
}

/// Probe the index table for an int-keyed entry. Mirrors dict_probe but
/// compares the entry's key slot reinterpreted as int64.
static int64_t dict_probe_i64(DragonDict* d, int64_t key, uint64_t h) {
    int64_t mask = d->index_size - 1;
    int64_t slot = (int64_t)(h & (uint64_t)mask);
    int64_t first_tombstone = -1;
    for (;;) {
        int64_t idx = d->indices[slot];
        if (idx == DICT_EMPTY) {
            return (first_tombstone >= 0) ? first_tombstone : slot;
        }
        if (idx == DICT_TOMBSTONE) {
            if (first_tombstone < 0) first_tombstone = slot;
        } else {
            if (d->entries[idx].hash == h &&
                (int64_t)(uintptr_t)d->entries[idx].key == key) {
                return slot;
            }
        }
        slot = (slot + 1) & mask;
    }
}

void dragon_dict_int_set_tagged(DragonDict* d, int64_t key, int64_t value, int64_t tag) {
    // Concurrent-mutation detector - see dragon_dict_set_tagged.
    bool mut_armed = dragon_shared_mut_begin(&d->header, "dict");
    // Acyclic-skip enrollment (int-keyed dict): identical to the str-keyed setter.
    // int keys are non-pointers (leaves), so only a traceable VALUE makes the dict
    // cyclic-capable. This is the SECOND dict insert path - it must gate too, or a
    // dict[int, list] holding a cycle would never be tracked (a cycle-leak).
    if (value && dragon_value_tag_is_traceable((int8_t)tag) &&
        !(d->header.gc_flags & GC_FLAG_TRACKED)) {
        dragon_gc_track(d);
    }
    uint64_t h = dict_hash_i64(key);
    int64_t slot = dict_probe_i64(d, key, h);
    int64_t idx = d->indices[slot];

    // Write barrier: a SHARED dict propagates SHARED to its new value (if
    // heap-typed). Int keys are not heap, so the str-key marking the
    // str-keyed setter does is omitted here.
    bool dict_shared = (d->header.gc_flags & GC_FLAG_SHARED) != 0;
    if (dict_shared && value) {
        if (tag == TAG_STR)
            dragon_mark_shared_str((const char*)(uintptr_t)value);
        else if (tag == TAG_LIST || tag == TAG_DICT || tag == TAG_BYTES)
            dragon_mark_shared_deep((void*)(uintptr_t)value);
    }

    if (idx >= 0) {
        // Key exists - update the slot in place FIRST, then drop the old
        // value's ref (reentrancy hardening; see dragon_dict_set_tagged).
        int8_t old_tag = d->entries[idx].tag;
        int64_t old_val = d->entries[idx].value;
        d->entries[idx].value = value;
        d->entries[idx].tag = (int8_t)tag;
        dragon_shared_mut_end(&d->header, mut_armed);
        if (old_val && old_tag == TAG_STR) {
            dragon_decref_str_dispatch((const char*)(uintptr_t)old_val);
        } else if (old_val && (old_tag == TAG_LIST || old_tag == TAG_DICT || old_tag == TAG_BYTES)) {
            dragon_decref_dispatch((void*)(uintptr_t)old_val);
} else if (old_val && old_tag == DRAGON_TAG_CLOSURE) {
            dragon_decref_callable((void*)(uintptr_t)old_val);
        }
        return;
    }

    if (d->size >= d->capacity || d->size * 3 >= d->index_size * 2) {
        if (d->used < d->size) dict_compact(d);
        if (d->size >= d->capacity || d->size * 3 >= d->index_size * 2) dict_grow(d);
        slot = dict_probe_i64(d, key, h);
    }

    int64_t ei = d->size;
    d->entries[ei].hash = h;
    d->entries[ei].key = (const char*)(uintptr_t)key; // i64 stored in 8-byte slot
    d->entries[ei].value = value;
    d->entries[ei].tag = (int8_t)tag;
    d->entries[ei].dead = 0;
    d->indices[slot] = ei;
    d->size++;
    d->used++;
    dragon_shared_mut_end(&d->header, mut_armed);
}

void dragon_dict_int_set(DragonDict* d, int64_t key, int64_t value) {
    dragon_dict_int_set_tagged(d, key, value, TAG_INT);
}

void dragon_dict_int_set_f64(DragonDict* d, int64_t key, double value) {
    int64_t bits;
    memcpy(&bits, &value, sizeof(bits));
    dragon_dict_int_set_tagged(d, key, bits, TAG_FLOAT);
}

void dragon_dict_int_set_str(DragonDict* d, int64_t key, const char* value) {
    dragon_dict_int_set_tagged(d, key, (int64_t)(uintptr_t)value, TAG_STR);
}

void dragon_dict_int_set_ptr(DragonDict* d, int64_t key, void* value, int64_t tag) {
    dragon_dict_int_set_tagged(d, key, (int64_t)(uintptr_t)value, tag);
}

int64_t dragon_dict_int_get(DragonDict* d, int64_t key) {
    uint64_t h = dict_hash_i64(key);
    int64_t slot = dict_probe_i64(d, key, h);
    int64_t idx = d->indices[slot];
    if (idx >= 0) return d->entries[idx].value;
    dragon_raise_keyerror_int(key);
    return 0;
}

int64_t dragon_dict_int_get_tag(DragonDict* d, int64_t key) {
    uint64_t h = dict_hash_i64(key);
    int64_t slot = dict_probe_i64(d, key, h);
    int64_t idx = d->indices[slot];
    if (idx >= 0) return d->entries[idx].tag;
    return TAG_INT;
}

int64_t dragon_dict_int_get_checked(DragonDict* d, int64_t key, int64_t expected_tag) {
    uint64_t h = dict_hash_i64(key);
    int64_t slot = dict_probe_i64(d, key, h);
    int64_t idx = d->indices[slot];
    if (idx < 0) {
        dragon_raise_keyerror_int(key);
        return 0;
    }
    int8_t actual_tag = d->entries[idx].tag;
    if (actual_tag != (int8_t)expected_tag) {
        char tls_msg[256];
        snprintf(tls_msg, sizeof(tls_msg),
                 "TypeError: value for key %ld has tag %d, expected %ld",
                 (long)key, (int)actual_tag, (long)expected_tag);
        dragon_raise_exc_cstr(80, tls_msg);
    }
    return d->entries[idx].value;
}

double dragon_dict_int_get_f64(DragonDict* d, int64_t key) {
    int64_t bits = dragon_dict_int_get_checked(d, key, TAG_FLOAT);
    double f;
    memcpy(&f, &bits, sizeof(f));
    return f;
}

const char* dragon_dict_int_get_str(DragonDict* d, int64_t key) {
    int64_t bits = dragon_dict_int_get_checked(d, key, TAG_STR);
    return (const char*)(uintptr_t)bits;
}

void* dragon_dict_int_get_ptr(DragonDict* d, int64_t key, int64_t expected_tag) {
    int64_t bits = dragon_dict_int_get_checked(d, key, expected_tag);
    return (void*)(uintptr_t)bits;
}

/// Int-keyed sibling of dragon_dict_get_box: read `d[key]` as a {tag, payload}
/// box. Same BORROW contract - the dict keeps the +1 on refcounted payloads.
/// Used by dragon_box_subscript when indexing an Any-boxed int-keyed dict.
DragonBox dragon_dict_int_get_box(DragonDict* d, int64_t key) {
    uint64_t h = dict_hash_i64(key);
    int64_t slot = dict_probe_i64(d, key, h);
    int64_t idx = d->indices[slot];
    if (idx < 0) {
        dragon_raise_keyerror_int(key);
        return {};
    }
    DragonBox box;
    box.tag = (int64_t)d->entries[idx].tag;
    box.payload = d->entries[idx].value;
    return box;
}

int64_t dragon_dict_int_get_default(DragonDict* d, int64_t key, int64_t def) {
    uint64_t h = dict_hash_i64(key);
    int64_t slot = dict_probe_i64(d, key, h);
    int64_t idx = d->indices[slot];
    if (idx >= 0) return d->entries[idx].value;
    return def;
}

// Int-keyed owned-returning getters for HEAP-valued dicts (dict[int, list|...]).
// Same incref-on-return contract as the str-keyed dragon_dict_get_ptr; named
// _owned to avoid the existing dragon_dict_int_get_ptr, which is a tag-checked
// BORROW. Without these, a binding `g: list[int] = d.get(1)` frees the dict's
// stored value -> UAF. dragon_incref is generic over heap kinds.
void* dragon_dict_int_get_owned(DragonDict* d, int64_t key) {
    uint64_t h = dict_hash_i64(key);
    int64_t slot = dict_probe_i64(d, key, h);
    int64_t idx = d->indices[slot];
    if (idx >= 0) {
        void* v = (void*)(uintptr_t)d->entries[idx].value;
        if (v) dragon_incref(v);
        return v;
    }
    dragon_raise_keyerror_int(key);
    return nullptr;
}

void* dragon_dict_int_get_owned_default(DragonDict* d, int64_t key, void* def) {
    uint64_t h = dict_hash_i64(key);
    int64_t slot = dict_probe_i64(d, key, h);
    int64_t idx = d->indices[slot];
    void* v = (idx >= 0) ? (void*)(uintptr_t)d->entries[idx].value : def;
    if (v) dragon_incref(v);
    return v;
}

int64_t dragon_dict_int_has_key(DragonDict* d, int64_t key) {
    uint64_t h = dict_hash_i64(key);
    int64_t slot = dict_probe_i64(d, key, h);
    return d->indices[slot] >= 0 ? 1 : 0;
}

int64_t dragon_dict_int_pop(DragonDict* d, int64_t key) {
    // Concurrent-mutation detector - see dragon_dict_pop.
    bool mut_armed = dragon_shared_mut_begin(&d->header, "dict");
    uint64_t h = dict_hash_i64(key);
    int64_t slot = dict_probe_i64(d, key, h);
    int64_t idx = d->indices[slot];
    if (idx < 0) {
        dragon_shared_mut_end(&d->header, mut_armed);
        dragon_raise_keyerror_int(key);
        return 0;
    }
    int64_t val = d->entries[idx].value;
    d->indices[slot] = DICT_TOMBSTONE;
    d->entries[idx].dead = 1;
    d->entries[idx].key = nullptr;
    d->entries[idx].value = 0;
    d->used--;
    dict_maybe_compact(d);
    dragon_shared_mut_end(&d->header, mut_armed);
    return val;
}

int64_t dragon_dict_int_pop_default(DragonDict* d, int64_t key, int64_t def) {
    // Concurrent-mutation detector - see dragon_dict_pop.
    bool mut_armed = dragon_shared_mut_begin(&d->header, "dict");
    uint64_t h = dict_hash_i64(key);
    int64_t slot = dict_probe_i64(d, key, h);
    int64_t idx = d->indices[slot];
    if (idx < 0) {
        dragon_shared_mut_end(&d->header, mut_armed);
        return def;
    }
    int64_t val = d->entries[idx].value;
    d->indices[slot] = DICT_TOMBSTONE;
    d->entries[idx].dead = 1;
    d->entries[idx].key = nullptr;
    d->entries[idx].value = 0;
    d->used--;
    dict_maybe_compact(d);
    dragon_shared_mut_end(&d->header, mut_armed);
    return val;
}

/// `del d[key]` for int-keyed dicts. See dragon_dict_del for the str twin.
void dragon_dict_int_del(DragonDict* d, int64_t key) {
    // Concurrent-mutation detector - see dragon_dict_pop.
    bool mut_armed = dragon_shared_mut_begin(&d->header, "dict");
    uint64_t h = dict_hash_i64(key);
    int64_t slot = dict_probe_i64(d, key, h);
    int64_t idx = d->indices[slot];
    if (idx < 0) {
        dragon_shared_mut_end(&d->header, mut_armed);
        dragon_raise_keyerror_int(key);
        return;
    }
    int8_t tag = d->entries[idx].tag;
    int64_t val = d->entries[idx].value;
    // Reentrancy hardening: tombstone the entry and restore the dict to a
    // consistent state BEFORE dropping the value's ref (see dragon_dict_del).
    // O(1): mark dead in place (no shift, no rebuild).
    d->indices[slot] = DICT_TOMBSTONE;
    d->entries[idx].dead = 1;
    d->entries[idx].key = nullptr;
    d->entries[idx].value = 0;
    d->used--;
    dict_maybe_compact(d);
    dragon_shared_mut_end(&d->header, mut_armed);
    if (val && tag == TAG_STR) {
        dragon_decref_str_dispatch((const char*)(uintptr_t)val);
    } else if (val && (tag == TAG_LIST || tag == TAG_DICT || tag == TAG_BYTES)) {
        dragon_decref_dispatch((void*)(uintptr_t)val);
} else if (val && tag == DRAGON_TAG_CLOSURE) {
        dragon_decref_callable((void*)(uintptr_t)val);
    }
}

int64_t dragon_dict_int_setdefault(DragonDict* d, int64_t key, int64_t def) {
    uint64_t h = dict_hash_i64(key);
    int64_t slot = dict_probe_i64(d, key, h);
    int64_t idx = d->indices[slot];
    if (idx >= 0) return d->entries[idx].value;
    dragon_dict_int_set(d, key, def);
    return def;
}

// Int-keyed owned-returning setdefault for HEAP-OBJECT-valued dicts.
// Same contract as the str-keyed dragon_dict_setdefault_ptr: present ->
// incref + return the stored value; absent -> store the default with its heap
// `tag` then incref twice (dict's retained copy + the binding), balanced against
// the call site's single owned-temp drain.
void* dragon_dict_int_setdefault_owned(DragonDict* d, int64_t key, void* def, int64_t tag) {
    uint64_t h = dict_hash_i64(key);
    int64_t slot = dict_probe_i64(d, key, h);
    int64_t idx = d->indices[slot];
    if (idx >= 0) {
        void* v = (void*)(uintptr_t)d->entries[idx].value;
        if (v) dragon_incref(v);
        return v;
    }
    dragon_dict_int_set_tagged(d, key, (int64_t)(uintptr_t)def, tag);
    if (def) { dragon_incref(def); dragon_incref(def); }
    return def;
}

DragonList* dragon_dict_int_keys(DragonDict* d) {
    // Returns list[int]: the entry's key slot reinterpreted as int64.
    DragonList* l = dragon_list_new(d ? d->used : 0);
    if (d) {
        for (int64_t i = 0; i < d->size; i++) {
            if (d->entries[i].dead) continue;
            dragon_list_append(l, (int64_t)(uintptr_t)d->entries[i].key);
        }
    }
    return l;
}

// Forward declaration - dragon_box_eq lives in runtime_box.cpp.
struct DragonBoxAbi { int64_t tag; int64_t payload; };
extern int64_t dragon_box_eq(DragonBoxAbi a, DragonBoxAbi b);

/// Deep equality between two str-keyed dicts. Walks `a`'s entries, probes
/// `b` for the same key, and compares values via dragon_box_eq.
///
/// Pointer-identity short-circuits to True; NULL on either side returns 1
/// only if both are NULL. Same-length check pre-empts mismatched dicts in
/// O(1).
///
/// Used by:
///   1. Codegen `dict == dict` when both operands are statically dict[str, V].
///   2. dragon_box_eq on TAG_DICT (Any-typed dict ops; key type is always str).
int64_t dragon_dict_eq(DragonDict* a, DragonDict* b) {
    if (a == b) return 1;
    if (!a || !b) return 0;
    if (a->used != b->used) return 0;
    for (int64_t i = 0; i < a->size; i++) {
        if (a->entries[i].dead) continue;
        const char* k = a->entries[i].key;
        uint64_t h = a->entries[i].hash;
        int64_t bslot = dict_probe(b, k, h);
        int64_t bidx = b->indices[bslot];
        if (bidx < 0) return 0;  // key missing in b
        DragonBoxAbi av;
        av.tag = (int64_t)a->entries[i].tag;
        av.payload = a->entries[i].value;
        DragonBoxAbi bv;
        bv.tag = (int64_t)b->entries[bidx].tag;
        bv.payload = b->entries[bidx].value;
        if (!dragon_box_eq(av, bv)) return 0;
    }
    return 1;
}

/// Deep equality between two int-keyed dicts. Same shape as dragon_dict_eq
/// but probes via dict_probe_i64 (SplitMix64 hash + raw i64 key compare).
/// Codegen picks this when both operands are statically dict[int, V].
int64_t dragon_dict_int_eq(DragonDict* a, DragonDict* b) {
    if (a == b) return 1;
    if (!a || !b) return 0;
    if (a->used != b->used) return 0;
    for (int64_t i = 0; i < a->size; i++) {
        if (a->entries[i].dead) continue;
        int64_t k = (int64_t)(uintptr_t)a->entries[i].key;
        uint64_t h = a->entries[i].hash;
        int64_t bslot = dict_probe_i64(b, k, h);
        int64_t bidx = b->indices[bslot];
        if (bidx < 0) return 0;
        DragonBoxAbi av;
        av.tag = (int64_t)a->entries[i].tag;
        av.payload = a->entries[i].value;
        DragonBoxAbi bv;
        bv.tag = (int64_t)b->entries[bidx].tag;
        bv.payload = b->entries[bidx].value;
        if (!dragon_box_eq(av, bv)) return 0;
    }
    return 1;
}

void dragon_print_dict_int_raw(DragonDict* d) {
    printf("{");
    if (d) {
        bool first = true;
        for (int64_t i = 0; i < d->size; i++) {
            if (d->entries[i].dead) continue;
            if (!first) printf(", ");
            first = false;
            printf("%ld: ", (long)(int64_t)(uintptr_t)d->entries[i].key);
            switch (d->entries[i].tag) {
                case TAG_STR:
                    printf("'%s'", (const char*)(uintptr_t)d->entries[i].value);
                    break;
                case TAG_FLOAT: {
                    double fval;
                    memcpy(&fval, &d->entries[i].value, sizeof(double));
                    char ftmp[64];
                    dragon_format_double_into(fval, ftmp, sizeof(ftmp));
                    fputs(ftmp, stdout);
                    break;
                }
                case TAG_BOOL:
                    printf("%s", d->entries[i].value ? "True" : "False");
                    break;
                case TAG_NONE:
                    printf("None");
                    break;
                case TAG_LIST:
                case TAG_DICT:
                    dict_print_container_value(d->entries[i].tag,
                                               d->entries[i].value);
                    break;
                default:
                    printf("%ld", (long)d->entries[i].value);
                    break;
            }
        }
    }
    printf("}");
}
void dragon_print_dict_int(DragonDict* d) {
    dragon_print_dict_int_raw(d);
    putchar('\n');
}

} // extern "C"
