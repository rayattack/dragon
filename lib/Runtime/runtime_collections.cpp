#include "runtime_internal.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>

typedef struct { char* buf; size_t len; size_t cap; } DragonStrBuf;

static void sb_init(DragonStrBuf* b) {
    b->cap = 64;
    b->len = 0;
    b->buf = (char*)dragon_xmalloc(b->cap);
    b->buf[0] = '\0';
}
static void sb_ensure(DragonStrBuf* b, size_t extra) {
    if (b->len + extra + 1 > b->cap) {
        while (b->len + extra + 1 > b->cap) b->cap *= 2;
        b->buf = (char*)dragon_xrealloc(b->buf, b->cap);
    }
}
static void sb_putc(DragonStrBuf* b, char c) {
    sb_ensure(b, 1);
    b->buf[b->len++] = c;
    b->buf[b->len] = '\0';
}
static void sb_puts(DragonStrBuf* b, const char* s) {
    if (!s) return;
    size_t n = strlen(s);
    sb_ensure(b, n);
    memcpy(b->buf + b->len, s, n);
    b->len += n;
    b->buf[b->len] = '\0';
}

static void sb_put_dstr(DragonStrBuf* b, const char* s) {
    if (!s) return;
    int64_t blen = 0;
    char* enc = dragon_str_to_utf8_alloc(s, &blen);
    const char* bytes = enc ? enc : s;
    sb_ensure(b, (size_t)blen);
    memcpy(b->buf + b->len, bytes, (size_t)blen);
    b->len += (size_t)blen;
    b->buf[b->len] = '\0';
    if (enc) free(enc);
}

static void dragon_repr_list(DragonStrBuf* out, DragonList* l);
static void dragon_repr_list_box(DragonStrBuf* out, DragonListBox* l);
static void dragon_repr_dict(DragonStrBuf* out, DragonDict* d);
static void dragon_repr_set(DragonStrBuf* out, DragonSet* s);
static void dragon_repr_tuple(DragonStrBuf* out, DragonTuple* t);

extern "C" const char* dragon_instance_class_name(void* instance);

static void dragon_repr_value(DragonStrBuf* out, int64_t val, uint8_t tag) {
    switch (tag) {
        case TAG_STR: {
            const char* s = (const char*)(uintptr_t)val;
            sb_putc(out, '\'');
            sb_put_dstr(out, s);
            sb_putc(out, '\'');
            break;
        }
        case TAG_FLOAT: {
            double d;
            memcpy(&d, &val, sizeof(double));
            char tmp[64];
            dragon_format_double_into(d, tmp, sizeof(tmp));
            sb_puts(out, tmp);
            break;
        }
        case TAG_BOOL: sb_puts(out, val ? "True" : "False"); break;
        case TAG_NONE: sb_puts(out, "None"); break;
        case TAG_LIST: {
            DragonObjectHeader* h = (DragonObjectHeader*)(uintptr_t)val;
            if (h && h->type_tag == DRAGON_TAG_LIST_BOX)
                dragon_repr_list_box(out, (DragonListBox*)h);
            else
                dragon_repr_list(out, (DragonList*)h);
            break;
        }
        case TAG_DICT: dragon_repr_dict(out, (DragonDict*)(uintptr_t)val); break;
        case TAG_BYTES: {
            DragonObjectHeader* h = (DragonObjectHeader*)(uintptr_t)val;
            if (h && h->type_tag == DRAGON_TAG_BYTES) {
                auto* bv = (DragonBytes*)h;
                sb_puts(out, "b'");
                for (int64_t bi = 0; bi < bv->len; bi++) {
                    uint8_t c = bv->data[bi];
                    char tmp[8];
                    if (c >= 32 && c < 127 && c != '\\' && c != '\'') {
                        tmp[0] = (char)c; tmp[1] = '\0';
                    } else if (c == '\\') { snprintf(tmp, sizeof(tmp), "\\\\"); }
                    else if (c == '\'')  { snprintf(tmp, sizeof(tmp), "\\'"); }
                    else { snprintf(tmp, sizeof(tmp), "\\x%02x", c); }
                    sb_puts(out, tmp);
                }
                sb_putc(out, '\'');
            } else if (!h) {
                sb_puts(out, "None");
            } else {
                const char* nm = dragon_instance_class_name((void*)h);
                char tmp[96];
                if (nm) snprintf(tmp, sizeof(tmp), "<%s instance>", nm);
                else    snprintf(tmp, sizeof(tmp), "<object at 0x%llx>",
                                 (unsigned long long)val);
                sb_puts(out, tmp);
            }
            break;
        }
        default: {
            char tmp[32];
            snprintf(tmp, sizeof(tmp), "%ld", (long)val);
            sb_puts(out, tmp);
            break;
        }
    }
}

static void dragon_repr_list(DragonStrBuf* out, DragonList* l) {
    if (l && dragon_walk_enter(l) != DRAGON_WALK_OK) { sb_puts(out, "[...]"); return; }
    sb_putc(out, '[');
    if (l) {
        for (int64_t i = 0; i < l->size; i++) {
            if (i > 0) sb_puts(out, ", ");
            dragon_repr_value(out, dragon_list_load(l, i), l->elem_tag);
        }
    }
    sb_putc(out, ']');
    if (l) dragon_walk_exit();
}

static void dragon_repr_list_box(DragonStrBuf* out, DragonListBox* l) {
    if (l && dragon_walk_enter(l) != DRAGON_WALK_OK) { sb_puts(out, "[...]"); return; }
    sb_putc(out, '[');
    if (l) {
        for (int64_t i = 0; i < l->size; i++) {
            if (i > 0) sb_puts(out, ", ");
            dragon_repr_value(out, l->data[i].payload, (uint8_t)l->data[i].tag);
        }
    }
    sb_putc(out, ']');
    if (l) dragon_walk_exit();
}

static void dragon_repr_tuple(DragonStrBuf* out, DragonTuple* t) {
    if (t && dragon_walk_enter(t) != DRAGON_WALK_OK) { sb_puts(out, "(...)"); return; }
    sb_putc(out, '(');
    if (t) {
        for (int64_t i = 0; i < t->length; i++) {
            if (i > 0) sb_puts(out, ", ");
            uint8_t tag = t->elem_tags ? t->elem_tags[i] : (uint8_t)TAG_INT;
            dragon_repr_value(out, t->data[i], tag);
        }
        if (t->length == 1) sb_putc(out, ',');
    }
    sb_putc(out, ')');
    if (t) dragon_walk_exit();
}

static void dragon_repr_set(DragonStrBuf* out, DragonSet* s) {
    if (!s || s->count == 0) {
        sb_puts(out, "set()");
        return;
    }
    if (dragon_walk_enter(s) != DRAGON_WALK_OK) { sb_puts(out, "{...}"); return; }
    sb_putc(out, '{');
    bool first = true;
    for (int64_t i = 0; i < s->capacity; i++) {
        if (s->states[i] == 1) {
            if (!first) sb_puts(out, ", ");
            first = false;
            dragon_repr_value(out, s->buckets[i], s->elem_tag);
        }
    }
    sb_putc(out, '}');
    dragon_walk_exit();
}

static void dragon_repr_dict(DragonStrBuf* out, DragonDict* d) {
    if (d && dragon_walk_enter(d) != DRAGON_WALK_OK) { sb_puts(out, "{...}"); return; }
    sb_putc(out, '{');
    if (d) {
        bool first = true;
        for (int64_t i = 0; i < d->size; i++) {
            if (d->entries[i].dead) continue;
            if (!first) sb_puts(out, ", ");
            first = false;
            DictEntry& e = d->entries[i];
            sb_putc(out, '\'');
            sb_put_dstr(out, e.key);
            sb_puts(out, "': ");
            dragon_repr_value(out, e.value, (uint8_t)e.tag);
        }
    }
    sb_putc(out, '}');
    if (d) dragon_walk_exit();
}

static void dragon_repr_dict_int(DragonStrBuf* out, DragonDict* d) {
    if (d && dragon_walk_enter(d) != DRAGON_WALK_OK) { sb_puts(out, "{...}"); return; }
    sb_putc(out, '{');
    if (d) {
        bool first = true;
        for (int64_t i = 0; i < d->size; i++) {
            if (d->entries[i].dead) continue;
            if (!first) sb_puts(out, ", ");
            first = false;
            char tmp[64];
            if (d->key_kind == DRAGON_DICT_KEY_FLOAT) {
                double fk;
                int64_t bits = (int64_t)(uintptr_t)d->entries[i].key;
                memcpy(&fk, &bits, sizeof(double));
                dragon_format_double_into(fk, tmp, sizeof(tmp));
            } else {
                snprintf(tmp, sizeof(tmp), "%ld",
                         (long)(int64_t)(uintptr_t)d->entries[i].key);
            }
            sb_puts(out, tmp);
            sb_puts(out, ": ");
            dragon_repr_value(out, d->entries[i].value, (uint8_t)d->entries[i].tag);
        }
    }
    sb_putc(out, '}');
    if (d) dragon_walk_exit();
}

static void dragon_json_escape(DragonStrBuf* out, const char* s) {
    sb_putc(out, '"');
    if (s) {
        int64_t blen = 0;
        char* enc = dragon_str_to_utf8_alloc(s, &blen);
        const unsigned char* p = (const unsigned char*)(enc ? enc : s);
        for (int64_t i = 0; i < blen; ++i) {
            unsigned char c = p[i];
            switch (c) {
                case '"':  sb_puts(out, "\\\""); break;
                case '\\': sb_puts(out, "\\\\"); break;
                case '\n': sb_puts(out, "\\n"); break;
                case '\t': sb_puts(out, "\\t"); break;
                case '\r': sb_puts(out, "\\r"); break;
                case '\b': sb_puts(out, "\\b"); break;
                case '\f': sb_puts(out, "\\f"); break;
                default:
                    if (c < 0x20) {
                        char u[8];
                        snprintf(u, sizeof(u), "\\u%04x", (unsigned)c);
                        sb_puts(out, u);
                    } else {
                        sb_putc(out, (char)c);
                    }
            }
        }
        if (enc) free(enc);
    }
    sb_putc(out, '"');
}

static void dragon_json_list(DragonStrBuf* out, DragonList* l);
static void dragon_json_list_box(DragonStrBuf* out, DragonListBox* l);
static void dragon_json_dict(DragonStrBuf* out, DragonDict* d);

static thread_local const char* json_dumps_error = nullptr;
static thread_local char json_dumps_error_buf[128];
static thread_local int64_t json_dumps_error_code = 80;

static bool dragon_json_guard(DragonStrBuf* out, const void* obj) {
    int g = dragon_walk_enter(obj);
    if (g == DRAGON_WALK_OK) return true;
    if (!json_dumps_error) {
        if (g == DRAGON_WALK_CYCLE) {
            snprintf(json_dumps_error_buf, sizeof(json_dumps_error_buf),
                     "ValueError: Circular reference detected");
            json_dumps_error_code = 90;
        } else {
            snprintf(json_dumps_error_buf, sizeof(json_dumps_error_buf),
                     "RecursionError: maximum recursion depth exceeded while encoding a JSON object");
            json_dumps_error_code = 72;
        }
        json_dumps_error = json_dumps_error_buf;
    }
    sb_puts(out, "null");
    return false;
}

static void dragon_json_value(DragonStrBuf* out, int64_t val, uint8_t tag) {
    switch (tag) {
        case TAG_STR: dragon_json_escape(out, (const char*)(uintptr_t)val); break;
        case TAG_FLOAT: {
            double d; memcpy(&d, &val, sizeof(double));
            char tmp[64]; dragon_format_double_into(d, tmp, sizeof(tmp));
            sb_puts(out, tmp);
            break;
        }
        case TAG_BOOL: sb_puts(out, val ? "true" : "false"); break;
        case TAG_NONE: sb_puts(out, "null"); break;
        case TAG_LIST: {
            DragonObjectHeader* h = (DragonObjectHeader*)(uintptr_t)val;
            if (val && h->type_tag == DRAGON_TAG_LIST_BOX)
                dragon_json_list_box(out, (DragonListBox*)(uintptr_t)val);
            else
                dragon_json_list(out, (DragonList*)(uintptr_t)val);
            break;
        }
        case TAG_DICT: dragon_json_dict(out, (DragonDict*)(uintptr_t)val); break;
        case TAG_BYTES: {
            DragonObjectHeader* h = (DragonObjectHeader*)(uintptr_t)val;
            const char* nm = nullptr;
            if (h && h->type_tag != DRAGON_TAG_BYTES)
                nm = dragon_instance_class_name((void*)h);
            snprintf(json_dumps_error_buf, sizeof(json_dumps_error_buf),
                     "TypeError: Object of type %s is not JSON serializable",
                     (h && h->type_tag == DRAGON_TAG_BYTES) ? "bytes"
                                                            : (nm ? nm : "object"));
            json_dumps_error = json_dumps_error_buf;
            json_dumps_error_code = 80;
            sb_puts(out, "null");
            break;
        }
        default: {
            char tmp[32]; snprintf(tmp, sizeof(tmp), "%lld", (long long)val);
            sb_puts(out, tmp);
            break;
        }
    }
}

static void dragon_json_list(DragonStrBuf* out, DragonList* l) {
    if (l && !dragon_json_guard(out, l)) return;
    sb_putc(out, '[');
    if (l) {
        for (int64_t i = 0; i < l->size; i++) {
            if (i > 0) sb_puts(out, ", ");
            dragon_json_value(out, dragon_list_load(l, i), l->elem_tag);
        }
    }
    sb_putc(out, ']');
    if (l) dragon_walk_exit();
}

static void dragon_json_list_box(DragonStrBuf* out, DragonListBox* l) {
    if (l && !dragon_json_guard(out, l)) return;
    sb_putc(out, '[');
    if (l) {
        for (int64_t i = 0; i < l->size; i++) {
            if (i > 0) sb_puts(out, ", ");
            dragon_json_value(out, l->data[i].payload, (uint8_t)l->data[i].tag);
        }
    }
    sb_putc(out, ']');
    if (l) dragon_walk_exit();
}

static void dragon_json_dict(DragonStrBuf* out, DragonDict* d) {
    if (d && !dragon_json_guard(out, d)) return;
    sb_putc(out, '{');
    if (d) {
        bool first = true;
        for (int64_t i = 0; i < d->size; i++) {
            if (d->entries[i].dead) continue;
            if (!first) sb_puts(out, ", ");
            first = false;
            DictEntry& e = d->entries[i];
            if (d->key_kind == DRAGON_DICT_KEY_FLOAT) {
                double fk;
                int64_t bits = (int64_t)(uintptr_t)e.key;
                memcpy(&fk, &bits, sizeof(double));
                char tmp[64];
                dragon_format_double_into(fk, tmp, sizeof(tmp));
                sb_putc(out, '"');
                sb_puts(out, tmp);
                sb_putc(out, '"');
            } else if (d->key_kind == DRAGON_DICT_KEY_INT) {
                char tmp[32];
                snprintf(tmp, sizeof(tmp), "\"%ld\"",
                         (long)(int64_t)(uintptr_t)e.key);
                sb_puts(out, tmp);
            } else {
                dragon_json_escape(out, e.key);
            }
            sb_puts(out, ": ");
            dragon_json_value(out, e.value, (uint8_t)e.tag);
        }
    }
    sb_putc(out, '}');
    if (d) dragon_walk_exit();
}

extern "C" {
const char* dragon_json_dumps(DragonBox box) {
    json_dumps_error = nullptr;
    dragon_walk_reset();
    DragonStrBuf b; sb_init(&b);
    dragon_json_value(&b, box.payload, (uint8_t)box.tag);
    if (json_dumps_error) {
        free(b.buf);
        const char* msg = json_dumps_error;
        json_dumps_error = nullptr;
        dragon_raise_exc_cstr(json_dumps_error_code, msg);
        return dragon_string_alloc("", 0);
    }
    const char* r = dragon_string_alloc(b.buf, (int64_t)b.len);
    free(b.buf);
    return r;
}

const char* dragon_json_escape_str(const char* s) {
    DragonStrBuf b; sb_init(&b);
    dragon_json_escape(&b, s);
    const char* r = dragon_string_alloc(b.buf, (int64_t)b.len);
    free(b.buf);
    return r;
}

const char* dragon_list_to_str(DragonList* l) {
    dragon_walk_reset();
    DragonStrBuf b; sb_init(&b); dragon_repr_list(&b, l);
    const char* r = dragon_string_alloc(b.buf, (int64_t)b.len); free(b.buf); return r;
}
const char* dragon_list_box_to_str(DragonListBox* l) {
    dragon_walk_reset();
    DragonStrBuf b; sb_init(&b); dragon_repr_list_box(&b, l);
    const char* r = dragon_string_alloc(b.buf, (int64_t)b.len); free(b.buf); return r;
}
const char* dragon_dict_to_str(DragonDict* d) {
    dragon_walk_reset();
    DragonStrBuf b; sb_init(&b); dragon_repr_dict(&b, d);
    const char* r = dragon_string_alloc(b.buf, (int64_t)b.len); free(b.buf); return r;
}
const char* dragon_dict_int_to_str(DragonDict* d) {
    dragon_walk_reset();
    DragonStrBuf b; sb_init(&b); dragon_repr_dict_int(&b, d);
    const char* r = dragon_string_alloc(b.buf, (int64_t)b.len); free(b.buf); return r;
}
const char* dragon_set_to_str(DragonSet* s) {
    dragon_walk_reset();
    DragonStrBuf b; sb_init(&b); dragon_repr_set(&b, s);
    const char* r = dragon_string_alloc(b.buf, (int64_t)b.len); free(b.buf); return r;
}
const char* dragon_tuple_to_str(DragonTuple* t) {
    dragon_walk_reset();
    DragonStrBuf b; sb_init(&b); dragon_repr_tuple(&b, t);
    const char* r = dragon_string_alloc(b.buf, (int64_t)b.len); free(b.buf); return r;
}

void dragon_print_list_nested_raw(DragonList* l) {
    dragon_walk_reset();
    DragonStrBuf b; sb_init(&b); dragon_repr_list(&b, l);
    fwrite(b.buf, 1, (size_t)b.len, stdout); free(b.buf);
}
void dragon_print_dict_nested_raw(DragonDict* d) {
    dragon_walk_reset();
    DragonStrBuf b; sb_init(&b); dragon_repr_dict(&b, d);
    fwrite(b.buf, 1, (size_t)b.len, stdout); free(b.buf);
}
void dragon_print_dict_int_nested_raw(DragonDict* d) {
    dragon_walk_reset();
    DragonStrBuf b; sb_init(&b); dragon_repr_dict_int(&b, d);
    fwrite(b.buf, 1, (size_t)b.len, stdout); free(b.buf);
}
void dragon_print_list_box_nested_raw(DragonListBox* l) {
    dragon_walk_reset();
    DragonStrBuf b; sb_init(&b); dragon_repr_list_box(&b, l);
    fwrite(b.buf, 1, (size_t)b.len, stdout); free(b.buf);
}
}

extern "C" {

DragonTuple* dragon_tuple_new(int64_t count) {
    int64_t* data = count > 0
        ? (int64_t*)dragon_xmalloc_n(count, sizeof(int64_t)) : nullptr;
    auto* t = (DragonTuple*)dragon_malloc_nullable(sizeof(DragonTuple));
    if (!t) { free(data); dragon_raise_oom(); }
    dragon_obj_init(&t->header, DRAGON_TAG_TUPLE);
    t->length = count;
    t->data = data;
    t->elem_tags = nullptr;
    if (__atomic_add_fetch(&gc_alloc_counter, 1, __ATOMIC_RELAXED)
        >= __atomic_load_n(&gc_threshold, __ATOMIC_RELAXED)) {
        dragon_gc_collect();
    }
    return t;
}

void dragon_tuple_set_tagged(DragonTuple* t, int64_t index, int64_t val, int64_t tag) {
    if (!t || index < 0 || index >= t->length) return;
    if (val && dragon_value_tag_is_traceable((int8_t)tag) &&
        !(t->header.gc_flags & GC_FLAG_TRACKED)) {
        dragon_gc_track(t);
    }
    if (val && (t->header.gc_flags & GC_FLAG_SHARED)) {
        if (tag == TAG_STR)
            dragon_mark_shared_str((const char*)(uintptr_t)val);
        else if (tag == TAG_LIST || tag == TAG_DICT || tag == TAG_BYTES)
            dragon_mark_shared_deep((void*)(uintptr_t)val);
    }
    if (tag != TAG_INT && !t->elem_tags)
        t->elem_tags = (uint8_t*)dragon_xcalloc_n(t->length, sizeof(uint8_t));
    t->data[index] = val;
    if (tag != TAG_INT) t->elem_tags[index] = (uint8_t)tag;
}

int64_t dragon_tuple_get(DragonTuple* t, int64_t index) {
    if (index < 0) index += t->length;
    if (index < 0 || index >= t->length) {
        dragon_raise_exc_cstr(41, "IndexError: tuple index out of range");
        return 0;
    }
    return t->data[index];
}

void dragon_tuple_set(DragonTuple* t, int64_t index, int64_t val) {
    if (index >= 0 && index < t->length) {
        t->data[index] = val;
    }
}

DragonBox dragon_tuple_box_get(DragonTuple* t, int64_t index) {
    if (index < 0) index += t->length;
    if (index < 0 || index >= t->length) {
        dragon_raise_exc_cstr(41, "IndexError: tuple index out of range");
        return {};
    }
    DragonBox v;
    v.tag = t->elem_tags ? (int64_t)t->elem_tags[index] : (int64_t)TAG_INT;
    v.payload = t->data[index];
    return v;
}

int64_t dragon_tuple_len(DragonTuple* t) {
    return t ? t->length : 0;
}

DragonTuple* dragon_tuple_from_list(DragonList* l) {
    if (!l) return dragon_tuple_new(0);
    int64_t n = l->size;
    DragonTuple* t = dragon_tuple_new(n);
    int64_t tag = (int64_t)l->elem_tag;
    for (int64_t i = 0; i < n; i++) {
        int64_t v = dragon_list_load(l, i);
        dragon_incref_tagged(v, tag);
        dragon_tuple_set_tagged(t, i, v, tag);
    }
    return t;
}

void dragon_print_tuple_raw(DragonTuple* t) {
    DragonStrBuf b; sb_init(&b);
    dragon_repr_tuple(&b, t);
    fwrite(b.buf, 1, b.len, stdout);
    free(b.buf);
}
void dragon_print_tuple(DragonTuple* t) {
    dragon_print_tuple_raw(t);
    putchar('\n');
}

void dragon_tuple_destroy(DragonTuple* t) {
    if (!t) return;
    if (t->elem_tags && t->data) {
        for (int64_t i = 0; i < t->length; i++) {
            uint8_t tag = t->elem_tags[i];
            int64_t val = t->data[i];
            if (val && tag == TAG_STR) {
                dragon_decref_str_dispatch((const char*)(uintptr_t)val);
            } else if (val && (tag == TAG_LIST || tag == TAG_DICT || tag == TAG_BYTES)) {
                dragon_decref_dispatch((void*)(uintptr_t)val);
            } else if (val && tag == DRAGON_TAG_CLOSURE) {
                dragon_decref_callable((void*)(uintptr_t)val);
            }
        }
    }
    free(t->elem_tags);
    free(t->data);
    free(t);
}

static DragonSet* dragon_set_alloc(int64_t cap, uint8_t elem_tag = 0) {
    auto* buckets = (int64_t*)dragon_xcalloc_n(cap, sizeof(int64_t));
    auto* states = (uint8_t*)dragon_calloc_nullable((size_t)cap, sizeof(uint8_t));
    if (!states) { free(buckets); dragon_raise_oom(); }
    auto* s = (DragonSet*)dragon_malloc_nullable(sizeof(DragonSet));
    if (!s) { free(states); free(buckets); dragon_raise_oom(); }
    dragon_obj_init(&s->header, DRAGON_TAG_SET);
    s->capacity = cap;
    s->count = 0;
    s->elem_tag = elem_tag;
    s->buckets = buckets;
    s->states = states;
    dragon_gc_track(s);
    if (__atomic_add_fetch(&gc_alloc_counter, 1, __ATOMIC_RELAXED)
        >= __atomic_load_n(&gc_threshold, __ATOMIC_RELAXED)) {
        dragon_gc_collect();
    }
    return s;
}

static inline uint64_t dragon_set_hash(int64_t val, uint8_t tag) {
    if (tag == TAG_STR && val) {
        return dragon_str_content_hash((const char*)(uintptr_t)val);
    }
    return (uint64_t)val * 2654435761ULL;
}

static inline int dragon_set_value_eq(int64_t a, int64_t b, uint8_t tag) {
    if (a == b) return 1;
    if (tag == TAG_STR) {
        return dragon_str_bytes_equal(
            (const char*)(uintptr_t)a, (const char*)(uintptr_t)b);
    }
    return 0;
}

static void dragon_set_grow(DragonSet* s) {
    int64_t oldCap = s->capacity;
    int64_t* oldBuckets = s->buckets;
    uint8_t* oldStates = s->states;
    s->capacity = oldCap * 2;
    s->buckets = (int64_t*)dragon_xcalloc_n_or_abort(s->capacity, sizeof(int64_t));
    s->states = (uint8_t*)dragon_xcalloc_n_or_abort(s->capacity, sizeof(uint8_t));
    s->count = 0;
    for (int64_t i = 0; i < oldCap; i++) {
        if (oldStates[i] == 1) {
            uint64_t h = dragon_set_hash(oldBuckets[i], s->elem_tag);
            int64_t idx = (int64_t)(h % (uint64_t)s->capacity);
            while (s->states[idx] == 1) {
                idx = (idx + 1) % s->capacity;
            }
            s->buckets[idx] = oldBuckets[i];
            s->states[idx] = 1;
            s->count++;
        }
    }
    free(oldBuckets);
    free(oldStates);
}

DragonSet* dragon_set_new() {
    return dragon_set_alloc(16, 0);
}

DragonSet* dragon_set_new_tagged(int64_t elem_tag) {
    return dragon_set_alloc(16, (uint8_t)elem_tag);
}

void dragon_set_adopt_tag(DragonSet* s, int64_t tag) {
    if (s && s->count == 0 && s->elem_tag == 0 && tag != 0) {
        s->elem_tag = (uint8_t)tag;
    }
}

DragonSet* dragon_set_from_list(DragonList* list) {
    if (!list) return dragon_set_new();
    DragonSet* s = dragon_set_new_tagged(list->elem_tag);
    for (int64_t i = 0; i < list->size; i++) {
        dragon_set_add(s, dragon_list_load(list, i));
    }
    return s;
}

static void dragon_set_delete_slot(DragonSet* s, int64_t idx) {
    s->states[idx] = 0;
    s->buckets[idx] = 0;
    int64_t hole = idx;
    int64_t j = idx;
    for (;;) {
        j = (j + 1) % s->capacity;
        if (s->states[j] != 1) break;
        int64_t home = (int64_t)(dragon_set_hash(s->buckets[j], s->elem_tag)
                                 % (uint64_t)s->capacity);
        bool stays = (hole < j) ? (home > hole && home <= j)
                                : (home > hole || home <= j);
        if (!stays) {
            s->buckets[hole] = s->buckets[j];
            s->states[hole] = 1;
            s->states[j] = 0;
            s->buckets[j] = 0;
            hole = j;
        }
    }
}

void dragon_set_add(DragonSet* s, int64_t val) {
    bool mut_armed = dragon_shared_mut_begin(&s->header, "set");
    if (s->count * 2 >= s->capacity) dragon_set_grow(s);
    uint64_t h = dragon_set_hash(val, s->elem_tag);
    int64_t idx = (int64_t)(h % (uint64_t)s->capacity);
    while (s->states[idx] == 1) {
        if (dragon_set_value_eq(s->buckets[idx], val, s->elem_tag)) {
            dragon_shared_mut_end(&s->header, mut_armed);
            return;
        }
        idx = (idx + 1) % s->capacity;
    }
    dragon_incref_tagged(val, s->elem_tag);
    if (val && (s->header.gc_flags & GC_FLAG_SHARED)) {
        if (s->elem_tag == TAG_STR)
            dragon_mark_shared_str((const char*)(uintptr_t)val);
        else if (s->elem_tag == TAG_LIST || s->elem_tag == TAG_DICT || s->elem_tag == TAG_BYTES)
            dragon_mark_shared_deep((void*)(uintptr_t)val);
    }
    s->buckets[idx] = val;
    s->states[idx] = 1;
    s->count++;
    dragon_shared_mut_end(&s->header, mut_armed);
}

int64_t dragon_set_contains(DragonSet* s, int64_t val) {
    uint64_t h = dragon_set_hash(val, s->elem_tag);
    int64_t idx = (int64_t)(h % (uint64_t)s->capacity);
    while (s->states[idx] != 0) {
        if (s->states[idx] == 1 &&
            dragon_set_value_eq(s->buckets[idx], val, s->elem_tag)) return 1;
        idx = (idx + 1) % s->capacity;
    }
    return 0;
}

void dragon_set_remove(DragonSet* s, int64_t val) {
    bool mut_armed = dragon_shared_mut_begin(&s->header, "set");
    uint64_t h = dragon_set_hash(val, s->elem_tag);
    int64_t idx = (int64_t)(h % (uint64_t)s->capacity);
    while (s->states[idx] != 0) {
        if (s->states[idx] == 1 &&
            dragon_set_value_eq(s->buckets[idx], val, s->elem_tag)) {
            int64_t stored = s->buckets[idx];
            s->count--;
            dragon_set_delete_slot(s, idx);
            dragon_shared_mut_end(&s->header, mut_armed);
            if (stored && s->elem_tag == TAG_STR)
                dragon_decref_str_dispatch((const char*)(uintptr_t)stored);
            else if (stored && (s->elem_tag == TAG_LIST || s->elem_tag == TAG_DICT || s->elem_tag == TAG_BYTES))
                dragon_decref_dispatch((void*)(uintptr_t)stored);
            else if (stored && s->elem_tag == DRAGON_TAG_CLOSURE)
                dragon_decref_callable((void*)(uintptr_t)stored);
            return;
        }
        idx = (idx + 1) % s->capacity;
    }
    dragon_shared_mut_end(&s->header, mut_armed);
    if (s->elem_tag == TAG_INT || s->elem_tag == TAG_BOOL) {
        char msg[64];
        snprintf(msg, sizeof(msg), "KeyError: %lld", (long long)val);
        dragon_raise_exc_cstr(42, msg);
    }
    dragon_raise_exc_cstr(42, "KeyError: value not in set");
}

void dragon_set_discard(DragonSet* s, int64_t val) {
    bool mut_armed = dragon_shared_mut_begin(&s->header, "set");
    uint64_t h = dragon_set_hash(val, s->elem_tag);
    int64_t idx = (int64_t)(h % (uint64_t)s->capacity);
    while (s->states[idx] != 0) {
        if (s->states[idx] == 1 &&
            dragon_set_value_eq(s->buckets[idx], val, s->elem_tag)) {
            int64_t stored = s->buckets[idx];
            s->count--;
            dragon_set_delete_slot(s, idx);
            dragon_shared_mut_end(&s->header, mut_armed);
            if (stored && s->elem_tag == TAG_STR)
                dragon_decref_str_dispatch((const char*)(uintptr_t)stored);
            else if (stored && (s->elem_tag == TAG_LIST || s->elem_tag == TAG_DICT || s->elem_tag == TAG_BYTES))
                dragon_decref_dispatch((void*)(uintptr_t)stored);
            else if (stored && s->elem_tag == DRAGON_TAG_CLOSURE)
                dragon_decref_callable((void*)(uintptr_t)stored);
            return;
        }
        idx = (idx + 1) % s->capacity;
    }
    dragon_shared_mut_end(&s->header, mut_armed);
}

int64_t dragon_set_len(DragonSet* s) {
    return s ? s->count : 0;
}

void dragon_set_clear(DragonSet* s) {
    if (s) {
        bool mut_armed = dragon_shared_mut_begin(&s->header, "set");
        uint8_t tag = s->elem_tag;
        if (tag == TAG_STR) {
            for (int64_t i = 0; i < s->capacity; i++) {
                if (s->states[i] == 1 && s->buckets[i])
                    dragon_decref_str((const char*)(uintptr_t)s->buckets[i]);
            }
        } else if (tag == TAG_LIST || tag == TAG_DICT || tag == TAG_BYTES) {
            for (int64_t i = 0; i < s->capacity; i++) {
                if (s->states[i] == 1 && s->buckets[i])
                    dragon_decref((void*)(uintptr_t)s->buckets[i]);
            }
        } else if (tag == DRAGON_TAG_CLOSURE) {
            for (int64_t i = 0; i < s->capacity; i++) {
                if (s->states[i] == 1 && s->buckets[i])
                    dragon_decref_callable((void*)(uintptr_t)s->buckets[i]);
            }
        }
        memset(s->states, 0, s->capacity);
        s->count = 0;
        dragon_shared_mut_end(&s->header, mut_armed);
    }
}

DragonSet* dragon_set_copy(DragonSet* s) {
    if (!s) return dragon_set_new();
    auto* n = dragon_set_alloc(s->capacity, s->elem_tag);
    memcpy(n->buckets, s->buckets, s->capacity * sizeof(int64_t));
    memcpy(n->states, s->states, s->capacity * sizeof(uint8_t));
    n->count = s->count;
    uint8_t tag = s->elem_tag;
    if (tag == TAG_STR) {
        for (int64_t i = 0; i < n->capacity; i++) {
            if (n->states[i] == 1 && n->buckets[i])
                dragon_incref_str((const char*)(uintptr_t)n->buckets[i]);
        }
    } else if (tag == TAG_LIST || tag == TAG_DICT || tag == TAG_BYTES) {
        for (int64_t i = 0; i < n->capacity; i++) {
            if (n->states[i] == 1 && n->buckets[i])
                dragon_incref((void*)(uintptr_t)n->buckets[i]);
        }
    } else if (tag == DRAGON_TAG_CLOSURE) {
        // Inverse of the tag-10 arm in dragon_set_destroy: a memcpy'd
        // set[Callable] copy must co-own its closures or both destroys double-free them.
        for (int64_t i = 0; i < n->capacity; i++) {
            if (n->states[i] == 1 && n->buckets[i])
                dragon_incref_callable((void*)(uintptr_t)n->buckets[i]);
        }
    }
    return n;
}

DragonSet* dragon_set_union(DragonSet* a, DragonSet* b) {
    DragonSet* r = dragon_set_copy(a);
    if (b) {
        for (int64_t i = 0; i < b->capacity; i++) {
            if (b->states[i] == 1) dragon_set_add(r, b->buckets[i]);
        }
    }
    return r;
}

DragonSet* dragon_set_intersection(DragonSet* a, DragonSet* b) {
    DragonSet* r = dragon_set_new_tagged(a ? a->elem_tag : 0);
    if (a && b) {
        for (int64_t i = 0; i < a->capacity; i++) {
            if (a->states[i] == 1 && dragon_set_contains(b, a->buckets[i])) {
                dragon_set_add(r, a->buckets[i]);
            }
        }
    }
    return r;
}

DragonSet* dragon_set_difference(DragonSet* a, DragonSet* b) {
    DragonSet* r = dragon_set_new_tagged(a ? a->elem_tag : 0);
    if (a) {
        for (int64_t i = 0; i < a->capacity; i++) {
            if (a->states[i] == 1 && (!b || !dragon_set_contains(b, a->buckets[i]))) {
                dragon_set_add(r, a->buckets[i]);
            }
        }
    }
    return r;
}

DragonSet* dragon_set_symmetric_difference(DragonSet* a, DragonSet* b) {
    DragonSet* r = dragon_set_new_tagged(a ? a->elem_tag : 0);
    if (a) {
        for (int64_t i = 0; i < a->capacity; i++) {
            if (a->states[i] == 1 && (!b || !dragon_set_contains(b, a->buckets[i])))
                dragon_set_add(r, a->buckets[i]);
        }
    }
    if (b) {
        for (int64_t i = 0; i < b->capacity; i++) {
            if (b->states[i] == 1 && (!a || !dragon_set_contains(a, b->buckets[i])))
                dragon_set_add(r, b->buckets[i]);
        }
    }
    return r;
}

int64_t dragon_set_issubset(DragonSet* a, DragonSet* b) {
    if (!a) return 1;
    if (!b) return a->count == 0;
    for (int64_t i = 0; i < a->capacity; i++) {
        if (a->states[i] == 1 && !dragon_set_contains(b, a->buckets[i]))
            return 0;
    }
    return 1;
}

int64_t dragon_set_issuperset(DragonSet* a, DragonSet* b) {
    return dragon_set_issubset(b, a);
}

int64_t dragon_set_isdisjoint(DragonSet* a, DragonSet* b) {
    if (!a || !b) return 1;
    for (int64_t i = 0; i < a->capacity; i++) {
        if (a->states[i] == 1 && dragon_set_contains(b, a->buckets[i]))
            return 0;
    }
    return 1;
}

int64_t dragon_set_pop(DragonSet* s) {
    if (!s || s->count == 0) {
        dragon_raise_exc_cstr(42, "KeyError: 'pop from an empty set'");
        return 0;
    }
    for (int64_t i = 0; i < s->capacity; i++) {
        if (s->states[i] == 1) {
            int64_t v = s->buckets[i];
            s->count--;
            dragon_set_delete_slot(s, i);
            return v;
        }
    }
    dragon_raise_exc_cstr(70, "RuntimeError: set count/state mismatch in pop()");
    return 0;
}

void dragon_set_update(DragonSet* a, DragonSet* b) {
    if (b) {
        for (int64_t i = 0; i < b->capacity; i++) {
            if (b->states[i] == 1) dragon_set_add(a, b->buckets[i]);
        }
    }
}

void dragon_print_set_raw(DragonSet* s) {
    DragonStrBuf b; sb_init(&b); dragon_repr_set(&b, s);
    fwrite(b.buf, 1, (size_t)b.len, stdout); free(b.buf);
}
void dragon_print_set(DragonSet* s) {
    dragon_print_set_raw(s);
    putchar('\n');
}

void dragon_set_destroy(DragonSet* s) {
    if (!s) return;
    uint8_t tag = s->elem_tag;
    if (tag == TAG_STR) {
        for (int64_t i = 0; i < s->capacity; i++) {
            if (s->states[i] == 1 && s->buckets[i])
                dragon_decref_str_dispatch((const char*)(uintptr_t)s->buckets[i]);
        }
    } else if (tag == TAG_LIST || tag == TAG_DICT || tag == TAG_BYTES) {
        for (int64_t i = 0; i < s->capacity; i++) {
            if (s->states[i] == 1 && s->buckets[i])
                dragon_decref_dispatch((void*)(uintptr_t)s->buckets[i]);
        }
    } else if (tag == DRAGON_TAG_CLOSURE) {
        for (int64_t i = 0; i < s->capacity; i++) {
            if (s->states[i] == 1 && s->buckets[i])
                dragon_decref_callable((void*)(uintptr_t)s->buckets[i]);
        }
    }
    free(s->buckets);
    free(s->states);
    free(s);
}

DragonBytes* dragon_bytes_new(const uint8_t* data, int64_t len) {
    auto* buf = (uint8_t*)dragon_xmalloc(len > 0 ? (size_t)len : 1);
    auto* b = (DragonBytes*)dragon_xmalloc(sizeof(DragonBytes));
    dragon_obj_init(&b->header, DRAGON_TAG_BYTES);
    b->len = len;
    b->data = buf;
    if (data && len > 0) memcpy(b->data, data, len);
    else if (len > 0) memset(b->data, 0, (size_t)len);
    return b;
}

DragonBytes* dragon_bytes_from_literal(const char* data, int64_t len) {
    return dragon_bytes_new((const uint8_t*)data, len);
}

DragonBytes* dragon_bytes_from_list(DragonList* list) {
    if (!list) return dragon_bytes_new(nullptr, 0);
    int64_t n = list->size;
    auto* buf = (uint8_t*)dragon_xmalloc(n > 0 ? (size_t)n : 1);
    auto* b = (DragonBytes*)dragon_xmalloc(sizeof(DragonBytes));
    dragon_obj_init(&b->header, DRAGON_TAG_BYTES);
    b->len = n;
    b->data = buf;
    for (int64_t i = 0; i < n; ++i) {
        b->data[i] = (uint8_t)(dragon_list_load(list, i) & 0xFF);
    }
    return b;
}

int64_t dragon_bytes_len(DragonBytes* b) {
    return b ? b->len : 0;
}

uint8_t* dragon_bytes_data(DragonBytes* b) {
    return b ? b->data : nullptr;
}

void dragon_print_bytes_raw(DragonBytes* b) {
    printf("b'");
    if (b) {
        for (int64_t i = 0; i < b->len; i++) {
            uint8_t c = b->data[i];
            if (c == '\\') printf("\\\\");
            else if (c == '\'') printf("\\'");
            else if (c == '\t') printf("\\t");
            else if (c == '\n') printf("\\n");
            else if (c == '\r') printf("\\r");
            else if (c >= 32 && c < 127) printf("%c", c);
            else printf("\\x%02x", c);
        }
    }
    printf("'");
}
void dragon_print_bytes(DragonBytes* b) {
    dragon_print_bytes_raw(b);
    putchar('\n');
}

DragonBytes* dragon_bytes_concat(DragonBytes* a, DragonBytes* b) {
    int64_t na = a ? a->len : 0;
    int64_t nb = b ? b->len : 0;
    if (na > INT64_MAX - nb)
        dragon_raise_exc_cstr(43, "MemoryError: allocation size overflow");
    int64_t newLen = na + nb;
    auto* data = (uint8_t*)dragon_xmalloc(newLen > 0 ? (size_t)newLen : 1);
    auto* result = (DragonBytes*)dragon_xmalloc(sizeof(DragonBytes));
    dragon_obj_init(&result->header, DRAGON_TAG_BYTES);
    result->len = newLen;
    result->data = data;
    if (na > 0) memcpy(data, a->data, na);
    if (nb > 0) memcpy(data + na, b->data, nb);
    return result;
}

DragonBytes* dragon_bytes_repeat(DragonBytes* b, int64_t n) {
    if (!b || n <= 0 || b->len == 0) return dragon_bytes_new(nullptr, 0);
    if (n > INT64_MAX / b->len)
        dragon_raise_exc_cstr(43, "MemoryError: allocation size overflow");
    int64_t newLen = b->len * n;
    auto* data = (uint8_t*)dragon_xmalloc_n(n, (size_t)b->len);
    auto* result = (DragonBytes*)dragon_xmalloc(sizeof(DragonBytes));
    dragon_obj_init(&result->header, DRAGON_TAG_BYTES);
    result->len = newLen;
    result->data = data;
    for (int64_t i = 0; i < n; i++) {
        memcpy(data + i * b->len, b->data, b->len);
    }
    return result;
}

int64_t dragon_bytes_eq(DragonBytes* a, DragonBytes* b) {
    if (!a && !b) return 1;
    if (!a || !b) return 0;
    if (a->len != b->len) return 0;
    return memcmp(a->data, b->data, a->len) == 0 ? 1 : 0;
}

int64_t dragon_bytes_cmp(DragonBytes* a, DragonBytes* b) {
    if (!a && !b) return 0;
    if (!a) return -1;
    if (!b) return 1;
    int64_t minLen = a->len < b->len ? a->len : b->len;
    int r = memcmp(a->data, b->data, minLen);
    if (r != 0) return r < 0 ? -1 : 1;
    if (a->len < b->len) return -1;
    if (a->len > b->len) return 1;
    return 0;
}

int64_t dragon_bytes_get(DragonBytes* b, int64_t index) {
    if (!b) { dragon_raise_exc_cstr(41, "IndexError: bytes index out of range"); }
    if (index < 0) index += b->len;
    if (index < 0 || index >= b->len) {
        dragon_raise_exc_cstr(41, "IndexError: bytes index out of range");
    }
    return (int64_t)b->data[index];
}

DragonBytes* dragon_bytes_slice(DragonBytes* b, int64_t start, int64_t stop, int64_t step) {
    if (!b) return dragon_bytes_new(nullptr, 0);
    if (step == 0) {
        dragon_raise_exc_cstr(90, "ValueError: slice step cannot be zero");
    }
    dragon_slice_indices(b->len, &start, &stop, step);
    int64_t count = 0;
    if (step > 0) { for (int64_t i = start; i < stop; i += step) count++; }
    else { for (int64_t i = start; i > stop; i += step) count++; }
    auto* data = (uint8_t*)dragon_xmalloc(count > 0 ? (size_t)count : 1);
    auto* result = (DragonBytes*)dragon_xmalloc(sizeof(DragonBytes));
    dragon_obj_init(&result->header, DRAGON_TAG_BYTES);
    result->len = count;
    result->data = data;
    int64_t w = 0;
    if (step > 0) { for (int64_t i = start; i < stop; i += step) result->data[w++] = b->data[i]; }
    else { for (int64_t i = start; i > stop; i += step) result->data[w++] = b->data[i]; }
    return result;
}

int64_t dragon_bytes_contains(DragonBytes* b, int64_t byte_val) {
    if (!b) return 0;
    uint8_t target = (uint8_t)byte_val;
    for (int64_t i = 0; i < b->len; i++) {
        if (b->data[i] == target) return 1;
    }
    return 0;
}

int64_t dragon_bytes_contains_bytes(DragonBytes* haystack, DragonBytes* needle) {
    if (!haystack || !needle) return 0;
    if (needle->len == 0) return 1;
    if (needle->len > haystack->len) return 0;
    for (int64_t i = 0; i <= haystack->len - needle->len; i++) {
        if (memcmp(haystack->data + i, needle->data, needle->len) == 0) return 1;
    }
    return 0;
}


const char* dragon_bytes_decode(DragonBytes* b) {
    if (!b || b->len == 0) return dragon_string_alloc("", 0);
    return dragon_string_alloc((const char*)b->data, b->len);
}

const char* dragon_str_from_bytes(DragonBytes* b) {
    if (!b || b->len == 0) return dragon_string_alloc("", 0);
    DragonString* s = dragon_string_alloc_raw(b->len);
    memcpy(s->data, b->data, (size_t)b->len);
    return s->data;
}

DragonBytes* dragon_str_encode(const char* s) {
    if (!s) return dragon_bytes_new(nullptr, 0);
    int64_t blen = 0;
    char* enc = dragon_str_to_utf8_alloc(s, &blen);
    DragonBytes* b = dragon_bytes_new((const uint8_t*)(enc ? enc : s), blen);
    if (enc) free(enc);
    return b;
}

DragonBytes* dragon_str_to_utf8_bytes(const char* s) {
    int64_t blen = 0;
    char* enc = s ? dragon_str_to_utf8_alloc(s, &blen) : nullptr;
    const uint8_t* src = (const uint8_t*)(enc ? enc : (s ? s : ""));
    auto* data = (uint8_t*)dragon_malloc_nullable((size_t)(blen > 0 ? blen : 0) + 1);
    if (!data) { if (enc) free(enc); dragon_raise_oom(); }
    auto* b = (DragonBytes*)dragon_malloc_nullable(sizeof(DragonBytes));
    if (!b) { free(data); if (enc) free(enc); dragon_raise_oom(); }
    dragon_obj_init(&b->header, DRAGON_TAG_BYTES);
    b->len = blen;
    b->data = data;
    if (blen > 0) memcpy(b->data, src, (size_t)blen);
    b->data[blen] = '\0';
    if (enc) free(enc);
    return b;
}


static int64_t bytes_find_impl(DragonBytes* haystack, DragonBytes* needle, bool reverse) {
    if (!haystack || !needle) return -1;
    if (needle->len == 0) return reverse ? haystack->len : 0;
    if (needle->len > haystack->len) return -1;
    if (!reverse) {
        for (int64_t i = 0; i <= haystack->len - needle->len; i++) {
            if (memcmp(haystack->data + i, needle->data, needle->len) == 0) return i;
        }
    } else {
        for (int64_t i = haystack->len - needle->len; i >= 0; i--) {
            if (memcmp(haystack->data + i, needle->data, needle->len) == 0) return i;
        }
    }
    return -1;
}

int64_t dragon_bytes_find(DragonBytes* h, DragonBytes* n) { return bytes_find_impl(h, n, false); }
int64_t dragon_bytes_rfind(DragonBytes* h, DragonBytes* n) { return bytes_find_impl(h, n, true); }

int64_t dragon_bytes_find_from(DragonBytes* h, DragonBytes* n, int64_t start) {
    if (!h || !n) return -1;
    if (start < 0) start = 0;
    if (n->len == 0) return start <= h->len ? start : -1;
    for (int64_t i = start; i <= h->len - n->len; i++) {
        if (memcmp(h->data + i, n->data, n->len) == 0) return i;
    }
    return -1;
}

int64_t dragon_bytes_index_of(DragonBytes* h, DragonBytes* n) {
    int64_t r = bytes_find_impl(h, n, false);
    if (r < 0) dragon_raise_exc_cstr(90, "ValueError: subsequence not found");
    return r;
}

int64_t dragon_bytes_rindex(DragonBytes* h, DragonBytes* n) {
    int64_t r = bytes_find_impl(h, n, true);
    if (r < 0) dragon_raise_exc_cstr(90, "ValueError: subsequence not found");
    return r;
}

int64_t dragon_bytes_count(DragonBytes* haystack, DragonBytes* needle) {
    if (!haystack || !needle || needle->len == 0) return 0;
    if (needle->len > haystack->len) return 0;
    int64_t count = 0;
    for (int64_t i = 0; i <= haystack->len - needle->len; i++) {
        if (memcmp(haystack->data + i, needle->data, needle->len) == 0) {
            count++;
            i += needle->len - 1;
        }
    }
    return count;
}

int64_t dragon_bytes_startswith(DragonBytes* b, DragonBytes* prefix) {
    if (!b || !prefix) return 0;
    if (prefix->len > b->len) return 0;
    return memcmp(b->data, prefix->data, prefix->len) == 0 ? 1 : 0;
}

int64_t dragon_bytes_endswith(DragonBytes* b, DragonBytes* suffix) {
    if (!b || !suffix) return 0;
    if (suffix->len > b->len) return 0;
    return memcmp(b->data + b->len - suffix->len, suffix->data, suffix->len) == 0 ? 1 : 0;
}


DragonBytes* dragon_bytes_replace(DragonBytes* b, DragonBytes* old_b, DragonBytes* new_b) {
    if (!b || !old_b || old_b->len == 0) {
        if (b) return dragon_bytes_new(b->data, b->len);
        return dragon_bytes_new(nullptr, 0);
    }
    int64_t count = 0;
    for (int64_t i = 0; i <= b->len - old_b->len; i++) {
        if (memcmp(b->data + i, old_b->data, old_b->len) == 0) { count++; i += old_b->len - 1; }
    }
    if (count == 0) return dragon_bytes_new(b->data, b->len);
    int64_t delta = new_b ? new_b->len - old_b->len : -old_b->len;
    if (delta > 0 && count > (INT64_MAX - b->len) / delta)
        dragon_raise_exc_cstr(43, "MemoryError: allocation size overflow");
    int64_t newLen = b->len + count * delta;
    auto* data = (uint8_t*)dragon_xmalloc(newLen > 0 ? (size_t)newLen : 1);
    auto* result = (DragonBytes*)dragon_xmalloc(sizeof(DragonBytes));
    dragon_obj_init(&result->header, DRAGON_TAG_BYTES);
    result->len = newLen;
    result->data = data;
    int64_t w = 0;
    for (int64_t i = 0; i < b->len; ) {
        if (i <= b->len - old_b->len && memcmp(b->data + i, old_b->data, old_b->len) == 0) {
            if (new_b && new_b->len > 0) { memcpy(result->data + w, new_b->data, new_b->len); w += new_b->len; }
            i += old_b->len;
        } else {
            result->data[w++] = b->data[i++];
        }
    }
    return result;
}

DragonBytes* dragon_bytes_upper(DragonBytes* b) {
    if (!b) return dragon_bytes_new(nullptr, 0);
    auto* r = dragon_bytes_new(b->data, b->len);
    for (int64_t i = 0; i < r->len; i++) {
        if (r->data[i] >= 'a' && r->data[i] <= 'z') r->data[i] -= 32;
    }
    return r;
}

DragonBytes* dragon_bytes_lower(DragonBytes* b) {
    if (!b) return dragon_bytes_new(nullptr, 0);
    auto* r = dragon_bytes_new(b->data, b->len);
    for (int64_t i = 0; i < r->len; i++) {
        if (r->data[i] >= 'A' && r->data[i] <= 'Z') r->data[i] += 32;
    }
    return r;
}

static bool is_ascii_whitespace(uint8_t c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f';
}

DragonBytes* dragon_bytes_strip(DragonBytes* b) {
    if (!b || b->len == 0) return dragon_bytes_new(nullptr, 0);
    int64_t start = 0, end = b->len;
    while (start < end && is_ascii_whitespace(b->data[start])) start++;
    while (end > start && is_ascii_whitespace(b->data[end - 1])) end--;
    return dragon_bytes_new(b->data + start, end - start);
}

DragonBytes* dragon_bytes_lstrip(DragonBytes* b) {
    if (!b || b->len == 0) return dragon_bytes_new(nullptr, 0);
    int64_t start = 0;
    while (start < b->len && is_ascii_whitespace(b->data[start])) start++;
    return dragon_bytes_new(b->data + start, b->len - start);
}

DragonBytes* dragon_bytes_rstrip(DragonBytes* b) {
    if (!b || b->len == 0) return dragon_bytes_new(nullptr, 0);
    int64_t end = b->len;
    while (end > 0 && is_ascii_whitespace(b->data[end - 1])) end--;
    return dragon_bytes_new(b->data, end);
}


DragonList* dragon_bytes_split(DragonBytes* b, DragonBytes* sep) {
    DragonList* result = dragon_list_new_tagged(8, TAG_BYTES);
    if (!b || b->len == 0) return result;
    if (!sep || sep->len == 0) {
        int64_t i = 0;
        while (i < b->len) {
            while (i < b->len && is_ascii_whitespace(b->data[i])) i++;
            if (i >= b->len) break;
            int64_t start = i;
            while (i < b->len && !is_ascii_whitespace(b->data[i])) i++;
            auto* part = dragon_bytes_new(b->data + start, i - start);
            dragon_list_append(result, (int64_t)(intptr_t)part);
        }
    } else {
        int64_t start = 0;
        for (int64_t i = 0; i <= b->len - sep->len; i++) {
            if (memcmp(b->data + i, sep->data, sep->len) == 0) {
                auto* part = dragon_bytes_new(b->data + start, i - start);
                dragon_list_append(result, (int64_t)(intptr_t)part);
                i += sep->len - 1;
                start = i + 1;
            }
        }
        auto* tail = dragon_bytes_new(b->data + start, b->len - start);
        dragon_list_append(result, (int64_t)(intptr_t)tail);
    }
    return result;
}

DragonBytes* dragon_bytes_join(DragonBytes* sep, DragonList* list) {
    if (!list || list->size == 0) return dragon_bytes_new(nullptr, 0);
    int64_t totalLen = 0;
    for (int64_t i = 0; i < list->size; i++) {
        auto* part = (DragonBytes*)(intptr_t)dragon_list_load(list, i);
        if (part && part->len > INT64_MAX - totalLen)
            dragon_raise_exc_cstr(43, "MemoryError: allocation size overflow");
        if (part) totalLen += part->len;
        if (i > 0 && sep && sep->len > INT64_MAX - totalLen)
            dragon_raise_exc_cstr(43, "MemoryError: allocation size overflow");
        if (i > 0 && sep) totalLen += sep->len;
    }
    auto* data = (uint8_t*)dragon_xmalloc(totalLen > 0 ? (size_t)totalLen : 1);
    auto* result = (DragonBytes*)dragon_xmalloc(sizeof(DragonBytes));
    dragon_obj_init(&result->header, DRAGON_TAG_BYTES);
    result->len = totalLen;
    result->data = data;
    int64_t w = 0;
    for (int64_t i = 0; i < list->size; i++) {
        if (i > 0 && sep && sep->len > 0) { memcpy(result->data + w, sep->data, sep->len); w += sep->len; }
        auto* part = (DragonBytes*)(intptr_t)dragon_list_load(list, i);
        if (part && part->len > 0) { memcpy(result->data + w, part->data, part->len); w += part->len; }
    }
    return result;
}


int64_t dragon_bytes_isdigit(DragonBytes* b) {
    if (!b || b->len == 0) return 0;
    for (int64_t i = 0; i < b->len; i++)
        if (b->data[i] < '0' || b->data[i] > '9') return 0;
    return 1;
}

int64_t dragon_bytes_isalpha(DragonBytes* b) {
    if (!b || b->len == 0) return 0;
    for (int64_t i = 0; i < b->len; i++)
        if (!((b->data[i] >= 'a' && b->data[i] <= 'z') || (b->data[i] >= 'A' && b->data[i] <= 'Z'))) return 0;
    return 1;
}

int64_t dragon_bytes_isalnum(DragonBytes* b) {
    if (!b || b->len == 0) return 0;
    for (int64_t i = 0; i < b->len; i++) {
        uint8_t c = b->data[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))) return 0;
    }
    return 1;
}

int64_t dragon_bytes_isspace(DragonBytes* b) {
    if (!b || b->len == 0) return 0;
    for (int64_t i = 0; i < b->len; i++)
        if (!is_ascii_whitespace(b->data[i])) return 0;
    return 1;
}


const char* dragon_bytes_hex(DragonBytes* b) {
    if (!b || b->len == 0) return dragon_string_alloc("", 0);
    if (b->len > INT64_MAX / 2) {
        dragon_raise_exc_cstr(43, "MemoryError: hex output too large");
    }
    DragonString* ds = dragon_string_alloc_raw(b->len * 2);
    dragon_hex_encode(ds->data, b->data, b->len);
    ds->data[b->len * 2] = '\0';
    return ds->data;
}

DragonBytes* dragon_bytes_fromhex(const char* hex_str) {
    if (!hex_str) return dragon_bytes_new(nullptr, 0);
    if (dragon_str_is_heap(hex_str) &&
        dragon_string_from_data(hex_str)->kind == 4) {
        dragon_raise_exc_cstr(90, "ValueError: non-hexadecimal number found in fromhex() arg");
    }
    auto hexval = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    int64_t slen = (int64_t)strlen(hex_str);
    int64_t nibbles = 0;
    for (int64_t i = 0; i < slen; i++) {
        char c = hex_str[i];
        if (c == ' ') continue;
        if (hexval(c) < 0) {
            dragon_raise_exc_cstr(90, "ValueError: non-hexadecimal number found in fromhex() arg");
        }
        nibbles++;
    }
    if (nibbles % 2 != 0) {
        dragon_raise_exc_cstr(90, "ValueError: non-hexadecimal number found in fromhex() arg");
    }
    int64_t byteLen = nibbles / 2;
    auto* data = (uint8_t*)dragon_xmalloc(byteLen > 0 ? (size_t)byteLen : 1);
    auto* result = (DragonBytes*)dragon_xmalloc(sizeof(DragonBytes));
    dragon_obj_init(&result->header, DRAGON_TAG_BYTES);
    result->len = byteLen;
    result->data = data;
    int64_t w = 0;
    for (int64_t i = 0; i < slen; ) {
        while (i < slen && hex_str[i] == ' ') i++;
        if (i >= slen) break;
        char hi = hex_str[i++];
        while (i < slen && hex_str[i] == ' ') i++;
        char lo = hex_str[i++];
        result->data[w++] = (uint8_t)((hexval(hi) << 4) | hexval(lo));
    }
    return result;
}

void dragon_bytes_destroy(DragonBytes* b) {
    if (!b) return;
    free(b->data);
    free(b);
}

static void _deque_grow(DragonDeque* d) {
    int64_t newCap = d->capacity * 2;
    if (newCap < 8) newCap = 8;
    int64_t* newData = (int64_t*)dragon_xmalloc_n_or_abort(newCap, sizeof(int64_t));
    for (int64_t i = 0; i < d->size; i++) {
        newData[i] = d->data[(d->head + i) % d->capacity];
    }
    free(d->data);
    d->data = newData;
    d->head = 0;
    d->capacity = newCap;
}

int64_t dragon_str_cmp(const char* a, const char* b);

static bool _deque_elem_eq(uint8_t tag, int64_t a, int64_t b) {
    switch (tag) {
        case TAG_STR:
        case TAG_BYTES: {
            const char* sa = (const char*)(uintptr_t)a;
            const char* sb = (const char*)(uintptr_t)b;
            if (sa == sb) return true;
            if (!sa || !sb) return false;
            return dragon_str_cmp(sa, sb) == 0;
        }
        case TAG_FLOAT: {
            double da, db;
            memcpy(&da, &a, sizeof(double));
            memcpy(&db, &b, sizeof(double));
            return da == db;
        }
        default:
            return a == b;
    }
}

DragonDeque* dragon_deque_new(int64_t maxlen, int64_t elem_tag) {
    int64_t capacity = 8;
    if (maxlen >= 0 && maxlen < capacity) capacity = maxlen > 0 ? maxlen : 1;
    auto* data = (int64_t*)dragon_xmalloc_n(capacity, sizeof(int64_t));
    DragonDeque* d = (DragonDeque*)dragon_calloc_nullable(1, sizeof(DragonDeque));
    if (!d) { free(data); dragon_raise_oom(); }
    dragon_obj_init(&d->header, DRAGON_TAG_DEQUE);
    d->data = data;
    d->capacity = capacity;
    d->head = 0;
    d->size = 0;
    d->maxlen = maxlen < 0 ? -1 : maxlen;
    d->elem_tag = (uint8_t)elem_tag;
    return d;
}

void dragon_deque_append(DragonDeque* d, int64_t value, int64_t tag) {
    if (tag) d->elem_tag = (uint8_t)tag;
    if (d->maxlen == 0) return;
    if (value && dragon_value_tag_is_traceable((int8_t)d->elem_tag) &&
        !(d->header.gc_flags & GC_FLAG_TRACKED)) {
        dragon_gc_track(d);
    }
    bool mut_armed = dragon_shared_mut_begin(&d->header, "deque");
    dragon_incref_tagged(value, d->elem_tag);
    if (d->maxlen > 0 && d->size == d->maxlen) {
        int64_t old = d->data[d->head];
        d->head = (d->head + 1) % d->capacity;
        d->size--;
        dragon_decref_tagged(old, d->elem_tag);
    }
    if (d->size == d->capacity) _deque_grow(d);
    int64_t idx = (d->head + d->size) % d->capacity;
    d->data[idx] = value;
    d->size++;
    dragon_shared_mut_end(&d->header, mut_armed);
}

void dragon_deque_appendleft(DragonDeque* d, int64_t value, int64_t tag) {
    if (tag) d->elem_tag = (uint8_t)tag;
    if (d->maxlen == 0) return;
    if (value && dragon_value_tag_is_traceable((int8_t)d->elem_tag) &&
        !(d->header.gc_flags & GC_FLAG_TRACKED)) {
        dragon_gc_track(d);
    }
    bool mut_armed = dragon_shared_mut_begin(&d->header, "deque");
    dragon_incref_tagged(value, d->elem_tag);
    if (d->maxlen > 0 && d->size == d->maxlen) {
        d->size--;
        int64_t old = d->data[(d->head + d->size) % d->capacity];
        dragon_decref_tagged(old, d->elem_tag);
    }
    if (d->size == d->capacity) _deque_grow(d);
    d->head = (d->head - 1 + d->capacity) % d->capacity;
    d->data[d->head] = value;
    d->size++;
    dragon_shared_mut_end(&d->header, mut_armed);
}

int64_t dragon_deque_popleft(DragonDeque* d) {
    if (!d || d->size == 0)
        dragon_raise_exc_cstr(41, "IndexError: pop from an empty deque");
    bool mut_armed = dragon_shared_mut_begin(&d->header, "deque");
    int64_t val = d->data[d->head];
    d->head = (d->head + 1) % d->capacity;
    d->size--;
    dragon_shared_mut_end(&d->header, mut_armed);
    return val;
}

int64_t dragon_deque_pop(DragonDeque* d) {
    if (!d || d->size == 0)
        dragon_raise_exc_cstr(41, "IndexError: pop from an empty deque");
    bool mut_armed = dragon_shared_mut_begin(&d->header, "deque");
    d->size--;
    int64_t idx = (d->head + d->size) % d->capacity;
    int64_t val = d->data[idx];
    dragon_shared_mut_end(&d->header, mut_armed);
    return val;
}

void* dragon_deque_popleft_ptr(DragonDeque* d) {
    return (void*)(uintptr_t)dragon_deque_popleft(d);
}
void* dragon_deque_pop_ptr(DragonDeque* d) {
    return (void*)(uintptr_t)dragon_deque_pop(d);
}

int64_t dragon_deque_len(DragonDeque* d) {
    return d ? d->size : 0;
}

int64_t dragon_deque_contains(DragonDeque* d, int64_t value) {
    if (!d) return 0;
    for (int64_t i = 0; i < d->size; i++) {
        int64_t elem = d->data[(d->head + i) % d->capacity];
        if (_deque_elem_eq(d->elem_tag, elem, value)) return 1;
    }
    return 0;
}

static void dragon_repr_deque(DragonStrBuf* out, DragonDeque* d) {
    sb_puts(out, "deque([");
    if (d) {
        for (int64_t i = 0; i < d->size; i++) {
            if (i > 0) sb_puts(out, ", ");
            dragon_repr_value(out, d->data[(d->head + i) % d->capacity],
                              d->elem_tag);
        }
    }
    sb_putc(out, ']');
    if (d && d->maxlen >= 0) {
        char tmp[32];
        snprintf(tmp, sizeof(tmp), ", maxlen=%ld", (long)d->maxlen);
        sb_puts(out, tmp);
    }
    sb_putc(out, ')');
}

const char* dragon_deque_to_str(DragonDeque* d) {
    DragonStrBuf b; sb_init(&b); dragon_repr_deque(&b, d);
    const char* r = dragon_string_alloc(b.buf, (int64_t)b.len); free(b.buf); return r;
}

void dragon_print_deque_raw(DragonDeque* d) {
    DragonStrBuf b; sb_init(&b); dragon_repr_deque(&b, d);
    fwrite(b.buf, 1, (size_t)b.len, stdout); free(b.buf);
}

DragonDeque* dragon_deque_from_list(void* listPtr, int64_t maxlen) {
    DragonList* l = (DragonList*)listPtr;
    DragonDeque* d = dragon_deque_new(maxlen, l ? l->elem_tag : 0);
    if (!l) return d;
    int64_t start = 0;
    if (maxlen >= 0 && l->size > maxlen) start = l->size - maxlen;
    for (int64_t i = start; i < l->size; i++)
        dragon_deque_append(d, dragon_list_load(l, i), l->elem_tag);
    return d;
}

void dragon_deque_destroy(DragonDeque* d) {
    if (!d) return;
    if (d->data && d->size > 0) {
        for (int64_t i = 0; i < d->size; i++) {
            int64_t v = d->data[(d->head + i) % d->capacity];
            if (!v) continue;
            if (d->elem_tag == TAG_STR) {
                dragon_decref_str_dispatch((const char*)(uintptr_t)v);
            } else if (d->elem_tag == TAG_LIST || d->elem_tag == TAG_DICT ||
                       d->elem_tag == TAG_BYTES) {
                dragon_decref_dispatch((void*)(uintptr_t)v);
            } else if (d->elem_tag == DRAGON_TAG_CLOSURE) {
                dragon_decref_callable((void*)(uintptr_t)v);
            }
        }
    }
    free(d->data);
    free(d);
}

}
