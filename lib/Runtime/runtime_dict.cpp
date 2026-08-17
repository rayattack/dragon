#include "runtime_internal.h"

extern "C" {
void dragon_decref_callable(void* p);

void dragon_print_list_nested_raw(DragonList* l);
void dragon_print_dict_nested_raw(DragonDict* d);
void dragon_print_dict_int_nested_raw(DragonDict* d);
void dragon_print_list_box_nested_raw(DragonListBox* l);
void dragon_print_str_raw(const char* s);

const char* dragon_instance_class_name(void* instance);

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

static const int64_t DICT_EMPTY = -1;
static const int64_t DICT_TOMBSTONE = -2;



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

static int64_t dict_probe_i64(DragonDict* d, int64_t key, uint64_t h);

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

static void dict_compact(DragonDict* d) {
    int64_t w = 0;
    for (int64_t r = 0; r < d->size; r++) {
        if (d->entries[r].dead) continue;
        if (w != r) d->entries[w] = d->entries[r];
        w++;
    }
    d->size = w;
    dict_rebuild_index(d);
}

static inline void dict_maybe_compact(DragonDict* d) {
    if (d->size >= 16 && d->used * 2 < d->size) dict_compact(d);
}

static void dict_grow(DragonDict* d) {
    int64_t new_cap = d->capacity * 2;
    DictEntry* etmp = (DictEntry*)dragon_xrealloc_n_or_abort(
        d->entries, new_cap, sizeof(DictEntry));
    d->entries = etmp;
    d->capacity = new_cap;
    int64_t new_isz = d->index_size * 2;
    int64_t* itmp = (int64_t*)dragon_xrealloc_n_or_abort(
        d->indices, new_isz, sizeof(int64_t));
    d->indices = itmp;
    d->index_size = new_isz;
    dict_rebuild_index(d);
}

DragonDict* dragon_dict_new(int64_t cap) {
    if (cap < 4) cap = 4;
    auto* entries = (DictEntry*)dragon_xmalloc_n(cap, sizeof(DictEntry));
    int64_t index_size = next_power_of_2(cap * 2);
    size_t ibytes;
    if (!dragon_alloc_bytes_try(index_size, sizeof(int64_t), 0, &ibytes)) {
        free(entries);
        dragon_raise_exc_cstr(43, "MemoryError: allocation size overflow");
    }
    auto* indices = (int64_t*)dragon_malloc_nullable(ibytes);
    if (!indices) { free(entries); dragon_raise_oom(); }
    auto* d = (DragonDict*)dragon_malloc_nullable(sizeof(DragonDict));
    if (!d) { free(indices); free(entries); dragon_raise_oom(); }
    dragon_obj_init(&d->header, DRAGON_TAG_DICT);
    d->size = 0;
    d->used = 0;
    d->key_kind = DRAGON_DICT_KEY_INT;
    d->capacity = cap;
    d->index_size = index_size;
    d->entries = entries;
    d->indices = indices;
    for (int64_t i = 0; i < d->index_size; i++) d->indices[i] = DICT_EMPTY;
    if (__atomic_add_fetch(&gc_alloc_counter, 1, __ATOMIC_RELAXED)
        >= __atomic_load_n(&gc_threshold, __ATOMIC_RELAXED)) {
        dragon_gc_collect();
    }
    return d;
}

static inline void dragon_dict_release_key(const DragonDict* d, const char* key) {
    if (d->key_kind != DRAGON_DICT_KEY_STR) return;
    if (key) dragon_decref_str_dispatch(key);
}

void dragon_dict_set_tagged(DragonDict* d, const char* key, int64_t value, int64_t tag) {
    bool mut_armed = dragon_shared_mut_begin(&d->header, "dict");
    d->key_kind = DRAGON_DICT_KEY_STR;
    if (value && dragon_value_tag_is_traceable((int8_t)tag) &&
        !(d->header.gc_flags & GC_FLAG_TRACKED)) {
        dragon_gc_track(d);
    }
    uint64_t h = dict_hash(key);
    int64_t slot = dict_probe(d, key, h);
    int64_t idx = d->indices[slot];

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
        dragon_dict_release_key(d, key);
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

static void dragon_raise_keyerror(const char* key) {
    char buf[256];
    snprintf(buf, sizeof(buf), "KeyError: '%s'", key ? key : "");
    dragon_raise_exc_cstr(42, buf);
}

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
    return TAG_INT;
}

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
        char tls_msg[256];
        snprintf(tls_msg, sizeof(tls_msg),
                 "TypeError: value for key '%s' is %s, not %s",
                 key, tag_name(actual_tag), tag_name(expected_tag));
        dragon_raise_exc_cstr(80, tls_msg);
    }
    return d->entries[idx].value;
}

int64_t dragon_dict_len(DragonDict* d) {
    return d ? d->used : 0;
}

int64_t dragon_dict_has_key(DragonDict* d, const char* key) {
    uint64_t h = dict_hash(key);
    int64_t slot = dict_probe(d, key, h);
    return d->indices[slot] >= 0 ? 1 : 0;
}

void dragon_dict_reject_unknown_keys(DragonDict* d, const char** allowed,
                                     int64_t n, const char* func_name) {
    if (!d || d->key_kind != DRAGON_DICT_KEY_STR) return;
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
        case 3:
        case 4:
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
    dragon_raise_keyerror(key);
    return nullptr;
}

void* dragon_dict_get_ptr_default(DragonDict* d, const char* key, void* def) {
    uint64_t h = dict_hash(key);
    int64_t slot = dict_probe(d, key, h);
    int64_t idx = d->indices[slot];
    void* v = (idx >= 0) ? (void*)(uintptr_t)d->entries[idx].value : def;
    if (v) dragon_incref(v);
    return v;
}

void dragon_dict_mark_float_keys(DragonDict* d) {
    if (d) d->key_kind = DRAGON_DICT_KEY_FLOAT;
}

DragonList* dragon_dict_keys(DragonDict* d) {
    if (d && d->key_kind == DRAGON_DICT_KEY_STR) {
        DragonList* l = dragon_list_new_tagged(d->used, TAG_STR);
        for (int64_t i = 0; i < d->size; i++) {
            if (d->entries[i].dead) continue;
            dragon_incref_str(d->entries[i].key);
            dragon_list_append(l, (int64_t)d->entries[i].key);
        }
        return l;
    }
    DragonList* l = (d && d->key_kind == DRAGON_DICT_KEY_FLOAT)
        ? dragon_list_new_tagged(d->used, TAG_FLOAT)
        : dragon_list_new(d ? d->used : 0);
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
        dragon_dict_release_key(d, d->entries[i].key);
    }
    free(d->entries);
    free(d->indices);
    free(d);
}

DragonList* dragon_dict_values(DragonDict* d) {
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

/// dict[str, Any].values() returns a DragonListBox with each
/// entry incref'd (OWNED); the old borrowed version let box destroy release refs it never took, UAF'ing the dict's values (ASan-verified).
DragonListBox* dragon_dict_values_box(DragonDict* d) {
    int64_t n = d ? d->used : 0;
    DragonListBox* lb = dragon_list_box_new(n);
    if (!d) return lb;
    for (int64_t i = 0; i < d->size; i++) {
        if (d->entries[i].dead) continue;
        int64_t tag = (int64_t)d->entries[i].tag;
        int64_t payload = d->entries[i].value;
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

DragonList* dragon_dict_items(DragonDict* d) {
    DragonList* l = dragon_list_new_tagged(d ? d->used : 0, TAG_LIST);
    if (d) {
        for (int64_t i = 0; i < d->size; i++) {
            if (d->entries[i].dead) continue;
            DragonTuple* t = dragon_tuple_new(2);
            if (d->key_kind == DRAGON_DICT_KEY_STR) {
                dragon_incref_str(d->entries[i].key);
                dragon_tuple_set_tagged(t, 0, (int64_t)d->entries[i].key, TAG_STR);
            } else if (d->key_kind == DRAGON_DICT_KEY_FLOAT) {
                dragon_tuple_set_tagged(t, 0, (int64_t)d->entries[i].key, TAG_FLOAT);
            } else {
                dragon_tuple_set(t, 0, (int64_t)d->entries[i].key);
            }
            dragon_incref_tagged(d->entries[i].value, d->entries[i].tag);
            dragon_tuple_set_tagged(t, 1, d->entries[i].value, d->entries[i].tag);
            dragon_list_append(l, (int64_t)t);
        }
    }
    return l;
}

DragonDict* dragon_dict_fromkeys(DragonList* keys, int64_t value, int64_t tag) {
    int64_t n = keys ? keys->size : 0;
    DragonDict* d = dragon_dict_new(n > 0 ? n : 4);
    if (!keys) return d;
    for (int64_t i = 0; i < n; i++) {
        const char* keyData = (const char*)(uintptr_t)dragon_list_load(keys, i);
        if (!keyData) continue;
        const char* ownedKey = dragon_string_dup(keyData);
        dragon_incref_tagged(value, tag);
        dragon_dict_set_tagged(d, ownedKey, value, tag);
    }
    return d;
}

int64_t dragon_dict_popitem(DragonDict* d) {
    if (!d || d->used == 0) {
        dragon_raise_exc_cstr(42, "KeyError: 'popitem(): dictionary is empty'");
        return 0;
    }
    bool mut_armed = dragon_shared_mut_begin(&d->header, "dict");
    int64_t lastIdx = d->size - 1;
    while (lastIdx >= 0 && d->entries[lastIdx].dead) lastIdx--;
    DictEntry e = d->entries[lastIdx];
    DragonTuple* t = dragon_tuple_new(2);
    if (d->key_kind == DRAGON_DICT_KEY_STR) {
        dragon_tuple_set_tagged(t, 0, (int64_t)e.key, TAG_STR);
    } else if (d->key_kind == DRAGON_DICT_KEY_FLOAT) {
        dragon_tuple_set_tagged(t, 0, (int64_t)e.key, TAG_FLOAT);
    } else {
        dragon_tuple_set(t, 0, (int64_t)e.key);
    }
    dragon_tuple_set_tagged(t, 1, e.value, e.tag);
    int64_t slot = d->key_kind == DRAGON_DICT_KEY_STR ? dict_probe(d, e.key, e.hash)
                                   : dict_probe_i64(d, (int64_t)(uintptr_t)e.key, e.hash);
    d->indices[slot] = DICT_TOMBSTONE;
    d->entries[lastIdx].dead = 1;
    d->entries[lastIdx].key = nullptr;
    d->entries[lastIdx].value = 0;
    d->used--;
    if (lastIdx == d->size - 1) {
        while (d->size > 0 && d->entries[d->size - 1].dead) d->size--;
    }
    dict_maybe_compact(d);
    dragon_shared_mut_end(&d->header, mut_armed);
    return (int64_t)t;
}

int64_t dragon_dict_pop(DragonDict* d, const char* key) {
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

int64_t dragon_dict_pop_default(DragonDict* d, const char* key, int64_t def) {
    bool mut_armed = dragon_shared_mut_begin(&d->header, "dict");
    uint64_t h = dict_hash(key);
    int64_t slot = dict_probe(d, key, h);
    int64_t idx = d->indices[slot];
    if (idx < 0) {
        dragon_shared_mut_end(&d->header, mut_armed);
        return def;
    }
    int64_t val = d->entries[idx].value;
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

void dragon_dict_del(DragonDict* d, const char* key) {
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
    const char* old_key = d->entries[idx].key;
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
    dragon_dict_release_key(d, old_key);
}

void dragon_dict_clear(DragonDict* d) {
    if (d) {
        bool mut_armed = dragon_shared_mut_begin(&d->header, "dict");
        int64_t n = d->size;
        for (int64_t i = 0; i < n; i++) {
            if (d->entries[i].dead) continue;
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

void dragon_dict_update(DragonDict* d, DragonDict* other) {
    if (!other) return;
    for (int64_t i = 0; i < other->size; i++) {
        if (other->entries[i].dead) continue;
        dragon_incref_tagged(other->entries[i].value, other->entries[i].tag);
        if (other->key_kind == DRAGON_DICT_KEY_STR) {
            // d takes its own ref to each str key; without it, d's keys
            // would dangle when `other` dies and d's destroy would double-free.
            dragon_incref_str(other->entries[i].key);
            dragon_dict_set_tagged(d, other->entries[i].key,
                                   other->entries[i].value, other->entries[i].tag);
        } else {
            dragon_dict_int_set_tagged(d, (int64_t)(uintptr_t)other->entries[i].key,
                                       other->entries[i].value, other->entries[i].tag);
            if (other->key_kind == DRAGON_DICT_KEY_FLOAT)
                d->key_kind = DRAGON_DICT_KEY_FLOAT;
        }
    }
}

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

void* dragon_dict_setdefault_ptr(DragonDict* d, const char* key, void* def, int64_t tag) {
    uint64_t h = dict_hash(key);
    int64_t slot = dict_probe(d, key, h);
    int64_t idx = d->indices[slot];
    if (idx >= 0) {
        void* v = (void*)(uintptr_t)d->entries[idx].value;
        if (v) dragon_incref(v);
        dragon_dict_release_key(d, key);
        return v;
    }
    dragon_dict_set_tagged(d, key, (int64_t)(uintptr_t)def, tag);
    if (def) { dragon_incref(def); dragon_incref(def); }
    return def;
}

DragonDict* dragon_dict_copy_excluding(DragonDict* d, const char** names,
                                       int64_t name_count) {
    DragonDict* copy = dragon_dict_new(d ? d->capacity : 4);
    if (!d) return copy;
    copy->key_kind = d->key_kind;
    for (int64_t i = 0; i < d->size; i++) {
        if (d->entries[i].dead) continue;
        if (d->key_kind == DRAGON_DICT_KEY_STR && name_count > 0) {
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
        if (d->key_kind == DRAGON_DICT_KEY_STR) {
            dragon_incref_str(d->entries[i].key);
            dragon_dict_set_tagged(copy, d->entries[i].key, val, tag);
        } else {
            dragon_dict_int_set_tagged(copy, (int64_t)(uintptr_t)d->entries[i].key,
                                       val, tag);
        }
    }
    return copy;
}

DragonDict* dragon_dict_deep_copy(DragonDict* d) {
    DragonDict* copy = dragon_dict_new(d ? d->capacity : 4);
    if (d) {
        copy->key_kind = d->key_kind;
        for (int64_t i = 0; i < d->size; i++) {
            if (d->entries[i].dead) continue;
            int64_t val = dragon_deep_copy_tagged(d->entries[i].value,
                                                  d->entries[i].tag);
            if (d->key_kind == DRAGON_DICT_KEY_STR) {
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

DragonDict* dragon_dict_copy(DragonDict* d) {
    DragonDict* copy = dragon_dict_new(d ? d->capacity : 4);
    if (d) {
        copy->key_kind = d->key_kind;
        for (int64_t i = 0; i < d->size; i++) {
            if (d->entries[i].dead) continue;
            int64_t val = d->entries[i].value;
            uint8_t tag = d->entries[i].tag;
            // dragon_incref_tagged covers closures too; the old chain missed
            // them (dict[str, Callable].copy() double-freed closures).
            if (val) dragon_incref_tagged(val, tag);
            if (d->key_kind == DRAGON_DICT_KEY_STR) {
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

double dragon_dict_get_str_f64(DragonDict* d, const char* key) {
    int64_t bits = dragon_dict_get_checked(d, key, TAG_FLOAT);
    double f;
    memcpy(&f, &bits, sizeof(f));
    return f;
}

void dragon_dict_set_str_f64(DragonDict* d, const char* key, double value) {
    int64_t bits;
    memcpy(&bits, &value, sizeof(bits));
    dragon_dict_set_tagged(d, key, bits, TAG_FLOAT);
}

void* dragon_dict_get_str_ptr(DragonDict* d, const char* key, int64_t expected_tag) {
    int64_t bits = dragon_dict_get_checked(d, key, expected_tag);
    return (void*)(uintptr_t)bits;
}

void dragon_dict_set_str_ptr(DragonDict* d, const char* key, void* value, int64_t tag) {
    dragon_dict_set_tagged(d, key, (int64_t)(uintptr_t)value, tag);
}

static inline uint64_t dict_hash_i64(int64_t k) {
    uint64_t z = (uint64_t)k + 0x9E3779B97F4A7C15ULL;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return ((z ^ (z >> 31)) | 1ULL);
}

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
    bool mut_armed = dragon_shared_mut_begin(&d->header, "dict");
    if (value && dragon_value_tag_is_traceable((int8_t)tag) &&
        !(d->header.gc_flags & GC_FLAG_TRACKED)) {
        dragon_gc_track(d);
    }
    uint64_t h = dict_hash_i64(key);
    int64_t slot = dict_probe_i64(d, key, h);
    int64_t idx = d->indices[slot];

    bool dict_shared = (d->header.gc_flags & GC_FLAG_SHARED) != 0;
    if (dict_shared && value) {
        if (tag == TAG_STR)
            dragon_mark_shared_str((const char*)(uintptr_t)value);
        else if (tag == TAG_LIST || tag == TAG_DICT || tag == TAG_BYTES)
            dragon_mark_shared_deep((void*)(uintptr_t)value);
    }

    if (idx >= 0) {
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
    d->entries[ei].key = (const char*)(uintptr_t)key;
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

void dragon_dict_int_del(DragonDict* d, int64_t key) {
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
    DragonList* l = dragon_list_new(d ? d->used : 0);
    if (d) {
        for (int64_t i = 0; i < d->size; i++) {
            if (d->entries[i].dead) continue;
            dragon_list_append(l, (int64_t)(uintptr_t)d->entries[i].key);
        }
    }
    return l;
}

struct DragonBoxAbi { int64_t tag; int64_t payload; };
extern int64_t dragon_box_eq(DragonBoxAbi a, DragonBoxAbi b);

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

}
