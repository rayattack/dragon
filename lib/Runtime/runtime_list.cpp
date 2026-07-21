/// Dragon Runtime - List Operations
#include "runtime_internal.h"
#include <cstring>

extern "C" {

// Defined in runtime_string.cpp - declare locally so the list-sort
// comparator can use it for str/bytes elements without a header round
// trip.
int64_t dragon_str_cmp(const char* a, const char* b);

// Defined in runtime_builtins.cpp - TAG-GATED closure decref. A
// list[Callable] element (elem_tag == DRAGON_TAG_CLOSURE) may be a real
// DragonClosure OR a bare fn ptr (no header); this frees the former (+ env)
// and no-ops on the latter, so the element decref never touches a headerless
// code pointer (generic dragon_decref would SIGSEGV).
void dragon_decref_callable(void* p);

// Defined in runtime_box.cpp - render a boxed Any value with no trailing
// newline. list[Any] repr delegates container/bytes elements here so nested
// lists/dicts render recursively instead of as a "<box tag=N>" placeholder.
void dragon_print_box_raw(DragonBox box);

/// Create a new empty list with given initial capacity and element type tag.
/// `elem_size` is derived from the tag (1B for bool, 8B otherwise) so the
/// data buffer is sized correctly for the packed-storage fast path.
DragonList* dragon_list_new_tagged(int64_t capacity, int64_t elem_tag) {
    auto* list = (DragonList*)malloc(sizeof(DragonList));
    dragon_obj_init(&list->header, DRAGON_TAG_LIST);
    list->capacity = capacity > 0 ? capacity : 8;
    list->size = 0;
    list->elem_tag = (uint8_t)elem_tag;
    list->elem_size = dragon_list_size_for_tag(list->elem_tag);
    list->data = malloc((size_t)(list->capacity * list->elem_size));
    // Acyclic-skip cycle tracking (mirrors codegen's classIsAcyclic): an
    // int/bool list holds only inline scalars - no heap pointers - so it can
    // never be part of a reference cycle. The cycle collector therefore never
    // needs it; tracking would only churn the gc_tracked array and bloat the
    // set every collection linearly scans (and removes a latent int-as-pointer
    // traverse false-positive). Pointer-bearing element tags (str/list/dict/
    // bytes/Any) must still be tracked.
    if (list->elem_tag != TAG_INT && list->elem_tag != TAG_BOOL) {
        dragon_gc_track(list);
    }
    // The alloc-counter bump stays UNCONDITIONAL so collection cadence is
    // identical to before even when the track is skipped - a pointer-free
    // workload still ages the GC, so genuine cycles elsewhere are not starved.
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
        void* tmp = realloc(list->data, (size_t)(new_cap * list->elem_size));
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
    // Capture the old element, store the new one, THEN drop the old ref.
    // Reentrancy hardening: the slot must hold `value` before the decref so a
    // finalizer that re-reads list[index] during the drop sees the new value,
    // never the freed `old`.
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
// List printers: `_raw` core (no trailing newline) + public wrapper. The
// element loop uses direct printf so no nested newlines leak; multi-arg print
// calls the `_raw` form so `print("xs:", [1,2])` renders inline.
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
    // Ensure capacity
    if (list->size >= list->capacity) {
        int64_t new_cap = list->capacity * 2;
        void* tmp = realloc(list->data, (size_t)(new_cap * list->elem_size));
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
    // Concurrent-mutation detector: window covers the search too (a
    // concurrent shift would invalidate the scan). Not-found path exits the
    // process (no longjmp), so the stranded bit is moot there.
    bool mut_armed = dragon_shared_mut_begin(&list->header, "list");
    for (int64_t i = 0; i < list->size; i++) {
        int64_t elem = dragon_list_load(list, i);
        if (dragon_list_elem_eq(list, elem, value)) {
            // Reentrancy hardening: shrink the list to a consistent state
            // BEFORE decref'ing the removed element, so a finalizer running
            // during the drop can't observe the freed slot. The decref'd
            // element is the list's OWN (not the search value - under content
            // equality they can be distinct pointers). Dispatch covers
            // atomic-context.
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
                    // list[Callable] element. dragon_list_delitem already
                    // releases closures on removal; remove() missed this arm,
                    // leaking one closure + env per cbs.remove(f).
                    dragon_decref_callable((void*)(uintptr_t)elem);
            }
            return;
        }
    }
    fprintf(stderr, "ValueError: list.remove(x): x not in list\n");
    exit(1);
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

/// Typed pop for a DragonListF64 (`list[float]`). Mirrors dragon_list_pop but
/// returns the value at its native `double` type, so a `list[float].pop()`
/// flows unboxed end-to-end (commandment #1) - the generic i64 pop would force
/// the caller to reinterpret the bits, and an SIToFP coercion there silently
/// corrupts the value (it converts the f64 BIT PATTERN as an integer instead
/// of bitcasting). The 8-byte memmove/shrink is identical to the i64 path
/// (elem_size is 8 for both DragonList and DragonListF64).
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

/// Delete element at index (`del lst[i]`). Unlike pop, the removed value is
/// discarded - so a heap-typed element must be decref'd here (the list owned
/// one reference to it). Negative indices supported; out-of-range is
/// IndexError. Layout is identical across DragonList / DragonListF64 /
/// DragonListPtr, so this one entry covers all three monomorphized variants;
/// the decref dispatch (incl. TAG_CLASS == TAG_BYTES) mirrors
/// dragon_list_remove.
void dragon_list_delitem(DragonList* list, int64_t index) {
    if (index < 0) index += list->size;
    if (index < 0 || index >= list->size) {
        dragon_raise_exc_cstr(41, "IndexError: list assignment index out of range");
        return;
    }
    // Concurrent-mutation detector: armed after the raise-y validation.
    bool mut_armed = dragon_shared_mut_begin(&list->header, "list");
    int64_t elem = dragon_list_load(list, index);
    // Reentrancy hardening: bulk-shift the tail down and shrink BEFORE
    // decref'ing the removed element, so a finalizer running during the drop
    // can't observe the freed slot. elem_size is 1 (bool) or 8 (everything
    // else), so a single memmove beats an element-by-element load/store loop.
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
        // Reentrancy hardening: null each slot before dropping its ref and
        // snapshot the count up front, so a finalizer that re-reads the list
        // during a drop sees an emptied slot (never a freed pointer) and a
        // re-entrant append lands past the snapshot rather than corrupting
        // this teardown loop.
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
// Defined later in this file (box section). Declared here so the polymorphic
// extend/concat entry points can dispatch the list[Any] layout.
void dragon_list_box_extend(DragonListBox* dst, DragonListBox* src);
DragonListBox* dragon_list_box_concat(DragonListBox* a, DragonListBox* b);

void dragon_list_extend(DragonList* list, DragonList* other) {
    if (!other) return;
    // list[Any] is a DragonListBox (16B/elem, per-element tag) - its bytes can't
    // be read as a monomorphic DragonList. Dispatch on the shared header tag so
    // `anyList.extend(...)` and `anyList += ...` extend the box layout instead
    // of scribbling 8-byte halves over the {tag,payload} pairs. The type system
    // guarantees a list[Any] is only ever extended by another list[Any].
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
            free(list->data);
            list->elem_size = new_size;
            list->data = malloc((size_t)(list->capacity * list->elem_size));
        }
    }
    bool tags_match = (list->elem_tag == other->elem_tag);
    // Snapshot the source length BEFORE the loop. Self-extend (`l.extend(l)`
    // or `l += l`, both of which reach here with list == other) otherwise
    // re-reads other->size through the aliased pointer every iteration; each
    // append bumps it, so the loop never terminates and allocates until the
    // machine is out of memory. Python's l.extend(l) doubles the list and
    // stops - snapshotting the pre-call length gives exactly that semantics.
    int64_t n = other->size;
    for (int64_t i = 0; i < n; i++) {
        int64_t elem = dragon_list_load(other, i);
        // dragon_incref_tagged is the single source of truth for per-tag RC:
        // STR/LIST/DICT/BYTES like the old chain, PLUS DRAGON_TAG_CLOSURE.
        // The old chain skipped closures while dragon_list_destroy decrefs
        // them, so extending a list[Callable] set up a double-free of every
        // shared closure on teardown.
        if (elem && tags_match) dragon_incref_tagged(elem, other->elem_tag);
        dragon_list_append(list, elem);
    }
}

/// Tag-aware element equality for value-based search (index/count/contains/
/// `in`). String/bytes elements compare by content - not by pointer - so a
/// freshly built string finds an equal element; floats undo the i64 bitcast
/// before comparing; everything else (int/bool/...) is a raw i64 compare.
/// Mirrors the comparator in dragon_list_sort.
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
    // form is meaningless; print a type-appropriate message instead.
    if (list->elem_tag == TAG_INT || list->elem_tag == TAG_BOOL)
        fprintf(stderr, "ValueError: %ld is not in list\n", value);
    else
        fprintf(stderr, "ValueError: value is not in list\n");
    exit(1);
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

/// Sort list in-place. `reverse` selects descending order (Python's
/// `reverse=True`). Insertion sort for simplicity; it is stable, so a
/// descending sort is the genuine stable descending order - NOT an ascending
/// sort reversed (which would invert the order of equal keys). For plain value
/// sorts the distinction is unobservable, but it matters the moment a key
/// projection is added, so we get it right here at zero extra cost.
void dragon_list_sort_ex(DragonList* list, int64_t reverse) {
    // Concurrent-mutation detector: the whole in-place sort is the window.
    bool mut_armed = dragon_shared_mut_begin(&list->header, "list");
    // Comparator switches on the list's element tag - sorting a list of
    // strings by their raw i64 representation orders by pointer address
    // (i.e. randomly), so str/bytes need a content-aware compare.  Floats
    // need their bitcast undone before the compare or the sort observes
    // the IEEE-754 sign+exponent layout, which for negative numbers reverses
    // the natural order.
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
    for (int64_t i = 1; i < list->size; i++) {
        int64_t key = dragon_list_load(list, i);
        int64_t j = i - 1;
        // Ascending: shift while prev > key. Descending: shift while prev < key
        // (i.e. key > prev). Both use the strict `>` so equal keys never move -
        // that is what keeps the sort stable in either direction.
        while (j >= 0 &&
               (desc ? cmp_gt(key, dragon_list_load(list, j))
                     : cmp_gt(dragon_list_load(list, j), key))) {
            dragon_list_store(list, j + 1, dragon_list_load(list, j));
            j--;
        }
        dragon_list_store(list, j + 1, key);
    }
    dragon_shared_mut_end(&list->header, mut_armed);
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

/// dub (docs/002 2.7): deep copy - the spine is fresh and every element is
/// deep-copied by tag (immutable str/bytes elements identity-retain, nested
/// lists/dicts recurse). dragon_deep_copy_tagged returns +1 per element and
/// dragon_list_append adopts it, mirroring dragon_list_copy's ownership
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
        // Incref copied elements: both old and new list own references.
        // dragon_incref_tagged covers STR/LIST/DICT/BYTES like the old chain
        // AND DRAGON_TAG_CLOSURE, which the chain missed - so copying a
        // list[Callable] left both lists decref'ing the same closure on
        // destroy (double-free / UAF, ASan-verified via fs.copy()).
        if (elem) dragon_incref_tagged(elem, list->elem_tag);
        dragon_list_append(copy, elem);
    }
    return copy;
}

/// list + list → a fresh list holding lhs's elements followed by rhs's
/// (Python's `a + b`, which never mutates either operand). Built by copying
/// lhs then extending with rhs: dragon_list_copy and dragon_list_extend already
/// own the per-element incref accounting and the empty-dest tag adoption, so a
/// fresh string list correctly owns +1 on every element. Tag- and
/// variant-agnostic - the same composition dragon_sorted uses - and box-aware
/// via the dispatch below, so list[Any] + list[Any] is handled too.
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

/// Materialize a range [start, stop) by step into a fresh int list - backs
/// `list(range(...))`. range() is otherwise for-loop-fused (no list object),
/// so this is the only place a range becomes a real DragonList.
DragonList* dragon_list_from_range(int64_t start, int64_t stop, int64_t step) {
    DragonList* l = dragon_list_new_tagged(8, TAG_INT);
    if (step == 0) step = 1;  // defensive; Python raises ValueError for step 0
    if (step > 0)
        for (int64_t i = start; i < stop; i += step) dragon_list_append(l, i);
    else
        for (int64_t i = start; i > stop; i += step) dragon_list_append(l, i);
    return l;
}

/// Destroy a list and free its memory (GC support).
/// Child decrefs go through `dragon_decref_*_dispatch`, which
/// route to atomic variants when called inside an atomic-context dealloc.
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

/// Repeat a list N times: [1,2] * 3 → [1,2,1,2,1,2].
/// Fast paths:
///   - bool source of size 1: single memset of total bytes (sieve's
///     `[True] * (limit + 1)` pattern; a 1M-fill went from a 1M-iter store
///     loop to one libc memset).
///   - other primitive sources: bulk memcpy block-by-block using elem_size,
///     no per-element incref work.
DragonListBox* dragon_list_box_repeat(DragonListBox* src, int64_t count);  // fwd

DragonList* dragon_list_repeat(DragonList* src, int64_t count) {
    // list[Any] is a DragonListBox (16B/elem, no elem_tag) - its bytes can't be
    // read as a monomorphic DragonList. Dispatch on the shared header so the
    // native `list[Any] * int` path (and any other caller) is correct. The
    // header tag lives in the common DragonObjectHeader prefix, so this read is
    // valid for both layouts.
    if (src && src->header.type_tag == DRAGON_TAG_LIST_BOX)
        return (DragonList*)dragon_list_box_repeat((DragonListBox*)(void*)src, count);
    if (count <= 0 || src->size == 0) {
        return dragon_list_new_tagged(0, src->elem_tag);
    }
    // Overflow guard: mirror dragon_bytes_repeat / dragon_str_repeat. Without
    // this, a crafted size*count wrap silently undersizes the result buffer
    // and the per-block memcpy below writes past the allocation.
    if (count > INT64_MAX / src->size) {
        fprintf(stderr, "MemoryError: list repeat too large\n"); exit(1);
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
            // dragon_incref_tagged, not a bare dragon_incref. A list[Callable]
            // element may be a bare function pointer with no object header
            //; the generic dragon_incref read/wrote refcount bytes inside
            // the function's .text (read-only) -> SIGSEGV on `[f] * 3`. The
            // tagged path routes closures through the tag-gated
            // dragon_incref_callable, which is safe on a headerless fn ptr.
            if (val) dragon_incref_tagged(val, src->elem_tag);
        }
    }
    return result;
}

//===----------------------------------------------------------------------===//
// D030 Phase 3 - Monomorphized list family: F64 and Ptr variants.
//
// These structs share the I64 variant's memory layout (header / data / size /
// capacity / elem_tag / elem_size) - see runtime_internal.h. Polymorphic ops
// (destroy, len, GC traverse) keep working on a `DragonList*` by reading the
// shared prefix; per-type ops below operate on the native data pointer
// directly so codegen never sees an i64 funnel for typed lists.
//===----------------------------------------------------------------------===//

/// list[float] - allocate with native f64 storage.
DragonListF64* dragon_list_new_f64(int64_t capacity) {
    auto* list = (DragonListF64*)malloc(sizeof(DragonListF64));
    dragon_obj_init(&list->header, DRAGON_TAG_LIST);
    list->capacity = capacity > 0 ? capacity : 8;
    list->size = 0;
    list->elem_tag = TAG_FLOAT;
    list->elem_size = 8;
    list->data = (double*)malloc((size_t)list->capacity * sizeof(double));
    // Acyclic-skip: a float list is native double[] storage - no heap pointers,
    // never cyclic - so it is never tracked (see dragon_list_new_tagged). The
    // counter bump stays unconditional to keep collection cadence unchanged.
    if (__atomic_add_fetch(&gc_alloc_counter, 1, __ATOMIC_RELAXED)
        >= __atomic_load_n(&gc_threshold, __ATOMIC_RELAXED)) {
        dragon_gc_collect();
    }
    return list;
}

double dragon_list_get_f64(DragonListF64* list, int64_t index) {
    if (index < 0) index += list->size;
    if (index < 0 || index >= list->size) {
        fprintf(stderr, "IndexError: list index out of range\n");
        exit(1);
    }
    return list->data[index];
}

void dragon_list_set_f64(DragonListF64* list, int64_t index, double value) {
    if (index < 0) index += list->size;
    if (index < 0 || index >= list->size) {
        dragon_raise_exc_cstr(41, "IndexError: list assignment index out of range");
        return;
    }
    // Concurrent-mutation detector: a plain 8-byte element store cannot
    // corrupt structure, but a concurrent shrink (pop/remove) can free the
    // buffer under this store - keep the same window discipline.
    bool mut_armed = dragon_shared_mut_begin(&list->header, "list");
    list->data[index] = value;
    dragon_shared_mut_end(&list->header, mut_armed);
}

void dragon_list_append_f64(DragonListF64* list, double value) {
    // Concurrent-mutation detector - see dragon_list_append.
    bool mut_armed = dragon_shared_mut_begin(&list->header, "list");
    if (list->size >= list->capacity) {
        int64_t new_cap = list->capacity * 2;
        double* tmp = (double*)realloc(list->data, (size_t)new_cap * sizeof(double));
        if (!tmp) { fprintf(stderr, "dragon: out of memory\n"); abort(); }
        list->data = tmp;
        list->capacity = new_cap;
    }
    list->data[list->size++] = value;
    dragon_shared_mut_end(&list->header, mut_armed);
}

/// list[<heap>] - allocate with native pointer storage. elem_tag selects the
/// per-element refcount semantics (TAG_STR uses dragon_incref_str family;
/// other heap tags use the generic dragon_incref family).
DragonListPtr* dragon_list_new_ptr(int64_t capacity, int64_t elem_tag) {
    auto* list = (DragonListPtr*)malloc(sizeof(DragonListPtr));
    dragon_obj_init(&list->header, DRAGON_TAG_LIST);
    list->capacity = capacity > 0 ? capacity : 8;
    list->size = 0;
    list->elem_tag = (uint8_t)elem_tag;
    list->elem_size = 8;
    list->data = (void**)malloc((size_t)list->capacity * sizeof(void*));
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
        fprintf(stderr, "IndexError: list index out of range\n");
        exit(1);
    }
    return list->data[index];
}

/// Refcount-aware set: decref the old element (if non-NULL and different),
/// then store the new value. The runtime owns the refcount accounting so
/// codegen doesn't have to emit incref/decref around `xs[i] = v`.
void dragon_list_set_ptr(DragonListPtr* list, int64_t index, void* value) {
    if (index < 0) index += list->size;
    if (index < 0 || index >= list->size) {
        dragon_raise_exc_cstr(41, "IndexError: list assignment index out of range");
        return;
    }
    // Concurrent-mutation detector: armed after the raise-y validation.
    bool mut_armed = dragon_shared_mut_begin(&list->header, "list");
    void* old = list->data[index];
    // Write barrier: if the parent list is SHARED across threads, the new
    // value transitively becomes SHARED too (so future plain RC ops on it
    // route to atomic).
    if (value && (list->header.gc_flags & GC_FLAG_SHARED)) {
        if (list->elem_tag == TAG_STR)
            dragon_mark_shared_str((const char*)value);
        else
            dragon_mark_shared_deep(value);
    }
    // Reentrancy hardening: store the new value BEFORE dropping the old one's
    // ref, so a finalizer that re-reads list[index] during the drop sees
    // `value`, never the freed `old`.
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

/// Refcount-aware append: takes ownership of one reference on `value`.
/// Caller is responsible for incref'ing if `value` is borrowed (matches the
/// existing append semantics for the I64 polymorphic path).
void dragon_list_append_ptr(DragonListPtr* list, void* value) {
    // Concurrent-mutation detector - see dragon_list_append.
    bool mut_armed = dragon_shared_mut_begin(&list->header, "list");
    if (list->size >= list->capacity) {
        int64_t new_cap = list->capacity * 2;
        void** tmp = (void**)realloc(list->data, (size_t)new_cap * sizeof(void*));
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

//===----------------------------------------------------------------------===//
// D039 Phase 4: list[Any] - DragonListBox with per-element {tag, payload}
//
// Dragon's equivalent of Go's `[]interface{}` and Rust's `Vec<Box<dyn Any>>`,
// but with INLINE storage (no per-element heap allocation). 16 bytes per
// element, contiguous; one cache miss per read. Same speed model as
// dict[str, Any] via DragonBox.
//===----------------------------------------------------------------------===//

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
    //
    // TAG_CLOSURE (10) deliberately has NO decref arm yet - a WALL, not an
    // oversight. Codegen's boxArgTagPayload increfs
    // BORROWED sources for tags 1/5/6/7 only, so `anyList.append(f)` with a
    // borrowed Callable local stores a tag-10 payload at +0. Adding
    // dragon_decref_callable here is ASan-PROVEN to double-free
    // that shape: box destroy released a ref the box never took, then the
    // local's scope-cleanup decref hit freed memory (heap-use-after-free in
    // dragon_decref_callable). The known cost of the skip: box repeat/extend/
    // concat's dragon_incref_tagged +1 on tag-10 payloads leaks (closures in
    // a churned list[Any] extend), and a fresh lambda appended to list[Any]
    // leaks its transferred +1. Fix ORDER: teach boxArgTagPayload
    // (src/codegen/ImplMethods2.cpp) a litTag == 10 ->
    // dragon_incref_callable arm FIRST, then land this decref arm, the
    // dict_values_box incref mirror, and the tag-10 arms in
    // dragon_list_box_traverse / dragon_list_box_clear_refs TOGETHER.
}

/// list[Any] allocation. capacity rounds up to at least 8 to match other variants.
DragonListBox* dragon_list_box_new(int64_t capacity) {
    auto* list = (DragonListBox*)malloc(sizeof(DragonListBox));
    // Distinct type tag so the GC dispatches destroy/traverse/clear to
    // dragon_list_box_* helpers - the layout differs from DragonList
    // (16B/elem stride, per-element tag) so we can't share dragon_list_destroy.
    dragon_obj_init(&list->header, DRAGON_TAG_LIST_BOX);
    list->capacity = capacity > 0 ? capacity : 8;
    list->size = 0;
    list->data = (DragonListBoxElem*)malloc((size_t)list->capacity * sizeof(DragonListBoxElem));
    dragon_gc_track(list);
    if (__atomic_add_fetch(&gc_alloc_counter, 1, __ATOMIC_RELAXED)
        >= __atomic_load_n(&gc_threshold, __ATOMIC_RELAXED)) {
        dragon_gc_collect();
    }
    return list;
}

/// list[Any] * int → fresh box list with `count` repetitions. The DragonListBox
/// analogue of dragon_list_repeat (which only handles the monomorphic
/// DragonList layout): each heap payload (str/list/dict/bytes/class) gets one
/// incref per copy so the new list owns its own element refs. count<=0 or an
/// empty source yields an empty list (Python parity). Called by
/// dragon_box_binop for `Any * int` when the boxed list is a list[Any].
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

/// Representation check for unboxing a box-tagged (TAG_LIST) payload into a
/// typed list view. The box tag alone says "some list" - it cannot tell a
/// monomorphized DragonList (8B/elem native storage) from a DragonListBox
/// (16B/elem {tag, payload} storage). Reading one at the other's stride is
/// silent value corruption or OOB, so a typed unbox must verify the payload's
/// HEADER first:
///   want_elem_tag == -1  -> the target is list[Any] / list[union]: require
///                           DRAGON_TAG_LIST_BOX.
///   want_elem_tag >=  0  -> the target is a concrete list[T]: require
///                           DRAGON_TAG_LIST with a matching elem_tag.
/// No empty-list exemption: a cross-representation view of an empty list
/// would still APPEND through the wrong layout. Raises TypeError (80) on
/// mismatch; a match returns and the caller uses the payload as-is (view, not
/// copy - matching representation aliases safely).
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

/// Read element `index` as a 16-byte {tag, payload} box.
/// BORROW contract - payload pointer is non-owning. Codegen increfs at the
/// store-into-longer-lived-slot site, matching dragon_dict_get_box.
struct DragonBoxValue { int64_t tag; int64_t payload; };
DragonBoxValue dragon_list_box_get(DragonListBox* list, int64_t index) {
    if (index < 0) index += list->size;
    if (index < 0 || index >= list->size) {
        fprintf(stderr, "IndexError: list index out of range\n");
        exit(1);
    }
    DragonBoxValue v;
    v.tag = list->data[index].tag;
    v.payload = list->data[index].payload;
    return v;
}

/// Refcount-aware set: decref the old element's payload by its tag, then
/// store the new tag+payload. Codegen ensures the new payload's refcount
/// has been incremented before calling this (matches dragon_list_set_ptr's
/// Model-B contract).
void dragon_list_box_set(DragonListBox* list, int64_t index, int64_t tag, int64_t payload) {
    if (index < 0) index += list->size;
    if (index < 0 || index >= list->size) {
        dragon_raise_exc_cstr(41, "IndexError: list assignment index out of range");
        return;
    }
    // Concurrent-mutation detector: armed after the raise-y validation.
    bool mut_armed = dragon_shared_mut_begin(&list->header, "list");
    // Reentrancy hardening: capture the old element, write the new {tag,
    // payload}, THEN drop the old ref - so a finalizer re-reading list[index]
    // during the drop sees the new element, never the freed payload.
    DragonListBoxElem old_elem = list->data[index];
    list->data[index].tag = tag;
    list->data[index].payload = payload;
    dragon_shared_mut_end(&list->header, mut_armed);
    dragon_listbox_decref_elem(&old_elem);
}

/// Delete element at index from a box list (`del lst[i]` on list[Any]).
/// 16-byte-aware: decref the removed element's payload (del discards it),
/// then shift the remaining {tag,payload} elements down. Negative index
/// supported; out-of-range is IndexError. A box list canNOT go through
/// dragon_list_delitem - that shifts 8-byte halves and scrambles the boxes.
void dragon_list_box_delitem(DragonListBox* list, int64_t index) {
    if (index < 0) index += list->size;
    if (index < 0 || index >= list->size) {
        dragon_raise_exc_cstr(41, "IndexError: list assignment index out of range");
        return;
    }
    // Concurrent-mutation detector: armed after the raise-y validation.
    bool mut_armed = dragon_shared_mut_begin(&list->header, "list");
    // Reentrancy hardening: capture the element, shift the tail down and
    // shrink, THEN drop the ref - so a finalizer running during the drop
    // can't observe the freed slot.
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
        DragonListBoxElem* tmp = (DragonListBoxElem*)realloc(list->data,
            (size_t)new_cap * sizeof(DragonListBoxElem));
        if (!tmp) { fprintf(stderr, "dragon: out of memory\n"); abort(); }
        list->data = tmp;
        list->capacity = new_cap;
    }
    list->data[list->size].tag = tag;
    list->data[list->size].payload = payload;
    list->size++;
    dragon_shared_mut_end(&list->header, mut_armed);
}

/// In-place extend of a list[Any] by another (`anyList.extend(o)` / `+=`).
/// Each appended payload gets one incref so the destination owns its own
/// reference, matching the monomorphic dragon_list_extend's discipline.
///
/// The source may be EITHER a box list OR a monomorphic DragonList - the latter
/// happens for `anyList += [1, 2]` / `anyList.extend([..])`, where the literal
/// is built at its concrete element type (list[int]) because list[T] <:
/// list[Any]. We detect that via the shared header tag and box each element by
/// the source list's single elem_tag (floats keep their bit pattern, which is
/// exactly how a box stores a float). This keeps the box/mono mix correct at
/// the root rather than forcing the literal to be pre-boxed.
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
    // Snapshot the source length: `anyList.extend(anyList)` / `+=` self-alias
    // would otherwise grow src->size every append and never terminate (OOM).
    // Matches the same fix in dragon_list_extend.
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

// Forward declaration - dragon_box_eq lives in runtime_box.cpp. Passed as
// two i64s on AMD64 SysV; we mirror that ABI here so cross-TU calls work
// without going through a 16B-by-value struct return.
struct DragonBoxAbi { int64_t tag; int64_t payload; };
extern int64_t dragon_box_eq(DragonBoxAbi a, DragonBoxAbi b);
extern int64_t dragon_box_cmp(DragonBoxAbi a, DragonBoxAbi b, int64_t op);

/// Pop element at index (default -1) from a box list, returning the {tag,
/// payload}. Ownership of any refcounted payload transfers to the caller -
/// NO decref here (mirrors dragon_list_pop's borrow-out contract). 16-byte
/// memmove shifts the tail. The other three list variants use dragon_list_pop.
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

/// Remove first element value-equal to {tag, payload} from a box list
/// (ValueError if absent). Value equality via dragon_box_eq (str/list/dict
/// compare by content, matching Python). Decrefs the removed element (the
/// list owned it); 16-byte memmove shifts the tail.
void dragon_list_box_remove(DragonListBox* list, int64_t tag, int64_t payload) {
    // Concurrent-mutation detector: window covers the search (see
    // dragon_list_remove); not-found exits the process, no longjmp.
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
    fprintf(stderr, "ValueError: list.remove(x): x not in list\n");
    exit(1);
}

/// Insert {tag, payload} at index into a box list (Python list.insert
/// clamp semantics: negative counts from end, out-of-range clamps). The
/// caller owns the incref on a refcounted payload (Model B, like append).
/// 16-byte memmove opens the gap.
void dragon_list_box_insert(DragonListBox* list, int64_t index, int64_t tag, int64_t payload) {
    // Concurrent-mutation detector: no raise below (index clamps, OOM aborts).
    bool mut_armed = dragon_shared_mut_begin(&list->header, "list");
    if (index < 0) index += list->size;
    if (index < 0) index = 0;
    if (index > list->size) index = list->size;
    if (list->size >= list->capacity) {
        int64_t new_cap = list->capacity * 2;
        DragonListBoxElem* tmp = (DragonListBoxElem*)realloc(list->data,
            (size_t)new_cap * sizeof(DragonListBoxElem));
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

/// Read element `i` of any list variant (DragonList[I64], DragonListF64,
/// DragonListPtr, DragonListBox) as a {tag, payload} box. Dispatches on the
/// header's type_tag so cross-variant comparisons work (e.g., comparing a
/// list[int] to a list[Any] elementwise).
///
/// For DragonListBox the box is read directly. For the other variants the
/// elem_tag is the box tag, and the payload is the raw 8-byte slot (with
/// bool zero-extended from its 1-byte slot).
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

/// Deep equality between two list values. Walks both lengthwise and
/// compares each element via dragon_box_eq, which recurses into nested
/// lists/dicts. Cross-variant safe - a list[int] compares equal to a
/// list[Any] holding the same boxed ints.
///
/// Pointer-identity short-circuits to True; NULL on either side returns
/// 1 only if both are NULL.
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

/// Lexicographic three-way comparison of two lists (Python `<`/`<=`/`>`/`>=`
/// semantics): compare element-wise via dragon_box_cmp; the first non-equal
/// pair decides, else the shorter list is less. Returns -1/0/1. Handles all
/// four list variants (the elem-as-box reader normalizes them) and recurses
/// through dragon_box_cmp for nested lists. Raises TypeError on an incomparable
/// element pair (e.g. `[1] < ["a"]`). Backs the native `list < list` codegen
/// path.
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

/// Pretty-print as `[a, b, c]` with each element formatted by its tag.
/// Used by print(list_any_value).
void dragon_print_list_box_raw(DragonListBox* list) {
    if (!list) { printf("None"); return; }
    printf("[");
    for (int64_t i = 0; i < list->size; i++) {
        if (i > 0) printf(", ");
        int64_t tag = list->data[i].tag;
        int64_t v = list->data[i].payload;
        switch (tag) {
            case TAG_INT:   printf("%lld", (long long)v); break;
            case TAG_STR:   printf("'%s'", (const char*)(uintptr_t)v); break;
            case TAG_FLOAT: {
                double f; memcpy(&f, &v, sizeof(double));
                char ftmp[64];
                dragon_format_double_into(f, ftmp, sizeof(ftmp));
                fputs(ftmp, stdout);
                break;
            }
            case TAG_BOOL:  printf("%s", v ? "True" : "False"); break;
            case TAG_NONE:  printf("None"); break;
            case TAG_LIST:
            case TAG_DICT: {
                // Nested container element: delegate to dragon_print_box_raw,
                // which renders lists/dicts (recursively for a nested
                // list[Any]) rather than the old "<box tag=N>" placeholder.
                DragonBox nested{tag, v};
                dragon_print_box_raw(nested);
                break;
            }
            case TAG_BYTES: {
                // TAG_BYTES == TAG_CLASS: only a genuine bytes object is safe to
                // render via the box printer (a class instance shares the value
                // tag but has no runtime repr here), so disambiguate on the
                // object header's type_tag before delegating.
                DragonObjectHeader* h = (DragonObjectHeader*)(uintptr_t)v;
                if (h && h->type_tag == DRAGON_TAG_BYTES) {
                    DragonBox nested{tag, v};
                    dragon_print_box_raw(nested);
                } else {
                    printf("<box tag=%lld>", (long long)tag);
                }
                break;
            }
            default:        printf("<box tag=%lld>", (long long)tag); break;
        }
    }
    printf("]");
}
void dragon_print_list_box(DragonListBox* list) {
    dragon_print_list_box_raw(list);
    putchar('\n');
}

} // extern "C"
