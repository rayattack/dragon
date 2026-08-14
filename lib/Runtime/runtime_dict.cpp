/// Dragon Runtime - Dict Operations
#include "runtime_internal.h"

extern "C" {
// Tag-gated closure decref (runtime_builtins.cpp): a dict[*, Callable] value
// may be a real DragonClosure or a bare fn ptr; frees the former, no-ops the latter.
void dragon_decref_callable(void* p);

// C5 / Any-box: tag-aware printers for container-valued dict entries. Without
// these, plain dict printers fell through to `%ld` and printed a raw pointer.
void dragon_print_list_nested_raw(DragonList* l);
void dragon_print_dict_nested_raw(DragonDict* d);
void dragon_print_dict_int_nested_raw(DragonDict* d);
void dragon_print_list_box_nested_raw(DragonListBox* l);
void dragon_print_str_raw(const char* s);

// Defined in runtime_builtins.cpp - class-id -> name for `<Name instance>`.
const char* dragon_instance_class_name(void* instance);

// Print one tag-7 dict value. TAG_BYTES == TAG_CLASS share a tag, so gate on
// the real header (a blind cast previously read hundreds of KB out of bounds).
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

// Hash table dict (CPython compact design): dense entries array in insertion
// order + sparse index table (hash -> entry index); -1=empty, -2=tombstone.

static const int64_t DICT_EMPTY = -1;
static const int64_t DICT_TOMBSTONE = -2;



// Keyed string hash (SipHash-1-3, per-process key, HashDoS defense). Uses
// total_bytes so kind=4 (UCS-4) strings hash fully, not truncated at an internal NUL byte.
static uint64_t dict_hash(const char* key) {
    return dragon_str_content_hash(key);
}

static int64_t next_power_of_2(int64_t n) {
    int64_t p = 8;
    while (p < n) {
        if (p > (INT64_MAX >> 1)) {
            dragon_raise_exc_cstr(43, "MemoryError: dict too large");
        }
        p <<= 1;
    }
    return p;
}

// Find the index-table slot for a key: indices[slot] is the entry index if
// found, else DICT_EMPTY/DICT_TOMBSTONE (insertion point).
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
            // Live entry: compare hash first, then byte-equal. Canonical
            // storage makes total_bytes+memcmp safe even for kind=4 (NUL-containing) strings.
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

// Rebuild the index table (after resize/compaction), skipping dead dense
// slots; re-probing every live entry also clears index-table tombstones.
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

// Reclaim dead dense slots: slide live entries down (stable, preserves
// insertion order), shrink size to used, rebuild index. O(size), amortized O(1) per delete.
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

// Compact when at least half the dense slots are dead, bounding tombstone
// density so delete-heavy workloads can't degrade lookups to O(n); 16-slot floor avoids churn on tiny dicts.
static inline void dict_maybe_compact(DragonDict* d) {
    if (d->size >= 16 && d->used * 2 < d->size) dict_compact(d);
}

// Grow entries + index when needed
static void dict_grow(DragonDict* d) {
    // Double entries capacity via a realloc temp (NULL-safe). Byte count now
    // forms inside the trap; a raw i64 multiply here was the shape 7c888e3 removed elsewhere.
    int64_t new_cap = d->capacity * 2;
    DictEntry* etmp = (DictEntry*)dragon_xrealloc_n_or_abort(
        d->entries, new_cap, sizeof(DictEntry));
    d->entries = etmp;
    d->capacity = new_cap;
    // Double index size and rebuild
    int64_t new_isz = d->index_size * 2;
    int64_t* itmp = (int64_t*)dragon_xrealloc_n_or_abort(
        d->indices, new_isz, sizeof(int64_t));
    d->indices = itmp;
    d->index_size = new_isz;
    dict_rebuild_index(d);
}

DragonDict* dragon_dict_new(int64_t cap) {
    if (cap < 4) cap = 4;
    // Buffers allocated before the header (OOM longjmp can't strand it); entries
    // alloc precedes next_power_of_2, whose overflow could hang without this trap.
    auto* entries = (DictEntry*)dragon_xmalloc_n(cap, sizeof(DictEntry));
    // Index table is 2x entries capacity (load factor ~0.5 for good probe performance)
    int64_t index_size = next_power_of_2(cap * 2);
    size_t ibytes;
    if (!dragon_alloc_bytes_try(index_size, sizeof(int64_t), 0, &ibytes)) {
        free(entries);
        dragon_raise_exc_cstr(43, "MemoryError: allocation size overflow");
    }
    auto* indices = (int64_t*)malloc(ibytes);
    if (!indices) { free(entries); dragon_raise_oom(); }
    auto* d = (DragonDict*)malloc(sizeof(DragonDict));
    if (!d) { free(indices); free(entries); dragon_raise_oom(); }
    dragon_obj_init(&d->header, DRAGON_TAG_DICT);
    d->size = 0;
    d->used = 0;
    d->keys_are_ptr = 0;     // flipped to 1 by the str setter; int-keyed dicts stay 0
    d->capacity = cap;
    d->index_size = index_size;
    d->entries = entries;
    d->indices = indices;
    for (int64_t i = 0; i < d->index_size; i++) d->indices[i] = DICT_EMPTY;
    // Acyclic skip: created untracked; set-tagged paths enroll it in cycle
    // tracking on the first traceable value. Counter bump stays unconditional.
    if (__atomic_add_fetch(&gc_alloc_counter, 1, __ATOMIC_RELAXED)
        >= __atomic_load_n(&gc_threshold, __ATOMIC_RELAXED)) {
        dragon_gc_collect();
    }
    return d;
}

// Release one owned str key (dict owns one ref per key, codegen contract in
// Assign.cpp). No-op for int-keyed/NULL/literal/immortal keys; frees only dup'd heap keys.
static inline void dragon_dict_release_key(const DragonDict* d, const char* key) {
    if (!d->keys_are_ptr) return;
    if (key) dragon_decref_str_dispatch(key);
}

void dragon_dict_set_tagged(DragonDict* d, const char* key, int64_t value, int64_t tag) {
    // Concurrent-mutation detector: whole op is the window (like Go's
    // hashWriting); ends before old-value decrefs, or at function exit on insert.
    bool mut_armed = dragon_shared_mut_begin(&d->header, "dict");
    // str-key path: the dict owns one ref per key. Mark str-keyed so
    // destroy/clear/del release keys; int-keyed dicts never flip this bit.
    d->keys_are_ptr = 1;
    // Acyclic-skip enrollment: enroll on the first traceable value inserted
    // (str keys are leaves); shares its gate with dragon_dict_traverse.
    if (value && dragon_value_tag_is_traceable((int8_t)tag) &&
        !(d->header.gc_flags & GC_FLAG_TRACKED)) {
        dragon_gc_track(d);
    }
    uint64_t h = dict_hash(key);
    int64_t slot = dict_probe(d, key, h);
    int64_t idx = d->indices[slot];

    // Write barrier: a SHARED dict propagates SHARED to its key and to a
    // heap-typed value, before either branch so both update and insert are covered.
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
        // Update-in-place keeps the existing key; codegen added one owned ref
        // to the incoming key, so drop it here or re-assigning a key leaks one per call.
        dragon_dict_release_key(d, key);
        // Reentrancy hardening: store the new value before dropping the old
        // ref, so a re-entrant read of d[key] sees `value`, never the freed `old_val`.
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

    // New key: reclaim dead slots via compaction first when the array is
    // full, only realloc-grow when genuinely full of live entries.
    if (d->size >= d->capacity || d->size * 3 >= d->index_size * 2) {
        if (d->used < d->size) dict_compact(d);
        if (d->size >= d->capacity || d->size * 3 >= d->index_size * 2) dict_grow(d);
        // Re-probe: compaction and/or grow rebuilt the index table.
        slot = dict_probe(d, key, h);
    }

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

// Str-valued get(key, default) returns an OWNED reference (incref'd stored
// value or a fresh default copy); the generic BORROW variant double-freed here (registry CSRF/login form UAF).
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

/// D039 Phase 2: dict[str, Any] read returns a {tag, payload} DragonBox by
/// value (BORROW contract; caller increfs if storing it longer-lived).
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
        // Static buffer avoids a strdup leak: the exception machinery stores
        // the msg by borrowed pointer, so fresh heap here would leak forever.
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

/// C9-B `**dict` spread validation: raise TypeError (Python's "unexpected
/// keyword argument") for the first stray key in source order; no-op for int-keyed/empty dicts.
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

/// Fused augmented int assignment `d[key] OP= operand` in one hash+probe (vs
/// get+set probes); KeyError if absent. Codes: 0+= 1-= 2*= 3//= 4%= 5&= 6|= 7^= 8<<= 9>>=
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

// Owned-returning getters for heap-valued dicts: the generic i64 getters
// return a BORROW, so `g = d.get(k)` decref'd at scope exit frees the dict's value (UAF); these incref what they return.
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
    // Incref whatever we return (stored or default); the caller drains its
    // `def` temp after the call, so both paths stay balanced.
    if (v) dragon_incref(v);
    return v;
}

DragonList* dragon_dict_keys(DragonDict* d) {
    if (d && d->keys_are_ptr) {
        // str-keyed: the keys list co-owns each key (incref + TAG_STR) so it
        // stays valid after the dict dies; without this, keys would dangle once the dict frees them.
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
    dragon_print_dict_nested_raw(d);
}
void dragon_print_dict(DragonDict* d) {
    dragon_print_dict_raw(d);
    putchar('\n');
}

void dragon_print_tagged_raw(int64_t value, int64_t tag) {
    switch (tag) {
        case TAG_STR:
            dragon_print_str_raw((const char*)(uintptr_t)value);
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
        case TAG_LIST: {
            DragonObjectHeader* h = (DragonObjectHeader*)(uintptr_t)value;
            if (!h) { printf("None"); break; }
            if (h->type_tag == DRAGON_TAG_LIST_BOX)
                dragon_print_list_box_nested_raw((DragonListBox*)h);
            else
                dragon_print_list_nested_raw((DragonList*)h);
            break;
        }
        case TAG_DICT:
            dragon_print_dict_nested_raw((DragonDict*)(uintptr_t)value);
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

/// Destroy a dict and free its memory (GC support). Child decrefs go through
/// dispatch helpers (atomic variants in atomic-context dealloc).
void dragon_dict_destroy(DragonDict* d) {
    if (!d) return;
    // Phase 5: decref heap-typed values; dead slots already released at
    // delete time, so skip them (re-releasing would double-free).
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

// Additional dict methods (placed here because dict_items needs DragonTuple).

/// Get all values as a list (insertion order)
DragonList* dragon_dict_values(DragonDict* d) {
    // If all entries share one tag, use it so the result list decrefs
    // properly; if mixed, fall back to TAG_INT and skip incref (result is borrowed).
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

/// D039 Phase 9: dict[str, Any].values() returns a DragonListBox with each
/// entry incref'd (OWNED); the old borrowed version let box destroy release refs it never took, UAF'ing the dict's values (ASan-verified).
DragonListBox* dragon_dict_values_box(DragonDict* d) {
    int64_t n = d ? d->used : 0;
    DragonListBox* lb = dragon_list_box_new(n);
    if (!d) return lb;
    for (int64_t i = 0; i < d->size; i++) {
        if (d->entries[i].dead) continue;
        int64_t tag = (int64_t)d->entries[i].tag;
        int64_t payload = d->entries[i].value;
        // Incref only STR/LIST/DICT/BYTES (inverse of what box destroy
        // drops); NOT dragon_incref_tagged, which would leak TAG_CLOSURE (see the WALL note in dragon_listbox_decref_elem, runtime_list.cpp).
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
            // Tuple co-owns the key (incref + TAG_STR) so it survives past
            // the dict's destroy; int-keyed dict stores the i64 untagged (no ownership).
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
/// from `keys` maps to `value` (with shared tag).
DragonDict* dragon_dict_fromkeys(DragonList* keys, int64_t value, int64_t tag) {
    int64_t n = keys ? keys->size : 0;
    DragonDict* d = dragon_dict_new(n > 0 ? n : 4);
    if (!keys) return d;
    for (int64_t i = 0; i < n; i++) {
        // List elements for list[str] are DragonString data pointers, valid
        // C strings for ASCII; dragon_string_dup below handles UCS-4 too.
        const char* keyData = (const char*)(uintptr_t)dragon_list_load(keys, i);
        if (!keyData) continue;
        // Must be dragon_string_dup (mortal +1), never dragon_str_intern: an
        // interned key is immortal with no dedup table, so fromkeys would leak one unfreeable string per key forever (unbounded RSS).
        const char* ownedKey = dragon_string_dup(keyData);
        // Each entry takes its own reference to the value.
        dragon_incref_tagged(value, tag);
        dragon_dict_set_tagged(d, ownedKey, value, tag);
    }
    return d;
}

/// Remove + return the last inserted (key, value) pair as a 2-tuple (Python
/// 3.7+ LIFO semantics); raises KeyError on empty dict. Caller owns the tuple.
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
    // Move the dict's key ref into the tuple (TAG_STR, no incref - ownership
    // transfers once); int-keyed "key" is an i64, stored untagged.
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

/// Pop key, return value (KeyError if absent). O(1): tombstone + mark dead
/// in place (no shift/rebuild); dead slots reclaimed by lazy compaction.
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

/// `del d[key]`: like pop() but the discarded value is decref'd here (values
/// are owned, keys are not). Missing key raises KeyError.
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
    // Reentrancy hardening: tombstone before dropping the value's ref, so a
    // re-entrant finalizer can't observe the removed entry. O(1): mark dead in place.
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
        // Concurrent-mutation detector: whole teardown is the window; per-entry
        // decrefs stay inside it (revisit if __del__ ever lands - no finalizer runs today).
        bool mut_armed = dragon_shared_mut_begin(&d->header, "dict");
        // Reentrancy hardening: null each slot before dropping its ref
        // (snapshot count up front) so a re-entrant read/insert can't corrupt the teardown or re-release a stale key later.
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
        // Key dispatch by source key flavor: routing an int key through the
        // str setter dereferenced it as a char* (SEGV) and poisoned keys_are_ptr on the destination.
        if (other->keys_are_ptr) {
            // d takes its own ref to each str key; without it, d's keys
            // would dangle when `other` dies and d's destroy would double-free.
            dragon_incref_str(other->entries[i].key);
            dragon_dict_set_tagged(d, other->entries[i].key,
                                   other->entries[i].value, other->entries[i].tag);
        } else {
            dragon_dict_int_set_tagged(d, (int64_t)(uintptr_t)other->entries[i].key,
                                       other->entries[i].value, other->entries[i].tag);
        }
    }
}

/// Get or set default. KEY ownership (#20a): codegen hands an owned key; on
/// the present branch it's unused, so release it here or it leaks per call.
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

// Owned-returning setdefault for heap-valued dicts (generic version BORROWs
// and mis-tags TAG_INT on insert): present increfs the stored value; absent stores with the proper tag then increfs TWICE (dict's copy + the binding).
void* dragon_dict_setdefault_ptr(DragonDict* d, const char* key, void* def, int64_t tag) {
    uint64_t h = dict_hash(key);
    int64_t slot = dict_probe(d, key, h);
    int64_t idx = d->indices[slot];
    if (idx >= 0) {
        void* v = (void*)(uintptr_t)d->entries[idx].value;
        if (v) dragon_incref(v);
        // Key present: incoming key is unused; codegen hands an owned key,
        // so release it here or it leaks (#20-followup, literal/immortal keys no-op).
        dragon_dict_release_key(d, key);
        return v;
    }
    // Key absent: dragon_dict_set_tagged adopts one ref of key+value directly.
    // Tags the value so dealloc/cycle-GC see it as heap (unlike the TAG_INT generic path).
    dragon_dict_set_tagged(d, key, (int64_t)(uintptr_t)def, tag);
    if (def) { dragon_incref(def); dragon_incref(def); }
    return def;
}

/// Shallow copy minus named keys (the `**dict` -> `**kwargs` spread path);
/// same ownership rules as dragon_dict_copy.
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
        // dragon_incref_tagged covers closures too; the old chain missed
        // them, so copying dict[str, Callable] double-freed the closure on destroy.
        if (val) dragon_incref_tagged(val, tag);
        // Key dispatch by key flavor: an int-keyed dict's key would be
        // dereferenced as a char* by the str setter (SEGV) if misrouted.
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

/// dub (docs/002 2.7): deep copy; mirrors dragon_dict_copy's key handling,
/// but values deep-copy by tag (dragon_deep_copy_tagged returns +1, adopted by set_tagged).
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
            // dragon_incref_tagged covers closures too; the old chain missed
            // them (dict[str, Callable].copy() double-freed closures).
            if (val) dragon_incref_tagged(val, tag);
            // Key dispatch by key flavor: the str setter would dereference
            // an int key as a char* (SEGV) and poison keys_are_ptr on the copy.
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

// D030 Phase 3.E: typed dict ops for str-keyed monomorphic dicts. Storage
// stays on DragonDict; wrappers cross the codegen boundary at native LLVM type.

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

/// dict[str, <heap>] read - returns native pointer; tag is the dict's
/// declared value tag, used for the runtime type check.
void* dragon_dict_get_str_ptr(DragonDict* d, const char* key, int64_t expected_tag) {
    int64_t bits = dragon_dict_get_checked(d, key, expected_tag);
    return (void*)(uintptr_t)bits;
}

/// dict[str, <heap>] write - accepts native pointer + value tag for
/// refcount semantics on overwrite.
void dragon_dict_set_str_ptr(DragonDict* d, const char* key, void* value, int64_t tag) {
    dragon_dict_set_tagged(d, key, (int64_t)(uintptr_t)value, tag);
}

// D030 Phase 3.G: typed dict ops for int-keyed dicts. Reuses DragonDict (i64
// key in the key slot); hashes via SplitMix64, probes compare keys as int64.

/// SplitMix64: branchless, well-distributed for monotonic int sequences.
/// `| 1` keeps hash 0 from colliding with the index probe's sentinel pattern.
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
    // Acyclic-skip enrollment (int-keyed): identical gate to the str-keyed
    // setter; must also gate here or dict[int, list] cycles go untracked (leak).
    if (value && dragon_value_tag_is_traceable((int8_t)tag) &&
        !(d->header.gc_flags & GC_FLAG_TRACKED)) {
        dragon_gc_track(d);
    }
    uint64_t h = dict_hash_i64(key);
    int64_t slot = dict_probe_i64(d, key, h);
    int64_t idx = d->indices[slot];

    // Write barrier: SHARED propagates to a heap-typed value; int keys are
    // not heap, so the str-key marking the str setter does is omitted here.
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

/// Int-keyed sibling of dragon_dict_get_box: reads `d[key]` as a {tag,
/// payload} box (same BORROW contract).
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

// Int-keyed owned-returning getters (heap-valued dicts); named _owned since
// dragon_dict_int_get_ptr is a BORROW. Without incref, `g = d.get(1)` UAFs the dict's value.
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
    // Reentrancy hardening: tombstone before dropping the value's ref (see
    // dragon_dict_del). O(1): mark dead in place.
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

// Int-keyed owned-returning setdefault (mirrors dragon_dict_setdefault_ptr):
// present increfs+returns the stored value; absent stores + increfs TWICE (dict's copy + the binding).
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

/// Deep equality between two str-keyed dicts: probes `b` for each of `a`'s
/// keys and compares values via dragon_box_eq. Pointer identity and length short-circuit.
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

/// Deep equality between two int-keyed dicts; same shape as dragon_dict_eq
/// but probes via dict_probe_i64 (SplitMix64 hash + raw i64 compare).
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
    dragon_print_dict_int_nested_raw(d);
}
void dragon_print_dict_int(DragonDict* d) {
    dragon_print_dict_int_raw(d);
    putchar('\n');
}

} // extern "C"
