/// Dragon Runtime - Box helpers (D039 Phase 3+)
///
/// `%dragon.box` is Dragon's `interface{}` - a 16-byte {i64 tag, i64 payload}
/// value used to carry heterogeneous typed values through subscript reads,
/// function returns, class fields, and any other "Any"-shaped slot.
///
/// This file holds the runtime helpers that dispatch on a box's tag:
///   - dragon_print_box(box)         → print(anyValue)
///   - (future) dragon_box_to_str    → str(anyValue), f-string interpolation
///   - (future) dragon_box_eq        → boxA == boxB
///
/// Tag taxonomy matches the existing DragonValueTag enum in runtime_internal.h
/// (TAG_INT=0, TAG_STR=1, TAG_FLOAT=2, TAG_BOOL=3, TAG_NONE=4, TAG_LIST=5,
///  TAG_DICT=6, TAG_BYTES=7).

#include "runtime_internal.h"
#include <cstdio>
#include <cstring>
#include <cmath>
#include <cstdint>

extern "C" {

// Forward declarations for runtime helpers used by dragon_print_box.
// (These live in runtime_list.cpp / runtime_dict.cpp / runtime_string.cpp;
//  runtime_internal.h doesn't declare them publicly but they're stable
//  extern "C" symbols.)
void dragon_print_dict(DragonDict* d);
void dragon_print_list_box(DragonListBox* l);
// `_raw` (no trailing newline) variants used by dragon_print_box_raw.
void dragon_print_dict_raw(DragonDict* d);
void dragon_print_list_box_raw(DragonListBox* l);
void dragon_print_list_int_raw(DragonList* l);
// Tag-aware recursive list printer (runtime_collections.cpp). A monomorphic
// list reached through a box carries its element kind in `elem_tag`, so it must
// render via the repr builder, not the int-only printer.
void dragon_print_list_nested_raw(DragonList* l);
int64_t dragon_str_eq(const char* a, const char* b);
const char* dragon_bool_to_str(int64_t value);
const char* dragon_string_alloc(const char* src, int64_t len);
void dragon_incref_str(const char* s);
// Container-deep equality helpers; defined in runtime_list.cpp / dict.cpp /
// collections.cpp. Used to back the TAG_LIST / TAG_DICT / TAG_BYTES paths
// of dragon_box_eq below (mirrors Python's `[1,2,3] == [1,2,3]` semantics).
int64_t dragon_list_eq(void* a, void* b);
int64_t dragon_dict_eq(DragonDict* a, DragonDict* b);
int64_t dragon_bytes_eq(DragonBytes* a, DragonBytes* b);

// Class-name lookup for an instance pointer (header.class_id -> descriptor
// name), defined in runtime_builtins.cpp. Returns a .rodata C string or NULL.
// Used to render a class instance that reached a box under the TAG_BYTES /
// TAG_CLASS value-tag collision (see the header-gate comment in the printers
// below) as `<ClassName instance>` instead of misreading it as bytes.
const char* dragon_instance_class_name(void* instance);

// DragonBox (the `%dragon.box = { i64, i64 }` Any/Union value) is defined once
// in runtime_internal.h.

/// Print a box's value followed by a newline. Tag dispatches to the right
/// per-type formatter. This is the `print(value)` lowering target when
/// `value` is `Any` / `Union[...]` / `T | None`.
///
/// Containers (TAG_LIST / TAG_DICT) print using the existing per-container
/// helpers - for lists, we default to int-format because the box has no
/// element-type metadata. Drs and other typed-data callers should narrow via
/// isinstance + the typed list_T accessor for richer formatting.
// Print a boxed value with NO trailing newline. Container cases delegate to
// the `_raw` container printers so nested newlines don't leak. The public
// dragon_print_box wrapper adds the single trailing '\n'.
void dragon_print_box_raw(DragonBox box) {
    int64_t tag = box.tag;
    int64_t value = box.payload;

    switch (tag) {
        case TAG_INT:
            printf("%lld", (long long)value);
            break;
        case TAG_STR: {
            const char* s = (const char*)(uintptr_t)value;
            if (!s) { printf("None"); break; }
            // Mirror dragon_print_str's UTF-8 encoding path for kind=4 strings.
            int64_t byte_len = 0;
            char* enc = dragon_str_to_utf8_alloc(s, &byte_len);
            if (enc) {
                fwrite(enc, 1, (size_t)byte_len, stdout);
                free(enc);
            } else {
                printf("%s", s);
            }
            break;
        }
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
        case TAG_LIST: {
            // The box's payload may be a DragonList (monomorphic) OR a
            // DragonListBox (list[Any] with 16B/elem stride) OR - because
            // codegen packs tuple and set into value-tag 5 as well - a
            // DragonTuple / DragonSet. Dispatch by the object's REAL header
            // type_tag; the value-tag alone cannot tell these apart, and
            // reading a set/tuple through DragonList offsets walks wild memory
            // (a set's `states` pointer read as `size` -> huge loop bound).
            DragonObjectHeader* h = (DragonObjectHeader*)(uintptr_t)value;
            if (!h) { printf("None"); break; }
            switch (h->type_tag) {
                case DRAGON_TAG_LIST_BOX:
                    dragon_print_list_box_raw((DragonListBox*)h);
                    break;
                case DRAGON_TAG_LIST:
                    // A monomorphic DragonList carries its element kind in
                    // `elem_tag`; render tag-aware so a nested list[str] /
                    // list[float] doesn't print payloads as ints.
                    dragon_print_list_nested_raw((DragonList*)h);
                    break;
                default:
                    // Tuple/set (or any future type collapsed into tag 5)
                    // reached here without a dedicated raw printer in this TU.
                    // Print a safe, honest placeholder rather than misread the
                    // struct. (A full tuple/set repr through box printing is a
                    // follow-up; not crashing / not lying is the contract.)
                    printf("<%s object at 0x%llx>",
                           dragon_instance_class_name(h) ? dragon_instance_class_name(h)
                                                         : "object",
                           (unsigned long long)value);
                    break;
            }
            break;
        }
        case TAG_DICT: {
            // Same header gate: only a real dict goes to the dict printer.
            DragonObjectHeader* h = (DragonObjectHeader*)(uintptr_t)value;
            if (!h) { printf("None"); break; }
            if (h->type_tag == DRAGON_TAG_DICT) {
                dragon_print_dict_raw((DragonDict*)h);
            } else {
                printf("<%s object at 0x%llx>",
                       dragon_instance_class_name(h) ? dragon_instance_class_name(h)
                                                     : "object",
                       (unsigned long long)value);
            }
            break;
        }
        case TAG_BYTES: {
            // value-tag 7 (TAG_BYTES) is ALSO how codegen tags a class
            // instance (varKindToTag: ClassInstance -> 7). A real DragonBytes
            // object has header type_tag == DRAGON_TAG_BYTES; a class instance
            // has DRAGON_TAG_CLASS. Trusting the value-tag and reading `bv->len`
            // /`bv->data` off a class instance dumped hundreds of KB of adjacent
            // process memory as fake "bytes" (verified: `print(Dog("rex"))`
            // emitted b'rex\x00...' + a 668 KB OOB spill) - a memory-safety
            // disclosure AND a silent lie. Gate on the real header.
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
                // Class instance boxed into Any. Mirror the direct-print
                // fallback for a dunder-less instance (`<ClassName instance>`).
                const char* nm = dragon_instance_class_name(h);
                if (nm) printf("<%s instance>", nm);
                else    printf("<object at 0x%llx>", (unsigned long long)value);
            }
            break;
        }
        default:
            // Unknown tag: print as raw i64 for debuggability. Future tag
            // additions should land here as explicit cases.
            printf("<box tag=%lld payload=%lld>",
                   (long long)tag, (long long)value);
            break;
    }
}
void dragon_print_box(DragonBox box) {
    dragon_print_box_raw(box);
    putchar('\n');
}

/// D039: str(anyValue) / f-string interpolation of Any-typed values.
///
/// Tag-dispatches the box to the right per-type formatter and returns a
/// refcounted heap DragonString. Caller owns the returned reference (matches
/// the "owned" convention for runtime str-returning helpers in Expressions.cpp).
/// For TAG_STR we incref the already-heap'd payload so the caller's release
/// path is uniform (no need to distinguish borrowed vs owned by tag).
///
/// List / dict / bytes formatting reuses Python's repr-like shape via
/// snprintf placeholders rather than running the full container printers -
/// hot path stays a single heap allocation, no buffered intermediate writes.
const char* dragon_box_to_str(DragonBox box) {
    char buf[64];
    switch (box.tag) {
        case TAG_INT:
            return dragon_int_to_str(box.payload);
        case TAG_BOOL:
            // dragon_bool_to_str returns the immortal "True"/"False" globals.
            return dragon_bool_to_str(box.payload);
        case TAG_FLOAT: {
            double fv;
            memcpy(&fv, &box.payload, sizeof(double));
            return dragon_float_to_str(fv);
        }
        case TAG_NONE:
            // "None" is a common interned constant; allocating a fresh heap
            // copy keeps the +1 ref contract simple for the caller.
            return dragon_string_alloc("None", 4);
        case TAG_STR: {
            const char* s = (const char*)(uintptr_t)box.payload;
            if (!s) return dragon_string_alloc("None", 4);
            dragon_incref_str(s);
            return s;
        }
        case TAG_LIST: {
            // Tag the container by its address; richer container formatting
            // (`[1, 2, 3]`) is a follow-up if/when print(list[Any]) gets a
            // string-returning sibling. TODO: Not optimal, revisit later but
            // for now this beats crashing. Gate on the real header so a tuple/set
            // (also value-tag 5) is not mislabelled `<list ...>`.
            DragonObjectHeader* h = (DragonObjectHeader*)(uintptr_t)box.payload;
            const char* nm = (h && h->type_tag != DRAGON_TAG_LIST &&
                              h->type_tag != DRAGON_TAG_LIST_BOX)
                                 ? dragon_instance_class_name(h) : nullptr;
            if (nm) snprintf(buf, sizeof(buf), "<%s object at 0x%llx>", nm,
                             (unsigned long long)box.payload);
            else    snprintf(buf, sizeof(buf), "<list 0x%llx>",
                             (unsigned long long)box.payload);
            return dragon_string_alloc(buf, (int64_t)strlen(buf));
        }
        case TAG_DICT: {
            snprintf(buf, sizeof(buf), "<dict 0x%llx>", (unsigned long long)box.payload);
            return dragon_string_alloc(buf, (int64_t)strlen(buf));
        }
        case TAG_BYTES: {
            // value-tag 7 is bytes OR a class instance (varKindToTag collision).
            // Mislabelling an instance as `<bytes 0x...>` is a silent lie; name
            // it honestly via its class descriptor when the header says it is
            // not really bytes.
            DragonObjectHeader* h = (DragonObjectHeader*)(uintptr_t)box.payload;
            if (h && h->type_tag != DRAGON_TAG_BYTES) {
                const char* nm = dragon_instance_class_name(h);
                if (nm) snprintf(buf, sizeof(buf), "<%s instance>", nm);
                else    snprintf(buf, sizeof(buf), "<object at 0x%llx>",
                                 (unsigned long long)box.payload);
            } else {
                snprintf(buf, sizeof(buf), "<bytes 0x%llx>",
                         (unsigned long long)box.payload);
            }
            return dragon_string_alloc(buf, (int64_t)strlen(buf));
        }
        default: {
            snprintf(buf, sizeof(buf), "<box tag=%lld payload=%lld>",
                     (long long)box.tag, (long long)box.payload);
            return dragon_string_alloc(buf, (int64_t)strlen(buf));
        }
    }
}

/// D039 Phase 10: box == box.
///
/// Returns 1 if both boxes have the same tag AND the same value at that tag's
/// native representation. Tag-mismatch always returns 0 (no implicit numeric
/// promotion - matches Python's `==` between different types: `1 == "1"` is
/// False).
///
/// Mostly used by codegen lowering of `==` / `!=` between Any-typed operands.
int64_t dragon_box_eq(DragonBox a, DragonBox b) {
    if (a.tag != b.tag) return 0;
    switch (a.tag) {
        case TAG_INT:
        case TAG_BOOL:
            return a.payload == b.payload ? 1 : 0;
        case TAG_FLOAT: {
            double fa, fb;
            memcpy(&fa, &a.payload, sizeof(double));
            memcpy(&fb, &b.payload, sizeof(double));
            return fa == fb ? 1 : 0;
        }
        case TAG_NONE:
            return 1;
        case TAG_STR: {
            const char* sa = (const char*)(uintptr_t)a.payload;
            const char* sb = (const char*)(uintptr_t)b.payload;
            if (sa == sb) return 1;
            if (!sa || !sb) return 0;
            return dragon_str_eq(sa, sb);
        }
        case TAG_LIST:
            // Pointer-identity fast path, then recursive elementwise compare
            // via dragon_list_eq (handles all four list variants: I64, F64,
            // Ptr, Box).
            if (a.payload == b.payload) return 1;
            if (!a.payload || !b.payload) return 0;
            return dragon_list_eq((void*)(uintptr_t)a.payload,
                                  (void*)(uintptr_t)b.payload);
        case TAG_DICT:
            // Pointer-identity fast path, then key-by-key compare. Any-boxed
            // dicts are dict[str, Any] by construction in the type system,
            // so we use the str-keyed eq helper.
            if (a.payload == b.payload) return 1;
            if (!a.payload || !b.payload) return 0;
            return dragon_dict_eq((DragonDict*)(uintptr_t)a.payload,
                                  (DragonDict*)(uintptr_t)b.payload);
        case TAG_BYTES:
            if (a.payload == b.payload) return 1;
            if (!a.payload || !b.payload) return 0;
            return dragon_bytes_eq((DragonBytes*)(uintptr_t)a.payload,
                                   (DragonBytes*)(uintptr_t)b.payload);
        default:
            return a.payload == b.payload ? 1 : 0;
    }
}

//===----------------------------------------------------------------------===//
// D039 Phase 11: box arithmetic - `Any OP Any` / `Any OP native` / `native OP
// Any` for + - * / // % **.
//
// The result type of arithmetic on a box depends on the RUNTIME tags (int+int
// → int, int+float → float, str+str → concat), so the only correct lowering is
// runtime dispatch returning a box. Codegen boxes the native operand (via its
// compile-time tag) and calls this; a native target slot then unboxes the
// result with the D039 Phase-7a runtime TypeError check. Matches Dragon's OWN
// typed operator surface (e.g. list+list raises here because list[int]+list[int]
// is also unsupported), not Python's full surface.
//===----------------------------------------------------------------------===//

// Op codes - MUST match the codegen mapping (binopOpcodeForToken) in
// Expressions.cpp / Assign.cpp.
enum {
    DRAGON_BINOP_ADD = 0, DRAGON_BINOP_SUB = 1, DRAGON_BINOP_MUL = 2,
    DRAGON_BINOP_TRUEDIV = 3, DRAGON_BINOP_FLOORDIV = 4,
    DRAGON_BINOP_MOD = 5, DRAGON_BINOP_POW = 6,
};

// Delegated heap ops (defined in other TUs; signatures must match).
const char*    dragon_str_concat(const char* a, const char* b);
const char*    dragon_str_repeat(const char* s, int64_t n);
int64_t        dragon_str_cmp(const char* a, const char* b);
DragonBytes*   dragon_bytes_concat(DragonBytes* a, DragonBytes* b);
DragonBytes*   dragon_bytes_repeat(DragonBytes* b, int64_t n);
DragonListBox* dragon_list_box_repeat(DragonListBox* src, int64_t count);
int64_t        dragon_pow_int(int64_t base, int64_t exp);
double         dragon_pow_float(double base, double exp);
int64_t        dragon_list_cmp(void* a, void* b);

static const char* dragon_box_type_name(int64_t valueTag, int64_t payload) {
    switch (valueTag) {
        case TAG_INT:   return "int";
        case TAG_STR:   return "str";
        case TAG_FLOAT: return "float";
        case TAG_BOOL:  return "bool";
        case TAG_NONE:  return "NoneType";
        case TAG_LIST:  return "list";
        case TAG_DICT:  return "dict";
        case TAG_BYTES: {  // value-tag 7 is bytes OR a class instance
            auto* h = (DragonObjectHeader*)(uintptr_t)payload;
            return (h && h->type_tag == DRAGON_TAG_BYTES) ? "bytes" : "object";
        }
        default: return "object";
    }
}

static const char* dragon_binop_symbol(int64_t op) {
    switch (op) {
        case DRAGON_BINOP_ADD:      return "+";
        case DRAGON_BINOP_SUB:      return "-";
        case DRAGON_BINOP_MUL:      return "*";
        case DRAGON_BINOP_TRUEDIV:  return "/";
        case DRAGON_BINOP_FLOORDIV: return "//";
        case DRAGON_BINOP_MOD:      return "%";
        case DRAGON_BINOP_POW:      return "**";
        default:                    return "?";
    }
}

static inline DragonBox dragon_mkbox(int64_t tag, int64_t payload) {
    DragonBox r; r.tag = tag; r.payload = payload; return r;
}
static inline DragonBox dragon_mkbox_f(double v) {
    DragonBox r; r.tag = TAG_FLOAT; memcpy(&r.payload, &v, sizeof(double)); return r;
}

static void dragon_box_binop_typeerror(int64_t op, DragonBox a, DragonBox b) {
    char buf[128];
    snprintf(buf, sizeof(buf),
             "TypeError: unsupported operand type(s) for %s: '%s' and '%s'",
             dragon_binop_symbol(op),
             dragon_box_type_name(a.tag, a.payload),
             dragon_box_type_name(b.tag, b.payload));
    dragon_raise_exc_cstr(80, buf);  // longjmps - never returns
}

// Python integer floor-div & modulo: round toward -inf; mod sign follows the
// divisor (C's / and % truncate toward zero, so we correct).
static inline int64_t dragon_py_ifloordiv(int64_t a, int64_t b) {
    int64_t q = a / b;
    if ((a % b != 0) && ((a < 0) != (b < 0))) q--;
    return q;
}
static inline int64_t dragon_py_imod(int64_t a, int64_t b) {
    int64_t r = a % b;
    if (r != 0 && ((r < 0) != (b < 0))) r += b;
    return r;
}
static inline double dragon_py_fmod(double a, double b) {
    double r = fmod(a, b);
    if (r != 0.0 && ((r < 0.0) != (b < 0.0))) r += b;
    return r;
}

/// Box arithmetic dispatcher. `a`/`b` are operands (native sides already boxed
/// by codegen); `op` is a DRAGON_BINOP_* code. Returns a result box owning +1
/// on any heap payload it produces (str/bytes/list). Raises TypeError for
/// unsupported combinations and ZeroDivisionError on /, //, % by zero.
DragonBox dragon_box_binop(DragonBox a, DragonBox b, int64_t op) {
    const int64_t ta = a.tag, tb = b.tag;
    const bool aNum = (ta == TAG_INT || ta == TAG_BOOL || ta == TAG_FLOAT);
    const bool bNum = (tb == TAG_INT || tb == TAG_BOOL || tb == TAG_FLOAT);

    // ---- Numeric tower (bool participates as 0/1, promotes to int) ----
    if (aNum && bNum) {
        const bool aF = (ta == TAG_FLOAT), bF = (tb == TAG_FLOAT);
        const bool useFloat = aF || bF || op == DRAGON_BINOP_TRUEDIV;
        if (useFloat) {
            double fa, fb;
            if (aF) memcpy(&fa, &a.payload, sizeof(double)); else fa = (double)a.payload;
            if (bF) memcpy(&fb, &b.payload, sizeof(double)); else fb = (double)b.payload;
            switch (op) {
                case DRAGON_BINOP_ADD: return dragon_mkbox_f(fa + fb);
                case DRAGON_BINOP_SUB: return dragon_mkbox_f(fa - fb);
                case DRAGON_BINOP_MUL: return dragon_mkbox_f(fa * fb);
                case DRAGON_BINOP_TRUEDIV:
                    if (fb == 0.0) dragon_raise_exc_cstr(23, "ZeroDivisionError: float division by zero");
                    return dragon_mkbox_f(fa / fb);
                case DRAGON_BINOP_FLOORDIV:
                    if (fb == 0.0) dragon_raise_exc_cstr(23, "ZeroDivisionError: float floor division by zero");
                    return dragon_mkbox_f(floor(fa / fb));
                case DRAGON_BINOP_MOD:
                    if (fb == 0.0) dragon_raise_exc_cstr(23, "ZeroDivisionError: float modulo");
                    return dragon_mkbox_f(dragon_py_fmod(fa, fb));
                case DRAGON_BINOP_POW: return dragon_mkbox_f(dragon_pow_float(fa, fb));
            }
        } else {
            const int64_t ia = a.payload, ib = b.payload;
            switch (op) {
                case DRAGON_BINOP_ADD: return dragon_mkbox(TAG_INT, ia + ib);
                case DRAGON_BINOP_SUB: return dragon_mkbox(TAG_INT, ia - ib);
                case DRAGON_BINOP_MUL: return dragon_mkbox(TAG_INT, ia * ib);
                case DRAGON_BINOP_FLOORDIV:
                    if (ib == 0) dragon_raise_exc_cstr(23, "ZeroDivisionError: integer division or modulo by zero");
                    return dragon_mkbox(TAG_INT, dragon_py_ifloordiv(ia, ib));
                case DRAGON_BINOP_MOD:
                    if (ib == 0) dragon_raise_exc_cstr(23, "ZeroDivisionError: integer division or modulo by zero");
                    return dragon_mkbox(TAG_INT, dragon_py_imod(ia, ib));
                case DRAGON_BINOP_POW:
                    // Python: int ** negative int → float.
                    if (ib < 0) return dragon_mkbox_f(dragon_pow_float((double)ia, (double)ib));
                    return dragon_mkbox(TAG_INT, dragon_pow_int(ia, ib));
            }
        }
    }

    // ---- str: concat (+) and repeat (*) ----
    if (op == DRAGON_BINOP_ADD && ta == TAG_STR && tb == TAG_STR) {
        const char* r = dragon_str_concat((const char*)(uintptr_t)a.payload,
                                          (const char*)(uintptr_t)b.payload);
        return dragon_mkbox(TAG_STR, (int64_t)(uintptr_t)r);
    }
    if (op == DRAGON_BINOP_MUL &&
        ((ta == TAG_STR && (tb == TAG_INT || tb == TAG_BOOL)) ||
         (tb == TAG_STR && (ta == TAG_INT || ta == TAG_BOOL)))) {
        const char* s = (const char*)(uintptr_t)(ta == TAG_STR ? a.payload : b.payload);
        int64_t n = (ta == TAG_STR ? b.payload : a.payload);
        return dragon_mkbox(TAG_STR, (int64_t)(uintptr_t)dragon_str_repeat(s, n));
    }

    // ---- bytes: concat (+) and repeat (*). value-tag 7 is bytes XOR class;
    // gate on the object header so a class instance never reaches bytes ops. ----
    auto isBytes = [](int64_t tag, int64_t payload) -> bool {
        if (tag != TAG_BYTES || !payload) return false;
        auto* h = (DragonObjectHeader*)(uintptr_t)payload;
        return h->type_tag == DRAGON_TAG_BYTES;
    };
    if (op == DRAGON_BINOP_ADD && isBytes(ta, a.payload) && isBytes(tb, b.payload)) {
        DragonBytes* r = dragon_bytes_concat((DragonBytes*)(uintptr_t)a.payload,
                                             (DragonBytes*)(uintptr_t)b.payload);
        return dragon_mkbox(TAG_BYTES, (int64_t)(uintptr_t)r);
    }
    if (op == DRAGON_BINOP_MUL &&
        ((isBytes(ta, a.payload) && (tb == TAG_INT || tb == TAG_BOOL)) ||
         (isBytes(tb, b.payload) && (ta == TAG_INT || ta == TAG_BOOL)))) {
        bool aIsBytes = isBytes(ta, a.payload);
        DragonBytes* by = (DragonBytes*)(uintptr_t)(aIsBytes ? a.payload : b.payload);
        int64_t n = aIsBytes ? b.payload : a.payload;
        return dragon_mkbox(TAG_BYTES, (int64_t)(uintptr_t)dragon_bytes_repeat(by, n));
    }

    // ---- list * int (repeat). list + list is unsupported in Dragon (typed
    // lists don't support it either), so it falls through to TypeError. Dispatch
    // on the header so a list[Any] (DragonListBox) uses the box-aware repeat. ----
    if (op == DRAGON_BINOP_MUL &&
        ((ta == TAG_LIST && (tb == TAG_INT || tb == TAG_BOOL)) ||
         (tb == TAG_LIST && (ta == TAG_INT || ta == TAG_BOOL)))) {
        bool aIsList = (ta == TAG_LIST);
        void* lp = (void*)(uintptr_t)(aIsList ? a.payload : b.payload);
        int64_t n = aIsList ? b.payload : a.payload;
        if (lp) {
            auto* h = (DragonObjectHeader*)lp;
            void* r = (h->type_tag == DRAGON_TAG_LIST_BOX)
                ? (void*)dragon_list_box_repeat((DragonListBox*)lp, n)
                : (void*)dragon_list_repeat((DragonList*)lp, n);
            return dragon_mkbox(TAG_LIST, (int64_t)(uintptr_t)r);
        }
    }

    dragon_box_binop_typeerror(op, a, b);
    return dragon_mkbox(TAG_NONE, 0);  // unreachable (typeerror longjmps)
}

//===----------------------------------------------------------------------===//
// D039 Phase 11b: box ordering comparison - `Any < / <= / > / >= Any`.
//
// `==` / `!=` already route through dragon_box_eq; ordering had no handler, so
// codegen emitted an ICmp on a {i64,i64} struct and crashed the compiler. This
// gives a three-way result (the caller compares to 0). Same scope as box
// arithmetic: numeric (by value) + str + bytes (lexicographic) are ordered;
// every other / mixed pair raises TypeError, matching Python's "'<' not
// supported between instances of 'X' and 'Y'".
//===----------------------------------------------------------------------===//

// Comparison op codes - used ONLY for the TypeError message (the three-way
// result is operator-independent). MUST match the codegen mapping.
enum {
    DRAGON_CMP_LT = 0, DRAGON_CMP_LE = 1, DRAGON_CMP_GT = 2, DRAGON_CMP_GE = 3,
};

static const char* dragon_cmp_symbol(int64_t op) {
    switch (op) {
        case DRAGON_CMP_LT: return "<";
        case DRAGON_CMP_LE: return "<=";
        case DRAGON_CMP_GT: return ">";
        case DRAGON_CMP_GE: return ">=";
        default:            return "<";
    }
}

int64_t dragon_box_cmp(DragonBox a, DragonBox b, int64_t op) {
    const int64_t ta = a.tag, tb = b.tag;
    const bool aNum = (ta == TAG_INT || ta == TAG_BOOL || ta == TAG_FLOAT);
    const bool bNum = (tb == TAG_INT || tb == TAG_BOOL || tb == TAG_FLOAT);

    // Numeric (bool as 0/1; promote to double if either is float).
    if (aNum && bNum) {
        if (ta == TAG_FLOAT || tb == TAG_FLOAT) {
            double fa, fb;
            if (ta == TAG_FLOAT) memcpy(&fa, &a.payload, sizeof(double)); else fa = (double)a.payload;
            if (tb == TAG_FLOAT) memcpy(&fb, &b.payload, sizeof(double)); else fb = (double)b.payload;
            return (fa < fb) ? -1 : (fa > fb) ? 1 : 0;
        }
        const int64_t ia = a.payload, ib = b.payload;
        return (ia < ib) ? -1 : (ia > ib) ? 1 : 0;
    }

    // str: lexicographic via the shared strcmp wrapper.
    if (ta == TAG_STR && tb == TAG_STR) {
        int64_t c = dragon_str_cmp((const char*)(uintptr_t)a.payload,
                                   (const char*)(uintptr_t)b.payload);
        return (c < 0) ? -1 : (c > 0) ? 1 : 0;
    }

    // bytes: value-tag 7 is bytes XOR class; gate on the object header so a
    // class instance never reaches the bytes compare.
    auto isBytes = [](int64_t tag, int64_t p) -> bool {
        if (tag != TAG_BYTES || !p) return false;
        return ((DragonObjectHeader*)(uintptr_t)p)->type_tag == DRAGON_TAG_BYTES;
    };
    if (isBytes(ta, a.payload) && isBytes(tb, b.payload)) {
        auto* ba = (DragonBytes*)(uintptr_t)a.payload;
        auto* bb = (DragonBytes*)(uintptr_t)b.payload;
        int64_t n = ba->len < bb->len ? ba->len : bb->len;
        int c = (n > 0) ? memcmp(ba->data, bb->data, (size_t)n) : 0;
        if (c != 0) return c < 0 ? -1 : 1;
        return (ba->len < bb->len) ? -1 : (ba->len > bb->len) ? 1 : 0;
    }

    // list: lexicographic via dragon_list_cmp (mutually recursive - handles
    // nested lists, and makes box-level `list < list` consistent with native).
    if (ta == TAG_LIST && tb == TAG_LIST && a.payload && b.payload)
        return dragon_list_cmp((void*)(uintptr_t)a.payload,
                               (void*)(uintptr_t)b.payload);

    char buf[128];
    snprintf(buf, sizeof(buf),
             "TypeError: '%s' not supported between instances of '%s' and '%s'",
             dragon_cmp_symbol(op),
             dragon_box_type_name(ta, a.payload),
             dragon_box_type_name(tb, b.payload));
    dragon_raise_exc_cstr(80, buf);
    return 0;  // unreachable (typeerror longjmps)
}

//===----------------------------------------------------------------------===//
// Subscripting an Any-boxed value - `anyVal[index]`.
//
// When the static type is `Any`, codegen can't pick a typed subscript path, so
// it boxes the receiver and the index and hands both to this dispatcher. We
// inspect the receiver's tag to find the real container, then read the element
// as a {tag, payload} box.
//
// OWNED contract - the returned box owns +1 on any refcounted payload. The
// str-index / bytes-index cases allocate a fresh result (already +1); the
// container element cases (list/dict) read a BORROW from the container and
// incref it before returning, so the contract is uniform regardless of the
// runtime tag. This is what lets codegen classify a dragon_box_subscript result
// as owned (isOwnedBoxResult) and release it after a transient use (print /
// discarded statement) or take ownership of it on store - exactly mirroring the
// owned-str convention. (The element-read helpers dragon_dict_get_box /
// dragon_list_box_get keep their own BORROW contracts; the incref here is what
// converts that borrow into the owned result this function promises.)
//===----------------------------------------------------------------------===//

// Box-returning element reads from the other TUs (ABI: {i64,i64} returned in
// two registers - the local struct name in runtime_list.cpp is irrelevant).
DragonBox dragon_list_box_get(DragonListBox* list, int64_t index);
DragonBox dragon_dict_get_box(DragonDict* d, const char* key);
DragonBox dragon_dict_int_get_box(DragonDict* d, int64_t key);
const char* dragon_str_index(const char* s, int64_t index);
int64_t     dragon_bytes_get(DragonBytes* b, int64_t index);

// A boxed index is usable as an integer subscript iff it is an int or bool.
// `what` names the container for the Python-shaped TypeError message.
static int64_t dragon_box_int_index(DragonBox index, const char* what) {
    if (index.tag == TAG_INT || index.tag == TAG_BOOL) return index.payload;
    char buf[128];
    snprintf(buf, sizeof(buf),
             "TypeError: %s indices must be integers, not %s",
             what, dragon_box_type_name(index.tag, index.payload));
    dragon_raise_exc_cstr(80, buf);
    return 0;  // unreachable
}

DragonBox dragon_box_subscript(DragonBox container, DragonBox index) {
    switch (container.tag) {
        case TAG_LIST: {
            void* p = (void*)(uintptr_t)container.payload;
            if (!p) {
                dragon_raise_exc_cstr(80,
                    "TypeError: 'NoneType' object is not subscriptable");
                return {};
            }
            int64_t idx = dragon_box_int_index(index, "list");
            DragonObjectHeader* h = (DragonObjectHeader*)p;
            // list[Any] keeps per-element {tag, payload} slots. The element is a
            // BORROW from the list; incref to honor the OWNED return contract.
            if (h->type_tag == DRAGON_TAG_LIST_BOX) {
                DragonBox r = dragon_list_box_get((DragonListBox*)p, idx);
                dragon_incref_tagged(r.payload, (uint8_t)r.tag);
                return r;
            }
            // Monomorphic list (DragonList / F64 / Ptr share header + size +
            // data + elem_tag offsets). Read elem_tag to interpret the slot.
            DragonList* l = (DragonList*)p;
            int64_t n = l->size;
            if (idx < 0) idx += n;
            if (idx < 0 || idx >= n) {
                dragon_raise_exc_cstr(41, "IndexError: list index out of range");
                return {};
            }
            DragonBox r;
            r.tag = (int64_t)l->elem_tag;
            switch (l->elem_tag) {
                case TAG_FLOAT: {
                    double dv = ((const double*)l->data)[idx];
                    memcpy(&r.payload, &dv, sizeof(double));
                    break;
                }
                case TAG_STR:
                case TAG_LIST:
                case TAG_DICT:
                case TAG_BYTES:
                    // Borrowed pointer from the list - incref for the owned
                    // contract.
                    r.payload = (int64_t)(uintptr_t)((void* const*)l->data)[idx];
                    dragon_incref_tagged(r.payload, (uint8_t)r.tag);
                    break;
                default:  // TAG_INT / TAG_BOOL (elem_size 8 or 1) - not refcounted
                    r.payload = dragon_list_load(l, idx);
                    break;
            }
            return r;
        }
        case TAG_DICT: {
            DragonDict* d = (DragonDict*)(uintptr_t)container.payload;
            if (!d) {
                dragon_raise_exc_cstr(80,
                    "TypeError: 'NoneType' object is not subscriptable");
                return {};
            }
            // Dict values are BORROWS from the dict; incref for the owned return.
            if (index.tag == TAG_STR) {
                DragonBox r = dragon_dict_get_box(
                    d, (const char*)(uintptr_t)index.payload);
                dragon_incref_tagged(r.payload, (uint8_t)r.tag);
                return r;
            }
            if (index.tag == TAG_INT || index.tag == TAG_BOOL) {
                DragonBox r = dragon_dict_int_get_box(d, index.payload);
                dragon_incref_tagged(r.payload, (uint8_t)r.tag);
                return r;
            }
            char buf[128];
            snprintf(buf, sizeof(buf),
                     "TypeError: unhashable key type '%s'",
                     dragon_box_type_name(index.tag, index.payload));
            dragon_raise_exc_cstr(80, buf);
            return {};
        }
        case TAG_STR: {
            const char* s = (const char*)(uintptr_t)container.payload;
            int64_t idx = dragon_box_int_index(index, "string");
            const char* r = dragon_str_index(s, idx);
            return dragon_mkbox(TAG_STR, (int64_t)(uintptr_t)r);
        }
        case TAG_BYTES: {
            // value-tag 7 is bytes XOR a class instance; only bytes is indexable.
            auto* h = (DragonObjectHeader*)(uintptr_t)container.payload;
            if (h && h->type_tag == DRAGON_TAG_BYTES) {
                int64_t idx = dragon_box_int_index(index, "bytes");
                return dragon_mkbox(TAG_INT,
                    dragon_bytes_get((DragonBytes*)h, idx));
            }
            break;  // class instance → fall through to "not subscriptable"
        }
        default:
            break;
    }
    char buf[128];
    snprintf(buf, sizeof(buf),
             "TypeError: '%s' object is not subscriptable",
             dragon_box_type_name(container.tag, container.payload));
    dragon_raise_exc_cstr(80, buf);
    return {};  // unreachable (typeerror longjmps)
}

// Defined in the string/dict TUs - declared here for dragon_box_len.
int64_t dragon_str_len(const char* s);
int64_t dragon_dict_len(DragonDict* d);

/// len() of a boxed Any value, tag + header dispatched. A TAG_LIST payload
/// may be EITHER list representation (the monomorphized DragonList family or
/// a DragonListBox) - all variants share the size field offset, but dispatch
/// on the header anyway so the layouts stay free to diverge. TAG_BYTES ==
/// TAG_CLASS, so bytes length is header-gated; a class instance (and every
/// other unsized value) raises the Python-shaped TypeError.
int64_t dragon_box_len(DragonBox box) {
    switch (box.tag) {
        case TAG_STR:
            return dragon_str_len((const char*)(uintptr_t)box.payload);
        case TAG_LIST: {
            auto* h = (DragonObjectHeader*)(uintptr_t)box.payload;
            if (!h) break;
            if (h->type_tag == DRAGON_TAG_LIST_BOX)
                return ((DragonListBox*)h)->size;
            return ((DragonList*)h)->size;
        }
        case TAG_DICT:
            return dragon_dict_len((DragonDict*)(uintptr_t)box.payload);
        case TAG_BYTES: {
            auto* h = (DragonObjectHeader*)(uintptr_t)box.payload;
            if (h && h->type_tag == DRAGON_TAG_BYTES)
                return ((DragonBytes*)h)->len;
            break;  // class instance -> "has no len()"
        }
        default:
            break;
    }
    char buf[128];
    snprintf(buf, sizeof(buf), "TypeError: object of type '%s' has no len()",
             dragon_box_type_name(box.tag, box.payload));
    dragon_raise_exc_cstr(80, buf);
    return 0;  // unreachable (typeerror longjmps)
}

/// Release an OWNED box temporary: decref its payload by tag (no-op for the
/// non-refcounted tags int/float/bool/none). Codegen emits this to free an
/// owned box result (dragon_box_binop / dragon_box_subscript) after a transient
/// use - printing it, discarding it as a statement - exactly as it decrefs an
/// owned-str temporary. Borrowed box results (dragon_dict_get_box /
/// dragon_list_box_get) are NEVER passed here (the container keeps the +1).
void dragon_box_decref(DragonBox box) {
    dragon_decref_tagged(box.payload, (uint8_t)box.tag);
}

} // extern "C"
