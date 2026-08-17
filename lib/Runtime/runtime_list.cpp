#include "runtime_internal.h"
#include <cstring>

extern "C" {

int64_t dragon_str_cmp(const char* a, const char* b);

void dragon_decref_callable(void* p);

void dragon_print_box_raw(DragonBox box);

DragonList* dragon_list_new_tagged(int64_t capacity, int64_t elem_tag) {
    int64_t cap = capacity > 0 ? capacity : 8;
    uint8_t esize = dragon_list_size_for_tag((uint8_t)elem_tag);
    void* data = dragon_xmalloc_n(cap, esize);

    auto* list = (DragonList*)dragon_xmalloc(sizeof(DragonList));
    dragon_obj_init(&list->header, DRAGON_TAG_LIST);
    list->capacity = cap;
    list->size = 0;
    list->elem_tag = (uint8_t)elem_tag;
    list->elem_size = esize;
    list->data = data;
    if (list->elem_tag != TAG_INT && list->elem_tag != TAG_BOOL) {
        dragon_gc_track(list);
    }
    if (__atomic_add_fetch(&gc_alloc_counter, 1, __ATOMIC_RELAXED)
        >= __atomic_load_n(&gc_threshold, __ATOMIC_RELAXED)) {
        dragon_gc_collect();
    }
    return list;
}

DragonList* dragon_list_new(int64_t capacity) {
    return dragon_list_new_tagged(capacity, 0);
}

void dragon_list_append(DragonList* list, int64_t value) {
    bool mut_armed = dragon_shared_mut_begin(&list->header, "list");
    if (list->size >= list->capacity) {
        int64_t new_cap = list->capacity * 2;
        void* tmp = dragon_realloc_nullable(list->data,
                            dragon_alloc_bytes_or_abort(new_cap, list->elem_size));
        if (!tmp) { fprintf(stderr, "dragon: out of memory\n"); abort(); }
        list->data = tmp;
        list->capacity = new_cap;
    }
    if (value && (list->header.gc_flags & GC_FLAG_SHARED)) {
        if (list->elem_tag == TAG_STR)
            dragon_mark_shared_str((const char*)(uintptr_t)value);
        else if (list->elem_tag == TAG_LIST || list->elem_tag == TAG_DICT ||
                 list->elem_tag == TAG_BYTES)
            dragon_mark_shared_deep((void*)(uintptr_t)value);
    }
    dragon_list_store(list, list->size++, value);
    dragon_shared_mut_end(&list->header, mut_armed);
}

int64_t dragon_list_get(DragonList* list, int64_t index) {
    if (index < 0) index += list->size;
    if (index < 0 || index >= list->size) {
        dragon_raise_exc_cstr(41, "IndexError: list index out of range");
        return 0;
    }
    return dragon_list_load(list, index);
}

void dragon_list_set(DragonList* list, int64_t index, int64_t value) {
    if (index < 0) index += list->size;
    if (index < 0 || index >= list->size) {
        dragon_raise_exc_cstr(41, "IndexError: list assignment index out of range");
        return;
    }
    bool mut_armed = dragon_shared_mut_begin(&list->header, "list");
    int64_t old = dragon_list_load(list, index);
    if (value && (list->header.gc_flags & GC_FLAG_SHARED)) {
        if (list->elem_tag == TAG_STR)
            dragon_mark_shared_str((const char*)(uintptr_t)value);
        else if (list->elem_tag == TAG_LIST || list->elem_tag == TAG_DICT ||
                 list->elem_tag == TAG_BYTES)
            dragon_mark_shared_deep((void*)(uintptr_t)value);
    }
    dragon_list_store(list, index, value);
    dragon_shared_mut_end(&list->header, mut_armed);
    if (old && old != value) {
        if (list->elem_tag == TAG_STR)
            dragon_decref_str_dispatch((const char*)(uintptr_t)old);
        else if (list->elem_tag == TAG_LIST || list->elem_tag == TAG_DICT ||
                 list->elem_tag == TAG_BYTES)
            dragon_decref_dispatch((void*)(uintptr_t)old);
    }
}

int64_t dragon_list_len(DragonList* list) {
    return list ? list->size : 0;
}

void dragon_print_list_int_raw(DragonList* list) {
    printf("[");
    for (int64_t i = 0; i < list->size; i++) {
        if (i > 0) printf(", ");
        printf("%ld", (long)dragon_list_load(list, i));
    }
    printf("]");
}
void dragon_print_list_int(DragonList* list) {
    dragon_print_list_int_raw(list);
    putchar('\n');
}

void dragon_print_list_str_raw(DragonList* list) {
    printf("[");
    for (int64_t i = 0; i < list->size; i++) {
        if (i > 0) printf(", ");
        const char* s = (const char*)(uintptr_t)dragon_list_load(list, i);
        printf("'%s'", s ? s : "");
    }
    printf("]");
}
void dragon_print_list_str(DragonList* list) {
    dragon_print_list_str_raw(list);
    putchar('\n');
}

void dragon_print_list_float_raw(DragonList* list) {
    printf("[");
    for (int64_t i = 0; i < list->size; i++) {
        if (i > 0) printf(", ");
        int64_t bits = dragon_list_load(list, i);
        double d;
        memcpy(&d, &bits, sizeof(double));
        char ftmp[64];
        dragon_format_double_into(d, ftmp, sizeof(ftmp));
        fputs(ftmp, stdout);
    }
    printf("]");
}
void dragon_print_list_float(DragonList* list) {
    dragon_print_list_float_raw(list);
    putchar('\n');
}

void dragon_print_list_bool_raw(DragonList* list) {
    printf("[");
    for (int64_t i = 0; i < list->size; i++) {
        if (i > 0) printf(", ");
        printf("%s", dragon_list_load(list, i) ? "True" : "False");
    }
    printf("]");
}
void dragon_print_list_bool(DragonList* list) {
    dragon_print_list_bool_raw(list);
    putchar('\n');
}

void dragon_list_insert(DragonList* list, int64_t index, int64_t value) {
    bool mut_armed = dragon_shared_mut_begin(&list->header, "list");
    if (index < 0) index += list->size;
    if (index < 0) index = 0;
    if (index > list->size) index = list->size;
    if (list->size >= list->capacity) {
        int64_t new_cap = list->capacity * 2;
        void* tmp = dragon_realloc_nullable(list->data,
                            dragon_alloc_bytes_or_abort(new_cap, list->elem_size));
        if (!tmp) { fprintf(stderr, "dragon: out of memory\n"); abort(); }
        list->data = tmp;
        list->capacity = new_cap;
    }
    uint8_t* base = (uint8_t*)list->data;
    memmove(base + (size_t)(index + 1) * list->elem_size,
            base + (size_t)index * list->elem_size,
            (size_t)(list->size - index) * list->elem_size);
    dragon_incref_tagged(value, list->elem_tag);
    if (value && (list->header.gc_flags & GC_FLAG_SHARED)) {
        if (list->elem_tag == TAG_STR)
            dragon_mark_shared_str((const char*)(uintptr_t)value);
        else if (list->elem_tag == TAG_LIST || list->elem_tag == TAG_DICT ||
                 list->elem_tag == TAG_BYTES)
            dragon_mark_shared_deep((void*)(uintptr_t)value);
    }
    dragon_list_store(list, index, value);
    list->size++;
    dragon_shared_mut_end(&list->header, mut_armed);
}

static bool dragon_list_elem_eq(DragonList* list, int64_t a, int64_t b);

void dragon_list_remove(DragonList* list, int64_t value) {
    bool mut_armed = dragon_shared_mut_begin(&list->header, "list");
    for (int64_t i = 0; i < list->size; i++) {
        int64_t elem = dragon_list_load(list, i);
        if (dragon_list_elem_eq(list, elem, value)) {
            uint8_t* base = (uint8_t*)list->data;
            memmove(base + (size_t)i * list->elem_size,
                    base + (size_t)(i + 1) * list->elem_size,
                    (size_t)(list->size - 1 - i) * list->elem_size);
            list->size--;
            dragon_shared_mut_end(&list->header, mut_armed);
            if (elem) {
                if (list->elem_tag == TAG_STR)
                    dragon_decref_str_dispatch((const char*)(uintptr_t)elem);
                else if (list->elem_tag == TAG_LIST || list->elem_tag == TAG_DICT ||
                         list->elem_tag == TAG_BYTES)
                    dragon_decref_dispatch((void*)(uintptr_t)elem);
                else if (list->elem_tag == DRAGON_TAG_CLOSURE)
                    dragon_decref_callable((void*)(uintptr_t)elem);
            }
            return;
        }
    }
    dragon_shared_mut_end(&list->header, mut_armed);
    dragon_raise_exc_cstr(90, "ValueError: list.remove(x): x not in list");
}

int64_t dragon_list_pop(DragonList* list, int64_t index) {
    if (list->size == 0) {
        dragon_raise_exc_cstr(41, "IndexError: pop from empty list");
        return 0;
    }
    if (index < 0) index += list->size;
    if (index < 0 || index >= list->size) {
        dragon_raise_exc_cstr(41, "IndexError: pop index out of range");
        return 0;
    }
    bool mut_armed = dragon_shared_mut_begin(&list->header, "list");
    int64_t val = dragon_list_load(list, index);
    uint8_t* base = (uint8_t*)list->data;
    memmove(base + (size_t)index * list->elem_size,
            base + (size_t)(index + 1) * list->elem_size,
            (size_t)(list->size - 1 - index) * list->elem_size);
    list->size--;
    dragon_shared_mut_end(&list->header, mut_armed);
    return val;
}

double dragon_list_pop_f64(DragonListF64* list, int64_t index) {
    if (list->size == 0) {
        dragon_raise_exc_cstr(41, "IndexError: pop from empty list");
        return 0.0;
    }
    if (index < 0) index += list->size;
    if (index < 0 || index >= list->size) {
        dragon_raise_exc_cstr(41, "IndexError: pop index out of range");
        return 0.0;
    }
    bool mut_armed = dragon_shared_mut_begin(&list->header, "list");
    double val = list->data[index];
    memmove(list->data + index, list->data + index + 1,
            (size_t)(list->size - 1 - index) * sizeof(double));
    list->size--;
    dragon_shared_mut_end(&list->header, mut_armed);
    return val;
}

void dragon_list_delitem(DragonList* list, int64_t index) {
    if (index < 0) index += list->size;
    if (index < 0 || index >= list->size) {
        dragon_raise_exc_cstr(41, "IndexError: list assignment index out of range");
        return;
    }
    bool mut_armed = dragon_shared_mut_begin(&list->header, "list");
    int64_t elem = dragon_list_load(list, index);
    uint8_t* base = (uint8_t*)list->data;
    memmove(base + (size_t)index * list->elem_size,
            base + (size_t)(index + 1) * list->elem_size,
            (size_t)(list->size - 1 - index) * list->elem_size);
    list->size--;
    dragon_shared_mut_end(&list->header, mut_armed);
    if (elem) {
        if (list->elem_tag == TAG_STR)
            dragon_decref_str_dispatch((const char*)(uintptr_t)elem);
        else if (list->elem_tag == TAG_LIST || list->elem_tag == TAG_DICT ||
                 list->elem_tag == TAG_BYTES)
            dragon_decref_dispatch((void*)(uintptr_t)elem);
        else if (list->elem_tag == DRAGON_TAG_CLOSURE)
            dragon_decref_callable((void*)(uintptr_t)elem);
    }
}

void dragon_list_clear(DragonList* list) {
    bool mut_armed = dragon_shared_mut_begin(&list->header, "list");
    if (list->elem_tag == TAG_STR || list->elem_tag == TAG_LIST ||
        list->elem_tag == TAG_DICT || list->elem_tag == TAG_BYTES ||
        list->elem_tag == DRAGON_TAG_CLOSURE) {
        int64_t n = list->size;
        for (int64_t i = 0; i < n; i++) {
            int64_t elem = dragon_list_load(list, i);
            dragon_list_store(list, i, 0);
            dragon_decref_tagged(elem, list->elem_tag);
        }
    }
    list->size = 0;
    dragon_shared_mut_end(&list->header, mut_armed);
}

void dragon_list_box_extend(DragonListBox* dst, DragonListBox* src);
DragonListBox* dragon_list_box_concat(DragonListBox* a, DragonListBox* b);

void dragon_list_extend(DragonList* list, DragonList* other) {
    if (!other) return;
    if (list && list->header.type_tag == DRAGON_TAG_LIST_BOX) {
        dragon_list_box_extend((DragonListBox*)(void*)list,
                               (DragonListBox*)(void*)other);
        return;
    }
    if (list->size == 0 && list->elem_tag == TAG_INT && other->elem_tag != TAG_INT) {
        list->elem_tag = other->elem_tag;
        uint8_t new_size = dragon_list_size_for_tag(list->elem_tag);
        if (new_size != list->elem_size) {
            void* fresh = dragon_xmalloc_or_abort(
                dragon_alloc_bytes_or_abort(list->capacity, new_size));
            free(list->data);
            list->elem_size = new_size;
            list->data = fresh;
        }
    }
    bool tags_match = (list->elem_tag == other->elem_tag);
    int64_t n = other->size;
    for (int64_t i = 0; i < n; i++) {
        int64_t elem = dragon_list_load(other, i);
        // dragon_incref_tagged covers closures too; the old chain skipped them
        // while destroy decrefs them, double-freeing list[Callable] extends.
        if (elem && tags_match) dragon_incref_tagged(elem, other->elem_tag);
        dragon_list_append(list, elem);
    }
}

static bool dragon_list_elem_eq(DragonList* list, int64_t a, int64_t b) {
    switch (list->elem_tag) {
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

int64_t dragon_list_index(DragonList* list, int64_t value) {
    for (int64_t i = 0; i < list->size; i++) {
        if (dragon_list_elem_eq(list, dragon_list_load(list, i), value)) return i;
    }
    if (list->elem_tag == TAG_INT || list->elem_tag == TAG_BOOL) {
        char msg[64];
        snprintf(msg, sizeof(msg), "ValueError: %lld is not in list", (long long)value);
        dragon_raise_exc_cstr(90, msg);
    }
    dragon_raise_exc_cstr(90, "ValueError: value is not in list");
    return 0;
}

int64_t dragon_list_count(DragonList* list, int64_t value) {
    int64_t c = 0;
    for (int64_t i = 0; i < list->size; i++) {
        if (dragon_list_elem_eq(list, dragon_list_load(list, i), value)) c++;
    }
    return c;
}

int64_t dragon_list_contains(DragonList* list, int64_t value) {
    for (int64_t i = 0; i < list->size; i++) {
        if (dragon_list_elem_eq(list, dragon_list_load(list, i), value)) return 1;
    }
    return 0;
}

void dragon_list_sort_ex(DragonList* list, int64_t reverse) {
    int64_t n = list ? list->size : 0;
    if (n < 2) return;
    if (n > INT64_MAX / 16) {
        dragon_raise_exc_cstr(43, "MemoryError: list too large to sort");
    }
    int64_t* buf = (int64_t*)dragon_malloc_nullable(
        dragon_alloc_bytes(n * 2, sizeof(int64_t)));
    if (!buf) {
        dragon_raise_exc_cstr(43, "MemoryError: out of memory sorting list");
    }
    int64_t* src = buf;
    int64_t* dst = buf + n;
    auto cmp_gt = [&](int64_t a, int64_t b) -> bool {
        switch (list->elem_tag) {
            case TAG_STR:
            case TAG_BYTES:
                return dragon_str_cmp(
                    (const char*)(uintptr_t)a,
                    (const char*)(uintptr_t)b) > 0;
            case TAG_FLOAT: {
                double da, db;
                memcpy(&da, &a, sizeof(double));
                memcpy(&db, &b, sizeof(double));
                return da > db;
            }
            default:
                return a > b;
        }
    };
    bool desc = reverse != 0;
    auto right_first = [&](int64_t l, int64_t r) -> bool {
        return desc ? cmp_gt(r, l) : cmp_gt(l, r);
    };
    bool mut_armed = dragon_shared_mut_begin(&list->header, "list");
    for (int64_t i = 0; i < n; i++) src[i] = dragon_list_load(list, i);
    for (int64_t width = 1; width < n; width *= 2) {
        for (int64_t lo = 0; lo < n; lo += 2 * width) {
            int64_t mid = lo + width < n ? lo + width : n;
            int64_t hi = lo + 2 * width < n ? lo + 2 * width : n;
            int64_t a = lo, b = mid, w = lo;
            while (a < mid && b < hi)
                dst[w++] = right_first(src[a], src[b]) ? src[b++] : src[a++];
            while (a < mid) dst[w++] = src[a++];
            while (b < hi) dst[w++] = src[b++];
        }
        int64_t* tmp = src;
        src = dst;
        dst = tmp;
    }
    for (int64_t i = 0; i < n; i++) dragon_list_store(list, i, src[i]);
    dragon_shared_mut_end(&list->header, mut_armed);
    free(buf);
}

void dragon_list_sort(DragonList* list) {
    dragon_list_sort_ex(list, 0);
}

void dragon_list_reverse(DragonList* list) {
    bool mut_armed = dragon_shared_mut_begin(&list->header, "list");
    for (int64_t i = 0, j = list->size - 1; i < j; i++, j--) {
        int64_t tmp = dragon_list_load(list, i);
        dragon_list_store(list, i, dragon_list_load(list, j));
        dragon_list_store(list, j, tmp);
    }
    dragon_shared_mut_end(&list->header, mut_armed);
}

DragonList* dragon_list_deep_copy(DragonList* list) {
    DragonList* copy = dragon_list_new_tagged(
        list->size > 0 ? list->size : 8, list->elem_tag);
    for (int64_t i = 0; i < list->size; i++) {
        int64_t elem = dragon_list_load(list, i);
        dragon_list_append(copy,
                           dragon_deep_copy_tagged(elem, list->elem_tag));
    }
    return copy;
}

DragonList* dragon_list_copy(DragonList* list) {
    DragonList* copy = dragon_list_new_tagged(list->size > 0 ? list->size : 8, list->elem_tag);
    for (int64_t i = 0; i < list->size; i++) {
        int64_t elem = dragon_list_load(list, i);
        // dragon_incref_tagged covers closures too; the old chain missed them,
        // so copying list[Callable] double-freed on destroy (ASan-verified via fs.copy()).
        if (elem) dragon_incref_tagged(elem, list->elem_tag);
        dragon_list_append(copy, elem);
    }
    return copy;
}

DragonList* dragon_list_concat(DragonList* a, DragonList* b) {
    bool aBox = a && a->header.type_tag == DRAGON_TAG_LIST_BOX;
    bool bBox = b && b->header.type_tag == DRAGON_TAG_LIST_BOX;
    if (aBox || bBox)
        return (DragonList*)(void*)dragon_list_box_concat(
            (DragonListBox*)(void*)a, (DragonListBox*)(void*)b);
    if (!a) return b ? dragon_list_copy(b) : dragon_list_new(0);
    DragonList* result = dragon_list_copy(a);
    if (b) dragon_list_extend(result, b);
    return result;
}

DragonList* dragon_list_from_range(int64_t start, int64_t stop, int64_t step) {
    DragonList* l = dragon_list_new_tagged(8, TAG_INT);
    if (step == 0) step = 1;
    if (step > 0)
        for (int64_t i = start; i < stop; i += step) dragon_list_append(l, i);
    else
        for (int64_t i = start; i > stop; i += step) dragon_list_append(l, i);
    return l;
}

void dragon_list_destroy(DragonList* l) {
    if (!l) return;
    if (l->data && l->size > 0) {
        if (l->elem_tag == TAG_STR) {
            for (int64_t i = 0; i < l->size; i++) {
                int64_t v = dragon_list_load(l, i);
                if (v) dragon_decref_str_dispatch((const char*)(uintptr_t)v);
            }
        } else if (l->elem_tag == TAG_LIST || l->elem_tag == TAG_DICT ||
                   l->elem_tag == TAG_BYTES) {
            for (int64_t i = 0; i < l->size; i++) {
                int64_t v = dragon_list_load(l, i);
                if (v) dragon_decref_dispatch((void*)(uintptr_t)v);
            }
        } else if (l->elem_tag == DRAGON_TAG_CLOSURE) {
            for (int64_t i = 0; i < l->size; i++) {
                int64_t v = dragon_list_load(l, i);
                if (v) dragon_decref_callable((void*)(uintptr_t)v);
            }
        }
    }
    free(l->data);
    free(l);
}

DragonListBox* dragon_list_box_repeat(DragonListBox* src, int64_t count);

DragonList* dragon_list_repeat(DragonList* src, int64_t count) {
    if (src && src->header.type_tag == DRAGON_TAG_LIST_BOX)
        return (DragonList*)dragon_list_box_repeat((DragonListBox*)(void*)src, count);
    if (count <= 0 || src->size == 0) {
        return dragon_list_new_tagged(0, src->elem_tag);
    }
    if (count > INT64_MAX / src->size) {
        dragon_raise_exc_cstr(43, "MemoryError: list repeat too large");
    }
    int64_t total = src->size * count;
    DragonList* result = dragon_list_new_tagged(total, src->elem_tag);
    result->size = total;
    bool needIncref = (src->elem_tag == TAG_STR || src->elem_tag >= TAG_LIST);

    if (!needIncref && src->elem_size == 1 && src->size == 1) {
        uint8_t v = ((const uint8_t*)src->data)[0];
        memset(result->data, v, (size_t)total);
        return result;
    }
    if (!needIncref) {
        size_t block = (size_t)(src->size * src->elem_size);
        for (int64_t c = 0; c < count; ++c) {
            memcpy((char*)result->data + (size_t)c * block, src->data, block);
        }
        return result;
    }
    for (int64_t c = 0; c < count; c++) {
        for (int64_t i = 0; i < src->size; i++) {
            int64_t val = dragon_list_load(src, i);
            dragon_list_store(result, c * src->size + i, val);
            if (val) dragon_incref_tagged(val, src->elem_tag);
        }
    }
    return result;
}

DragonListF64* dragon_list_new_f64(int64_t capacity) {
    int64_t cap = capacity > 0 ? capacity : 8;
    auto* data = (double*)dragon_xmalloc_n(cap, sizeof(double));

    auto* list = (DragonListF64*)dragon_xmalloc(sizeof(DragonListF64));
    dragon_obj_init(&list->header, DRAGON_TAG_LIST);
    list->capacity = cap;
    list->size = 0;
    list->elem_tag = TAG_FLOAT;
    list->elem_size = 8;
    list->data = data;
    if (__atomic_add_fetch(&gc_alloc_counter, 1, __ATOMIC_RELAXED)
        >= __atomic_load_n(&gc_threshold, __ATOMIC_RELAXED)) {
        dragon_gc_collect();
    }
    return list;
}

double dragon_list_get_f64(DragonListF64* list, int64_t index) {
    if (index < 0) index += list->size;
    if (index < 0 || index >= list->size) {
        dragon_raise_exc_cstr(41, "IndexError: list index out of range");
        return 0.0;
    }
    return list->data[index];
}

void dragon_list_set_f64(DragonListF64* list, int64_t index, double value) {
    if (index < 0) index += list->size;
    if (index < 0 || index >= list->size) {
        dragon_raise_exc_cstr(41, "IndexError: list assignment index out of range");
        return;
    }
    bool mut_armed = dragon_shared_mut_begin(&list->header, "list");
    list->data[index] = value;
    dragon_shared_mut_end(&list->header, mut_armed);
}

void dragon_list_append_f64(DragonListF64* list, double value) {
    bool mut_armed = dragon_shared_mut_begin(&list->header, "list");
    if (list->size >= list->capacity) {
        int64_t new_cap = list->capacity * 2;
        double* tmp = (double*)dragon_realloc_nullable(
            list->data, dragon_alloc_bytes_or_abort(new_cap, sizeof(double)));
        if (!tmp) { fprintf(stderr, "dragon: out of memory\n"); abort(); }
        list->data = tmp;
        list->capacity = new_cap;
    }
    list->data[list->size++] = value;
    dragon_shared_mut_end(&list->header, mut_armed);
}

DragonListPtr* dragon_list_new_ptr(int64_t capacity, int64_t elem_tag) {
    int64_t cap = capacity > 0 ? capacity : 8;
    auto* data = (void**)dragon_xmalloc_n(cap, sizeof(void*));

    auto* list = (DragonListPtr*)dragon_xmalloc(sizeof(DragonListPtr));
    dragon_obj_init(&list->header, DRAGON_TAG_LIST);
    list->capacity = cap;
    list->size = 0;
    list->elem_tag = (uint8_t)elem_tag;
    list->elem_size = 8;
    list->data = data;
    dragon_gc_track(list);
    if (__atomic_add_fetch(&gc_alloc_counter, 1, __ATOMIC_RELAXED)
        >= __atomic_load_n(&gc_threshold, __ATOMIC_RELAXED)) {
        dragon_gc_collect();
    }
    return list;
}

void* dragon_list_get_ptr(DragonListPtr* list, int64_t index) {
    if (index < 0) index += list->size;
    if (index < 0 || index >= list->size) {
        dragon_raise_exc_cstr(41, "IndexError: list index out of range");
        return nullptr;
    }
    return list->data[index];
}

void dragon_list_set_ptr(DragonListPtr* list, int64_t index, void* value) {
    if (index < 0) index += list->size;
    if (index < 0 || index >= list->size) {
        dragon_raise_exc_cstr(41, "IndexError: list assignment index out of range");
        return;
    }
    bool mut_armed = dragon_shared_mut_begin(&list->header, "list");
    void* old = list->data[index];
    if (value && (list->header.gc_flags & GC_FLAG_SHARED)) {
        if (list->elem_tag == TAG_STR)
            dragon_mark_shared_str((const char*)value);
        else
            dragon_mark_shared_deep(value);
    }
    list->data[index] = value;
    dragon_shared_mut_end(&list->header, mut_armed);
    if (old && old != value) {
        if (list->elem_tag == TAG_STR)
            dragon_decref_str_dispatch((const char*)old);
        else if (list->elem_tag == DRAGON_TAG_CLOSURE)
            dragon_decref_callable(old);
        else
            dragon_decref_dispatch(old);
    }
}

void dragon_list_append_ptr(DragonListPtr* list, void* value) {
    bool mut_armed = dragon_shared_mut_begin(&list->header, "list");
    if (list->size >= list->capacity) {
        int64_t new_cap = list->capacity * 2;
        void** tmp = (void**)dragon_realloc_nullable(
            list->data, dragon_alloc_bytes_or_abort(new_cap, sizeof(void*)));
        if (!tmp) { fprintf(stderr, "dragon: out of memory\n"); abort(); }
        list->data = tmp;
        list->capacity = new_cap;
    }
    if (value && (list->header.gc_flags & GC_FLAG_SHARED)) {
        if (list->elem_tag == TAG_STR)
            dragon_mark_shared_str((const char*)value);
        else
            dragon_mark_shared_deep(value);
    }
    list->data[list->size++] = value;
    dragon_shared_mut_end(&list->header, mut_armed);
}

static inline void dragon_listbox_decref_elem(DragonListBoxElem* e) {
    if (!e->payload) return;
    int64_t tag = e->tag;
    if (tag == TAG_STR)
        dragon_decref_str_dispatch((const char*)(uintptr_t)e->payload);
    else if (tag == TAG_LIST || tag == TAG_DICT || tag == TAG_BYTES)
        dragon_decref_dispatch((void*)(uintptr_t)e->payload);
    // TAG_CLOSURE (10) deliberately has no decref arm (a WALL): adding one is ASan-proven
    // to double-free a borrowed closure in list[Any]; known cost is a leak on tag-10 payloads.
}

DragonListBox* dragon_list_box_new(int64_t capacity) {
    int64_t cap = capacity > 0 ? capacity : 8;
    auto* data = (DragonListBoxElem*)dragon_xmalloc_n(cap, sizeof(DragonListBoxElem));

    auto* list = (DragonListBox*)dragon_xmalloc(sizeof(DragonListBox));
    dragon_obj_init(&list->header, DRAGON_TAG_LIST_BOX);
    list->capacity = cap;
    list->size = 0;
    list->data = data;
    dragon_gc_track(list);
    if (__atomic_add_fetch(&gc_alloc_counter, 1, __ATOMIC_RELAXED)
        >= __atomic_load_n(&gc_threshold, __ATOMIC_RELAXED)) {
        dragon_gc_collect();
    }
    return list;
}

DragonListBox* dragon_list_box_repeat(DragonListBox* src, int64_t count) {
    int64_t srcN = src ? src->size : 0;
    if (count <= 0 || srcN == 0)
        return dragon_list_box_new(0);
    if (count > INT64_MAX / srcN) {
        dragon_raise_exc_cstr(43, "MemoryError: list repeat too large");
        return dragon_list_box_new(0);
    }
    int64_t total = srcN * count;
    DragonListBox* result = dragon_list_box_new(total);
    result->size = total;
    for (int64_t c = 0; c < count; ++c) {
        for (int64_t i = 0; i < srcN; ++i) {
            DragonListBoxElem e = src->data[i];
            dragon_incref_tagged(e.payload, (uint8_t)e.tag);
            result->data[c * srcN + i] = e;
        }
    }
    return result;
}

void dragon_list_view_check(void* p, int64_t want_elem_tag) {
    if (!p) return;
    DragonObjectHeader* h = (DragonObjectHeader*)p;
    if (want_elem_tag < 0) {
        if (h->type_tag == DRAGON_TAG_LIST_BOX) return;
        dragon_raise_exc_cstr(80,
            "TypeError: expected a boxed-element list (list[Any]) but the "
            "value holds a monomorphized list (e.g. list[str]); build it with "
            "element type Any at its declaration, or copy it element-wise");
        return;
    }
    if (h->type_tag == DRAGON_TAG_LIST) {
        DragonList* l = (DragonList*)p;
        if ((int64_t)l->elem_tag == want_elem_tag) return;
        dragon_raise_exc_cstr(80,
            "TypeError: list element type does not match the annotated "
            "element type (the value holds a differently-monomorphized list)");
        return;
    }
    dragon_raise_exc_cstr(80,
        "TypeError: expected a monomorphized list (concrete element type) but "
        "the value holds a boxed-element list (list[Any]); read it as "
        "list[Any] and narrow per element, or copy it element-wise");
}

struct DragonBoxValue { int64_t tag; int64_t payload; };
DragonBoxValue dragon_list_box_get(DragonListBox* list, int64_t index) {
    if (index < 0) index += list->size;
    if (index < 0 || index >= list->size) {
        dragon_raise_exc_cstr(41, "IndexError: list index out of range");
        return {};
    }
    DragonBoxValue v;
    v.tag = list->data[index].tag;
    v.payload = list->data[index].payload;
    return v;
}

void dragon_list_box_set(DragonListBox* list, int64_t index, int64_t tag, int64_t payload) {
    if (index < 0) index += list->size;
    if (index < 0 || index >= list->size) {
        dragon_raise_exc_cstr(41, "IndexError: list assignment index out of range");
        return;
    }
    bool mut_armed = dragon_shared_mut_begin(&list->header, "list");
    DragonListBoxElem old_elem = list->data[index];
    list->data[index].tag = tag;
    list->data[index].payload = payload;
    dragon_shared_mut_end(&list->header, mut_armed);
    dragon_listbox_decref_elem(&old_elem);
}

void dragon_list_box_delitem(DragonListBox* list, int64_t index) {
    if (index < 0) index += list->size;
    if (index < 0 || index >= list->size) {
        dragon_raise_exc_cstr(41, "IndexError: list assignment index out of range");
        return;
    }
    bool mut_armed = dragon_shared_mut_begin(&list->header, "list");
    DragonListBoxElem old_elem = list->data[index];
    memmove(&list->data[index], &list->data[index + 1],
            (size_t)(list->size - 1 - index) * sizeof(DragonListBoxElem));
    list->size--;
    dragon_shared_mut_end(&list->header, mut_armed);
    dragon_listbox_decref_elem(&old_elem);
}

void dragon_list_box_append(DragonListBox* list, int64_t tag, int64_t payload) {
    bool mut_armed = dragon_shared_mut_begin(&list->header, "list");
    if (list->size >= list->capacity) {
        int64_t new_cap = list->capacity * 2;
        DragonListBoxElem* tmp = (DragonListBoxElem*)dragon_realloc_nullable(list->data,
            dragon_alloc_bytes_or_abort(new_cap, sizeof(DragonListBoxElem)));
        if (!tmp) { fprintf(stderr, "dragon: out of memory\n"); abort(); }
        list->data = tmp;
        list->capacity = new_cap;
    }
    list->data[list->size].tag = tag;
    list->data[list->size].payload = payload;
    list->size++;
    dragon_shared_mut_end(&list->header, mut_armed);
}

void dragon_list_box_extend(DragonListBox* dst, DragonListBox* src) {
    if (!dst || !src) return;
    if (src->header.type_tag != DRAGON_TAG_LIST_BOX) {
        DragonList* m = (DragonList*)(void*)src;
        for (int64_t i = 0; i < m->size; ++i) {
            int64_t v = dragon_list_load(m, i);
            dragon_incref_tagged(v, m->elem_tag);
            dragon_list_box_append(dst, m->elem_tag, v);
        }
        return;
    }
    int64_t n = src->size;
    for (int64_t i = 0; i < n; ++i) {
        DragonListBoxElem e = src->data[i];
        dragon_incref_tagged(e.payload, (uint8_t)e.tag);
        dragon_list_box_append(dst, e.tag, e.payload);
    }
}

DragonListBox* dragon_list_box_concat(DragonListBox* a, DragonListBox* b) {
    int64_t na = a ? a->size : 0, nb = b ? b->size : 0;
    DragonListBox* result = dragon_list_box_new(na + nb);
    if (a) dragon_list_box_extend(result, a);
    if (b) dragon_list_box_extend(result, b);
    return result;
}

void dragon_list_box_destroy(DragonListBox* list) {
    if (!list) return;
    for (int64_t i = 0; i < list->size; i++) {
        dragon_listbox_decref_elem(&list->data[i]);
    }
    free(list->data);
    free(list);
}

struct DragonBoxAbi { int64_t tag; int64_t payload; };
extern int64_t dragon_box_eq(DragonBoxAbi a, DragonBoxAbi b);
extern int64_t dragon_box_cmp(DragonBoxAbi a, DragonBoxAbi b, int64_t op);

DragonBoxValue dragon_list_box_pop(DragonListBox* list, int64_t index) {
    if (list->size == 0) {
        dragon_raise_exc_cstr(41, "IndexError: pop from empty list");
        return {};
    }
    if (index < 0) index += list->size;
    if (index < 0 || index >= list->size) {
        dragon_raise_exc_cstr(41, "IndexError: pop index out of range");
        return {};
    }
    bool mut_armed = dragon_shared_mut_begin(&list->header, "list");
    DragonBoxValue v;
    v.tag = list->data[index].tag;
    v.payload = list->data[index].payload;
    memmove(&list->data[index], &list->data[index + 1],
            (size_t)(list->size - 1 - index) * sizeof(DragonListBoxElem));
    list->size--;
    dragon_shared_mut_end(&list->header, mut_armed);
    return v;
}

void dragon_list_box_remove(DragonListBox* list, int64_t tag, int64_t payload) {
    bool mut_armed = dragon_shared_mut_begin(&list->header, "list");
    DragonBoxAbi needle{tag, payload};
    for (int64_t i = 0; i < list->size; i++) {
        DragonBoxAbi cur{list->data[i].tag, list->data[i].payload};
        if (dragon_box_eq(cur, needle)) {
            DragonListBoxElem old_elem = list->data[i];
            memmove(&list->data[i], &list->data[i + 1],
                    (size_t)(list->size - 1 - i) * sizeof(DragonListBoxElem));
            list->size--;
            dragon_shared_mut_end(&list->header, mut_armed);
            dragon_listbox_decref_elem(&old_elem);
            return;
        }
    }
    dragon_shared_mut_end(&list->header, mut_armed);
    dragon_raise_exc_cstr(90, "ValueError: list.remove(x): x not in list");
}

void dragon_list_box_insert(DragonListBox* list, int64_t index, int64_t tag, int64_t payload) {
    bool mut_armed = dragon_shared_mut_begin(&list->header, "list");
    if (index < 0) index += list->size;
    if (index < 0) index = 0;
    if (index > list->size) index = list->size;
    if (list->size >= list->capacity) {
        int64_t new_cap = list->capacity * 2;
        DragonListBoxElem* tmp = (DragonListBoxElem*)dragon_realloc_nullable(list->data,
            dragon_alloc_bytes_or_abort(new_cap, sizeof(DragonListBoxElem)));
        if (!tmp) { fprintf(stderr, "dragon: out of memory\n"); abort(); }
        list->data = tmp;
        list->capacity = new_cap;
    }
    memmove(&list->data[index + 1], &list->data[index],
            (size_t)(list->size - index) * sizeof(DragonListBoxElem));
    list->data[index].tag = tag;
    list->data[index].payload = payload;
    list->size++;
    dragon_shared_mut_end(&list->header, mut_armed);
}

static inline DragonBoxAbi dragon_list_elem_as_box(const DragonObjectHeader* h, int64_t i) {
    DragonBoxAbi b;
    if (h->type_tag == DRAGON_TAG_LIST_BOX) {
        const DragonListBox* l = (const DragonListBox*)h;
        b.tag = l->data[i].tag;
        b.payload = l->data[i].payload;
        return b;
    }
    const DragonList* l = (const DragonList*)h;
    b.tag = (int64_t)l->elem_tag;
    if (l->elem_size == 1) {
        b.payload = (int64_t)((const uint8_t*)l->data)[i];
    } else {
        b.payload = ((const int64_t*)l->data)[i];
    }
    return b;
}

int64_t dragon_list_eq(void* a, void* b) {
    if (a == b) return 1;
    if (!a || !b) return 0;
    DragonObjectHeader* ha = (DragonObjectHeader*)a;
    DragonObjectHeader* hb = (DragonObjectHeader*)b;
    if (ha->type_tag != DRAGON_TAG_LIST && ha->type_tag != DRAGON_TAG_LIST_BOX) return 0;
    if (hb->type_tag != DRAGON_TAG_LIST && hb->type_tag != DRAGON_TAG_LIST_BOX) return 0;
    int64_t na = (ha->type_tag == DRAGON_TAG_LIST_BOX)
                   ? ((DragonListBox*)a)->size : ((DragonList*)a)->size;
    int64_t nb = (hb->type_tag == DRAGON_TAG_LIST_BOX)
                   ? ((DragonListBox*)b)->size : ((DragonList*)b)->size;
    if (na != nb) return 0;
    for (int64_t i = 0; i < na; i++) {
        DragonBoxAbi ea = dragon_list_elem_as_box(ha, i);
        DragonBoxAbi eb = dragon_list_elem_as_box(hb, i);
        if (!dragon_box_eq(ea, eb)) return 0;
    }
    return 1;
}

int64_t dragon_list_cmp(void* a, void* b) {
    if (a == b) return 0;
    if (!a || !b) return (!a && !b) ? 0 : (!a ? -1 : 1);
    DragonObjectHeader* ha = (DragonObjectHeader*)a;
    DragonObjectHeader* hb = (DragonObjectHeader*)b;
    int64_t na = (ha->type_tag == DRAGON_TAG_LIST_BOX)
                   ? ((DragonListBox*)a)->size : ((DragonList*)a)->size;
    int64_t nb = (hb->type_tag == DRAGON_TAG_LIST_BOX)
                   ? ((DragonListBox*)b)->size : ((DragonList*)b)->size;
    int64_t n = na < nb ? na : nb;
    for (int64_t i = 0; i < n; i++) {
        DragonBoxAbi ea = dragon_list_elem_as_box(ha, i);
        DragonBoxAbi eb = dragon_list_elem_as_box(hb, i);
        int64_t c = dragon_box_cmp(ea, eb, 0);
        if (c != 0) return c < 0 ? -1 : 1;
    }
    return (na < nb) ? -1 : (na > nb) ? 1 : 0;
}

void dragon_print_list_box_nested_raw(DragonListBox* l);

void dragon_print_list_box_raw(DragonListBox* list) {
    if (!list) { printf("None"); return; }
    dragon_print_list_box_nested_raw(list);
}
void dragon_print_list_box(DragonListBox* list) {
    dragon_print_list_box_raw(list);
    putchar('\n');
}

}
