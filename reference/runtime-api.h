/**
 * Dragon Runtime API Reference
 * ============================
 * Auto-generated stub file for quick reference. All functions are exported
 * as `extern "C"` from lib/Runtime/runtime.cpp with the `dragon_` prefix.
 *
 * Per Decision 030 values flow at their native LLVM types: int->i64, float->f64,
 * bool->i1, and str/bytes/list/dict/instances->ptr. Union[...]/Any use a 16-byte
 * { i64 tag, i64 payload } box. (The earlier "everything is i64" model is gone.)
 *
 * STABILITY (ADR 041 -- public native-extension FFI): only the symbols marked
 * "[STABLE FFI -- ADR 041]" are a contract external `.dr` code may link against:
 * dragon_string_alloc, dragon_str_to_utf8_bytes, dragon_str_encode,
 * dragon_bytes_new, dragon_bytes_data, dragon_bytes_len, dragon_incref/decref,
 * dragon_raise_exc. Everything else here is INTERNAL and may change without
 * notice -- do not depend on it from out-of-tree extensions.
 *
 * Tags: TAG_INT=0, TAG_STR=1, TAG_FLOAT=2, TAG_BOOL=3, TAG_NONE=4,
 *  TAG_LIST=5, TAG_DICT=6, TAG_BYTES=7, TAG_GENERATOR=8
 *
 * Object Header (16 bytes): { i64 refcount, u8 type_tag, u8 gc_flags,
 *  u16 class_id, i32 gc_track_idx }
 */

#pragma once
#include <cstdint>

// ============================================================================
// Forward declarations (opaque runtime types)
// ============================================================================
struct DragonList;
struct DragonDict;
struct DragonTuple;
struct DragonSet;
struct DragonBytes;
struct DragonSyncList;
struct DragonSyncDict;
struct DragonVThread;
struct DragonThread;

extern "C" {

// ============================================================================
// 1. GARBAGE COLLECTION & MEMORY MANAGEMENT
// ============================================================================

/**
 * Increment reference count of a heap-allocated Dragon object.
 * No-op if obj is NULL or points to an immortal/static object.
 * @param obj Pointer to any Dragon heap object (has DragonObjectHeader)
 */
void dragon_incref(void* obj) {}

/**
 * Decrement reference count. Deallocates the object when refcount reaches zero.
 * Dispatches to type-specific dealloc based on the header's type_tag.
 * @param obj Pointer to any Dragon heap object
 */
void dragon_decref(void* obj) {}

/**
 * Thread-safe atomic increment (uses __atomic_add_fetch with __ATOMIC_RELAXED).
 * Used for objects shared across fire/async spawn sites.
 * @param obj Pointer to any Dragon heap object
 */
void dragon_incref_atomic(void* obj) {}

/**
 * Thread-safe atomic decrement (uses __atomic_sub_fetch with __ATOMIC_ACQ_REL).
 * Deallocates when refcount reaches zero, same as dragon_decref.
 * @param obj Pointer to any Dragon heap object
 */
void dragon_decref_atomic(void* obj) {}

/**
 * Increment refcount of a heap-allocated DragonString.
 * Backs up from the char* data pointer to find the DragonObjectHeader.
 * No-op for string literals (detected by immortal refcount or static storage).
 * @param s Pointer to string data (char* inside a DragonString)
 */
void dragon_incref_str(const char* s) {}

/**
 * Decrement refcount of a heap-allocated DragonString. Frees at zero.
 * @param s Pointer to string data
 */
void dragon_decref_str(const char* s) {}

/**
 * Thread-safe atomic string incref for cross-thread string sharing.
 * @param s Pointer to string data
 */
void dragon_incref_str_atomic(const char* s) {}

/**
 * Thread-safe atomic string decref for cross-thread string sharing.
 * @param s Pointer to string data
 */
void dragon_decref_str_atomic(const char* s) {}

/**
 * Register object with the GC cycle collector's tracked-objects array.
 * Only containers (list, dict, tuple, class instances) need tracking.
 * Auto-triggers collection when tracked count exceeds threshold (default=700).
 * @param obj Pointer to a container object
 */
void dragon_gc_track(void* obj) {}

/**
 * Remove object from GC cycle collector tracking.
 * Called during deallocation to prevent dangling references in the GC array.
 * @param obj Pointer to a tracked container object
 */
void dragon_gc_untrack(void* obj) {}

/**
 * Set the allocation threshold that triggers automatic cycle collection.
 * @param n Number of tracked allocations before auto-collect (default=700)
 */
void dragon_gc_set_threshold(int64_t n) {}

/**
 * Perform manual cycle collection using CPython's trial deletion algorithm.
 * Steps: copy refcounts -> subtract internal refs -> mark reachable (BFS) ->
 * clear unreachable objects.
 * @return Number of objects collected
 */
int64_t dragon_gc_collect() { return 0; }

/**
 * Register a per-class dealloc function for custom classes.
 * Called once during class definition codegen.
 * @param fn Function pointer: void(*)(void* instance)
 * @return Assigned class_id (used in object header)
 */
int64_t dragon_class_register_dealloc(void* fn) { return 0; }

/**
 * Register a per-class GC traverse function for cycle detection.
 * The traverse function visits all object references the class holds.
 * @param class_id The class_id returned by dragon_class_register_dealloc
 * @param fn Function pointer: void(*)(void* instance, void(*visit)(void*))
 * @return 0 on success
 */
int64_t dragon_class_register_traverse(int64_t class_id, void* fn) { return 0; }


// ============================================================================
// 2. STRING OPERATIONS
// ============================================================================

// --- 2.1 Allocation & Duplication ---

/**
 * Allocate a heap-refcounted copy of a string.
 * If the input is already a heap DragonString, increments its refcount instead.
 * @param s Source C string (NUL-terminated)
 * @return Heap-allocated DragonString data pointer (refcount=1)
 */
const char* dragon_string_dup(const char* s) { return nullptr; }

// --- 2.2 Concatenation & Repetition ---

/**
 * Concatenate two strings into a new heap-allocated string.
 * @param a Left string
 * @param b Right string
 * @return New string: a + b
 */
const char* dragon_str_concat(const char* a, const char* b) { return nullptr; }

/**
 * Append b onto a, mutating in place when a is uniquely owned.
 * Consumes one reference to a; borrows b. Returns the new value of the slot.
 * @return a with b appended (possibly a relocated buffer)
 */
const char* dragon_str_append_inplace(const char* a, const char* b) { return nullptr; }

/**
 * Repeat a string n times.
 * @param s Source string
 * @param n Repeat count (<=0 returns empty string)
 * @return New string: s * n
 */
const char* dragon_str_repeat(const char* s, int64_t n) { return nullptr; }

/**
 * Replace all occurrences of old_s with new_s in s.
 * @param s Source string
 * @param old_s Substring to find
 * @param new_s Replacement substring
 * @return New string with replacements applied
 */
const char* dragon_str_replace(const char* s, const char* old_s, const char* new_s) { return nullptr; }

// --- 2.3 Type Conversion ---

/**
 * Convert integer to its decimal string representation.
 * @param value Integer value
 * @return Heap-allocated string (e.g., "42", "-7")
 */
const char* dragon_int_to_str(int64_t value) { return nullptr; }

/**
 * Convert float to string, stripping trailing zeros.
 * @param value Float value (passed as double)
 * @return Heap-allocated string (e.g., "3.14", "0.0")
 */
const char* dragon_float_to_str(double value) { return nullptr; }

/**
 * Format float with Python format spec (e.g., ".2f", ".4e").
 * @param value Float value
 * @param spec Format specifier string
 * @return Heap-allocated formatted string
 */
const char* dragon_float_format(double value, const char* spec) { return nullptr; }

/**
 * Format integer with Python format spec (e.g., "x", "X", "o", "b", "05d").
 * @param value Integer value
 * @param spec Format specifier string
 * @return Heap-allocated formatted string
 */
const char* dragon_int_format(int64_t value, const char* spec) { return nullptr; }

/**
 * Convert boolean to "True" or "False" string.
 * @param value Boolean as i64 (0=false, nonzero=true)
 * @return Static string "True" or "False"
 */
const char* dragon_bool_to_str(int64_t value) { return nullptr; }

/**
 * Parse string to integer. Raises ValueError on invalid input.
 * @param s String containing integer (e.g., "42", "-7", "0xff")
 * @return Parsed integer value
 */
int64_t dragon_str_to_int(const char* s) { return 0; }

/**
 * Parse string to float. Raises ValueError on invalid input.
 * @param s String containing float (e.g., "3.14", "1e10")
 * @return Parsed float value (as double)
 */
double dragon_str_to_float(const char* s) { return 0.0; }

// --- 2.4 Metrics ---

/**
 * Get string length in bytes.
 * @param s Source string
 * @return Length (0 if NULL)
 */
int64_t dragon_str_len(const char* s) { return 0; }

/**
 * String equality comparison.
 * @param a First string
 * @param b Second string
 * @return 1 if equal, 0 if not
 */
int64_t dragon_str_eq(const char* a, const char* b) { return 0; }

// --- 2.5 Indexing & Slicing ---

/**
 * Get character at index as a new single-character string.
 * Supports negative indexing (Python semantics). Raises IndexError if out of bounds.
 * @param s Source string
 * @param index Character position (negative counts from end)
 * @return New single-char heap string
 */
const char* dragon_str_index(const char* s, int64_t index) { return nullptr; }

/**
 * Slice string with start:stop:step (Python slice semantics).
 * Sentinel value INT64_MIN used for omitted parameters.
 * @param s Source string
 * @param start Start index (inclusive)
 * @param stop Stop index (exclusive)
 * @param step Step size (can be negative for reverse)
 * @return New heap-allocated substring
 */
const char* dragon_str_slice(const char* s, int64_t start, int64_t stop, int64_t step) { return nullptr; }

// --- 2.6 Case Methods ---

/** Convert all characters to uppercase. */
const char* dragon_str_upper(const char* s) { return nullptr; }

/** Convert all characters to lowercase. */
const char* dragon_str_lower(const char* s) { return nullptr; }

/** Convert to title case (first letter of each word capitalized). */
const char* dragon_str_title(const char* s) { return nullptr; }

/** Capitalize first character, lowercase rest. */
const char* dragon_str_capitalize(const char* s) { return nullptr; }

/** Swap case of all characters. */
const char* dragon_str_swapcase(const char* s) { return nullptr; }

/** Case-fold for caseless matching (same as lower for ASCII). */
const char* dragon_str_casefold(const char* s) { return nullptr; }

// --- 2.7 Whitespace Stripping ---

/** Remove leading and trailing whitespace. */
const char* dragon_str_strip(const char* s) { return nullptr; }

/** Remove leading whitespace only. */
const char* dragon_str_lstrip(const char* s) { return nullptr; }

/** Remove trailing whitespace only. */
const char* dragon_str_rstrip(const char* s) { return nullptr; }

/**
 * Expand tab characters to spaces.
 * @param s Source string
 * @param tabsize Number of spaces per tab (default 8 in Python)
 * @return New string with tabs expanded
 */
const char* dragon_str_expandtabs(const char* s, int64_t tabsize) { return nullptr; }

// --- 2.8 Padding & Alignment ---

/**
 * Center string in a field of given width, padded with fill character.
 * @param s Source string
 * @param w Total field width
 * @param fill Padding character (single char)
 * @return Centered string (unchanged if len >= w)
 */
const char* dragon_str_center(const char* s, int64_t w, char fill) { return nullptr; }

/** Left-justify string in field of width w, padded with fill char on right. */
const char* dragon_str_ljust(const char* s, int64_t w, char fill) { return nullptr; }

/** Right-justify string in field of width w, padded with fill char on left. */
const char* dragon_str_rjust(const char* s, int64_t w, char fill) { return nullptr; }

/**
 * Pad numeric string with leading zeros to width w.
 * Handles sign prefix correctly ("-007" not "00-7").
 * @param s Source string
 * @param w Target width
 * @return Zero-padded string
 */
const char* dragon_str_zfill(const char* s, int64_t w) { return nullptr; }

// --- 2.9 Prefix & Suffix ---

/** Remove prefix if string starts with it, else return copy. */
const char* dragon_str_removeprefix(const char* s, const char* prefix) { return nullptr; }

/** Remove suffix if string ends with it, else return copy. */
const char* dragon_str_removesuffix(const char* s, const char* suffix) { return nullptr; }

/** Check if string starts with prefix. Returns 1 or 0. */
int64_t dragon_str_startswith(const char* s, const char* prefix) { return 0; }

/** Check if string ends with suffix. Returns 1 or 0. */
int64_t dragon_str_endswith(const char* s, const char* suffix) { return 0; }

// --- 2.10 Search & Count ---

/** Find first occurrence of substring. Returns byte index or -1. */
int64_t dragon_str_find(const char* s, const char* sub) { return 0; }

/** Find last occurrence of substring. Returns byte index or -1. */
int64_t dragon_str_rfind(const char* s, const char* sub) { return 0; }

/** Find first occurrence of substring. Raises ValueError if not found. */
int64_t dragon_str_index_of(const char* s, const char* sub) { return 0; }

/** Find last occurrence of substring. Raises ValueError if not found. */
int64_t dragon_str_rindex(const char* s, const char* sub) { return 0; }

/** Count non-overlapping occurrences of substring. */
int64_t dragon_str_count(const char* s, const char* sub) { return 0; }

/** Check if string contains substring. Returns 1 or 0. */
int64_t dragon_str_contains(const char* s, const char* sub) { return 0; }

// --- 2.11 Character Classification ---

/** True if all characters are ASCII digits (0-9). Empty string returns false. */
int64_t dragon_str_isdigit(const char* s) { return 0; }

/** True if all characters are alphabetic. Empty string returns false. */
int64_t dragon_str_isalpha(const char* s) { return 0; }

/** True if all characters are alphanumeric. Empty string returns false. */
int64_t dragon_str_isalnum(const char* s) { return 0; }

/** True if all characters are whitespace. Empty string returns false. */
int64_t dragon_str_isspace(const char* s) { return 0; }

/** True if all cased characters are uppercase. */
int64_t dragon_str_isupper(const char* s) { return 0; }

/** True if all cased characters are lowercase. */
int64_t dragon_str_islower(const char* s) { return 0; }

/** True if string is title-cased (first letter of each word capitalized). */
int64_t dragon_str_istitle(const char* s) { return 0; }

/** True if all bytes are in the ASCII range (0x00-0x7F). */
int64_t dragon_str_isascii(const char* s) { return 0; }

/** True if all characters are decimal digits. */
int64_t dragon_str_isdecimal(const char* s) { return 0; }

/** True if all characters are numeric (digits + numeric Unicode). */
int64_t dragon_str_isnumeric(const char* s) { return 0; }

/** True if all characters are printable (not control chars). */
int64_t dragon_str_isprintable(const char* s) { return 0; }

/** True if string is a valid Python identifier. */
int64_t dragon_str_isidentifier(const char* s) { return 0; }

// --- 2.12 Joining ---

/**
 * Join list elements with separator string (like Python's sep.join(list)).
 * @param sep Separator string
 * @param l DragonList of string values (as i64 pointers)
 * @return New joined string
 */
const char* dragon_str_join(const char* sep, DragonList* l) { return nullptr; }


// ============================================================================
// 3. LIST OPERATIONS
// ============================================================================

// --- 3.1 Creation ---

/**
 * Create a new empty list with initial capacity.
 * Allocates DragonList with DragonObjectHeader (type_tag=TAG_LIST, elem_tag=TAG_INT).
 * @param capacity Initial backing array size
 * @return New DragonList pointer (refcount=1)
 */
DragonList* dragon_list_new(int64_t capacity) { return nullptr; }

/**
 * Create a new list with a specific element type tag.
 * Used when the compiler knows the element type at creation time.
 * @param capacity Initial backing array size
 * @param elem_tag Element type tag (TAG_INT, TAG_STR, TAG_FLOAT, etc.)
 * @return New DragonList pointer (refcount=1)
 */
DragonList* dragon_list_new_tagged(int64_t capacity, int64_t elem_tag) { return nullptr; }

/**
 * Shallow copy of a list. Increfs all heap elements (strings, objects).
 * @param list Source list
 * @return New list with copied elements
 */
DragonList* dragon_list_copy(DragonList* list) { return nullptr; }

// --- 3.2 Access ---

/**
 * Get element at index. Supports negative indexing (Python semantics).
 * Raises IndexError if out of bounds.
 * @param list Target list
 * @param index Element position (negative counts from end)
 * @return Element value as i64 (may be pointer for heap types)
 */
int64_t dragon_list_get(DragonList* list, int64_t index) { return 0; }

/**
 * Set element at index with proper reference counting.
 * Decrefs the old value, increfs the new value (for heap types).
 * @param list Target list
 * @param index Element position (supports negative)
 * @param value New value as i64
 */
void dragon_list_set(DragonList* list, int64_t index, int64_t value) {}

/**
 * Get list length.
 * @param list Target list
 * @return Number of elements
 */
int64_t dragon_list_len(DragonList* list) { return 0; }

// --- 3.3 Modification ---

/**
 * Append element to end of list. Grows backing array if needed (2x).
 * @param list Target list
 * @param value Value to append
 */
void dragon_list_append(DragonList* list, int64_t value) {}

/**
 * Insert element at index. Shifts subsequent elements right.
 * @param list Target list
 * @param index Insertion position
 * @param value Value to insert
 */
void dragon_list_insert(DragonList* list, int64_t index, int64_t value) {}

/**
 * Remove first occurrence of value. Raises ValueError if not found.
 * Decrefs the removed element if it's a heap type.
 * @param list Target list
 * @param value Value to remove
 */
void dragon_list_remove(DragonList* list, int64_t value) {}

/**
 * Remove and return element at index. Shifts subsequent elements left.
 * @param list Target list
 * @param index Position to pop (-1 for last element)
 * @return Removed element value
 */
int64_t dragon_list_pop(DragonList* list, int64_t index) { return 0; }

/**
 * Remove all elements from the list. Decrefs heap elements.
 * @param list Target list
 */
void dragon_list_clear(DragonList* list) {}

/**
 * Extend list with all elements from another list. Increfs heap elements.
 * @param list Target list (modified in-place)
 * @param other Source list to copy from
 */
void dragon_list_extend(DragonList* list, DragonList* other) {}

// --- 3.4 Search ---

/**
 * Find index of first occurrence of value. Raises ValueError if not found.
 * @param list Target list
 * @param value Value to search for
 * @return Zero-based index
 */
int64_t dragon_list_index(DragonList* list, int64_t value) { return 0; }

/**
 * Count occurrences of value in list.
 * @param list Target list
 * @param value Value to count
 * @return Number of occurrences
 */
int64_t dragon_list_count(DragonList* list, int64_t value) { return 0; }

// --- 3.5 Sorting & Reversal ---

/** Sort list in-place (ascending order, integer comparison). */
void dragon_list_sort(DragonList* list) {}

/** Reverse list in-place. */
void dragon_list_reverse(DragonList* list) {}

// --- 3.6 Printing ---

/** Print list of integers: [1, 2, 3] */
void dragon_print_list_int(DragonList* list) {}

/** Print list of strings: ['a', 'b', 'c'] */
void dragon_print_list_str(DragonList* list) {}

/** Print list of floats: [1.0, 2.5, 3.14] */
void dragon_print_list_float(DragonList* list) {}

/** Print list of bools: [True, False, True] */
void dragon_print_list_bool(DragonList* list) {}


// ============================================================================
// 4. DICTIONARY OPERATIONS
// ============================================================================

// --- 4.1 Creation ---

/**
 * Create a new empty dictionary backed by a hash table.
 * Allocates DragonDict with DragonObjectHeader (type_tag=TAG_DICT).
 * Keys are always strings. Values are i64 with per-entry type tags.
 * @param cap Initial hash table capacity
 * @return New DragonDict pointer (refcount=1)
 */
DragonDict* dragon_dict_new(int64_t cap) { return nullptr; }

// --- 4.2 Access ---

/**
 * Get value by string key. Raises KeyError if key not found.
 * @param d Target dict
 * @param key String key to look up
 * @return Value as i64
 */
int64_t dragon_dict_get(DragonDict* d, const char* key) { return 0; }

/**
 * Get value by key with default fallback. Returns def if key not found.
 * @param d Target dict
 * @param key String key
 * @param def Default value to return if key missing
 * @return Value or default as i64
 */
int64_t dragon_dict_get_default(DragonDict* d, const char* key, int64_t def) { return 0; }

/**
 * Get the DragonValueTag for a key's value. Used for polymorphic dispatch.
 * @param d Target dict
 * @param key String key
 * @return Tag value (TAG_INT=0, TAG_STR=1, TAG_FLOAT=2, etc.)
 */
int64_t dragon_dict_get_tag(DragonDict* d, const char* key) { return 0; }

/**
 * Check if key exists in dict.
 * @param d Target dict
 * @param key String key
 * @return 1 if present, 0 if not
 */
int64_t dragon_dict_has_key(DragonDict* d, const char* key) { return 0; }

/**
 * Get number of entries in dict.
 * @param d Target dict
 * @return Entry count
 */
int64_t dragon_dict_len(DragonDict* d) { return 0; }

// --- 4.3 Modification ---

/**
 * Set key-value pair with implicit TAG_INT tag.
 * If key already exists, decrefs old value and updates.
 * @param d Target dict
 * @param key String key (duped internally)
 * @param value Value as i64
 */
void dragon_dict_set(DragonDict* d, const char* key, int64_t value) {}

/**
 * Set key-value pair with explicit type tag for polymorphic storage.
 * @param d Target dict
 * @param key String key
 * @param value Value as i64
 * @param tag DragonValueTag for this entry
 */
void dragon_dict_set_tagged(DragonDict* d, const char* key, int64_t value, int64_t tag) {}

/**
 * Fused d[key] OP= operand for a str-keyed int dict, in one hash+probe.
 * KeyError (+exit) if absent. op: 0+= 1-= 2*= 3//= 4%= 5&= 6|= 7^= 8<<= 9>>=.
 */
int64_t dragon_dict_str_iaug_i64(DragonDict* d, const char* key, int64_t operand, int64_t op) { return 0; }

/**
 * Remove and return value for key. Raises KeyError if not found.
 * @param d Target dict
 * @param key String key to remove
 * @return Removed value as i64
 */
int64_t dragon_dict_pop(DragonDict* d, const char* key) { return 0; }

/**
 * Remove and return value for key, or return default if not found.
 * @param d Target dict
 * @param key String key
 * @param def Default value if key missing
 * @return Removed value or default
 */
int64_t dragon_dict_pop_default(DragonDict* d, const char* key, int64_t def) { return 0; }

/** Remove all entries from dict. Decrefs all heap values. */
void dragon_dict_clear(DragonDict* d) {}

/**
 * Merge all entries from other dict into d. Existing keys are overwritten.
 * @param d Target dict (modified in-place)
 * @param other Source dict
 */
void dragon_dict_update(DragonDict* d, DragonDict* other) {}

/**
 * Get value for key if it exists, otherwise set key to def and return def.
 * @param d Target dict
 * @param key String key
 * @param def Default value to set if key missing
 * @return Existing value or def
 */
int64_t dragon_dict_setdefault(DragonDict* d, const char* key, int64_t def) { return 0; }

// --- 4.4 Utilities ---

/**
 * Return a new list containing all keys in the dict.
 * @param d Target dict
 * @return DragonList of string pointers (as i64)
 */
DragonList* dragon_dict_keys(DragonDict* d) { return nullptr; }

/** Print dict in Python format: {'key': value, ...} with tag-aware formatting. */
void dragon_print_dict(DragonDict* d) {}


// ============================================================================
// 5. TUPLE OPERATIONS
// ============================================================================

/**
 * Create a new tuple of fixed size. Elements are initially unset.
 * Allocates DragonTuple with DragonObjectHeader and per-element type tags.
 * @param size Number of elements (fixed at creation)
 * @return New DragonTuple pointer (refcount=1)
 */
DragonTuple* dragon_tuple_new(int64_t size) { return nullptr; }

/**
 * Get element at index. Raises IndexError if out of bounds.
 * @param t Target tuple
 * @param index Element position (supports negative)
 * @return Element value as i64
 */
int64_t dragon_tuple_get(DragonTuple* t, int64_t index) { return 0; }

/**
 * Set element at index (used during tuple construction only).
 * @param t Target tuple
 * @param index Element position
 * @param val Value as i64
 */
void dragon_tuple_set(DragonTuple* t, int64_t index, int64_t val) {}

/**
 * Set element at index with explicit type tag.
 * @param t Target tuple
 * @param index Element position
 * @param val Value as i64
 * @param tag DragonValueTag for this element
 */
void dragon_tuple_set_tagged(DragonTuple* t, int64_t index, int64_t val, int64_t tag) {}

/**
 * Get tuple length.
 * @param t Target tuple
 * @return Number of elements
 */
int64_t dragon_tuple_len(DragonTuple* t) { return 0; }

/** Print tuple in Python format: (1, 'hello', 3.14) with tag-aware formatting. */
void dragon_print_tuple(DragonTuple* t) {}


// ============================================================================
// 6. SET OPERATIONS
// ============================================================================

/**
 * Create a new empty set backed by a hash table.
 * @param capacity Initial hash table capacity
 * @return New DragonSet pointer
 */
DragonSet* dragon_set_new(int64_t capacity) { return nullptr; }

/** Add element to set (no-op if already present). */
void dragon_set_add(DragonSet* s, int64_t val) {}

/** Remove element from set. Raises KeyError if not present. */
void dragon_set_remove(DragonSet* s, int64_t val) {}

/** Remove element from set if present (no error if absent). */
void dragon_set_discard(DragonSet* s, int64_t val) {}

/** Check if set contains element. Returns 1 or 0. */
int64_t dragon_set_contains(DragonSet* s, int64_t val) { return 0; }

/** Get number of elements in set. */
int64_t dragon_set_len(DragonSet* s) { return 0; }

/** Remove all elements from set. */
void dragon_set_clear(DragonSet* s) {}

/** Remove and return an arbitrary element. Raises KeyError if empty. */
int64_t dragon_set_pop(DragonSet* s) { return 0; }

/** Check if a is a subset of b (all elements of a are in b). */
int64_t dragon_set_issubset(DragonSet* a, DragonSet* b) { return 0; }

/** Check if a is a superset of b (all elements of b are in a). */
int64_t dragon_set_issuperset(DragonSet* a, DragonSet* b) { return 0; }

/** Check if sets have no elements in common. */
int64_t dragon_set_isdisjoint(DragonSet* a, DragonSet* b) { return 0; }

/** Add all elements from b into a (union update). */
void dragon_set_update(DragonSet* a, DragonSet* b) {}

/** Print set in Python format: {1, 2, 3} */
void dragon_print_set(DragonSet* s) {}


// ============================================================================
// 7. BYTES OPERATIONS
// ============================================================================

// --- 7.1 Creation & Conversion ---

/**
 * Create a new bytes object from raw data.
 * @param data Pointer to byte data
 * @param len Number of bytes
 * @return New DragonBytes pointer (refcount=1)
 */
DragonBytes* dragon_bytes_new(const uint8_t* data, int64_t len) { return nullptr; }

/**
 * Create bytes from a C string literal (interprets escape sequences).
 * @param data C string with potential escapes
 * @param len Length of data
 * @return New DragonBytes pointer
 */
DragonBytes* dragon_bytes_from_literal(const char* data, int64_t len) { return nullptr; }

/**
 * Encode string to bytes (UTF-8). Correct for all string kinds (kind=1 ASCII
 * and kind=4 UCS-4). Implements str.encode().
 * @param s Source string
 * @return New DragonBytes containing the string's UTF-8 byte representation
 */
DragonBytes* dragon_str_encode(const char* s) { return nullptr; }

/**
 * [STABLE FFI -- ADR 041] Convert a `str` to a NUL-terminated UTF-8 `bytes` for
 * handoff to C functions taking `const char*`. Correct for arbitrary text
 * (a raw `str` pointer is a valid C string only for ASCII content). The
 * returned `bytes` owns the buffer; dragon_bytes_data() on it yields a
 * '\0'-terminated pointer (the NUL is not counted by dragon_bytes_len). Keep
 * the `bytes` in scope across the C call.
 * @param s Source string
 * @return New DragonBytes (NUL-terminated UTF-8)
 */
DragonBytes* dragon_str_to_utf8_bytes(const char* s) { return nullptr; }

/**
 * Decode bytes to string (UTF-8). Raises UnicodeDecodeError on invalid bytes.
 * @param b Source bytes object
 * @return New heap-allocated string
 */
const char* dragon_bytes_decode(DragonBytes* b) { return nullptr; }

// --- 7.2 Access ---

/** Get number of bytes. */
int64_t dragon_bytes_len(DragonBytes* b) { return 0; }

/**
 * [STABLE FFI -- ADR 041] Raw byte buffer behind a `bytes`, for handoff to a C
 * library. Valid for the lifetime of the `bytes` object (keep it in scope
 * across the C call). Pair with dragon_bytes_len() for the length.
 * @param b Source bytes
 * @return Pointer to the underlying uint8_t[] (NULL if b is NULL)
 */
uint8_t* dragon_bytes_data(DragonBytes* b) { return nullptr; }

/**
 * Get byte at index as integer (0-255). Supports negative indexing.
 * @param b Source bytes
 * @param index Byte position
 * @return Byte value as i64 (0-255)
 */
int64_t dragon_bytes_get(DragonBytes* b, int64_t index) { return 0; }

/**
 * Slice bytes with start:stop:step (Python semantics).
 * @return New DragonBytes with sliced data
 */
DragonBytes* dragon_bytes_slice(DragonBytes* b, int64_t start, int64_t stop, int64_t step) { return nullptr; }

// --- 7.3 Operations ---

/** Concatenate two bytes objects into a new one. */
DragonBytes* dragon_bytes_concat(DragonBytes* a, DragonBytes* b) { return nullptr; }

/** Repeat bytes n times. */
DragonBytes* dragon_bytes_repeat(DragonBytes* b, int64_t n) { return nullptr; }

// --- 7.4 Comparison ---

/** Check bytes equality. Returns 1 or 0. */
int64_t dragon_bytes_eq(DragonBytes* a, DragonBytes* b) { return 0; }

/** Compare bytes lexicographically. Returns -1, 0, or 1. */
int64_t dragon_bytes_cmp(DragonBytes* a, DragonBytes* b) { return 0; }

// --- 7.5 Search ---

/** Check if bytes contains a specific byte value. */
int64_t dragon_bytes_contains(DragonBytes* b, int64_t byte_val) { return 0; }

/** Check if haystack contains needle bytes subsequence. */
int64_t dragon_bytes_contains_bytes(DragonBytes* haystack, DragonBytes* needle) { return 0; }

/** Find first occurrence of needle in haystack. Returns index or -1. */
int64_t dragon_bytes_find(DragonBytes* h, DragonBytes* n) { return 0; }

/** Find last occurrence of needle. Returns index or -1. */
int64_t dragon_bytes_rfind(DragonBytes* h, DragonBytes* n) { return 0; }

/** Find first occurrence of needle. Raises ValueError if not found. */
int64_t dragon_bytes_index_of(DragonBytes* h, DragonBytes* n) { return 0; }

/** Find last occurrence of needle. Raises ValueError if not found. */
int64_t dragon_bytes_rindex(DragonBytes* h, DragonBytes* n) { return 0; }

/** Count non-overlapping occurrences of needle in haystack. */
int64_t dragon_bytes_count(DragonBytes* haystack, DragonBytes* needle) { return 0; }

/** Check if bytes starts with prefix. */
int64_t dragon_bytes_startswith(DragonBytes* b, DragonBytes* prefix) { return 0; }

/** Check if bytes ends with suffix. */
int64_t dragon_bytes_endswith(DragonBytes* b, DragonBytes* suffix) { return 0; }

// --- 7.6 Classification ---

/** True if all bytes are ASCII digits (0x30-0x39). */
int64_t dragon_bytes_isdigit(DragonBytes* b) { return 0; }

/** True if all bytes are ASCII alpha (a-z, A-Z). */
int64_t dragon_bytes_isalpha(DragonBytes* b) { return 0; }

/** True if all bytes are ASCII alphanumeric. */
int64_t dragon_bytes_isalnum(DragonBytes* b) { return 0; }

/** True if all bytes are ASCII whitespace. */
int64_t dragon_bytes_isspace(DragonBytes* b) { return 0; }

// --- 7.7 Display ---

/** Convert bytes to hex string (e.g., "48656c6c6f"). */
const char* dragon_bytes_hex(DragonBytes* b) { return nullptr; }

/** Print bytes in Python b'...' format with escape sequences. */
void dragon_print_bytes(DragonBytes* b) {}


// ============================================================================
// 8. MATH & NUMERIC OPERATIONS
// ============================================================================

/** Absolute value of integer. */
int64_t dragon_abs_int(int64_t x) { return 0; }

/** Absolute value of float. */
double dragon_abs_float(double x) { return 0.0; }

/** Integer power (base^exp) via iterative multiplication. */
int64_t dragon_pow_int(int64_t base, int64_t exp) { return 0; }

/** Float power via C math library pow(). */
double dragon_pow_float(double base, double exp) { return 0.0; }

/**
 * Floor division with Python semantics (rounds toward negative infinity).
 * Raises ZeroDivisionError if b == 0.
 */
int64_t dragon_floordiv_int(int64_t a, int64_t b) { return 0; }

/**
 * Modulo with Python semantics (result has same sign as divisor).
 * Raises ZeroDivisionError if b == 0.
 */
int64_t dragon_mod_int(int64_t a, int64_t b) { return 0; }

/**
 * Return (quotient, remainder) tuple with Python semantics.
 * @return DragonTuple of size 2: (a // b, a % b)
 */
DragonTuple* dragon_divmod(int64_t a, int64_t b) { return nullptr; }

/**
 * Round float to nearest integer using banker's rounding (round half to even).
 * @param x Float value
 * @return Rounded integer
 */
int64_t dragon_round_int(double x) { return 0; }


// ============================================================================
// 9. AGGREGATE FUNCTIONS (min, max, sum, any, all)
// ============================================================================

/** Minimum of two integers. */
int64_t dragon_min_int(int64_t a, int64_t b) { return 0; }

/** Maximum of two integers. */
int64_t dragon_max_int(int64_t a, int64_t b) { return 0; }

/** Minimum of two floats. */
double dragon_min_float(double a, double b) { return 0.0; }

/** Maximum of two floats. */
double dragon_max_float(double a, double b) { return 0.0; }

/** Minimum element in a list of integers. Raises ValueError if empty. */
int64_t dragon_min_list(DragonList* list) { return 0; }

/** Maximum element in a list of integers. Raises ValueError if empty. */
int64_t dragon_max_list(DragonList* list) { return 0; }

/** Sum of all elements in a list of integers. */
int64_t dragon_sum_list(DragonList* list) { return 0; }

/** True (1) if any element in the list is truthy, else 0. */
int64_t dragon_any_list(DragonList* list) { return 0; }

/** True (1) if all elements in the list are truthy, else 0. */
int64_t dragon_all_list(DragonList* list) { return 0; }


// ============================================================================
// 10. BUILTINS: hash, id, ord, chr
// ============================================================================

/** Hash integer value (identity hash — returns the value itself). */
int64_t dragon_hash_int(int64_t x) { return 0; }

/** Hash string using djb2 algorithm. */
int64_t dragon_hash_str(const char* s) { return 0; }

/** Return the identity (memory address) of a value. */
int64_t dragon_id(int64_t val) { return 0; }

/**
 * Get the Unicode code point of the first character.
 * Raises TypeError if string length != 1.
 * @param s Single-character string
 * @return Unicode code point as integer
 */
int64_t dragon_ord(const char* s) { return 0; }

/**
 * Convert Unicode code point to a single-character string.
 * @param code Unicode code point (0-0x10FFFF)
 * @return New single-char heap string
 */
const char* dragon_chr(int64_t code) { return nullptr; }


// ============================================================================
// 11. REPR & FORMAT FUNCTIONS
// ============================================================================

/** String representation of integer (e.g., "42"). */
const char* dragon_repr_int(int64_t x) { return nullptr; }

/** String representation with quotes (e.g., "'hello'"). */
const char* dragon_repr_str(const char* s) { return nullptr; }

/** String representation of float (e.g., "3.14"). */
const char* dragon_repr_float(double x) { return nullptr; }

/** String representation of bool ("True" or "False"). */
const char* dragon_repr_bool(int64_t x) { return nullptr; }

/** Convert integer to hex string with 0x prefix (e.g., "0x2a"). */
const char* dragon_hex(int64_t x) { return nullptr; }

/** Convert integer to octal string with 0o prefix (e.g., "0o52"). */
const char* dragon_oct(int64_t x) { return nullptr; }

/** Convert integer to binary string with 0b prefix (e.g., "0b101010"). */
const char* dragon_bin(int64_t x) { return nullptr; }


// ============================================================================
// 12. BUILTIN CONTAINER FUNCTIONS
// ============================================================================

/**
 * Create a list of integers in range [start, stop) with step.
 * Python semantics: empty list if step goes wrong direction.
 * @param start Start value (inclusive)
 * @param stop Stop value (exclusive)
 * @param step Step increment (nonzero, can be negative)
 * @return New DragonList of integers
 */
DragonList* dragon_range(int64_t start, int64_t stop, int64_t step) { return nullptr; }

/**
 * Create list of (index, value) tuples from a list.
 * @param list Source list
 * @return New DragonList of DragonTuples: [(0, elem0), (1, elem1), ...]
 */
DragonList* dragon_enumerate(DragonList* list) { return nullptr; }

/**
 * Zip two lists into a list of 2-tuples.
 * Length is min(len(a), len(b)).
 * @param a First list
 * @param b Second list
 * @return New DragonList of DragonTuples: [(a[0],b[0]), (a[1],b[1]), ...]
 */
DragonList* dragon_zip(DragonList* a, DragonList* b) { return nullptr; }

/**
 * Return a new sorted copy of the list (ascending).
 * @param list Source list (not modified)
 * @return New sorted DragonList
 */
DragonList* dragon_sorted(DragonList* list) { return nullptr; }

/**
 * Return a new reversed copy of the list.
 * @param list Source list (not modified)
 * @return New reversed DragonList
 */
DragonList* dragon_reversed(DragonList* list) { return nullptr; }


// ============================================================================
// 13. I/O & PRINTING
// ============================================================================

/** Print integer with newline (e.g., "42\n"). */
void dragon_print_int(int64_t value) {}

/** Print float with newline, stripping trailing zeros (e.g., "3.14\n"). */
void dragon_print_float(double value) {}

/** Print string with newline. Prints "None" if NULL. */
void dragon_print_str(const char* s) {}

/** Print "True\n" or "False\n". */
void dragon_print_bool(int64_t value) {}

/** Print "None\n". */
void dragon_print_none() {}

/** Print blank line ("\n"). */
void dragon_print_newline() {}

/**
 * Print value using dynamic type dispatch based on tag.
 * Used for polymorphic print (e.g., dict values, any-typed variables).
 * @param value Value as i64
 * @param tag DragonValueTag indicating the type
 */
void dragon_print_tagged(int64_t value, int64_t tag) {}

/**
 * Read a line from stdin with optional prompt (Python input() builtin).
 * @param prompt Prompt string to display (NULL for no prompt)
 * @return Heap-allocated string (without trailing newline)
 */
const char* dragon_input(const char* prompt) { return nullptr; }


// ============================================================================
// 14. EXCEPTION HANDLING (setjmp/longjmp)
// ============================================================================

/**
 * Push a new exception catch frame onto the per-thread exception stack.
 * Returns a pointer to the jmp_buf for use with setjmp().
 * Pattern: buf = dragon_exc_push_frame(); if (setjmp(buf) == 0) { try } else { catch }
 * @return Pointer to jmp_buf (cast to void*)
 */
void* dragon_exc_push_frame() { return nullptr; }

/** Pop the top exception catch frame. Called at end of try/except block. */
void dragon_exc_pop_frame() {}

/** Get the exception type code for the current in-flight exception. */
int64_t dragon_exc_get_type() { return 0; }

/** Get the exception message string for the current in-flight exception. */
const char* dragon_exc_get_msg() { return nullptr; }

/**
 * Raise an exception by name and exit (used for top-level uncaught errors).
 * @param type Exception type name (e.g., "ValueError")
 * @param message Error message
 */
void dragon_raise(const char* type, const char* message) {}

/**
 * Raise an exception with hierarchical type code via longjmp.
 * Jumps to the nearest enclosing catch frame.
 * @param type Exception code (e.g., 5=ValueError, 1000+=user-defined)
 * @param msg Error message string
 */
void dragon_raise_exc(int64_t type, const char* msg) {}

/**
 * Register a user-defined exception in the hierarchy.
 * Enables parent-type catch matching (e.g., catch Exception catches ValueError).
 * @param code New exception's code (>= 1000 for user-defined)
 * @param parent_code Parent exception's code
 */
void dragon_exc_register(int64_t code, int64_t parent_code) {}

/**
 * Check if a raised exception matches a caught exception type.
 * Uses [lo, hi] range matching for hierarchical exception types.
 * @param raised Code of the raised exception
 * @param caught Code of the exception type in the catch clause
 * @return 1 if matches (raised is caught or a subtype), 0 otherwise
 */
int64_t dragon_exc_matches(int64_t raised, int64_t caught) { return 0; }

/**
 * Assert with message. Raises AssertionError if condition is false.
 * @param condition Boolean condition as i64
 * @param message Error message if assertion fails
 */
void dragon_assert(int64_t condition, const char* message) {}

/** Assert without message. Raises AssertionError if condition is false. */
void dragon_assert_no_msg(int64_t condition) {}


// ============================================================================
// 15. FILE I/O
// ============================================================================

/**
 * Open a file. Raises FileNotFoundError on failure.
 * @param filename File path
 * @param mode Open mode ("r", "w", "a", "rb", "wb", etc.)
 * @return FILE* handle as void*
 */
void* dragon_file_open(const char* filename, const char* mode) { return nullptr; }

/** Close a file handle. */
void dragon_file_close(void* handle) {}

/**
 * Read entire file contents as a string.
 * @param handle FILE* handle
 * @return Heap-allocated string with file contents
 */
const char* dragon_file_read(void* handle) { return nullptr; }

/**
 * Read next line from file (including newline character).
 * @param handle FILE* handle
 * @return Heap-allocated string, or empty string at EOF
 */
const char* dragon_file_readline(void* handle) { return nullptr; }

/**
 * Read all lines as a list of strings.
 * @param handle FILE* handle
 * @return DragonList of string pointers
 */
DragonList* dragon_file_readlines(void* handle) { return nullptr; }

/**
 * Write string to file.
 * @param handle FILE* handle
 * @param data String to write
 */
void dragon_file_write(void* handle, const char* data) {}


// ============================================================================
// 16. THREADING & CONCURRENCY
// ============================================================================

// --- 16.1 OS Threads ---

/**
 * Start an OS thread. The thread begins executing its function.
 * @param handle Thread handle (DragonThread*)
 * @return 0 on success, nonzero on failure
 */
int64_t dragon_osthread_start(void* handle) { return 0; }

/**
 * Wait for an OS thread to complete (blocking join).
 * @param handle Thread handle
 * @return Thread's return value as i64
 */
int64_t dragon_osthread_join(void* handle) { return 0; }

/**
 * Check if an OS thread is still running.
 * @param handle Thread handle
 * @return 1 if alive, 0 if completed
 */
int64_t dragon_osthread_is_alive(void* handle) { return 0; }

// --- 16.2 Green Threads (Virtual Threads / minicoro) ---

/**
 * Wait for a green thread (virtual thread) to complete.
 * If called from a green thread context, yields while waiting.
 * @param vt Virtual thread handle
 * @return Thread's result value as i64
 */
int64_t dragon_vthread_join(DragonVThread* vt) { return 0; }

/**
 * Check if a green thread is still alive.
 * @param vt Virtual thread handle
 * @return 1 if still running, 0 if completed
 */
int64_t dragon_vthread_is_alive(DragonVThread* vt) { return 0; }

/**
 * Sleep (yield) for the given number of milliseconds.
 * In green thread context, yields to scheduler. In OS thread, calls nanosleep.
 * Integrates with libuv event loop for I/O-aware scheduling.
 * @param ms Milliseconds to sleep
 */
void dragon_vthread_sleep(int64_t ms) {}

/** Yield execution to the green thread scheduler (cooperative yield). */
void dragon_vthread_yield() {}

// --- 16.3 Legacy Thread API ---

/** Check if a legacy thread has completed. Returns 1 if done, 0 if running. */
int64_t dragon_thread_is_done(DragonThread* t) { return 0; }

/** Join a legacy thread and return its result. */
int64_t dragon_thread_join(DragonThread* t) { return 0; }

// --- 16.4 Synchronization Primitives ---

/** Acquire a mutex lock (blocking). */
void dragon_lock_acquire(void* lock) {}

/**
 * Try to acquire a mutex lock (non-blocking).
 * @param lock Mutex handle
 * @return 1 if acquired, 0 if already held by another thread
 */
int64_t dragon_lock_try_acquire(void* lock) { return 0; }

/** Release a mutex lock. */
void dragon_lock_release(void* lock) {}

/** Destroy a mutex lock and free resources. */
void dragon_lock_destroy(void* lock) {}


// ============================================================================
// 17. SYNCHRONIZED CONTAINERS (Thread-Safe)
// ============================================================================

// --- 17.1 SyncList ---

/** Append element to thread-safe list (acquires internal lock). */
void dragon_synclist_append(DragonSyncList* sl, int64_t val) {}

/** Get element at index from thread-safe list (locked). */
int64_t dragon_synclist_get(DragonSyncList* sl, int64_t idx) { return 0; }

/** Set element at index in thread-safe list (locked). */
void dragon_synclist_set(DragonSyncList* sl, int64_t idx, int64_t val) {}

/** Pop element at index from thread-safe list (locked). */
int64_t dragon_synclist_pop(DragonSyncList* sl, int64_t idx) { return 0; }

/** Get length of thread-safe list (locked). */
int64_t dragon_synclist_len(DragonSyncList* sl) { return 0; }

/** Clear all elements from thread-safe list (locked). */
void dragon_synclist_clear(DragonSyncList* sl) {}

/** Extend thread-safe list with elements from a regular list (locked). */
void dragon_synclist_extend(DragonSyncList* sl, DragonList* other) {}

/** Remove first occurrence of value from thread-safe list (locked). */
void dragon_synclist_remove(DragonSyncList* sl, int64_t val) {}

/** Insert element at index in thread-safe list (locked). */
void dragon_synclist_insert(DragonSyncList* sl, int64_t idx, int64_t val) {}

/** Find index of value in thread-safe list (locked). */
int64_t dragon_synclist_index(DragonSyncList* sl, int64_t val) { return 0; }

/** Count occurrences of value in thread-safe list (locked). */
int64_t dragon_synclist_count(DragonSyncList* sl, int64_t val) { return 0; }

/** Sort thread-safe list in-place (locked). */
void dragon_synclist_sort(DragonSyncList* sl) {}

/** Reverse thread-safe list in-place (locked). */
void dragon_synclist_reverse(DragonSyncList* sl) {}

/** Destroy and deallocate thread-safe list (locked). */
void dragon_synclist_destroy(DragonSyncList* sl) {}

// --- 17.2 SyncDict ---

/** Set key-value pair in thread-safe dict (locked). */
void dragon_syncdict_set(DragonSyncDict* sd, const char* key, int64_t val) {}

/** Get value by key from thread-safe dict (locked). Raises KeyError if missing. */
int64_t dragon_syncdict_get(DragonSyncDict* sd, const char* key) { return 0; }

/** Get value with default from thread-safe dict (locked). */
int64_t dragon_syncdict_get_default(DragonSyncDict* sd, const char* key, int64_t def) { return 0; }

/** Get entry count of thread-safe dict (locked). */
int64_t dragon_syncdict_len(DragonSyncDict* sd) { return 0; }

/** Check if key exists in thread-safe dict (locked). */
int64_t dragon_syncdict_has_key(DragonSyncDict* sd, const char* key) { return 0; }

/** Remove and return value for key from thread-safe dict (locked). */
int64_t dragon_syncdict_pop(DragonSyncDict* sd, const char* key) { return 0; }

/** Remove and return value with default from thread-safe dict (locked). */
int64_t dragon_syncdict_pop_default(DragonSyncDict* sd, const char* key, int64_t def) { return 0; }

/** Clear all entries from thread-safe dict (locked). */
void dragon_syncdict_clear(DragonSyncDict* sd) {}

/** Merge regular dict into thread-safe dict (locked). */
void dragon_syncdict_update(DragonSyncDict* sd, DragonDict* other) {}

/** Get or set default value in thread-safe dict (locked). */
int64_t dragon_syncdict_setdefault(DragonSyncDict* sd, const char* key, int64_t def) { return 0; }

/** Destroy and deallocate thread-safe dict (locked). */
void dragon_syncdict_destroy(DragonSyncDict* sd) {}


// ============================================================================
// 18. NETWORKING & SOCKETS
// ============================================================================

/** Get the size of struct sockaddr_in (platform-dependent). */
int64_t dragon_sockaddr_in_size() { return 0; }

/**
 * Write a 32-bit value at a byte offset from a pointer.
 * Used for constructing sockaddr_in fields.
 * @param p Base pointer
 * @param offset Byte offset from p
 * @param val 32-bit value to write
 */
void dragon_ptr_write_i32(void* p, int64_t offset, int64_t val) {}

/**
 * Receive from socket and return as a Dragon string.
 * @param fd Socket file descriptor
 * @param buf Receive buffer
 * @param length Max bytes to receive
 * @param flags recv() flags
 * @return Heap-allocated string with received data
 */
const char* dragon_recv_to_str(int64_t fd, void* buf, int64_t length, int64_t flags) { return nullptr; }

/**
 * Send UDP datagram.
 * @param fd Socket file descriptor
 * @param buf Data to send
 * @param len Data length
 * @return Number of bytes sent or -1 on error
 */
int64_t dragon_udp_sendto(int64_t fd, const char* buf, int64_t len /*, addr params */) { return 0; }


// ============================================================================
// 19. CRYPTOGRAPHY & HASHING
// ============================================================================

/**
 * Compute SHA-256 hash of data.
 * @param data Input data as string
 * @param len Length of data in bytes
 * @return Hex-encoded hash string (64 chars)
 */
const char* dragon_sha256(const char* data, int64_t len) { return nullptr; }

/**
 * Compute SHA-1 hash of data.
 * @param data Input data as string
 * @param len Length of data in bytes
 * @return Hex-encoded hash string (40 chars)
 */
const char* dragon_sha1(const char* data, int64_t len) { return nullptr; }

/**
 * Compute MD5 hash of data.
 * @param data Input data as string
 * @param len Length of data in bytes
 * @return Hex-encoded hash string (32 chars)
 */
const char* dragon_md5(const char* data, int64_t len) { return nullptr; }

/**
 * Convert raw byte buffer to hex string.
 * @param data Raw byte data
 * @param len Number of bytes
 * @return Heap-allocated hex string
 */
const char* dragon_raw_bytes_hex(const void* data, int64_t len) { return nullptr; }


// ============================================================================
// 20. REGULAR EXPRESSIONS (PCRE2)
// ============================================================================

/**
 * Test if a compiled PCRE2 pattern matches the subject string.
 * @param compiled Compiled PCRE2 regex (pcre2_code*)
 * @param subject String to test
 * @return 1 if matches, 0 if not
 */
int64_t dragon_re_match(void* compiled, const char* subject) { return 0; }

/**
 * Search for the first match of compiled pattern in subject.
 * @param compiled Compiled PCRE2 regex
 * @param subject String to search
 * @return Matched substring as new string, or NULL if no match
 */
const char* dragon_re_search(void* compiled, const char* subject) { return nullptr; }

/**
 * Get a capture group from the last match.
 * @param compiled Compiled PCRE2 regex
 * @param subject Original subject string
 * @param index Capture group index (0 = full match)
 * @return Captured substring, or NULL if group didn't participate
 */
const char* dragon_re_group(void* compiled, const char* subject, int64_t index) { return nullptr; }

/** Free a compiled PCRE2 regex object. */
void dragon_re_free(void* compiled) {}

/**
 * Match using a string pattern (compiles on-the-fly).
 * @param pattern Regex pattern string
 * @param subject String to test
 * @return 1 if matches, 0 if not
 */
int64_t dragon_re_match_str(const char* pattern, const char* subject) { return 0; }

/**
 * Search using a string pattern (compiles on-the-fly).
 * @param pattern Regex pattern string
 * @param subject String to search
 * @return First matched substring, or NULL
 */
const char* dragon_re_search_str(const char* pattern, const char* subject) { return nullptr; }

/**
 * Extract a match from an ovector (PCRE2 match data).
 * @param subject Original subject string
 * @param ovector PCRE2 ovector array (pairs of start/end offsets)
 * @param index Match index
 * @return Matched substring
 */
const char* dragon_re_get_match(const char* subject, int64_t* ovector, int64_t index) { return nullptr; }

/**
 * Get an offset value from a PCRE2 ovector.
 * @param ovector PCRE2 ovector array
 * @param index Offset index
 * @return Offset value
 */
int64_t dragon_re_ovector_get(int64_t* ovector, int64_t index) { return 0; }


// ============================================================================
// 21. FILESYSTEM & PATH OPERATIONS
// ============================================================================

/**
 * Get file size in bytes via stat().
 * @param path File path
 * @return Size in bytes, or -1 on error
 */
int64_t dragon_stat_size(const char* path) { return 0; }

/** Get file last modification time as Unix timestamp. */
int64_t dragon_stat_mtime(const char* path) { return 0; }

/** Get file last access time as Unix timestamp. */
int64_t dragon_stat_atime(const char* path) { return 0; }

/** Get file status change time as Unix timestamp. */
int64_t dragon_stat_ctime(const char* path) { return 0; }

/** Get file mode (permissions + type bits). */
int64_t dragon_stat_mode(const char* path) { return 0; }

/**
 * Read the next directory entry name from an opendir() handle.
 * @param dirp DIR* handle from opendir()
 * @return Entry name string, or NULL at end
 */
const char* dragon_readdir_name(void* dirp) { return nullptr; }

/**
 * Read symbolic link target.
 * @param path Symlink path
 * @return Target path as heap string
 */
const char* dragon_readlink(const char* path) { return nullptr; }


// ============================================================================
// 22. SYSTEM INFORMATION
// ============================================================================

/** Get OS/kernel name (e.g., "Linux", "Darwin"). */
const char* dragon_uname_sysname() { return nullptr; }

/** Get network hostname. */
const char* dragon_uname_nodename() { return nullptr; }

/** Get OS release version (e.g., "6.1.0"). */
const char* dragon_uname_release() { return nullptr; }

/** Get OS version string (e.g., "#1 SMP PREEMPT_DYNAMIC ..."). */
const char* dragon_uname_version() { return nullptr; }

/** Get machine architecture (e.g., "x86_64", "aarch64"). */
const char* dragon_uname_machine() { return nullptr; }

/** Get current login username. */
const char* dragon_getlogin() { return nullptr; }


// ============================================================================
// 23. TIME & TIMESPEC
// ============================================================================

/**
 * Extract seconds field from a struct timespec.
 * @param ts Pointer to struct timespec
 * @return Seconds component
 */
int64_t dragon_timespec_sec(void* ts) { return 0; }

/**
 * Extract nanoseconds field from a struct timespec.
 * @param ts Pointer to struct timespec
 * @return Nanoseconds component
 */
int64_t dragon_timespec_nsec(void* ts) { return 0; }


// ============================================================================
// 24. ATOMIC OPERATIONS
// ============================================================================

/**
 * Atomically load a 64-bit value (__ATOMIC_SEQ_CST).
 * @param p Pointer to int64_t
 * @return Current value
 */
int64_t dragon_atomic_load(int64_t* p) { return 0; }

/**
 * Atomically store a 64-bit value (__ATOMIC_SEQ_CST).
 * @param p Pointer to int64_t
 * @param val Value to store
 */
void dragon_atomic_store(int64_t* p, int64_t val) {}

/**
 * Atomically add to a 64-bit value and return the new value.
 * @param p Pointer to int64_t
 * @param val Value to add
 * @return New value after addition
 */
int64_t dragon_atomic_add(int64_t* p, int64_t val) { return 0; }


// ============================================================================
// 25. TEMPLATE STRING ESCAPING
// ============================================================================

/**
 * Escape HTML special characters (&, <, >, ", ').
 * @param s Input string
 * @return Escaped string safe for HTML insertion
 */
const char* dragon_template_escape_html(const char* s) { return nullptr; }

/**
 * Escape SQL special characters (single quotes -> '').
 * @param s Input string
 * @return Escaped string safe for SQL literals
 */
const char* dragon_template_escape_sql(const char* s) { return nullptr; }

/**
 * URL-encode a string (percent-encoding for non-unreserved chars).
 * @param s Input string
 * @return URL-encoded string
 */
const char* dragon_template_escape_url(const char* s) { return nullptr; }


// ============================================================================
// 26. GENERATORS (minicoro-based coroutines)
// ============================================================================

/**
 * Yield a value from within a generator function body.
 * Suspends the minicoro coroutine and passes the value to the caller.
 * @param gen_ptr Pointer to DragonGenerator
 * @param value Value to yield (as i64)
 */
void dragon_generator_yield(void* gen_ptr, int64_t value) {}

/**
 * Resume generator and get the next yielded value.
 * Raises StopIteration (code 11) when generator is exhausted.
 * @param gen_ptr Pointer to DragonGenerator
 * @return Next yielded value as i64
 */
int64_t dragon_generator_next(void* gen_ptr) { return 0; }

/**
 * Destroy generator and free its minicoro coroutine resources.
 * @param gen_ptr Pointer to DragonGenerator
 */
void dragon_generator_destroy(void* gen_ptr) {}


} // extern "C"
