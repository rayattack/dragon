/// Dragon Runtime - Builtins, Print, Math, File I/O, Generators, Class Descriptors
#include "runtime_internal.h"

extern "C" {

//===----------------------------------------------------------------------===//
// I/O Functions
//===----------------------------------------------------------------------===//

// Each printer splits into a `_raw` core (emits the value, NO trailing
// newline) and a public wrapper (`_raw` + '\n'). Single-arg print uses the
// wrappers' behavior; multi-arg print calls the `_raw` cores with spaces
// between and one trailing newline (Python: sep=' ', end='\n'). Splitting this
// way keeps single- and multi-arg formatting byte-identical.

/// Print integer (no newline)
void dragon_print_int_raw(int64_t value) {
    printf("%ld", value);
}
void dragon_print_int(int64_t value) {
    dragon_print_int_raw(value);
    putchar('\n');
}

/// Print float (no newline)
void dragon_print_float_raw(double value) {
    char tmp[64];
    dragon_format_double_into(value, tmp, sizeof(tmp));
    fputs(tmp, stdout);
}
void dragon_print_float(double value) {
    dragon_print_float_raw(value);
    putchar('\n');
}

/// Print string (no newline). Handles kind=4 (UCS-4) DragonStrings by
/// encoding each code point to UTF-8 on the way to stdout.
void dragon_print_str_raw(const char* s) {
    if (!s) { printf("None"); return; }
    int64_t byte_len = 0;
    char* enc = dragon_str_to_utf8_alloc(s, &byte_len);
    if (enc) {
        fwrite(enc, 1, (size_t)byte_len, stdout);
        free(enc);
    } else {
        // kind=1 or literal: data is valid UTF-8 / NUL-terminated.
        fwrite(s, 1, (size_t)byte_len, stdout);
    }
}
void dragon_print_str(const char* s) {
    dragon_print_str_raw(s);
    putchar('\n');
}

/// Print bool (no newline)
void dragon_print_bool_raw(int64_t value) {
    printf("%s", value ? "True" : "False");
}
void dragon_print_bool(int64_t value) {
    dragon_print_bool_raw(value);
    putchar('\n');
}

/// Print None (no newline)
void dragon_print_none_raw() {
    printf("None");
}
void dragon_print_none() {
    dragon_print_none_raw();
    putchar('\n');
}

/// Print a newline (empty print())
void dragon_print_newline() {
    printf("\n");
}

/// Print a single space - the default arg separator for multi-arg print().
void dragon_print_space() {
    putchar(' ');
}

/// Input function (maps to Python's input). The scratch buffer is on the
/// stack (was `static` - concurrent fire/thread callers raced on it), and
/// dragon_string_alloc copies into a refcount-managed heap string before
/// the buffer goes out of scope.
const char* dragon_input(const char* prompt) {
    if (prompt) printf("%s", prompt);
    char buffer[4096];
    if (fgets(buffer, sizeof(buffer), stdin)) {
        // Strip trailing newline
        size_t len = strlen(buffer);
        if (len > 0 && buffer[len - 1] == '\n') { buffer[--len] = '\0'; }
        return dragon_string_alloc(buffer, (int64_t)len);
    }
    return dragon_string_alloc("", 0);
}

//===----------------------------------------------------------------------===//
// Math / Builtins
//===----------------------------------------------------------------------===//

/// Absolute value
int64_t dragon_abs_int(int64_t x) {
    return x < 0 ? -x : x;
}

double dragon_abs_float(double x) {
    return x < 0 ? -x : x;
}

/// Power
int64_t dragon_pow_int(int64_t base, int64_t exp) {
    int64_t result = 1;
    for (int64_t i = 0; i < exp; i++) result *= base;
    return result;
}

/// Overflow-checked power. Multiplies via __builtin_smull_overflow each step;
/// raises OverflowError (parent code 22) on the first overflowing step.
/// Negative exponents return 0 for `|base| > 1` and 1 for `|base| <= 1` to
/// preserve int semantics under --check-overflow (Python returns float here,
/// but Dragon's typed-int arithmetic doesn't promote to float yet).
int64_t dragon_pow_int_checked(int64_t base, int64_t exp) {
    if (exp < 0) {
        if (base == 1 || base == -1) return 1;
        if (base == 0) {
            dragon_raise_exc_cstr(23, "ZeroDivisionError: 0 cannot be raised to a negative power");
            return 0;
        }
        return 0;
    }
    int64_t result = 1;
    for (int64_t i = 0; i < exp; ++i) {
        long long out;
        if (__builtin_smulll_overflow((long long)result, (long long)base, &out)) {
            dragon_raise_exc_cstr(22, "OverflowError: integer exponentiation overflowed");
            return 0;
        }
        result = (int64_t)out;
    }
    return result;
}

/// Floor division
int64_t dragon_floordiv_int(int64_t a, int64_t b) {
    if (b == 0) {
        dragon_raise_exc_cstr(23, "ZeroDivisionError: integer division by zero");
        return 0;
    }
    int64_t d = a / b;
    // Adjust for Python-style floor division
    if ((a ^ b) < 0 && d * b != a) d--;
    return d;
}

/// Modulo (Python semantics)
int64_t dragon_mod_int(int64_t a, int64_t b) {
    if (b == 0) {
        dragon_raise_exc_cstr(23, "ZeroDivisionError: integer modulo by zero");
        return 0;
    }
    int64_t r = a % b;
    if ((r != 0) && ((r ^ b) < 0)) r += b;
    return r;
}


//===----------------------------------------------------------------------===//
// Phase G: Builtin Functions
//===----------------------------------------------------------------------===//

// G.1: Aggregate functions

int64_t dragon_min_int(int64_t a, int64_t b) {
    return a < b ? a : b;
}

int64_t dragon_max_int(int64_t a, int64_t b) {
    return a > b ? a : b;
}

double dragon_min_float(double a, double b) {
    return a < b ? a : b;
}

double dragon_max_float(double a, double b) {
    return a > b ? a : b;
}

int64_t dragon_min_list(DragonList* list) {
    if (!list || list->size == 0) return 0;
    int64_t result = dragon_list_load(list, 0);
    for (int64_t i = 1; i < list->size; i++) {
        int64_t v = dragon_list_load(list, i);
        if (v < result) result = v;
    }
    return result;
}

int64_t dragon_max_list(DragonList* list) {
    if (!list || list->size == 0) return 0;
    int64_t result = dragon_list_load(list, 0);
    for (int64_t i = 1; i < list->size; i++) {
        int64_t v = dragon_list_load(list, i);
        if (v > result) result = v;
    }
    return result;
}

int64_t dragon_sum_list(DragonList* list) {
    if (!list) return 0;
    int64_t result = 0;
    for (int64_t i = 0; i < list->size; i++) {
        result += dragon_list_load(list, i);
    }
    return result;
}

int64_t dragon_any_list(DragonList* list) {
    if (!list) return 0;
    for (int64_t i = 0; i < list->size; i++) {
        if (dragon_list_load(list, i) != 0) return 1;
    }
    return 0;
}

int64_t dragon_all_list(DragonList* list) {
    if (!list) return 1;
    for (int64_t i = 0; i < list->size; i++) {
        if (dragon_list_load(list, i) == 0) return 0;
    }
    return 1;
}

// G.2: Iteration helpers

DragonList* dragon_enumerate(DragonList* list, int64_t start) {
    // Returns a list of tuples (index, element). Each element is incref'd
    // and stored with its tag so the tuple co-owns it: when the source list
    // is destroyed, the enumerate result still holds valid references.
    if (!list) return dragon_list_new(0);
    DragonList* result = dragon_list_new_tagged(list->size, TAG_LIST);
    int64_t etag = list->elem_tag;
    for (int64_t i = 0; i < list->size; i++) {
        DragonTuple* t = dragon_tuple_new(2);
        // Slot 0: index (always TAG_INT, plain set is correct).
        dragon_tuple_set(t, 0, start + i);
        // Slot 1: element. Incref + tagged set so destroy decrefs.
        int64_t v = dragon_list_load(list, i);
        dragon_incref_tagged(v, (uint8_t)etag);
        dragon_tuple_set_tagged(t, 1, v, etag);
        dragon_list_append(result, (int64_t)(intptr_t)t);
    }
    return result;
}

DragonList* dragon_zip(DragonList* a, DragonList* b) {
    // Both inputs may carry different elem_tags; each tuple slot is tagged
    // with the corresponding source's tag and incref'd before storage so
    // the result outlives either source list independently.
    if (!a || !b) return dragon_list_new(0);
    int64_t minLen = a->size < b->size ? a->size : b->size;
    DragonList* result = dragon_list_new_tagged(minLen, TAG_LIST);
    int64_t atag = a->elem_tag;
    int64_t btag = b->elem_tag;
    for (int64_t i = 0; i < minLen; i++) {
        DragonTuple* t = dragon_tuple_new(2);
        int64_t va = dragon_list_load(a, i);
        int64_t vb = dragon_list_load(b, i);
        dragon_incref_tagged(va, (uint8_t)atag);
        dragon_tuple_set_tagged(t, 0, va, atag);
        dragon_incref_tagged(vb, (uint8_t)btag);
        dragon_tuple_set_tagged(t, 1, vb, btag);
        dragon_list_append(result, (int64_t)(intptr_t)t);
    }
    return result;
}

DragonList* dragon_sorted(DragonList* list) {
    if (!list) return dragon_list_new(0);
    DragonList* result = dragon_list_copy(list);
    dragon_list_sort(result);
    return result;
}

/// `sorted(xs, reverse=...)` - like dragon_sorted but honors the descending
/// flag. A fresh copy is sorted in the requested direction; the input is left
/// untouched (Python's sorted() never mutates its argument).
DragonList* dragon_sorted_ex(DragonList* list, int64_t reverse) {
    if (!list) return dragon_list_new(0);
    DragonList* result = dragon_list_copy(list);
    dragon_list_sort_ex(result, reverse);
    return result;
}

DragonList* dragon_reversed(DragonList* list) {
    if (!list) return dragon_list_new(0);
    DragonList* result = dragon_list_new_tagged(list->size, list->elem_tag);
    for (int64_t i = list->size - 1; i >= 0; i--) {
        int64_t v = dragon_list_load(list, i);
        dragon_incref_tagged(v, list->elem_tag);
        dragon_list_append(result, v);
    }
    return result;
}

// G.3: Type introspection - basic

int64_t dragon_hash_int(int64_t x) {
    // Simple hash: the value itself (Python does this for ints)
    return x;
}

int64_t dragon_hash_str(const char* s) {
    if (!s) return 0;
    // djb2 hash
    int64_t hash = 5381;
    while (*s) {
        hash = ((hash << 5) + hash) + (unsigned char)(*s);
        s++;
    }
    return hash;
}

int64_t dragon_id(int64_t val) {
    // For pointers, return the address; for values, return the value itself
    return val;
}

// G.4: Numeric functions

int64_t dragon_ord(const char* s) {
    if (!s || !*s) return 0;
    return (int64_t)(unsigned char)s[0];
}

const char* dragon_chr(int64_t code) {
    // Python parity: chr() accepts U+0000..U+10FFFF, else ValueError.
    if (code < 0 || code > 0x10FFFF) {
        dragon_raise_exc_cstr(90, "ValueError: chr() arg not in range(0x110000)");
    }
    // UTF-8-encode the code point into 1-4 bytes, then hand it to
    // dragon_string_alloc, which decodes UTF-8 back into the proper PEP-393
    // representation (kind=1 for ASCII, kind=4 otherwise) with a code-point
    // length of 1. The old `(char)code` truncated every code point to a single
    // byte - astral-plane points (> U+FFFF) became NUL, and anything >= 256 was
    // mangled. Encoding through the UTF-8-aware allocator fixes the full range.
    unsigned char buf[4];
    int n;
    uint32_t c = (uint32_t)code;
    if (c < 0x80) {
        buf[0] = (unsigned char)c;
        n = 1;
    } else if (c < 0x800) {
        buf[0] = (unsigned char)(0xC0 | (c >> 6));
        buf[1] = (unsigned char)(0x80 | (c & 0x3F));
        n = 2;
    } else if (c < 0x10000) {
        buf[0] = (unsigned char)(0xE0 | (c >> 12));
        buf[1] = (unsigned char)(0x80 | ((c >> 6) & 0x3F));
        buf[2] = (unsigned char)(0x80 | (c & 0x3F));
        n = 3;
    } else {
        buf[0] = (unsigned char)(0xF0 | (c >> 18));
        buf[1] = (unsigned char)(0x80 | ((c >> 12) & 0x3F));
        buf[2] = (unsigned char)(0x80 | ((c >> 6) & 0x3F));
        buf[3] = (unsigned char)(0x80 | (c & 0x3F));
        n = 4;
    }
    return dragon_string_alloc((const char*)buf, n);
}

int64_t dragon_round_int(double x) {
    // Python-style banker's rounding
    double rounded = round(x);
    // Check for halfway case: round to even
    if (fabs(x - floor(x) - 0.5) < 1e-9) {
        int64_t f = (int64_t)floor(x);
        return (f % 2 == 0) ? f : f + 1;
    }
    return (int64_t)rounded;
}

double dragon_pow_float(double base, double exp) {
    return pow(base, exp);
}

DragonTuple* dragon_divmod(int64_t a, int64_t b) {
    DragonTuple* t = dragon_tuple_new(2);
    if (b == 0) {
        dragon_tuple_set(t, 0, 0);
        dragon_tuple_set(t, 1, 0);
        return t;
    }
    // Python-style floor division and modulo
    int64_t q = a / b;
    int64_t r = a % b;
    if ((r != 0) && ((r ^ b) < 0)) {
        q -= 1;
        r += b;
    }
    dragon_tuple_set(t, 0, q);
    dragon_tuple_set(t, 1, r);
    return t;
}

const char* dragon_hex(int64_t x) {
    char tmp[32];
    int len;
    if (x < 0) {
        len = snprintf(tmp, sizeof(tmp), "-0x%llx", (unsigned long long)(-x));
    } else {
        len = snprintf(tmp, sizeof(tmp), "0x%llx", (unsigned long long)x);
    }
    return dragon_string_alloc(tmp, len);
}

const char* dragon_oct(int64_t x) {
    char tmp[32];
    int len;
    if (x < 0) {
        len = snprintf(tmp, sizeof(tmp), "-0o%llo", (unsigned long long)(-x));
    } else {
        len = snprintf(tmp, sizeof(tmp), "0o%llo", (unsigned long long)x);
    }
    return dragon_string_alloc(tmp, len);
}

const char* dragon_bin(int64_t x) {
    char buf[80];
    int64_t val = x < 0 ? -x : x;
    if (val == 0) {
        return dragon_string_alloc("0b0", 3);
    }
    char tmp[66];
    int pos = 65;
    tmp[pos--] = '\0';
    while (val > 0) {
        tmp[pos--] = (val & 1) ? '1' : '0';
        val >>= 1;
    }
    int len;
    if (x < 0) {
        len = snprintf(buf, sizeof(buf), "-0b%s", &tmp[pos + 1]);
    } else {
        len = snprintf(buf, sizeof(buf), "0b%s", &tmp[pos + 1]);
    }
    return dragon_string_alloc(buf, len);
}

const char* dragon_repr_int(int64_t x) {
    return dragon_int_to_str(x);
}

const char* dragon_repr_str(const char* s) {
    if (!s) return dragon_string_alloc("None", 4);
    size_t len = strlen(s);
    DragonString* ds = dragon_string_alloc_raw((int64_t)(len + 2));
    ds->data[0] = '\'';
    memcpy(ds->data + 1, s, len);
    ds->data[len + 1] = '\'';
    ds->data[len + 2] = '\0';
    return ds->data;
}

const char* dragon_repr_float(double x) {
    return dragon_float_to_str(x);
}

const char* dragon_repr_bool(int64_t x) {
    return x ? dragon_string_alloc("True", 4) : dragon_string_alloc("False", 5);
}

//===----------------------------------------------------------------------===//
// Phase H: File I/O
//===----------------------------------------------------------------------===//

// File handle is an opaque pointer to FILE*
// dragon_file_open returns the FILE* cast to i8*

void* dragon_file_open(const char* filename, const char* mode) {
    FILE* f = fopen(filename, mode);
    if (!f) {
        // One error model everywhere: an unopenable path raises a catchable
        // FileNotFoundError (code 51), matching io.File's constructor - the
        // old NULL return made every later read a silent empty string.
        char buf[512];
        snprintf(buf, sizeof(buf),
                 "FileNotFoundError: [Errno 2] No such file or directory: '%s'",
                 filename ? filename : "");
        dragon_raise_exc_cstr(51, buf);
    }
    return (void*)f;
}

// Write a string to stderr, appending a newline. Used by stdlib/logging.dr.
// Buffered fwrite + flush keeps interleaved log lines from concurrent
// callers from getting tangled (fprintf is line-atomic on most platforms,
// but explicit flush makes the contract explicit).
void dragon_log_stderr(const char* msg) {
    if (!msg) return;
    fputs(msg, stderr);
    fputc('\n', stderr);
    fflush(stderr);
}

// Process argv exposure. main() forwards (argc, argv) here at startup;
// user code reads them via sys.argv() or directly through these helpers.
static int    g_dragon_argc = 0;
static char** g_dragon_argv = NULL;

void dragon_set_argv(int argc, char** argv) {
    g_dragon_argc = argc;
    g_dragon_argv = argv;
}

int64_t dragon_argv_count(void) {
    return (int64_t)g_dragon_argc;
}

// Returns the i-th argv element as a fresh owned DragonString (+1). Returns
// "" for out-of-range. Must stay a normal mortal +1, never dragon_str_intern:
// argv_at is not a borrowed-returner, so codegen's owned-str drain reclaims
// the +1, while an interned result is IMMORTAL with no dedup table (reading
// sys.argv in a loop grows RSS without bound and the drain is a no-op on it).
// Equality/hashing compare by content, so nothing relies on interned identity.
const char* dragon_argv_at(int64_t i) {
    if (i < 0 || i >= (int64_t)g_dragon_argc || !g_dragon_argv) {
        return dragon_string_alloc("", 0);
    }
    const char* s = g_dragon_argv[i];
    return dragon_string_alloc(s, (int64_t)strlen(s));
}

void dragon_file_close(void* handle) {
    if (handle) fclose((FILE*)handle);
}

const char* dragon_file_read(void* handle) {
    if (!handle) return dragon_string_alloc("", 0);
    FILE* f = (FILE*)handle;
    // Try the seek-based fast path first. Pipes/FIFOs/stdin make ftell return -1;
    // fall back to incremental fread with a growing buffer.
    long pos = ftell(f);
    if (pos >= 0 && fseek(f, 0, SEEK_END) == 0) {
        long size = ftell(f);
        if (size >= 0 && fseek(f, pos, SEEK_SET) == 0) {
            long remaining = size - pos;
            if (remaining <= 0) return dragon_string_alloc("", 0);
            char* buf = (char*)malloc(remaining + 1);
            size_t nread = fread(buf, 1, remaining, f);
            buf[nread] = '\0';
            const char* result = dragon_string_alloc(buf, (int64_t)nread);
            free(buf);
            return result;
        }
    }
    // Non-seekable: read in chunks until EOF.
    size_t cap = 4096;
    size_t len = 0;
    char* buf = (char*)malloc(cap);
    while (1) {
        if (len + 1024 > cap) {
            cap *= 2;
            char* nb = (char*)realloc(buf, cap);
            if (!nb) { free(buf); return dragon_string_alloc("", 0); }
            buf = nb;
        }
        size_t n = fread(buf + len, 1, cap - len - 1, f);
        len += n;
        if (n == 0) break;
    }
    buf[len] = '\0';
    const char* result = dragon_string_alloc(buf, (int64_t)len);
    free(buf);
    return result;
}

/// Read a file's full contents as raw bytes. Unlike `dragon_file_read`,
/// this skips UTF-8 detection on the input - every byte becomes one
/// 8-bit code point in a kind=1 DragonString. Use this for binary
/// content (images, archives, audio, anything not guaranteed to be
/// text). The returned string's `len` equals the byte count, not a
/// code-point count, and `dragon_str_byte_len_pub` (the wire-byte
/// helper) reports the same value - so HTTP `Content-Length` and
/// `nb_send` ship the file verbatim with no re-encoding.
///
/// Must NOT route through `dragon_string_alloc`: any non-ASCII byte
/// (a PNG header has 0x89) trips its UTF-8 path and expands the body
/// to 4-byte code points (a 309 KB PNG becomes 442 KB), so the bytes
/// on the wire stop being the file.
const char* dragon_file_read_bytes(void* handle) {
    if (!handle) return dragon_string_alloc("", 0);
    FILE* f = (FILE*)handle;
    long pos = ftell(f);
    if (pos >= 0 && fseek(f, 0, SEEK_END) == 0) {
        long size = ftell(f);
        if (size >= 0 && fseek(f, pos, SEEK_SET) == 0) {
            long remaining = size - pos;
            if (remaining <= 0) return dragon_string_alloc("", 0);
            DragonString* ds = dragon_string_alloc_raw((int64_t)remaining);
            size_t nread = fread(ds->data, 1, (size_t)remaining, f);
            ds->len = (int64_t)nread;
            ds->data[nread] = '\0';
            return ds->data;
        }
    }
    // Non-seekable fallback: read in chunks then memcpy into a sized
    // DragonString so the result is still a single allocation.
    size_t cap = 4096;
    size_t len = 0;
    char* buf = (char*)malloc(cap);
    while (1) {
        if (len + 1024 > cap) {
            cap *= 2;
            char* nb = (char*)realloc(buf, cap);
            if (!nb) { free(buf); return dragon_string_alloc("", 0); }
            buf = nb;
        }
        size_t n = fread(buf + len, 1, cap - len, f);
        len += n;
        if (n == 0) break;
    }
    DragonString* ds = dragon_string_alloc_raw((int64_t)len);
    memcpy(ds->data, buf, len);
    ds->data[len] = '\0';
    ds->len = (int64_t)len;
    free(buf);
    return ds->data;
}

// Read one line through and including the trailing '\n' (or to EOF) from an
// open FILE*, decoded as UTF-8. Grows its buffer so there is NO line-length
// limit (the old fixed char[4096] silently split lines longer than 4095 bytes),
// and decodes the whole line in one pass so a multibyte code point is never cut
// at the buffer boundary. Returns "" only at EOF with no data - the stop
// signal for readlines() and `for line in f`.
const char* dragon_file_readline(void* handle) {
    if (!handle) return dragon_string_alloc("", 0);
    FILE* f = (FILE*)handle;
    size_t cap = 256, len = 0;
    uint8_t* buf = (uint8_t*)dragon_xmalloc(cap);   // was unchecked -> SEGV on OOM (#6)
    int c;
    while ((c = fgetc(f)) != EOF) {
        if (len + 1 > cap) { cap *= 2; buf = (uint8_t*)dragon_xrealloc(buf, cap); }  // self-assign fixed (#7)
        buf[len++] = (uint8_t)c;
        if (c == '\n') break;
    }
    const char* result = dragon_string_alloc((const char*)buf, (int64_t)len);
    free(buf);
    return result;
}

//===----------------------------------------------------------------------===//

//===----------------------------------------------------------------------===//
// Generators (coroutine-based lazy iteration)
//===----------------------------------------------------------------------===//

#define GEN_STATE_INITIAL   0
#define GEN_STATE_SUSPENDED 1
#define GEN_STATE_EXHAUSTED 2

/// D030: Create a generator with a per-callsite typed args struct + decref fn.
///   trampoline: codegen-synthesized; reads native-typed args, calls body, marks exhausted.
///   args:       pointer to a stack-allocated typed struct at the call site
///               (field 0 reserved for the DragonGenerator*, patched here).
///   args_size:  byte size of the struct for malloc + memcpy (0 if no args).
///   args_decref_fn: codegen-synthesized; called at destroy to decref heap-typed args.
///                   May be NULL if no heap-typed args.
/// The args buffer outlives the body call (until generator destroy) so heap
/// captures survive abandonment. Destroy invokes args_decref_fn then frees args.
DragonGenerator* dragon_generator_create_typed(
    void (*trampoline)(mco_coro*), void* args, int64_t args_size,
    void (*args_decref_fn)(void*)) {
    DragonGenerator* gen = (DragonGenerator*)calloc(1, sizeof(DragonGenerator));
    dragon_obj_init(&gen->header, DRAGON_TAG_GENERATOR);
    gen->state = GEN_STATE_INITIAL;
    gen->yielded_value = 0;

    void* heap_args = NULL;
    if (args_size > 0 && args) {
        heap_args = malloc((size_t)args_size);
        memcpy(heap_args, args, (size_t)args_size);
        // Patch field 0 (DragonGenerator*) so the trampoline can address self.
        *(DragonGenerator**)heap_args = gen;
    }
    gen->args = heap_args;
    gen->args_decref_fn = args_decref_fn;

    mco_desc desc = mco_desc_init(trampoline, 0); // default 56KB stack
    desc.user_data = heap_args;
    mco_result r = mco_create(&gen->coro, &desc);
    if (r != MCO_SUCCESS) {
        fprintf(stderr, "generator: failed to create coroutine: %s\n",
                mco_result_description(r));
        // Destroy will not run since we return NULL - clean up args here.
        if (heap_args) {
            if (args_decref_fn) args_decref_fn(heap_args);
            free(heap_args);
        }
        free(gen);
        return NULL;
    }
    return gen;
}

/// D030: Mark a generator as exhausted. Called by the per-callsite trampoline
/// when the body returns normally (vs. mid-yield).
void dragon_generator_set_exhausted(void* gen_ptr) {
    DragonGenerator* gen = (DragonGenerator*)gen_ptr;
    if (gen) gen->state = GEN_STATE_EXHAUSTED;
}

/// Called by the trampoline's setjmp barrier when the body raised an exception
/// that escaped every try/except INSIDE the generator. Flags it so
/// dragon_generator_next re-raises it in the caller's restored exc context. The
/// type/msg/obj are already in the generator's own exc context (gen->exc_vt),
/// which survives until generator destroy, so nothing is copied here.
void dragon_generator_set_raised(void* gen_ptr) {
    DragonGenerator* gen = (DragonGenerator*)gen_ptr;
    if (gen) gen->pending_exc = 1;
}

/// Yield a value from inside the generator body. Called by compiled yield expressions.
void dragon_generator_yield(void* gen_ptr, int64_t value) {
    DragonGenerator* gen = (DragonGenerator*)gen_ptr;
    gen->yielded_value = value;
    gen->state = GEN_STATE_SUSPENDED;
    mco_yield(gen->coro);
}

/// Resume the generator and return the next yielded value.
/// Raises StopIteration (exc code 11) when exhausted.
int64_t dragon_generator_next(void* gen_ptr) {
    DragonGenerator* gen = (DragonGenerator*)gen_ptr;
    if (!gen || gen->state == GEN_STATE_EXHAUSTED) {
        dragon_raise_exc_cstr(11, "StopIteration");
        return 0; // unreachable
    }
    // Install the generator's own exception context for the duration of the
    // body's run, so its try/except frames are ISOLATED from the caller's exc
    // stack (a generator that yields inside a try must keep that setjmp frame
    // alive across the suspend; sharing the caller's stack let the caller's
    // per-iteration push/pop clobber it -> wrong-frame longjmp / SIGSEGV).
    // Mirrors the scheduler's vthread swap (active_frames travels with the
    // context). Allocated lazily here on first resume. Restored on yield/return
    // so the caller (which may itself be a generator or a green thread) sees
    // its own context again. Nested generators chain correctly via save/restore.
    if (!gen->exc_vt) {
        gen->exc_vt = (DragonVThread*)calloc(1, sizeof(DragonVThread));
        gen->exc_vt->exc_sp = -1;
    }
    DragonVThread* prev_exc_vt = __dragon_exc_vt;
    int prev_active_frames = __dragon_active_frames;
    __dragon_exc_vt = gen->exc_vt;
    __dragon_active_frames = gen->exc_vt->active_frames;

    mco_resume(gen->coro);

    gen->exc_vt->active_frames = __dragon_active_frames;
    __dragon_exc_vt = prev_exc_vt;
    __dragon_active_frames = prev_active_frames;

    // The body raised an exception not caught inside the generator: the
    // trampoline barrier captured it and returned normally. Re-raise it now in
    // the caller's (restored) context so it propagates to the caller's handler,
    // never having to longjmp across the coroutine boundary.
    if (gen->pending_exc) {
        gen->pending_exc = 0;
        gen->state = GEN_STATE_EXHAUSTED;
        dragon_raise_exc_obj(gen->exc_vt->exc_type, gen->exc_vt->exc_obj,
                             gen->exc_vt->exc_msg);
        return 0; // unreachable
    }
    if (gen->state == GEN_STATE_EXHAUSTED || mco_status(gen->coro) == MCO_DEAD) {
        gen->state = GEN_STATE_EXHAUSTED;
        dragon_raise_exc_cstr(11, "StopIteration");
        return 0; // unreachable
    }
    return gen->yielded_value;
}

/// Destroy a generator (free coroutine + args + struct).
/// D030: Heap-typed captured args are decref'd by a per-callsite decref fn
/// generated by codegen with knowledge of each field's native type. This
/// replaces the previous tagged-loop approach.
void dragon_generator_destroy(void* gen_ptr) {
    DragonGenerator* gen = (DragonGenerator*)gen_ptr;
    if (!gen) return;
    if (gen->coro) {
        // mco_destroy only accepts a coroutine in MCO_DEAD or MCO_SUSPENDED
        // state. A generator whose body raised an exception that propagated out
        // was abandoned mid-resume and is left MCO_RUNNING - calling mco_destroy
        // on it prints "attempt to uninitialize a coroutine that is not dead or
        // suspended" and reclaims nothing. Skip it in that case: minicoro cannot
        // reclaim a running coroutine (a known limitation of longjmp-ing out of
        // one - a proper fix needs a setjmp barrier inside the generator body),
        // so its stack leaks, but we avoid the spurious error and still free the
        // struct + captured args below.
        mco_state st = mco_status(gen->coro);
        if (st == MCO_DEAD || st == MCO_SUSPENDED) mco_destroy(gen->coro);
    }
    if (gen->args) {
        if (gen->args_decref_fn) gen->args_decref_fn(gen->args);
        free(gen->args);
    }
    // Free the generator's isolated exception context (its cleanup stack may
    // have lazily grown heap buffers). Only present if the generator was
    // resumed at least once.
    if (gen->exc_vt) {
        free(gen->exc_vt->cleanup.vals);
        free(gen->exc_vt->cleanup.kinds);
        free(gen->exc_vt->cleanup.tags);
        free(gen->exc_vt);
    }
    free(gen);
}

//===----------------------------------------------------------------------===//
// Decision 025: First-Class Class Descriptors
//===----------------------------------------------------------------------===//


// class_id → descriptor lookup table (for isinstance ancestor walk)
static DragonClassDescriptor* __class_descriptor_table[DRAGON_MAX_CLASS_IDS];

/// Create a class descriptor. Returns ptr as i64.
/// Called from CodeGen at module init time, after class_id registration.
/// `doc` is the class docstring as a NUL-terminated C string in .rodata
/// (or NULL when the class has no docstring) - powers `Cls.__doc__`.
int64_t dragon_class_descriptor_create(const char* name, int64_t constructor,
                                        int64_t class_id, int64_t parent_descriptor,
                                        const char* doc) {
    DragonClassDescriptor* desc = (DragonClassDescriptor*)calloc(1, sizeof(DragonClassDescriptor));
    dragon_obj_init(&desc->header, DRAGON_TAG_TYPE);
    dragon_make_immortal(desc);
    desc->class_id = class_id;
    desc->name = name;
    desc->doc = doc;
    desc->parent = parent_descriptor;
    desc->constructor = constructor;

    // Build ancestor_ids chain: [self_id, parent_id, grandparent_id, ...]
    int64_t count = 1; // self
    {
        int64_t p = parent_descriptor;
        while (p) {
            count++;
            p = ((DragonClassDescriptor*)(void*)p)->parent;
        }
    }
    desc->ancestor_ids = (int64_t*)malloc(count * sizeof(int64_t));
    desc->num_ancestors = count;
    desc->ancestor_ids[0] = class_id;
    {
        int64_t idx = 1;
        int64_t p = parent_descriptor;
        while (p) {
            DragonClassDescriptor* pd = (DragonClassDescriptor*)(void*)p;
            desc->ancestor_ids[idx++] = pd->class_id;
            p = pd->parent;
        }
    }

    // Register in lookup table for isinstance
    if (class_id > 0 && class_id < DRAGON_MAX_CLASS_IDS)
        __class_descriptor_table[class_id] = desc;

    return (int64_t)(void*)desc;
}

// ADR 025 removal: dragon_class_descriptor_call() (runtime construction through
// a class descriptor) was deleted. Classes are compile-time entities (D021):
// construction is always resolved statically to the concrete ClassName_new at
// codegen time. The `constructor` field on DragonClassDescriptor is retained
// because dragon_dir() reads it to decide whether to list "__init__".

/// Get the class name from a descriptor. Returns a DragonString data ptr as i64.
int64_t dragon_class_descriptor_get_name(int64_t descriptor) {
    DragonClassDescriptor* desc = (DragonClassDescriptor*)(void*)descriptor;
    if (!desc || !desc->name) return 0;
    return (int64_t)dragon_string_alloc(desc->name, (int64_t)strlen(desc->name));
}

/// Get the class docstring from a descriptor. Returns the raw .rodata C
/// string pointer (NUL-terminated, kind=1 from Dragon's POV - see
/// dragon_is_heap_string) or NULL if the class has no docstring.
/// NULL flows through `dragon_print_str` etc. as Python's None per the
/// niche-ptr Optional[str] ABI.
const char* dragon_class_descriptor_get_doc(int64_t descriptor) {
    if (!descriptor) return NULL;
    return ((DragonClassDescriptor*)(void*)descriptor)->doc;
}

/// Get the class docstring for an instance: header.class_id → descriptor → doc.
/// Returns NULL when the instance is null, has no class_id, or the class has
/// no docstring.
const char* dragon_instance_get_doc(void* instance) {
    if (!instance) return NULL;
    DragonObjectHeader* h = (DragonObjectHeader*)instance;
    uint16_t cid = h->class_id;
    if (cid == 0 || cid >= DRAGON_MAX_CLASS_IDS) return NULL;
    DragonClassDescriptor* desc = __class_descriptor_table[cid];
    if (!desc) return NULL;
    return desc->doc;
}

/// Get the class name for an instance: header.class_id → descriptor → name.
/// Returns the descriptor's .rodata name C-string, or NULL when the instance
/// is null / has no class_id / the class isn't registered. Used by the box
/// printers (runtime_box.cpp) to render a class instance that reached a box
/// under the TAG_BYTES/TAG_LIST value-tag collision as `<ClassName instance>`
/// instead of misreading its bytes. Read-only lookup; allocates nothing.
const char* dragon_instance_class_name(void* instance) {
    if (!instance) return NULL;
    DragonObjectHeader* h = (DragonObjectHeader*)instance;
    uint16_t cid = h->class_id;
    if (cid == 0 || cid >= DRAGON_MAX_CLASS_IDS) return NULL;
    DragonClassDescriptor* desc = __class_descriptor_table[cid];
    if (!desc) return NULL;
    return desc->name;
}

// ADR 025 removal: dragon_isinstance_runtime() was deleted. isinstance() is
// resolved statically at codegen time - the 2nd arg must name a class known at
// compile time (a literal class or a compile-time alias). The inheritance walk
// now happens over classParentNames in codegen, not over a runtime descriptor
// ancestor chain.

//===----------------------------------------------------------------------===//
// hasattr() / getattr() - runtime attribute reflection
//===----------------------------------------------------------------------===//

/// Set field metadata on a class descriptor (called from codegen after create).
/// `field_offsets` are BYTE offsets into the instance and `field_widths` the
/// byte width of each field's native type (1 for bool, 8 for int/float/ptr).
/// Both are needed for a correct, in-bounds getattr read: fields use native
/// LLVM types (bool=i1, float=f64), so a fixed i64 read at a GEP-index-derived
/// position misaligned for consecutive narrow fields and could read past the
/// allocation for a trailing narrow field.
void dragon_class_descriptor_set_fields(int64_t descriptor,
                                         const char** field_names,
                                         int64_t* field_offsets,
                                         int64_t* field_widths,
                                         int64_t num_fields) {
    DragonClassDescriptor* desc = (DragonClassDescriptor*)(void*)descriptor;
    if (!desc) return;
    desc->field_names  = field_names;
    desc->field_offsets = field_offsets;
    desc->field_widths = field_widths;
    desc->num_fields   = num_fields;
}

/// Find a field by name in the descriptor (including parent chain). On success
/// returns the field's BYTE offset and writes its byte width to *out_width;
/// returns -1 if not found.
static int64_t _find_field(int64_t instance, const char* attr_name, int64_t* out_width) {
    if (!instance || !attr_name) return -1;
    DragonObjectHeader* h = (DragonObjectHeader*)(void*)instance;
    if (h->type_tag != DRAGON_TAG_CLASS) return -1;
    uint16_t cid = h->class_id;
    if (cid == 0 || cid >= DRAGON_MAX_CLASS_IDS) return -1;
    DragonClassDescriptor* desc = __class_descriptor_table[cid];
    while (desc) {
        for (int64_t i = 0; i < desc->num_fields; i++) {
            if (strcmp(desc->field_names[i], attr_name) == 0) {
                if (out_width) *out_width = desc->field_widths ? desc->field_widths[i] : 8;
                return desc->field_offsets[i];
            }
        }
        desc = desc->parent ? (DragonClassDescriptor*)(void*)desc->parent : nullptr;
    }
    return -1;
}

/// Read a field's value, width-aware, into a zero-extended i64. Reads exactly
/// `width` bytes at `byte_offset` so a narrow field never over-reads adjacent
/// fields or the heap past the instance, and never leaks uninitialized bytes.
static int64_t _read_field(int64_t instance, int64_t byte_offset, int64_t width) {
    const unsigned char* base = (const unsigned char*)(void*)instance + byte_offset;
    uint64_t v = 0;
    if (width <= 0) width = 8;
    if (width > 8) width = 8;
    memcpy(&v, base, (size_t)width);  // little-endian: low bytes carry the value
    return (int64_t)v;
}

/// Back-compat helper retained for any internal caller: byte offset only.
static int64_t _find_field_offset(int64_t instance, const char* attr_name) {
    return _find_field(instance, attr_name, nullptr);
}

//===----------------------------------------------------------------------===//
// D033: Method reflection - set per-class method metadata and look up
// methods by name across the inheritance chain. Field-name reflection above
// is the template (codegen emits parallel arrays as static globals and the
// setter wires them into the descriptor).
//===----------------------------------------------------------------------===//

/// Attach the method-name/fn-ptr/kind arrays to a class descriptor. Called
/// once per class from module-init right after dragon_class_descriptor_create.
/// Each class advertises ITS OWN methods only; inheritance is resolved at
/// lookup time by walking the parent chain.
void dragon_class_descriptor_set_methods(int64_t descriptor,
                                          const char** method_names,
                                          void** method_fn_ptrs,
                                          uint8_t* method_kinds,
                                          int64_t num_methods) {
    DragonClassDescriptor* desc = (DragonClassDescriptor*)(void*)descriptor;
    if (!desc) return;
    desc->method_names   = method_names;
    desc->method_fn_ptrs = method_fn_ptrs;
    desc->method_kinds   = method_kinds;
    desc->num_methods    = num_methods;
}

/// Find a method's function pointer by name. Walks the inheritance chain
/// via DragonClassDescriptor.parent so an inherited method on a subclass
/// resolves to the parent's fn ptr. Returns NULL on miss.
void* dragon_class_find_method(int64_t descriptor, const char* name) {
    DragonClassDescriptor* desc = (DragonClassDescriptor*)(void*)descriptor;
    while (desc) {
        for (int64_t i = 0; i < desc->num_methods; i++) {
            if (strcmp(desc->method_names[i], name) == 0)
                return desc->method_fn_ptrs[i];
        }
        desc = desc->parent ? (DragonClassDescriptor*)(void*)desc->parent : nullptr;
    }
    return nullptr;
}

/// D033 Phase 3: like find_method, but returns the codegen-emitted "bound
/// thunk" (closure ABI: `(user_args..., env)`) instead of the raw method.
/// Used by getattr() to construct a DragonClosure that the existing closure
/// call path can invoke without knowing about method-vs-function ABIs.
void* dragon_class_find_method_bound(int64_t descriptor, const char* name) {
    DragonClassDescriptor* desc = (DragonClassDescriptor*)(void*)descriptor;
    while (desc) {
        if (desc->method_bound_thunks) {
            for (int64_t i = 0; i < desc->num_methods; i++) {
                if (strcmp(desc->method_names[i], name) == 0)
                    return desc->method_bound_thunks[i];
            }
        }
        desc = desc->parent ? (DragonClassDescriptor*)(void*)desc->parent : nullptr;
    }
    return nullptr;
}

/// Codegen-supplied setter for the bound-thunks array. Called from module
/// init right after set_methods. Kept separate so a class with reflection
/// metadata but no bound thunks (e.g., a synthesized data class) doesn't
/// need to materialize a stub thunks array.
void dragon_class_descriptor_set_method_bound_thunks(int64_t descriptor,
                                                      void** bound_thunks) {
    DragonClassDescriptor* desc = (DragonClassDescriptor*)(void*)descriptor;
    if (!desc) return;
    desc->method_bound_thunks = bound_thunks;
}

/// Find a method's kind by name (0 = instance, 1 = static, 2 = classmethod).
/// Returns -1 if the name is not a method on this class or any ancestor.
/// Used by getattr() to decide whether to bind `self` before returning a
/// callable.
int64_t dragon_class_find_method_kind(int64_t descriptor, const char* name) {
    DragonClassDescriptor* desc = (DragonClassDescriptor*)(void*)descriptor;
    while (desc) {
        for (int64_t i = 0; i < desc->num_methods; i++) {
            if (strcmp(desc->method_names[i], name) == 0) {
                return desc->method_kinds ? (int64_t)desc->method_kinds[i] : 0;
            }
        }
        desc = desc->parent ? (DragonClassDescriptor*)(void*)desc->parent : nullptr;
    }
    return -1;
}

/// D033 Phase 2: dir(obj) / dir(Cls).
///
/// Walks the class's MRO chain collecting unique field + method names,
/// includes "__init__" once if any class in the chain has a constructor,
/// sorts lexicographically, and returns a list[str] of refcounted heap
/// strings. Matches Python's dir() semantics for user-defined classes
/// (Dragon doesn't ship an object root with the `__class__` /
/// `__getattribute__` family, so those aren't surfaced).
///
/// `is_descriptor` non-zero ⇒ `instance_or_desc` is a DragonClassDescriptor*.
/// Zero ⇒ it's an instance pointer; we look up the descriptor via class_id.
///
/// Empty list on null / unknown class. Caller owns the returned list (+1).
DragonListPtr* dragon_dir(int64_t instance_or_desc, int64_t is_descriptor) {
    DragonClassDescriptor* desc = nullptr;
    if (is_descriptor) {
        desc = (DragonClassDescriptor*)(void*)instance_or_desc;
    } else if (instance_or_desc) {
        DragonObjectHeader* h = (DragonObjectHeader*)(void*)instance_or_desc;
        if (h->type_tag == DRAGON_TAG_CLASS) {
            uint16_t cid = h->class_id;
            if (cid != 0 && cid < DRAGON_MAX_CLASS_IDS)
                desc = __class_descriptor_table[cid];
        }
    }
    if (!desc) {
        return dragon_list_new_ptr(0, TAG_STR);
    }

    // Two-pass: first count an upper bound on names so we can stack-allocate
    // a flat array; then collect with linear-scan dedupe. Linear dedupe is
    // fine for typical class sizes (< 30 attrs); we still beat std::set.
    int64_t maxNames = 0;
    DragonClassDescriptor* d = desc;
    while (d) {
        maxNames += d->num_fields + d->num_methods;
        if (d->constructor) maxNames++;
        d = d->parent ? (DragonClassDescriptor*)(void*)d->parent : nullptr;
    }
    if (maxNames == 0) {
        return dragon_list_new_ptr(0, TAG_STR);
    }

    // Borrowed C-string pointers - the descriptor owns the storage (it's
    // in `.rodata` from the codegen emission). We just shuffle pointers
    // until the final dragon_string_alloc copies into heap strings.
    const char** names = (const char**)alloca((size_t)maxNames * sizeof(const char*));
    int64_t count = 0;
    auto pushUnique = [&](const char* nm) {
        if (!nm) return;
        for (int64_t j = 0; j < count; j++) {
            if (strcmp(names[j], nm) == 0) return;
        }
        names[count++] = nm;
    };
    d = desc;
    while (d) {
        for (int64_t i = 0; i < d->num_fields; i++)  pushUnique(d->field_names[i]);
        for (int64_t i = 0; i < d->num_methods; i++) pushUnique(d->method_names[i]);
        if (d->constructor) pushUnique("__init__");
        d = d->parent ? (DragonClassDescriptor*)(void*)d->parent : nullptr;
    }

    // Insertion sort (small N; minimal codegen footprint).
    for (int64_t i = 1; i < count; i++) {
        const char* key = names[i];
        int64_t j = i - 1;
        while (j >= 0 && strcmp(names[j], key) > 0) {
            names[j + 1] = names[j];
            j--;
        }
        names[j + 1] = key;
    }

    // Materialize as list[str] of refcounted DragonStrings. Capacity hint =
    // count so we avoid resizing during the loop.
    DragonListPtr* list = dragon_list_new_ptr(count > 0 ? count : 8, TAG_STR);
    for (int64_t i = 0; i < count; i++) {
        const char* heapStr = dragon_string_alloc(names[i], (int64_t)strlen(names[i]));
        // dragon_string_alloc returns a +1 string; list takes ownership of
        // that reference (no extra incref needed).
        dragon_list_append_ptr(list, (void*)(uintptr_t)heapStr);
    }
    return list;
}

/// D033 Phase 3: forward declarations for closure construction. These live
/// further down in this TU and the existing field path doesn't reach them;
/// adding the proto here keeps the getattr extension self-contained.
/// env_alloc gained the multi-op gc_fn + a `trackable` gate.
void* dragon_env_alloc(int64_t total_size,
                       void (*gc_fn)(void*, int32_t, dragon_gc_visit_fn, void*),
                       int32_t trackable);
void* dragon_closure_create(void* fn_ptr, void* env);

/// Per-class shared GC hook for bound-method envs. The env body is exactly
/// `{ void* self }`, captured with a +1. this is now MULTI-OP so
/// a bound method stored in a field of `self` (instance -> closure -> env ->
/// self, a real cycle) is collectable. One copy serves every class because the
/// body layout is uniform.
static void _bound_method_env_gc(void* env_ptr, int32_t op,
                                 dragon_gc_visit_fn visit, void* arg) {
    if (!env_ptr) return;
    // env body sits immediately after the 24-byte DragonEnv prefix
    // (16B header + 8B gc_fn). Body layout: { void* self }.
    void** body = (void**)((char*)env_ptr + sizeof(DragonEnv));
    void* self = *body;
    switch (op) {
        case DRAGON_ENV_OP_DEALLOC:
            if (self) dragon_decref_dispatch(self);
            break;
        case DRAGON_ENV_OP_TRAVERSE:
            // self is a class instance (tracked) - visit so the collector
            // subtracts the env's internal reference to it.
            if (self && visit) visit(self, arg);
            break;
        case DRAGON_ENV_OP_CLEAR:
            if (self) { dragon_decref(self); *body = nullptr; }
            break;
        default: break;
    }
}

/// Build a bound-method closure for (self, method-thunk). Allocates a small
/// env carrying self (+1 reference) and wraps it in a DragonClosure with
/// `fn_ptr = thunk_fn`. The existing closure call path (CallExpr.cpp at
/// the `VarKind::Closure` site) invokes it as `thunk_fn(user_args..., env)`,
/// and the codegen-emitted thunk loads self out of env's body and calls the
/// underlying method.
static void* _make_bound_method_closure(void* self, void* thunk_fn) {
    if (!thunk_fn) return nullptr;
    // total_size = 16B header + 8B gc_fn + 8B self
    int64_t total = (int64_t)(sizeof(DragonEnv) + sizeof(void*));
    // trackable=1: self is a heap instance, so this env can close a cycle.
    void* env = dragon_env_alloc(total, _bound_method_env_gc, /*trackable=*/1);
    void** body = (void**)((char*)env + sizeof(DragonEnv));
    *body = self;
    if (self) dragon_incref(self);
    return dragon_closure_create(thunk_fn, env);
}

/// hasattr(obj, "name") → 1 if field OR method exists, 0 otherwise.
/// D033 Phase 3 extension: also walks the method table so
/// `hasattr(obj, "test_foo")` returns true for a method named `test_foo`.
int64_t dragon_hasattr(int64_t instance, const char* attr_name) {
    if (_find_field_offset(instance, attr_name) >= 0) return 1;
    // Method check: resolve descriptor from instance and walk method names.
    if (!instance || !attr_name) return 0;
    DragonObjectHeader* h = (DragonObjectHeader*)(void*)instance;
    if (h->type_tag != DRAGON_TAG_CLASS) return 0;
    uint16_t cid = h->class_id;
    if (cid == 0 || cid >= DRAGON_MAX_CLASS_IDS) return 0;
    DragonClassDescriptor* desc = __class_descriptor_table[cid];
    if (!desc) return 0;
    return dragon_class_find_method((int64_t)(void*)desc, attr_name) != nullptr ? 1 : 0;
}

/// Look up a bound method as a callable. Returns a DragonClosure* ptr cast
/// to i64 if found, otherwise 0. Resolves descriptor → bound thunk → builds
/// the closure with self captured in env. Handles static methods by
/// returning the raw fn pointer (no self bind).
static int64_t _getattr_method(int64_t instance, const char* attr_name) {
    if (!instance) return 0;
    DragonObjectHeader* h = (DragonObjectHeader*)(void*)instance;
    if (h->type_tag != DRAGON_TAG_CLASS) return 0;
    uint16_t cid = h->class_id;
    if (cid == 0 || cid >= DRAGON_MAX_CLASS_IDS) return 0;
    DragonClassDescriptor* desc = __class_descriptor_table[cid];
    if (!desc) return 0;
    int64_t descI = (int64_t)(void*)desc;
    // Static (1) or @classmethod (2): no self to bind - codegen emits
    // these without a leading self/cls param. Return the raw fn ptr.
    int64_t kind = dragon_class_find_method_kind(descI, attr_name);
    if (kind == 1 || kind == 2) {
        void* fn = dragon_class_find_method(descI, attr_name);
        return (int64_t)(uintptr_t)fn;
    }
    // Instance method: bind via thunk + env-captured self.
    if (kind == 0) {
        void* thunk = dragon_class_find_method_bound(descI, attr_name);
        if (!thunk) return 0;
        void* closure = _make_bound_method_closure((void*)(uintptr_t)instance, thunk);
        return (int64_t)(uintptr_t)closure;
    }
    return 0;
}

/// getattr(obj, "name") → field value (i64) or bound callable for methods.
/// Raises AttributeError if neither field nor method is found.
int64_t dragon_getattr(int64_t instance, const char* attr_name) {
    int64_t width = 8;
    int64_t offset = _find_field(instance, attr_name, &width);
    if (offset >= 0) return _read_field(instance, offset, width);
    int64_t bound = _getattr_method(instance, attr_name);
    if (bound) return bound;
    fprintf(stderr, "AttributeError: object has no attribute '%s'\n", attr_name);
    return 0;
}

/// getattr(obj, "name", default) → field value, bound method, or default
int64_t dragon_getattr_default(int64_t instance, const char* attr_name, int64_t default_val) {
    int64_t width = 8;
    int64_t offset = _find_field(instance, attr_name, &width);
    if (offset >= 0) return _read_field(instance, offset, width);
    int64_t bound = _getattr_method(instance, attr_name);
    if (bound) return bound;
    return default_val;
}

//===----------------------------------------------------------------------===//
// D027: Closure and Environment runtime support
//===----------------------------------------------------------------------===//

/// D030: Allocate a closure environment with native-typed body layout.
///   total_size: sizeof(DragonEnv) + sizeof(per-lambda body struct)
///   gc_fn:      codegen-generated multi-op hook (DEALLOC/TRAVERSE/CLEAR over
///               the heap captures). NULL for a scalar-only env (no cleanup).
///   trackable:  1 iff the env captures a cycle-capable heap object (instance,
///               container, closure, cell, ...). only trackable
///               envs join gc_tracked so the cycle collector can reclaim an
///               instance/list -> closure -> env -> back-edge cycle. A str-only
///               or scalar-only env can never close a cycle, so it stays
///               untracked (#1: it pays no GC-scan cost and decrefs lock-free).
/// Returns env pointer (raw void*). Refcount starts at 1.
void* dragon_env_alloc(int64_t total_size,
                       void (*gc_fn)(void*, int32_t, dragon_gc_visit_fn, void*),
                       int32_t trackable) {
    DragonEnv* env = (DragonEnv*)calloc(1, (size_t)total_size);
    dragon_obj_init(&env->header, DRAGON_TAG_ENV);
    env->gc_fn = gc_fn;
    if (trackable) dragon_gc_track(env);
    return env;
}

/// Create a closure wrapping fn_ptr + env. Returns the closure as a raw
/// pointer (no i64 round-trip). Takes ownership of the env's existing
/// reference (env_alloc starts at refcount 1 - no additional incref needed).
/// Codegen accesses the closure's fn_ptr / env fields via inline GEPs.
/// a closure whose env is tracked is itself tracked, so the
/// `instance/list -> closure -> env` chain is fully visible to the collector.
/// A non-capturing or scalar-only-env closure stays untracked (#1).
void* dragon_closure_create(void* fn_ptr, void* env) {
    DragonClosure* cls = (DragonClosure*)calloc(1, sizeof(DragonClosure));
    dragon_obj_init(&cls->header, DRAGON_TAG_CLOSURE);
    cls->fn_ptr = fn_ptr;
    cls->env = (DragonEnv*)env;
    if (env && (((DragonEnv*)env)->header.gc_flags & GC_FLAG_TRACKED))
        dragon_gc_track(cls);
    return cls;
}

/// Dealloc a closure: decref the env, then free self.
/// Called by dragon_decref when refcount hits 0 for TAG_CLOSURE objects.
/// The env decref goes through dragon_decref_dispatch so it takes the atomic
/// variant inside atomic context. (cls->env may have been NULLed by the cycle
/// collector's clear_refs - the guard handles that exactly-once.)
void dragon_closure_dealloc(DragonClosure* cls) {
    if (!cls) return;
    if (cls->env) {
        dragon_decref_dispatch(&cls->env->header);
    }
    free(cls);
}

/// Dealloc an env: run the per-site gc_fn DEALLOC op (which decrefs heap
/// captures), then free. Called by dragon_decref when refcount hits 0 for
/// TAG_ENV objects. (Capture slots NULLed by clear_refs decref to nothing.)
void dragon_env_dealloc(DragonEnv* env) {
    if (!env) return;
    if (env->gc_fn) {
        env->gc_fn(env, DRAGON_ENV_OP_DEALLOC, nullptr, nullptr);
    }
    free(env);
}

//===----------------------------------------------------------------------===//
// Heap-boxed mutable cells - backing storage for `nonlocal` semantics.
//===----------------------------------------------------------------------===//
//
// One cell per nonlocal-declared variable. The owning function allocates the
// cell at the var's introduction; every nested function that lists the var
// in its `nonlocal` clause captures the cell pointer (not the value) into
// its closure environment. All reads/writes go through dragon_cell_get/set,
// so reads chain and writes mutate through a single backing slot.
//
// Cell ownership:
//   - The owning function holds one ref from creation; it decrefs on scope
//     exit (which drops the held heap value via the TAG_CELL dealloc path).
//   - Each closure env that captures the cell holds one ref (incref'd at
//     env-populate time); the env's per-site dealloc decrefs on env destroy.
//
// Atomic ops:
//   - Cells are not yet thread-safe. Capturing a mutated nonlocal into a
//     `fire`-spawned vthread is racy; the SHARED-bit dispatch path used by
//     other heap objects can be added when needed (track issue if surfaces).

/// Allocate a cell with an initial i64 value.
///   `kind` is a DragonValueTag (TAG_INT, TAG_STR, ...) recording how `value`
///   should be interpreted on dealloc / overwrite.
///   `holds_heap` is non-zero iff `value` is a heap pointer at allocation time
///   (ints/bools/floats clear it; strings/lists/dicts/instances set it). The
///   caller is responsible for the +1 incref of `value` *before* this call,
///   matching the standard "ownership transfer into store" discipline.
void* dragon_cell_alloc(int64_t value, int32_t kind, int32_t holds_heap) {
    DragonCell* c = (DragonCell*)calloc(1, sizeof(DragonCell));
    dragon_obj_init(&c->header, DRAGON_TAG_CELL);
    c->value = value;
    c->kind = kind;
    c->holds_heap = holds_heap;
    return c;
}

/// Read the cell's current i64 value. Returns a borrowed reference for heap
/// kinds - the caller is responsible for any incref needed to extend the
/// value's lifetime past the cell's. (Reads followed by typed allocation of
/// a new local follow the existing `emitIncrefByKind` discipline already in
/// codegen for AssignStmt.)
int64_t dragon_cell_get(void* cell) {
    if (!cell) return 0;
    return ((DragonCell*)cell)->value;
}

/// Replace the cell's value with `new_value` and return the previous one,
/// so the caller can decref it under the same ownership discipline used by
/// scalar overwrites elsewhere in codegen. The runtime does NOT decref old
/// or incref new - that stays at the call site so the kind-specific
/// (`dragon_decref_str` vs `dragon_decref` vs no-op for ints) dispatch lives
/// where the static type information already is.
int64_t dragon_cell_set(void* cell, int64_t new_value) {
    DragonCell* c = (DragonCell*)cell;
    int64_t old = c->value;
    c->value = new_value;
    return old;
}

// Tag-aware incref / decref for Callable[[...], R] fields.
//
// A Callable field can hold either a bare LLVM function pointer (no header)
// or a DragonClosure pointer (with header, type_tag = DRAGON_TAG_CLOSURE).
// Inspecting type_tag at offset 8 picks the right path: closure → real RC,
// bare fn → no-op. This lets a class field own a reference to a capturing
// closure (so the closure outlives the local that produced it) without
// imposing RC machinery on bare fn pointers, which have no header to mutate.
void dragon_incref_callable(void* p) {
    if (!p) return;
    DragonObjectHeader* h = (DragonObjectHeader*)p;
    if (h->type_tag != DRAGON_TAG_CLOSURE) return;
    dragon_incref(p);
}

void dragon_decref_callable(void* p) {
    if (!p) return;
    DragonObjectHeader* h = (DragonObjectHeader*)p;
    if (h->type_tag != DRAGON_TAG_CLOSURE) return;
    dragon_decref(p);
}

} // extern "C"
