/// Dragon Runtime - Builtins, Print, Math, File I/O, Generators, Class Descriptors
#include "runtime_internal.h"
#include "runtime_string_shared.h"

extern "C" {

// Each printer splits into a `_raw` core (value, no newline) and a public
// wrapper (`_raw`+'\n'); multi-arg print calls the cores with spaces between, matching Python's sep=' ' end='\n'.

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

/// input(): scratch buffer is on the stack (was `static`, causing a race
/// between concurrent fire/thread callers); dragon_string_alloc copies it into a heap string before scope exit.
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

/// Overflow-checked power: multiplies via __builtin_smull_overflow, raising
/// OverflowError(22) on the first overflow. Negative exponents return 0/1 (int-only; no float promotion yet).
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

int64_t dragon_str_cmp(const char* a, const char* b);

static void dragon_empty_seq_raise(const char* fn) {
    char msg[64];
    snprintf(msg, sizeof(msg), "ValueError: %s() arg is an empty sequence", fn);
    dragon_raise_exc_cstr(90, msg);
}

static double dragon_list_load_f64(DragonList* list, int64_t i) {
    int64_t bits = dragon_list_load(list, i);
    double d;
    memcpy(&d, &bits, sizeof(d));
    return d;
}

int64_t dragon_min_list(DragonList* list) {
    if (!list || list->size == 0) dragon_empty_seq_raise("min");
    int64_t result = dragon_list_load(list, 0);
    for (int64_t i = 1; i < list->size; i++) {
        int64_t v = dragon_list_load(list, i);
        if (v < result) result = v;
    }
    return result;
}

int64_t dragon_max_list(DragonList* list) {
    if (!list || list->size == 0) dragon_empty_seq_raise("max");
    int64_t result = dragon_list_load(list, 0);
    for (int64_t i = 1; i < list->size; i++) {
        int64_t v = dragon_list_load(list, i);
        if (v > result) result = v;
    }
    return result;
}

double dragon_min_list_f64(DragonList* list) {
    if (!list || list->size == 0) dragon_empty_seq_raise("min");
    double result = dragon_list_load_f64(list, 0);
    for (int64_t i = 1; i < list->size; i++) {
        double v = dragon_list_load_f64(list, i);
        if (v < result) result = v;
    }
    return result;
}

double dragon_max_list_f64(DragonList* list) {
    if (!list || list->size == 0) dragon_empty_seq_raise("max");
    double result = dragon_list_load_f64(list, 0);
    for (int64_t i = 1; i < list->size; i++) {
        double v = dragon_list_load_f64(list, i);
        if (v > result) result = v;
    }
    return result;
}

const char* dragon_min_list_str(DragonList* list) {
    if (!list || list->size == 0) dragon_empty_seq_raise("min");
    int64_t result = dragon_list_load(list, 0);
    for (int64_t i = 1; i < list->size; i++) {
        int64_t v = dragon_list_load(list, i);
        if (dragon_str_cmp((const char*)(uintptr_t)v,
                           (const char*)(uintptr_t)result) < 0) result = v;
    }
    dragon_incref_tagged(result, TAG_STR);
    return (const char*)(uintptr_t)result;
}

const char* dragon_max_list_str(DragonList* list) {
    if (!list || list->size == 0) dragon_empty_seq_raise("max");
    int64_t result = dragon_list_load(list, 0);
    for (int64_t i = 1; i < list->size; i++) {
        int64_t v = dragon_list_load(list, i);
        if (dragon_str_cmp((const char*)(uintptr_t)v,
                           (const char*)(uintptr_t)result) > 0) result = v;
    }
    dragon_incref_tagged(result, TAG_STR);
    return (const char*)(uintptr_t)result;
}

int64_t dragon_sum_list(DragonList* list) {
    if (!list) return 0;
    int64_t result = 0;
    for (int64_t i = 0; i < list->size; i++) {
        result += dragon_list_load(list, i);
    }
    return result;
}

double dragon_sum_list_f64(DragonList* list) {
    if (!list) return 0.0;
    double result = 0.0;
    for (int64_t i = 0; i < list->size; i++) {
        result += dragon_list_load_f64(list, i);
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
    // Returns a list of tuples (index, element); each is incref'd and stored
    // with its tag so the tuple co-owns it, staying valid after the source list is destroyed.
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
    // Both inputs may carry different elem_tags; each tuple slot is tagged and
    // incref'd from its own source so the result outlives either list independently.
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

/// sorted(xs, reverse=...): like dragon_sorted but honors direction; sorts a
/// fresh copy, leaving the input untouched (Python's sorted() never mutates its argument).
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
    return (int64_t)dragon_str_content_hash(s);
}

int64_t dragon_id(int64_t val) {
    // For pointers, return the address; for values, return the value itself
    return val;
}

// G.4: Numeric functions

int64_t dragon_ord(const char* s) {
    DragonString* ds = (s && dragon_str_is_heap(s)) ? dragon_string_from_data(s) : NULL;
    int64_t len = ds ? ds->len : (s ? (int64_t)strlen(s) : 0);
    if (len != 1) {
        char msg[96];
        snprintf(msg, sizeof(msg),
                 "TypeError: ord() expected a character, but string of length %lld found",
                 (long long)len);
        dragon_raise_exc_cstr(80, msg);
    }
    return (int64_t)dragon_str_cp_at(s, ds, 0);
}

const char* dragon_chr(int64_t code) {
    // Python parity: chr() accepts U+0000..U+10FFFF, else ValueError.
    if (code < 0 || code > 0x10FFFF) {
        dragon_raise_exc_cstr(90, "ValueError: chr() arg not in range(0x110000)");
    }
    // UTF-8-encode into 1-4 bytes then decode via dragon_string_alloc into the
    // proper kind. The old `(char)code` truncated every code point to one byte - astral points became NUL.
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
    DragonString* src = dragon_str_is_heap(s) ? dragon_string_from_data(s) : NULL;
    if (src && src->kind == 4) {
        if (src->len > INT64_MAX - 2) {
            dragon_raise_exc_cstr(43, "MemoryError: string too large");
        }
        DragonString* out = dragon_string_alloc_ucs4(src->len + 2);
        uint32_t* o = (uint32_t*)out->data;
        o[0] = '\'';
        memcpy(o + 1, src->data, (size_t)src->len * 4);
        o[src->len + 1] = '\'';
        return out->data;
    }
    size_t len = src ? (size_t)src->len : strlen(s);
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

// File handle is FILE* cast to i8*; dragon_file_open returns it that way.

void* dragon_file_open(const char* filename, const char* mode) {
    FILE* f = fopen(filename, mode);
    if (!f) {
        // Unopenable path raises catchable FileNotFoundError(51), matching
        // io.File's constructor; the old NULL return made every later read silently return "".
        char buf[512];
        snprintf(buf, sizeof(buf),
                 "FileNotFoundError: [Errno 2] No such file or directory: '%s'",
                 filename ? filename : "");
        dragon_raise_exc_cstr(51, buf);
    }
    return (void*)f;
}

// Write a string to stderr + newline, used by stdlib/logging.dr. Explicit
// fputs+flush keeps interleaved log lines from concurrent callers from tangling.
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

// Returns argv[i] as a fresh owned (+1) DragonString, never interned: an
// interned result is immortal with no dedup, so reading sys.argv in a loop would grow RSS unbounded.
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

/// Read raw bytes (no UTF-8 detection, kind=1, one byte per cp) - for binary
/// content. Must NOT route through dragon_string_alloc: a PNG's 0x89 byte trips its UTF-8 path, expanding 309 KB to 442 KB.
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

// Reads one line through '\n' or EOF, growing its buffer so there's no line
// limit (the old fixed char[4096] silently split lines over 4095 bytes). "" at EOF signals stop.
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

#define GEN_STATE_INITIAL   0
#define GEN_STATE_SUSPENDED 1
#define GEN_STATE_EXHAUSTED 2

/// D030: Create a generator with a per-callsite typed args struct: trampoline
/// reads/calls the body, args is a copied typed struct (field 0 = self), args_decref_fn frees heap captures at destroy.
DragonGenerator* dragon_generator_create_typed(
    void (*trampoline)(mco_coro*), void* args, int64_t args_size,
    void (*args_decref_fn)(void*)) {
    DragonGenerator* gen = (DragonGenerator*)dragon_xcalloc_n(1, sizeof(DragonGenerator));
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

/// Called when the body raised past every internal try/except; flags it so
/// dragon_generator_next re-raises in the caller's context (type/msg/obj already live in gen->exc_vt).
void dragon_generator_set_raised(void* gen_ptr) {
    DragonGenerator* gen = (DragonGenerator*)gen_ptr;
    if (gen) gen->pending_exc = 1;
}

/// Release the +1 the yielded-value slot owns for heap tags (mirrors the
/// DCLEAN_UNION unwind arm; TAG_CLOSURE checked before the generic heap range).
static void dragon_generator_release_yielded(DragonGenerator* gen) {
    int64_t tag = gen->yielded_tag;
    void* p = (void*)(uintptr_t)gen->yielded_value;
    if (!p) { gen->yielded_tag = 0; return; }
    if (tag == TAG_STR) dragon_decref_str((const char*)p);
    else if (tag == DRAGON_TAG_CLOSURE) dragon_decref_callable(p);
    else if (tag >= TAG_LIST) dragon_decref(p);
    gen->yielded_tag = 0;
}

/// Yield a value from inside the generator body. Called by compiled yield
/// expressions. A heap `value` transfers a +1 into the slot (codegen increfs
/// borrowed sources first); the previous slot value's +1 is released here.
void dragon_generator_yield(void* gen_ptr, int64_t value, int64_t tag) {
    DragonGenerator* gen = (DragonGenerator*)gen_ptr;
    dragon_generator_release_yielded(gen);
    gen->yielded_value = value;
    gen->yielded_tag = tag;
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
    // Install the generator's own exception context, isolated from the caller's:
    // sharing one stack let the caller's push/pop clobber a suspended try frame, causing a wrong-frame longjmp/SIGSEGV.
    if (!gen->exc_vt) {
        gen->exc_vt = (DragonVThread*)dragon_xcalloc_n(1, sizeof(DragonVThread));
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

    // The body raised uncaught; the trampoline barrier captured it and returned
    // normally. Re-raise now in the caller's context, never longjmp-ing across the coroutine boundary.
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

/// Destroy a generator (coroutine + args + struct); D030: heap-typed captured
/// args are decref'd by a per-callsite codegen fn, replacing the old tagged-loop approach.
void dragon_generator_destroy(void* gen_ptr) {
    DragonGenerator* gen = (DragonGenerator*)gen_ptr;
    if (!gen) return;
    dragon_generator_release_yielded(gen);
    if (gen->coro) {
        // mco_destroy needs MCO_DEAD/MCO_SUSPENDED; a generator abandoned mid-
        // resume (exception propagated out) is left MCO_RUNNING - skip destroy (its stack leaks, a known minicoro limitation) and still free args below.
        mco_state st = mco_status(gen->coro);
        if (st == MCO_DEAD || st == MCO_SUSPENDED) mco_destroy(gen->coro);
    }
    if (gen->args) {
        if (gen->args_decref_fn) gen->args_decref_fn(gen->args);
        free(gen->args);
    }
    // Free the isolated exception context; only present if the generator was
    // resumed at least once. A generator abandoned mid-iteration still holds
    // its registered locals on the cleanup stack and may hold the last raise's
    // message/instance - drain and release before freeing the arrays.
    if (gen->exc_vt) {
        dragon_cleanup_stack_drain(&gen->exc_vt->cleanup, 0);
        free(gen->exc_vt->cleanup.vals);
        free(gen->exc_vt->cleanup.kinds);
        free(gen->exc_vt->cleanup.tags);
        dragon_decref_str_dispatch(gen->exc_vt->exc_msg);
        if (gen->exc_vt->exc_obj) dragon_decref_dispatch(gen->exc_vt->exc_obj);
        free(gen->exc_vt);
    }
    free(gen);
}

// class_id → descriptor lookup table (for isinstance ancestor walk)
static DragonClassDescriptor* __class_descriptor_table[DRAGON_MAX_CLASS_IDS];

/// Create a class descriptor (called from CodeGen at module init, after
/// class_id registration); `doc` is the .rodata docstring or NULL, powering Cls.__doc__.
int64_t dragon_class_descriptor_create(const char* name, int64_t constructor,
                                        int64_t class_id, int64_t parent_descriptor,
                                        const char* doc) {
    DragonClassDescriptor* desc = (DragonClassDescriptor*)dragon_xcalloc_n(1, sizeof(DragonClassDescriptor));
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

// ADR 025 removal: dragon_class_descriptor_call() deleted (D021: construction
// resolves statically to ClassName_new). `constructor` field retained only for dragon_dir()'s __init__ listing.

/// Get the class name from a descriptor. Returns a DragonString data ptr as i64.
int64_t dragon_class_descriptor_get_name(int64_t descriptor) {
    DragonClassDescriptor* desc = (DragonClassDescriptor*)(void*)descriptor;
    if (!desc || !desc->name) return 0;
    return (int64_t)dragon_string_alloc(desc->name, (int64_t)strlen(desc->name));
}

/// Get the class docstring: raw .rodata C string (kind=1 to Dragon), or NULL
/// (flows through dragon_print_str etc. as Python's None per the niche-ptr Optional[str] ABI).
const char* dragon_class_descriptor_get_doc(int64_t descriptor) {
    if (!descriptor) return NULL;
    return ((DragonClassDescriptor*)(void*)descriptor)->doc;
}

/// Get the class docstring for an instance (class_id -> descriptor -> doc);
/// NULL when null, has no class_id, or the class has no docstring.
const char* dragon_instance_get_doc(void* instance) {
    if (!instance) return NULL;
    DragonObjectHeader* h = (DragonObjectHeader*)instance;
    uint16_t cid = h->class_id;
    if (cid == 0 || cid >= DRAGON_MAX_CLASS_IDS) return NULL;
    DragonClassDescriptor* desc = __class_descriptor_table[cid];
    if (!desc) return NULL;
    return desc->doc;
}

/// Get the class name for an instance (class_id -> descriptor -> name), or
/// NULL. Used by runtime_box.cpp to render `<ClassName instance>` instead of misreading bytes under a TAG_BYTES/TAG_LIST collision.
const char* dragon_instance_class_name(void* instance) {
    if (!instance) return NULL;
    DragonObjectHeader* h = (DragonObjectHeader*)instance;
    uint16_t cid = h->class_id;
    if (cid == 0 || cid >= DRAGON_MAX_CLASS_IDS) return NULL;
    DragonClassDescriptor* desc = __class_descriptor_table[cid];
    if (!desc) return NULL;
    return desc->name;
}

// ADR 025 removal: dragon_isinstance_runtime() deleted. isinstance() resolves
// statically at codegen (2nd arg must name a compile-time-known class); the inheritance walk moved to classParentNames.

/// Set field metadata (offsets/widths in bytes) on a class descriptor. Both
/// are needed for a correct getattr read: a fixed i64 read at a GEP index would misalign narrow fields and could read past the allocation.
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

/// Find a field by name (including parent chain); returns its BYTE offset and
/// writes its width to *out_width, or -1 if not found.
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

/// Read a field into a zero-extended i64, reading exactly `width` bytes so a
/// narrow field never over-reads adjacent fields or leaks uninitialized bytes.
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

// D033: method reflection sets per-class method metadata and looks up methods
// by name across the inheritance chain, mirroring the field-reflection pattern above.

/// Attach method-name/fn-ptr/kind arrays to a class descriptor (once per class,
/// from module-init). Each class advertises its own methods; inheritance resolves at lookup via the parent chain.
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

/// Find a method's fn pointer by name, walking the parent chain so an
/// inherited method resolves to the parent's fn ptr; NULL on miss.
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

/// Like find_method but returns the codegen-emitted bound thunk (closure ABI:
/// (user_args..., env)); getattr() uses it to build a DragonClosure the existing call path can invoke.
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

/// Codegen-supplied setter for the bound-thunks array (after set_methods); kept
/// separate so a class without bound thunks (e.g. a data class) skips a stub array.
void dragon_class_descriptor_set_method_bound_thunks(int64_t descriptor,
                                                      void** bound_thunks) {
    DragonClassDescriptor* desc = (DragonClassDescriptor*)(void*)descriptor;
    if (!desc) return;
    desc->method_bound_thunks = bound_thunks;
}

/// Find a method's kind (0=instance, 1=static, 2=classmethod) by name, -1 if
/// not found; getattr() uses this to decide whether to bind `self`.
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

/// dir(obj)/dir(Cls): walks the MRO collecting unique field+method names (plus
/// "__init__" if any ancestor has a constructor), sorted; `is_descriptor` selects descriptor vs instance input. Caller owns the +1 list.
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

    // Two-pass: count an upper bound to stack-allocate a flat array, then
    // collect with linear-scan dedupe (fine under ~30 attrs, still beats std::set).
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

    // Borrowed C-string pointers (descriptor-owned .rodata); we shuffle
    // pointers until the final dragon_string_alloc copies them into heap strings.
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

/// Forward decls for closure construction (defined later); keeps the getattr
/// extension self-contained. env_alloc gained the multi-op gc_fn + a trackable gate.
void* dragon_env_alloc(int64_t total_size,
                       void (*gc_fn)(void*, int32_t, dragon_gc_visit_fn, void*),
                       int32_t trackable);
void* dragon_closure_create(void* fn_ptr, void* env);

/// Shared GC hook for bound-method envs (body: `{ void* self }`, +1 captured).
/// Multi-op so a bound method stored back on `self` (a real cycle) is collectable; one copy serves every class.
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

/// Build a bound-method closure: env carries self (+1), wrapped in a
/// DragonClosure(fn_ptr=thunk_fn); the existing closure call path invokes it as thunk_fn(user_args..., env).
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

/// hasattr(obj, "name"): 1 if field OR method exists, 0 otherwise (D033
/// extension: also walks the method table, so a method name returns true too).
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

/// Look up a bound method as a callable (DragonClosure* as i64, or 0). Resolves
/// descriptor -> bound thunk -> closure with self in env; static methods return the raw fn pointer instead.
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
    char msg[160];
    snprintf(msg, sizeof(msg), "AttributeError: object has no attribute '%s'",
             attr_name ? attr_name : "");
    dragon_raise_exc_cstr(25, msg);
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

/// D030: Allocate a closure env (total_size, gc_fn = multi-op DEALLOC/TRAVERSE/
/// CLEAR hook or NULL, trackable=1 iff it can close a cycle so the collector can reclaim it). Refcount starts at 1.
void* dragon_env_alloc(int64_t total_size,
                       void (*gc_fn)(void*, int32_t, dragon_gc_visit_fn, void*),
                       int32_t trackable) {
    DragonEnv* env = (DragonEnv*)dragon_xcalloc_n(total_size, 1);
    dragon_obj_init(&env->header, DRAGON_TAG_ENV);
    env->gc_fn = gc_fn;
    if (trackable) dragon_gc_track(env);
    return env;
}

/// Create a closure(fn_ptr, env), taking ownership of env's existing +1 ref
/// (no extra incref). A closure with a tracked env is itself tracked, so instance->closure->env cycles are visible to the collector.
void* dragon_closure_create(void* fn_ptr, void* env) {
    DragonClosure* cls = (DragonClosure*)dragon_xcalloc_n(1, sizeof(DragonClosure));
    dragon_obj_init(&cls->header, DRAGON_TAG_CLOSURE);
    cls->fn_ptr = fn_ptr;
    cls->env = (DragonEnv*)env;
    if (env && (((DragonEnv*)env)->header.gc_flags & GC_FLAG_TRACKED))
        dragon_gc_track(cls);
    return cls;
}

/// Dealloc a closure: decref the env (via dragon_decref_dispatch for the atomic
/// variant), then free self. Guarded since clear_refs may have already NULLed cls->env.
void dragon_closure_dealloc(DragonClosure* cls) {
    if (!cls) return;
    if (cls->env) {
        dragon_decref_dispatch(&cls->env->header);
    }
    free(cls);
}

/// Dealloc an env: run the per-site gc_fn DEALLOC op (decrefs heap captures),
/// then free. Capture slots NULLed by clear_refs decref to nothing.
void dragon_env_dealloc(DragonEnv* env) {
    if (!env) return;
    if (env->gc_fn) {
        env->gc_fn(env, DRAGON_ENV_OP_DEALLOC, nullptr, nullptr);
    }
    free(env);
}

// Heap-boxed mutable cells back `nonlocal`: the owning function holds one ref,
// each capturing closure env holds another. NOT yet thread-safe: a mutated nonlocal captured by a fire-spawned vthread is racy.

/// Allocate a cell with initial i64 `value`: `kind` records its DragonValueTag
/// for dealloc/overwrite, `holds_heap` flags a heap pointer. Caller must +1 incref `value` before this call.
void* dragon_cell_alloc(int64_t value, int32_t kind, int32_t holds_heap) {
    DragonCell* c = (DragonCell*)dragon_xcalloc_n(1, sizeof(DragonCell));
    dragon_obj_init(&c->header, DRAGON_TAG_CELL);
    c->value = value;
    c->kind = kind;
    c->holds_heap = holds_heap;
    return c;
}

/// Read the cell's value; a borrowed reference for heap kinds, so the caller
/// increfs if extending its lifetime past the cell's (per codegen's emitIncrefByKind discipline).
int64_t dragon_cell_get(void* cell) {
    if (!cell) return 0;
    return ((DragonCell*)cell)->value;
}

/// Replace the cell's value, returning the old one for the caller to decref
/// (the runtime does NOT decref old / incref new; kind-specific dispatch stays at the call site).
int64_t dragon_cell_set(void* cell, int64_t new_value) {
    DragonCell* c = (DragonCell*)cell;
    int64_t old = c->value;
    c->value = new_value;
    return old;
}

// Tag-aware incref/decref for Callable[[...], R] fields: a bare fn pointer has
// no header (no-op), a DragonClosure does (real RC via type_tag), letting a class field own a capturing closure without touching bare fn ptrs.
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
