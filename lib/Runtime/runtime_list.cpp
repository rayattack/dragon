/// Dragon Runtime - List Operations
#include "runtime_internal.h"
#include <cstring>

extern "C" {

// Defined in runtime_string.cpp; used by the sort comparator for str/bytes.
int64_t dragon_str_cmp(const char* a, const char* b);

// Defined in runtime_builtins.cpp. Tag-gated: a list[Callable] element may be
// a real closure or a bare fn ptr; generic decref on the latter SIGSEGVs.
void dragon_decref_callable(void* p);

// Defined in runtime_box.cpp. Lets nested list/dict elements render
// recursively instead of a "<box tag=N>" placeholder.
void dragon_print_box_raw(DragonBox box);

/// Create a new empty list with given capacity and element type tag.
/// elem_size is derived from the tag (1B for bool, 8B otherwise).
DragonList* dragon_list_new_tagged(int64_t capacity, int64_t elem_tag) {
    // Data buffer allocated before the header: alloc traps unwind via longjmp,
    // which would leak an already-allocated header.
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
    // Acyclic skip: int/bool lists hold only inline scalars, never part of a
    // cycle, so skip GC tracking; other element tags still get tracked.
    if (list->elem_tag != TAG_INT && list->elem_tag != TAG_BOOL) {
        dragon_gc_track(list);
    }
    // Counter bump stays unconditional so GC cadence is unaffected by the
    // tracking skip above.
    if (__atomic_add_fetch(&gc_alloc_counter, 1, __ATOMIC_RELAXED)
        >= __atomic_load_n(&gc_threshold, __ATOMIC_RELAXED)) {
        dragon_gc_collect();
    }
    return list;
}

/// Create a new empty list with given initial capacity (default elem_tag = TAG_INT)
DragonList* dragon_list_new(int64_t capacity) {
    return dragon_list_new_tagged(capacity, 0);  // TAG_INT = 0
}

/// Append a value to the list. Width-aware: bool lists store one byte,
/// everything else stores an 8-byte slot.
void dragon_list_append(DragonList* list, int64_t value) {
    // Concurrent-mutation detector (runtime_internal.h): grow + store is the
    // window; no raise below (OOM aborts). extend() rides on this guard.
    bool mut_armed = dragon_shared_mut_begin(&list->header, "list");
    if (list->size >= list->capacity) {
        int64_t new_cap = list->capacity * 2;
        // Realloc into a temp: on NULL the original buffer is still valid;
        // self-assigning the result would leak the live buffer + NULL-deref.
        void* tmp = dragon_realloc_nullable(list->data,
                            dragon_alloc_bytes_or_abort(new_cap, list->elem_size));
        if (!tmp) { fprintf(stderr, "dragon: out of memory\n"); abort(); }
        list->data = tmp;
        list->capacity = new_cap;
    }
    // Write barrier: SHARED list propagates SHARED to the new element.
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

/// Get element at index (supports negative indexing). Bool elements are
/// zero-extended to i64 by `dragon_list_load`.
int64_t dragon_list_get(DragonList* list, int64_t index) {
    if (index < 0) index += list->size;
    if (index < 0 || index >= list->size) {
        dragon_raise_exc_cstr(41, "IndexError: list index out of range");
        return 0;
    }
    return dragon_list_load(list, index);
}

/// Set element at index (supports negative indexing). Width-aware store.
void dragon_list_set(DragonList* list, int64_t index, int64_t value) {
    if (index < 0) index += list->size;
    if (index < 0 || index >= list->size) {
        dragon_raise_exc_cstr(41, "IndexError: list assignment index out of range");
        return;
    }
    // Concurrent-mutation detector: armed after the raise-y validation.
    bool mut_armed = dragon_shared_mut_begin(&list->header, "list");
    // Reentrancy hardening: store the new value before dropping the old ref,
    // so a finalizer re-reading list[index] during the drop sees the new value.
    int64_t old = dragon_list_load(list, index);
    // Write barrier - see dragon_list_append above.
    if (value && (list->header.gc_flags & GC_FLAG_SHARED)) {
        if (list->elem_tag == TAG_STR)
            dragon_mark_shared_str((const char*)(uintptr_t)value);
        else if (list->elem_tag == TAG_LIST || list->elem_tag == TAG_DICT ||
                 list->elem_tag == TAG_BYTES)
            dragon_mark_shared_deep((void*)(uintptr_t)value);
    }
    dragon_list_store(list, index, value);
    // Structural work done - close the window before the drop.
    dragon_shared_mut_end(&list->header, mut_armed);
    if (old && old != value) {
        if (list->elem_tag == TAG_STR)
            dragon_decref_str_dispatch((const char*)(uintptr_t)old);
        else if (list->elem_tag == TAG_LIST || list->elem_tag == TAG_DICT ||
                 list->elem_tag == TAG_BYTES)
            dragon_decref_dispatch((void*)(uintptr_t)old);
    }
}

/// Get list length
int64_t dragon_list_len(DragonList* list) {
    return list ? list->size : 0;
}

/// Print list of ints: [1, 2, 3]
// `_raw` core has no trailing newline; multi-arg print uses it so
// `print("xs:", [1,2])` renders inline.
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

/// Insert value at index, shifting elements right
void dragon_list_insert(DragonList* list, int64_t index, int64_t value) {
    // Concurrent-mutation detector: no raise below (index clamps, OOM aborts).
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
    // Shift elements right to open a gap at `index` (single bulk memmove
    // beats an element-by-element loop; elem_size is 1 or 8).
    uint8_t* base = (uint8_t*)list->data;
    memmove(base + (size_t)(index + 1) * list->elem_size,
            base + (size_t)index * list->elem_size,
            (size_t)(list->size - index) * list->elem_size);
    // Incref: the list now owns a reference to this value
    dragon_incref_tagged(value, list->elem_tag);
    // Write barrier - see dragon_list_append above.
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

// Tag-aware element equality (defined below; used by remove/index/count/
// contains for value-based search, so string elements compare by content).
static bool dragon_list_elem_eq(DragonList* list, int64_t a, int64_t b);

/// Remove first occurrence of value (ValueError if not found)
void dragon_list_remove(DragonList* list, int64_t value) {
    // Concurrent-mutation detector: window covers the search too; the
    // not-found raise below closes the window first (no stranded bit).
    bool mut_armed = dragon_shared_mut_begin(&list->header, "list");
    for (int64_t i = 0; i < list->size; i++) {
        int64_t elem = dragon_list_load(list, i);
        if (dragon_list_elem_eq(list, elem, value)) {
            // Reentrancy hardening: shrink to a consistent state before the
            // decref, so a re-entrant finalizer can't observe the freed slot.
            uint8_t* base = (uint8_t*)list->data;
            memmove(base + (size_t)i * list->elem_size,
                    base + (size_t)(i + 1) * list->elem_size,
                    (size_t)(list->size - 1 - i) * list->elem_size);
            list->size--;
            // Structural work done - close the window before the drop.
            dragon_shared_mut_end(&list->header, mut_armed);
            if (elem) {
                if (list->elem_tag == TAG_STR)
                    dragon_decref_str_dispatch((const char*)(uintptr_t)elem);
                else if (list->elem_tag == TAG_LIST || list->elem_tag == TAG_DICT ||
                         list->elem_tag == TAG_BYTES)
                    dragon_decref_dispatch((void*)(uintptr_t)elem);
                else if (list->elem_tag == DRAGON_TAG_CLOSURE)
                    // remove() previously missed this arm, leaking a closure
                    // + env per cbs.remove(f).
                    dragon_decref_callable((void*)(uintptr_t)elem);
            }
            return;
        }
    }
    dragon_shared_mut_end(&list->header, mut_armed);
    dragon_raise_exc_cstr(90, "ValueError: list.remove(x): x not in list");
}

/// Pop element at index (default -1), return it
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
    // Concurrent-mutation detector: armed after the raise-y validation.
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

/// Typed pop for list[float]; returns native double so pop() stays unboxed
/// (a generic i64 pop + SIToFP would corrupt the bit pattern, not bitcast it).
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
    // Concurrent-mutation detector: armed after the raise-y validation.
    bool mut_armed = dragon_shared_mut_begin(&list->header, "list");
    double val = list->data[index];
    memmove(list->data + index, list->data + index + 1,
            (size_t)(list->size - 1 - index) * sizeof(double));
    list->size--;
    dragon_shared_mut_end(&list->header, mut_armed);
    return val;
}

/// Delete element at index (`del lst[i]`); unlike pop, the discarded value is
/// decref'd. Covers all monomorphized list variants (shared layout).
void dragon_list_delitem(DragonList* list, int64_t index) {
    if (index < 0) index += list->size;
    if (index < 0 || index >= list->size) {
        dragon_raise_exc_cstr(41, "IndexError: list assignment index out of range");
        return;
    }
    // Concurrent-mutation detector: armed after the raise-y validation.
    bool mut_armed = dragon_shared_mut_begin(&list->header, "list");
    int64_t elem = dragon_list_load(list, index);
    // Reentrancy hardening: shift+shrink before the decref, so a re-entrant
    // finalizer can't observe the freed slot.
    uint8_t* base = (uint8_t*)list->data;
    memmove(base + (size_t)index * list->elem_size,
            base + (size_t)(index + 1) * list->elem_size,
            (size_t)(list->size - 1 - index) * list->elem_size);
    list->size--;
    // Structural work done - close the window before the drop.
    dragon_shared_mut_end(&list->header, mut_armed);
    if (elem) {
        if (list->elem_tag == TAG_STR)
            dragon_decref_str_dispatch((const char*)(uintptr_t)elem);
        else if (list->elem_tag == TAG_LIST || list->elem_tag == TAG_DICT ||
                 list->elem_tag == TAG_BYTES)
            dragon_decref_dispatch((void*)(uintptr_t)elem);
        else if (list->elem_tag == DRAGON_TAG_CLOSURE)
            dragon_decref_callable((void*)(uintptr_t)elem);  // tag-gated (bare fn safe)
    }
}

/// Clear all elements (decref heap-typed elements first)
void dragon_list_clear(DragonList* list) {
    // Concurrent-mutation detector: whole teardown is the window; decrefs
    // stay inside (see dragon_dict_clear for the rationale).
    bool mut_armed = dragon_shared_mut_begin(&list->header, "list");
    if (list->elem_tag == TAG_STR || list->elem_tag == TAG_LIST ||
        list->elem_tag == TAG_DICT || list->elem_tag == TAG_BYTES ||
        list->elem_tag == DRAGON_TAG_CLOSURE) {  // dragon_decref_tagged handles closures
        // Reentrancy hardening: null each slot before the decref and snapshot
        // the count, so a re-entrant append/read can't corrupt the teardown.
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

/// Extend list with all elements from another list. Reconciles elem_tag so
/// destroy will correctly decref heap elements we're about to add.
// Defined later (box section); declared here so extend/concat can dispatch
// the list[Any] layout.
void dragon_list_box_extend(DragonListBox* dst, DragonListBox* src);
DragonListBox* dragon_list_box_concat(DragonListBox* a, DragonListBox* b);

void dragon_list_extend(DragonList* list, DragonList* other) {
    if (!other) return;
    // list[Any] is a DragonListBox (16B/elem); dispatch on the header tag so
    // extend/+= use the box layout instead of scribbling over {tag,payload}.
    if (list && list->header.type_tag == DRAGON_TAG_LIST_BOX) {
        dragon_list_box_extend((DragonListBox*)(void*)list,
                               (DragonListBox*)(void*)other);
        return;
    }
    // If dest is empty with the default TAG_INT, adopt source's tag (and
    // matching elem_size) so dest's destroy properly decrefs heap elements.
    if (list->size == 0 && list->elem_tag == TAG_INT && other->elem_tag != TAG_INT) {
        list->elem_tag = other->elem_tag;
        // Promote storage width if needed (TAG_INT==8B, switch to other tag's width).
        uint8_t new_size = dragon_list_size_for_tag(list->elem_tag);
        if (new_size != list->elem_size) {
            // Allocate before the free: an OOM/wrap-trap between free and
            // malloc would leave list->data dangling on a GC-walked object.
            void* fresh = dragon_xmalloc_or_abort(
                dragon_alloc_bytes_or_abort(list->capacity, new_size));
            free(list->data);
            list->elem_size = new_size;
            list->data = fresh;
        }
    }
    bool tags_match = (list->elem_tag == other->elem_tag);
    // Snapshot source length: self-extend (`l.extend(l)`) would otherwise
    // re-read the growing size each append and OOM; this matches Python.
    int64_t n = other->size;
    for (int64_t i = 0; i < n; i++) {
        int64_t elem = dragon_list_load(other, i);
        // dragon_incref_tagged covers closures too; the old chain skipped them
        // while destroy decrefs them, double-freeing list[Callable] extends.
        if (elem && tags_match) dragon_incref_tagged(elem, other->elem_tag);
        dragon_list_append(list, elem);
    }
}

/// Tag-aware element equality for search (index/count/contains/`in`):
/// str/bytes compare by content, floats undo the bitcast, else raw i64.
static bool dragon_list_elem_eq(DragonList* list, int64_t a, int64_t b) {
    switch (list->elem_tag) {
        case TAG_STR:
        case TAG_BYTES: {
            const char* sa = (const char*)(uintptr_t)a;
            const char* sb = (const char*)(uintptr_t)b;
            if (sa == sb) return true;          // same pointer (incl. both null)
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

/// Find index of first occurrence of value (ValueError if not found)
int64_t dragon_list_index(DragonList* list, int64_t value) {
    for (int64_t i = 0; i < list->size; i++) {
        if (dragon_list_elem_eq(list, dragon_list_load(list, i), value)) return i;
    }
    // For non-int element tags `value` is a bitcast/pointer, so its decimal
    // form is meaningless; raise a type-appropriate message instead.
    if (list->elem_tag == TAG_INT || list->elem_tag == TAG_BOOL) {
        char msg[64];
        snprintf(msg, sizeof(msg), "ValueError: %lld is not in list", (long long)value);
        dragon_raise_exc_cstr(90, msg);
    }
    dragon_raise_exc_cstr(90, "ValueError: value is not in list");
    return 0;
}

/// Count occurrences of value
int64_t dragon_list_count(DragonList* list, int64_t value) {
    int64_t c = 0;
    for (int64_t i = 0; i < list->size; i++) {
        if (dragon_list_elem_eq(list, dragon_list_load(list, i), value)) c++;
    }
    return c;
}

/// Membership test for the `in` operator: 1 if value is present, else 0.
/// Non-raising sibling of dragon_list_index, using the same value equality.
int64_t dragon_list_contains(DragonList* list, int64_t value) {
    for (int64_t i = 0; i < list->size; i++) {
        if (dragon_list_elem_eq(list, dragon_list_load(list, i), value)) return 1;
    }
    return 0;
}

/// Sort list in-place; `reverse` gives genuine stable descending order (not
/// an ascending sort reversed, which would invert equal-key order).
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
    // str/bytes need content compare (raw i64 would sort by pointer address);
    // floats need the bitcast undone or negative numbers sort backwards.
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
    // Concurrent-mutation detector: the whole in-place sort is the window.
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

/// Ascending in-place sort (thin wrapper; the historical entry point).
void dragon_list_sort(DragonList* list) {
    dragon_list_sort_ex(list, 0);
}

/// Reverse list in-place
void dragon_list_reverse(DragonList* list) {
    // Concurrent-mutation detector: the whole in-place swap is the window.
    bool mut_armed = dragon_shared_mut_begin(&list->header, "list");
    for (int64_t i = 0, j = list->size - 1; i < j; i++, j--) {
        int64_t tmp = dragon_list_load(list, i);
        dragon_list_store(list, i, dragon_list_load(list, j));
        dragon_list_store(list, j, tmp);
    }
    dragon_shared_mut_end(&list->header, mut_armed);
}

/// dub (docs/002 2.7): deep copy; spine is fresh, elements deep-copied by tag
/// (str/bytes identity-retain, lists/dicts recurse).
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

/// Shallow copy (returns new list)
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

/// list + list -> fresh list of lhs then rhs, never mutating either operand.
/// Built via copy+extend so ref accounting is inherited; box-aware for list[Any].
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

/// Materialize [start, stop) by step into a fresh int list; backs
/// `list(range(...))` (range() is otherwise for-loop-fused, no list object).
DragonList* dragon_list_from_range(int64_t start, int64_t stop, int64_t step) {
    DragonList* l = dragon_list_new_tagged(8, TAG_INT);
    if (step == 0) step = 1;  // defensive; Python raises ValueError for step 0
    if (step > 0)
        for (int64_t i = start; i < stop; i += step) dragon_list_append(l, i);
    else
        for (int64_t i = start; i > stop; i += step) dragon_list_append(l, i);
    return l;
}

/// Destroy a list and free its memory (GC support). Child decrefs go through
/// `dragon_decref_*_dispatch`, routed to atomic variants in atomic-context dealloc.
void dragon_list_destroy(DragonList* l) {
    if (!l) return;
    // Phase 5: decref contained heap-typed elements before freeing
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
            // list[Callable] - tag-gated drop (closure + env, or no-op on
            // a bare fn ptr).
            for (int64_t i = 0; i < l->size; i++) {
                int64_t v = dragon_list_load(l, i);
                if (v) dragon_decref_callable((void*)(uintptr_t)v);
            }
        }
        // TAG_INT, TAG_FLOAT, TAG_BOOL, TAG_NONE: no heap cleanup needed
    }
    free(l->data);
    free(l);
}

/// Repeat a list N times: [1,2] * 3 -> [1,2,1,2,1,2]. Fast paths: memset for
/// a size-1 bool source, bulk memcpy for other primitive sources.
DragonListBox* dragon_list_box_repeat(DragonListBox* src, int64_t count);  // fwd

DragonList* dragon_list_repeat(DragonList* src, int64_t count) {
    // list[Any] is a DragonListBox (16B/elem); dispatch on the shared header
    // tag (common DragonObjectHeader prefix) since the layouts differ.
    if (src && src->header.type_tag == DRAGON_TAG_LIST_BOX)
        return (DragonList*)dragon_list_box_repeat((DragonListBox*)(void*)src, count);
    if (count <= 0 || src->size == 0) {
        return dragon_list_new_tagged(0, src->elem_tag);
    }
    // Element-count guard only; byte-size wrap is caught later in
    // dragon_list_new_tagged (A-B proven heap overflow if skipped here too).
    if (count > INT64_MAX / src->size) {
        dragon_raise_exc_cstr(43, "MemoryError: list repeat too large");
    }
    int64_t total = src->size * count;
    DragonList* result = dragon_list_new_tagged(total, src->elem_tag);
    result->size = total;
    bool needIncref = (src->elem_tag == TAG_STR || src->elem_tag >= TAG_LIST);

    // 6.13 fast path: bool source of size 1 → memset to a constant byte.
    if (!needIncref && src->elem_size == 1 && src->size == 1) {
        uint8_t v = ((const uint8_t*)src->data)[0];
        memset(result->data, v, (size_t)total);
        return result;
    }
    // Bulk memcpy: copy the source block once per repetition. Avoids the
    // per-element load/store hot loop for primitive (no-RC) elements.
    if (!needIncref) {
        size_t block = (size_t)(src->size * src->elem_size);
        for (int64_t c = 0; c < count; ++c) {
            memcpy((char*)result->data + (size_t)c * block, src->data, block);
        }
        return result;
    }
    // Heap-typed elements: per-element copy + incref.
    for (int64_t c = 0; c < count; c++) {
        for (int64_t i = 0; i < src->size; i++) {
            int64_t val = dragon_list_load(src, i);
            dragon_list_store(result, c * src->size + i, val);
            // dragon_incref_tagged, not bare dragon_incref: a bare fn ptr has
            // no header, so generic incref wrote into .text -> SIGSEGV on [f]*3.
            if (val) dragon_incref_tagged(val, src->elem_tag);
        }
    }
    return result;
}

// D030 Phase 3: monomorphized F64/Ptr list variants share the I64 variant's
// memory layout (see runtime_internal.h); per-type ops use the native pointer.

/// list[float] - allocate with native f64 storage.
DragonListF64* dragon_list_new_f64(int64_t capacity) {
    // Data buffer first - see dragon_list_new_tagged for the ordering rule.
    int64_t cap = capacity > 0 ? capacity : 8;
    auto* data = (double*)dragon_xmalloc_n(cap, sizeof(double));

    auto* list = (DragonListF64*)dragon_xmalloc(sizeof(DragonListF64));
    dragon_obj_init(&list->header, DRAGON_TAG_LIST);
    list->capacity = cap;
    list->size = 0;
    list->elem_tag = TAG_FLOAT;
    list->elem_size = 8;
    list->data = data;
    // Acyclic skip: float lists are native double[] storage, never tracked
    // (see dragon_list_new_tagged); counter bump stays unconditional.
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
    // Concurrent-mutation detector: an 8-byte store can't corrupt structure,
    // but a concurrent shrink could free the buffer under it.
    bool mut_armed = dragon_shared_mut_begin(&list->header, "list");
    list->data[index] = value;
    dragon_shared_mut_end(&list->header, mut_armed);
}

void dragon_list_append_f64(DragonListF64* list, double value) {
    // Concurrent-mutation detector - see dragon_list_append.
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

/// list[<heap>]: native pointer storage. elem_tag selects the refcount
/// family (TAG_STR uses dragon_incref_str; other heap tags use generic).
DragonListPtr* dragon_list_new_ptr(int64_t capacity, int64_t elem_tag) {
    // Data buffer first - see dragon_list_new_tagged for the ordering rule.
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

/// Refcount-aware set: decref the old element then store the new value. The
/// runtime owns refcount accounting so codegen skips incref/decref emission.
void dragon_list_set_ptr(DragonListPtr* list, int64_t index, void* value) {
    if (index < 0) index += list->size;
    if (index < 0 || index >= list->size) {
        dragon_raise_exc_cstr(41, "IndexError: list assignment index out of range");
        return;
    }
    // Concurrent-mutation detector: armed after the raise-y validation.
    bool mut_armed = dragon_shared_mut_begin(&list->header, "list");
    void* old = list->data[index];
    // Write barrier: if the parent list is SHARED, the new value transitively
    // becomes SHARED too, so future RC ops on it route to atomic.
    if (value && (list->header.gc_flags & GC_FLAG_SHARED)) {
        if (list->elem_tag == TAG_STR)
            dragon_mark_shared_str((const char*)value);
        else
            dragon_mark_shared_deep(value);
    }
    // Reentrancy hardening: store the new value before dropping the old ref,
    // so a re-entrant finalizer sees `value`, never the freed `old`.
    list->data[index] = value;
    // Structural work done - close the window before the drop.
    dragon_shared_mut_end(&list->header, mut_armed);
    if (old && old != value) {
        if (list->elem_tag == TAG_STR)
            dragon_decref_str_dispatch((const char*)old);
        else if (list->elem_tag == DRAGON_TAG_CLOSURE)
            dragon_decref_callable(old);  // tag-gated (bare fn safe)
        else
            dragon_decref_dispatch(old);
    }
}

/// Refcount-aware append: takes ownership of one reference on `value`;
/// caller increfs first if `value` is borrowed.
void dragon_list_append_ptr(DragonListPtr* list, void* value) {
    // Concurrent-mutation detector - see dragon_list_append.
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

// D039 Phase 4: list[Any] via DragonListBox, per-element {tag, payload},
// inline storage (no per-element heap alloc), 16B/elem.

/// Decref a single boxed element's payload by its tag. Used by set
/// (overwrite path) and destroy (full-list cleanup).
static inline void dragon_listbox_decref_elem(DragonListBoxElem* e) {
    if (!e->payload) return;
    int64_t tag = e->tag;
    if (tag == TAG_STR)
        dragon_decref_str_dispatch((const char*)(uintptr_t)e->payload);
    else if (tag == TAG_LIST || tag == TAG_DICT || tag == TAG_BYTES)
        dragon_decref_dispatch((void*)(uintptr_t)e->payload);
    // TAG_INT / TAG_FLOAT / TAG_BOOL / TAG_NONE: no refcount to drop.
    // TAG_CLOSURE (10) deliberately has no decref arm (a WALL): adding one is
    // ASan-proven to double-free a borrowed closure appended to list[Any];
    // known cost is a leak on repeat/extend/append for tag-10 payloads.
}

/// list[Any] allocation. capacity rounds up to at least 8 to match other variants.
DragonListBox* dragon_list_box_new(int64_t capacity) {
    // Data buffer first - see dragon_list_new_tagged for the ordering rule.
    // 16B stride, so this layout wraps a capacity at 2^60, not 2^61.
    int64_t cap = capacity > 0 ? capacity : 8;
    auto* data = (DragonListBoxElem*)dragon_xmalloc_n(cap, sizeof(DragonListBoxElem));

    auto* list = (DragonListBox*)dragon_xmalloc(sizeof(DragonListBox));
    // Distinct type tag routes GC destroy/traverse/clear to dragon_list_box_*
    // helpers; the 16B/elem layout differs from DragonList so it can't share.
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

/// list[Any] * int -> fresh box list with `count` repetitions (box analogue
/// of dragon_list_repeat); each payload is incref'd per copy.
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

/// Verifies a boxed list's header matches the expected representation before
/// unboxing (wrong stride = silent corruption/OOB); raises TypeError (80).
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

/// Read element `index` as a 16-byte {tag, payload} box (BORROW contract:
/// non-owning; codegen increfs at the store site, matching dragon_dict_get_box).
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

/// Refcount-aware set: decref the old payload then store the new tag+payload.
/// Codegen increfs the new payload first (Model-B, matches dragon_list_set_ptr).
void dragon_list_box_set(DragonListBox* list, int64_t index, int64_t tag, int64_t payload) {
    if (index < 0) index += list->size;
    if (index < 0 || index >= list->size) {
        dragon_raise_exc_cstr(41, "IndexError: list assignment index out of range");
        return;
    }
    // Concurrent-mutation detector: armed after the raise-y validation.
    bool mut_armed = dragon_shared_mut_begin(&list->header, "list");
    // Reentrancy hardening: write the new {tag, payload} before dropping the
    // old ref, so a re-entrant read sees the new element, not the freed one.
    DragonListBoxElem old_elem = list->data[index];
    list->data[index].tag = tag;
    list->data[index].payload = payload;
    dragon_shared_mut_end(&list->header, mut_armed);
    dragon_listbox_decref_elem(&old_elem);
}

/// Delete element at index from a box list (`del lst[i]`). Can't reuse
/// dragon_list_delitem: that shifts 8-byte halves and scrambles the boxes.
void dragon_list_box_delitem(DragonListBox* list, int64_t index) {
    if (index < 0) index += list->size;
    if (index < 0 || index >= list->size) {
        dragon_raise_exc_cstr(41, "IndexError: list assignment index out of range");
        return;
    }
    // Concurrent-mutation detector: armed after the raise-y validation.
    bool mut_armed = dragon_shared_mut_begin(&list->header, "list");
    // Reentrancy hardening: shift+shrink before dropping the ref, so a
    // re-entrant finalizer can't observe the freed slot.
    DragonListBoxElem old_elem = list->data[index];
    memmove(&list->data[index], &list->data[index + 1],
            (size_t)(list->size - 1 - index) * sizeof(DragonListBoxElem));
    list->size--;
    dragon_shared_mut_end(&list->header, mut_armed);
    dragon_listbox_decref_elem(&old_elem);
}

/// Refcount-aware append: takes ownership of one reference on a refcounted
/// payload (caller increfs borrowed inputs).
void dragon_list_box_append(DragonListBox* list, int64_t tag, int64_t payload) {
    // Concurrent-mutation detector - see dragon_list_append. box_extend and
    // box_concat ride on this guard.
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

/// In-place extend of list[Any]; each payload is incref'd. Source may be a
/// box list or a monomorphic DragonList (e.g. `+= [1,2]`, list[T] <: list[Any]).
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
    // Snapshot source length: self-alias extend would otherwise grow src->size
    // every append and OOM (matches the fix in dragon_list_extend).
    int64_t n = src->size;
    for (int64_t i = 0; i < n; ++i) {
        DragonListBoxElem e = src->data[i];
        dragon_incref_tagged(e.payload, (uint8_t)e.tag);
        dragon_list_box_append(dst, e.tag, e.payload);
    }
}

/// list[Any] + list[Any] → fresh box list (lhs's elements then rhs's), each
/// payload incref'd once. The box-layout analogue of dragon_list_concat.
DragonListBox* dragon_list_box_concat(DragonListBox* a, DragonListBox* b) {
    int64_t na = a ? a->size : 0, nb = b ? b->size : 0;
    DragonListBox* result = dragon_list_box_new(na + nb);
    if (a) dragon_list_box_extend(result, a);
    if (b) dragon_list_box_extend(result, b);
    return result;
}

/// Decref all refcounted payloads and free the list.
void dragon_list_box_destroy(DragonListBox* list) {
    if (!list) return;
    for (int64_t i = 0; i < list->size; i++) {
        dragon_listbox_decref_elem(&list->data[i]);
    }
    free(list->data);
    free(list);
}

// Forward declaration: dragon_box_eq lives in runtime_box.cpp. Mirrors the
// AMD64 SysV two-i64 ABI so cross-TU calls avoid a 16B struct return.
struct DragonBoxAbi { int64_t tag; int64_t payload; };
extern int64_t dragon_box_eq(DragonBoxAbi a, DragonBoxAbi b);
extern int64_t dragon_box_cmp(DragonBoxAbi a, DragonBoxAbi b, int64_t op);

/// Pop element at index (default -1) from a box list; ownership transfers to
/// the caller, no decref here (mirrors dragon_list_pop's borrow-out contract).
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
    // Concurrent-mutation detector: armed after the raise-y validation.
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

/// Remove first element value-equal to {tag, payload} (ValueError if absent);
/// value equality via dragon_box_eq (content compare, matching Python).
void dragon_list_box_remove(DragonListBox* list, int64_t tag, int64_t payload) {
    // Concurrent-mutation detector: window covers the search (see
    // dragon_list_remove); the not-found raise closes the window first.
    bool mut_armed = dragon_shared_mut_begin(&list->header, "list");
    DragonBoxAbi needle{tag, payload};
    for (int64_t i = 0; i < list->size; i++) {
        DragonBoxAbi cur{list->data[i].tag, list->data[i].payload};
        if (dragon_box_eq(cur, needle)) {
            // Reentrancy hardening: capture the element, shift + shrink, THEN
            // drop the ref - see dragon_list_box_delitem.
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

/// Insert {tag, payload} at index (Python list.insert clamp semantics);
/// caller owns the incref on a refcounted payload (Model B, like append).
void dragon_list_box_insert(DragonListBox* list, int64_t index, int64_t tag, int64_t payload) {
    // Concurrent-mutation detector: no raise below (index clamps, OOM aborts).
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

/// Read element `i` of any list variant as a {tag, payload} box (dispatched
/// on header type_tag; non-box variants use elem_tag + the raw 8-byte slot).
static inline DragonBoxAbi dragon_list_elem_as_box(const DragonObjectHeader* h, int64_t i) {
    DragonBoxAbi b;
    if (h->type_tag == DRAGON_TAG_LIST_BOX) {
        const DragonListBox* l = (const DragonListBox*)h;
        b.tag = l->data[i].tag;
        b.payload = l->data[i].payload;
        return b;
    }
    const DragonList* l = (const DragonList*)h;  // shared layout prefix
    b.tag = (int64_t)l->elem_tag;
    if (l->elem_size == 1) {
        b.payload = (int64_t)((const uint8_t*)l->data)[i];
    } else {
        b.payload = ((const int64_t*)l->data)[i];
    }
    return b;
}

/// Deep equality: compares elements via dragon_box_eq (recurses into nested
/// lists/dicts); cross-variant safe. Pointer identity short-circuits True.
int64_t dragon_list_eq(void* a, void* b) {
    if (a == b) return 1;
    if (!a || !b) return 0;
    DragonObjectHeader* ha = (DragonObjectHeader*)a;
    DragonObjectHeader* hb = (DragonObjectHeader*)b;
    // Reject non-list inputs defensively (e.g., if codegen ever routes a
    // non-list ptr here we want a clean False rather than UB).
    if (ha->type_tag != DRAGON_TAG_LIST && ha->type_tag != DRAGON_TAG_LIST_BOX) return 0;
    if (hb->type_tag != DRAGON_TAG_LIST && hb->type_tag != DRAGON_TAG_LIST_BOX) return 0;
    // size lives at the same offset in DragonList / F64 / Ptr (after
    // header + data ptr) AND in DragonListBox.
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

/// Lexicographic three-way list compare (Python semantics): first non-equal
/// element decides, else shorter is less; raises TypeError if incomparable.
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
        int64_t c = dragon_box_cmp(ea, eb, /*'<' for msg*/ 0);
        if (c != 0) return c < 0 ? -1 : 1;
    }
    return (na < nb) ? -1 : (na > nb) ? 1 : 0;
}

void dragon_print_list_box_nested_raw(DragonListBox* l);

/// Pretty-print as `[a, b, c]` with each element formatted by its tag.
/// Used by print(list_any_value).
void dragon_print_list_box_raw(DragonListBox* list) {
    if (!list) { printf("None"); return; }
    dragon_print_list_box_nested_raw(list);
}
void dragon_print_list_box(DragonListBox* list) {
    dragon_print_list_box_raw(list);
    putchar('\n');
}

} // extern "C"
