/// Dragon Runtime - Shared Internal Header
/// Types, macros, enums, and extern declarations used across runtime TUs.
#ifndef DRAGON_RUNTIME_INTERNAL_H
#define DRAGON_RUNTIME_INTERNAL_H

#include <cmath>
#include <csetjmp>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
// On Windows, winsock2 must be included BEFORE windows.h (and before any
// header that might transitively pull in the legacy winsock.h, including
// MinGW's <unistd.h>). Including it up front pins the v2 API.
#ifdef _WIN32
  #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
  #endif
  #include <winsock2.h>
  #include <ws2tcpip.h>
  #include <windows.h>
  // Unify error names: Winsock uses WSAEWOULDBLOCK, BSD uses EWOULDBLOCK/EAGAIN.
  #ifndef EWOULDBLOCK
    #define EWOULDBLOCK WSAEWOULDBLOCK
  #endif
  #ifndef EAGAIN
    #define EAGAIN WSAEWOULDBLOCK
  #endif
  #define dragon_sock_errno() WSAGetLastError()
#else
  #include <errno.h>
  #define dragon_sock_errno() errno
#endif
// Windows MinGW ships winpthreads, so <pthread.h> works on all platforms we
// support. unistd.h is also provided by MinGW for the small subset of POSIX
// calls we use (close, read, write, pipe, getpid). MSVC is not a target.
#include <pthread.h>
#include <unistd.h>
// Atomics: use __atomic_* builtins only. Do NOT include <stdatomic.h>; it is a
// C11 header that GCC < 13 rejects in C++ TUs (breaks the ubuntu-22.04 CI).

// minicoro - declarations only (MINICORO_IMPL defined in runtime_concurrency.cpp).
// MCO_USE_VMEM_ALLOCATOR makes every green-thread stack a virtual-memory
// (mmap / VirtualAlloc) region of ~2 MB instead of a 56 KB calloc'd heap block.
// Two wins: (1) lazily-committed pages mean the 2 MB costs ~0 physical memory
// until actually used, and (2) overflowing the stack runs into the unmapped
// pages just past the region and faults (SIGSEGV) instead of silently
// corrupting adjacent heap objects' refcount headers - the latter was an
// exploitable memory-safety hole (a deep/large-frame fired handler could smash
// a neighbouring object). This is the same guarded-stack model pthread uses for
// OS threads. MUST be defined before EVERY include of minicoro.h (it changes
// MCO_DEFAULT_STACK_SIZE and the allocator), hence here in the shared header.
#ifndef MCO_USE_VMEM_ALLOCATOR
#define MCO_USE_VMEM_ALLOCATOR
#endif
#include "minicoro.h"

#ifdef __cplusplus
extern "C" {
#endif

//===----------------------------------------------------------------------===//
// GC Object Header (Decision 018 - Reference Counting)
//===----------------------------------------------------------------------===//

typedef struct {
    int64_t  refcount;       // 8 bytes (offset 0)
    uint8_t  type_tag;       // 1 byte  (offset 8)
    uint8_t  gc_flags;       // 1 byte  (offset 9)
    uint16_t class_id;       // 2 bytes (offset 10)
    int32_t  gc_track_idx;   // 4 bytes (offset 12)
} DragonObjectHeader;        // Total: 16 bytes

// GC flag constants
#define GC_FLAG_TRACKED   0x01
#define GC_FLAG_REACHABLE 0x02
// Set on objects that have escaped to another OS thread (via fire/thread or
// reachable from a SHARED parent). When set, dragon_incref/decref route to the
// atomic variants; when clear, they use the plain non-atomic fast path. The
// bit lives in the same cache line as `refcount` (offset 0-15), so the check
// is essentially free for non-shared objects.
#define GC_FLAG_SHARED    0x04
// Set by the cycle collector on every object queued into `to_free` after
// trial-deletion identifies it as unreachable. Cleared implicitly when the
// object's storage is freed (next allocation re-inits via dragon_obj_init).
// Mutator decref paths use this bit (not bare `gc_collecting`) so that ONLY
// objects the collector owns the dealloc of are skipped - concurrent decrefs
// on reachable / untracked / never-collected objects still free eagerly,
// which is what keeps gc_tracked from bloating with refcount-0 orphans
// (the cause of an O(N^2) GC slowdown under multi-threaded allocation).
#define GC_FLAG_IN_TO_FREE 0x08
#define GC_FLAG_HEAP_OBJ  0x80

#define DRAGON_IMMORTAL_REFCOUNT ((int64_t)0x4000000000000000LL)

static inline bool dragon_is_immortal(void* obj) {
    return obj && ((DragonObjectHeader*)obj)->refcount >= DRAGON_IMMORTAL_REFCOUNT;
}

// Header fields read by plain refcount paths while another thread may be
// writing them atomically (mark-shared's fetch_or on gc_flags; atomic
// refcount ops on refcount). A RELAXED atomic load emits the same single
// load instruction on x86-64 and AArch64 as the plain read, but stops the
// compiler from tearing, caching, or reordering it - the plain reads were
// formally data races. Ordering beyond that is provided by the protocol:
// objects are marked SHARED (deep walk) before being published to another
// thread; the lazy SHARED-set inside the *_atomic entry points is a backstop.
static inline uint8_t dragon_gc_flags_load(const DragonObjectHeader* h) {
    return __atomic_load_n(&h->gc_flags, __ATOMIC_RELAXED);
}
static inline int64_t dragon_refcount_load(const DragonObjectHeader* h) {
    return __atomic_load_n(&h->refcount, __ATOMIC_RELAXED);
}

enum DragonTypeTag : uint8_t {
    DRAGON_TAG_STR       = 1,
    DRAGON_TAG_LIST      = 2,
    DRAGON_TAG_DICT      = 3,
    DRAGON_TAG_TUPLE     = 4,
    DRAGON_TAG_SET       = 5,
    DRAGON_TAG_BYTES     = 6,
    DRAGON_TAG_CLASS     = 7,
    DRAGON_TAG_GENERATOR = 8,
    DRAGON_TAG_TYPE      = 9,
    DRAGON_TAG_CLOSURE   = 10,
    DRAGON_TAG_ENV       = 11,
    DRAGON_TAG_DEQUE     = 12,
    DRAGON_TAG_CELL      = 13,  // heap-boxed mutable nonlocal binding
    DRAGON_TAG_LIST_BOX  = 14,  // D039 Phase 4: list[Any] with 16B/elem slots
};

static inline void dragon_obj_init(DragonObjectHeader* h, uint8_t tag) {
    h->refcount = 1;
    h->type_tag = tag;
    h->gc_flags = GC_FLAG_HEAP_OBJ;
    h->class_id = 0;
    h->gc_track_idx = -1;
}

// Value type tags for dict entries and container elements
enum DragonValueTag : int8_t {
    TAG_INT = 0,
    TAG_STR = 1,
    TAG_FLOAT = 2,
    TAG_BOOL = 3,
    TAG_NONE = 4,
    TAG_LIST = 5,
    TAG_DICT = 6,
    TAG_BYTES = 7,
};

// A container element's value-tag is "traceable" iff it points at a heap object
// the cycle collector must follow to find cycles: list, dict, bytes, or a
// Callable slot (DRAGON_TAG_CLOSURE = 10). A Callable element may be a real
// DragonClosure whose env can point back at the container (a genuine cycle)
// or a bare fn pointer with no header; including tag 10 here is
// safe because the traverse visit fns only dereference children found in the
// tracked set, so a bare fn ptr is a hash-miss no-op. Class instances / sets /
// tuples stored as elements are stamped TAG_LIST=5 by codegen; str=1 and the
// scalar tags 0/2/3/4 are leaves the collector never follows.
// SINGLE SOURCE OF TRUTH shared by the container traverse functions AND the
// acyclic-skip allocation/insert gates, so the two cannot diverge - a gate that
// skipped tracking something the traverse follows would leak a live cycle.
// (The divergence is not hypothetical: if the traverse fns have explicit
// closure arms while the dict/tuple insert gates use this predicate without
// tag 10, a dict/tuple whose only heap values are closures is never enrolled
// and closure cycles through them are never collected.)
static inline bool dragon_value_tag_is_traceable(int8_t tag) {
    return tag == TAG_LIST || tag == TAG_DICT || tag == TAG_BYTES ||
           tag == (int8_t)DRAGON_TAG_CLOSURE;
}

//===----------------------------------------------------------------------===//
// Exception stack constants
//===----------------------------------------------------------------------===//

#define DRAGON_EXC_STACK_SIZE 32
#define DRAGON_EXC_MAX_USER 256

//===----------------------------------------------------------------------===//
// Exception-unwind cleanup stack
//===----------------------------------------------------------------------===//
//
// setjmp/longjmp unwinding restores the C stack pointer directly, bypassing the
// codegen-emitted scope cleanup (emitScopeCleanupFor) that decrefs owned heap
// locals at NORMAL exits. Without a side channel, every owned heap local (str,
// closure, container, instance, cell, union) in a frame the longjmp skips over
// LEAKS its reference.
//
// This stack is that side channel: codegen pushes a {value, kind, tag} snapshot
// for each owned heap local as it is declared, UPDATEs it on reassignment, and
// rewinds the stack pointer at normal scope exit (where the codegen decref
// already ran). Each try-frame records the cleanup depth at push; on longjmp
// arrival the dispatch frees everything above that depth (the leaked locals),
// mirroring emitScopeCleanupFor exactly. It is per-context (per-OS-thread TLS +
// per-vthread) because interleaved green threads on one worker would otherwise
// corrupt a shared LIFO. Grown lazily (cap=0 until first push) so a context
// that never declares an owned heap local inside a try costs zero memory.
typedef struct DragonCleanupStack {
    int64_t* vals;   // snapshot of the owned heap pointer (i64-shaped)
    int32_t* kinds;  // DragonCleanupKind - which decref to call on unwind
    int32_t* tags;   // box value-tag for DCLEAN_UNION; 0 otherwise
    int32_t  sp;     // next free slot (0 = empty)
    int32_t  cap;    // allocated capacity
} DragonCleanupStack;

// How to release a cleanup entry on unwind - mirrors emitScopeCleanupFor's
// per-VarKind dispatch. Kept distinct from DragonValueTag/DragonTypeTag so the
// codegen→runtime contract is explicit.
enum DragonCleanupKind {
    DCLEAN_STR      = 1,  // dragon_decref_str(val)
    DCLEAN_CALLABLE = 2,  // dragon_decref_callable(val)  (tag-gated: no-ops on bare fn ptr)
    DCLEAN_OBJ      = 3,  // dragon_decref(val)           (containers, instances, cells, deque, file)
    DCLEAN_UNION    = 4,  // tag-conditional, using the entry's `tag` (mirrors emitUnionDecref)
    // A pending `defer` call (defer.md): val is a codegen-built thunk
    // `void(*)(int64_t*)`, tag is its snapshot-arg count. The `tag` entries
    // pushed directly below carry the snapshot VALUES, each with its own
    // release kind (0 for scalars and own-moved values the callee adopts).
    // The unwinder calls the thunk over those entries while they are still
    // on the stack, then keeps popping so they drain per-kind as usual -
    // defers therefore run before the release of everything they borrow.
    DCLEAN_DEFER_CALL = 5,
};

enum VThreadYieldReason {
    YIELD_COOP  = 0,
    YIELD_IO    = 1,
    YIELD_SLEEP = 2
};

//===----------------------------------------------------------------------===//
// Struct declarations
//===----------------------------------------------------------------------===//

/// Dragon string - PEP 393-lite layout.
///
/// Storage modes ("kind"):
///   1 = ASCII / Latin-1 fast path. Each code point fits in one byte.
///       For pure ASCII (cp <= 0x7F), `data` is also a valid NUL-terminated
///       UTF-8 / C string, so existing C-FFI sites keep working.
///   4 = UCS-4. `data` is an array of uint32_t code points (host endian).
///       Used for any string containing a code point >= 0x80. Not NUL-
///       terminated as a C string.
///
/// `len` is the **code-point count** (Python's `len()`), never byte length.
/// Byte length of `data` is `len * kind`.
typedef struct {
    DragonObjectHeader header;
    int64_t len;          // code-point count
    uint8_t kind;         // 1 or 4
    uint8_t _pad[3];      // pad to 4-byte boundary for `cap`
    int32_t cap;          // byte capacity of `data` (excl. NUL terminator), for
                          // amortized in-place append. Packed into the former
                          // padding → header stays 32 B and `data[]` is
                          // still 8-byte aligned. A `cap` of 2 GiB is the ceiling
                          // for the in-place fast path; larger strings fall back.
    char    data[];       // len * kind bytes; kind=1 NUL-terminated
} DragonString;

/// Byte length of a DragonString's data buffer (excluding any NUL terminator).
static inline int64_t dragon_str_byte_len(const DragonString* s) {
    return s->len * (int64_t)s->kind;
}

/// Clamp a byte count to the int32 `cap` field range. Strings larger
/// than 2 GiB simply never enter the in-place append fast path - `cap` then
/// understates the true buffer size, which is safe (forces a realloc) but
/// never overstates it (which would risk an overflow).
static inline int32_t dragon_cap_clamp(int64_t bytes) {
    return bytes > 0x7fffffff ? (int32_t)0x7fffffff : (int32_t)bytes;
}

/// Packed-element storage. `elem_size` is the per-element width in bytes -
/// 1 for `list[bool]` (TAG_BOOL), 8 for everything else. Callers MUST go
/// through `dragon_list_load` / `dragon_list_store` to read/write so the
/// dispatch on width is centralized. The legacy `int64_t* data` field name
/// is kept as a bag of bytes; the actual indexing stride is `elem_size`.
///
/// Why packed bool:
///   `list[bool]` of size N took 8*N bytes; sieve's 1M-element `is_prime`
///   was 8MB and spilled out of L2. Packing to 1B/elem drops it to 1MB
///   (fits in L2 on every machine made in the last 15 years), turning the
///   counting-loop linear scan into a cache-resident hot path.
struct DragonList {
    DragonObjectHeader header;
    void*    data;        // bytes; stride = elem_size
    int64_t  size;        // # of elements
    int64_t  capacity;    // capacity in elements
    uint8_t  elem_tag;    // TAG_INT, TAG_STR, TAG_BOOL, ...
    uint8_t  elem_size;   // 1 (bool) or 8 (int/float/ptr/etc.)
};

/// Native `Any`/`Union` value: layout matches `%dragon.box = { i64, i64 }` in
/// LLVM (D039). Passed in two registers (System V ABI for <=16-byte structs).
/// Single source of truth shared by every TU that takes/returns a box.
struct DragonBox {
    int64_t tag;
    int64_t payload;
};

/// D030 Phase 3 - Monomorphized list family.
///
/// `DragonList` (above) is the I64 variant: int + bool elements, with bool
/// 1-byte packing preserved (D028). It will be renamed to DragonListI64 in
/// Phase 3.D once codegen has migrated.
///
/// The two new variants below have IDENTICAL memory layout to DragonList -
/// same field order, same offsets, same total size. The only difference is
/// the static type of `data`: `double*` for F64 and `void**` for Ptr. This
/// lets polymorphic ops (dragon_list_destroy, dragon_list_len, GC traverse)
/// cast a `DragonList*` to any of these and read header / size / capacity /
/// elem_tag at the same offsets. Per-element access uses the typed `data`
/// pointer in each per-type op - no stride math, no bitcast at the call site.
///
/// Codegen picks the variant from `list[T]`:
///   list[int], list[bool]                  → DragonList (I64; bool packs)
///   list[float]                            → DragonListF64
///   list[str], list[<container>], list[<class>], list[bytes], etc.
///                                          → DragonListPtr
///   list[Any], untyped list                → DragonListBox (Phase 4)

/// list[float] - native f64 storage, no bitcast at load/store.
struct DragonListF64 {
    DragonObjectHeader header;
    double*  data;        // stride = 8 always
    int64_t  size;
    int64_t  capacity;
    uint8_t  elem_tag;    // TAG_FLOAT (= 2) by definition
    uint8_t  elem_size;   // 8
};

/// list[<heap-type>] - native pointer storage. Refcount semantics are baked
/// into per-type ops (set/append/destroy), not the codegen.
struct DragonListPtr {
    DragonObjectHeader header;
    void**   data;        // stride = 8 always
    int64_t  size;
    int64_t  capacity;
    uint8_t  elem_tag;    // TAG_STR / TAG_LIST / TAG_DICT / TAG_BYTES / etc.
    uint8_t  elem_size;   // 8
};

/// D039 Phase 4: list[Any] - per-element {tag, payload} storage. Dragon's
/// equivalent of Go's `[]interface{}` / Rust's `Vec<Box<dyn Any>>`. Each
/// element is a 16-byte box, contiguous; one cache miss per read.
///
/// Refcount discipline matches DragonListPtr's Model-B pattern: the list
/// owns +1 on each refcounted payload (TAG_STR / TAG_LIST / TAG_DICT /
/// TAG_BYTES / TAG_CLASS). Set/append/destroy own the increment & decrement
/// accounting; codegen is free to borrow.
struct DragonListBoxElem {
    int64_t tag;
    int64_t payload;
};

struct DragonListBox {
    DragonObjectHeader header;
    DragonListBoxElem* data;  // stride = 16 (two i64s per element)
    int64_t size;
    int64_t capacity;
    // No list-level elem_tag - each element carries its own tag.
};

/// Per-tag stride. Bool gets the 1-byte fast path; all other tags pay 8B.
/// (Future extension: i32 / i16 list element types would slot in here.)
static inline uint8_t dragon_list_size_for_tag(uint8_t tag) {
    return (tag == TAG_BOOL) ? 1 : 8;
}

/// Read element `i` of a DragonList as an i64 value. For kind=1 (bool) we
/// zero-extend the byte; for kind=8 we load the native 8-byte slot.
static inline int64_t dragon_list_load(const struct DragonList* l, int64_t i) {
    if (l->elem_size == 1) return (int64_t)((const uint8_t*)l->data)[i];
    return ((const int64_t*)l->data)[i];
}

/// Write element `i` of a DragonList. For kind=1 we truncate to a byte;
/// for kind=8 we write the full slot. Caller is responsible for any RC
/// management on the previous value (this helper is byte-only).
static inline void dragon_list_store(struct DragonList* l, int64_t i, int64_t v) {
    if (l->elem_size == 1) ((uint8_t*)l->data)[i] = (uint8_t)(v & 0xFF);
    else ((int64_t*)l->data)[i] = v;
}

struct DictEntry {
    uint64_t hash;
    const char* key;
    int64_t value;
    int8_t tag;
    // Lazy-delete tombstone (audit 4.2/4.3). A removed entry is marked dead in
    // place (key/value cleared) instead of shifting the whole dense array down
    // and rebuilding the index on every delete - that was O(n) per delete and
    // O(n^2) for bulk deletion, a real algorithmic DoS. Dead dense slots are
    // skipped by every full scan and reclaimed lazily by dict_compact. Fits in
    // the existing 32-byte padding, so no per-entry memory cost.
    int8_t dead;
};

struct DragonDict {
    DragonObjectHeader header;
    DictEntry* entries;
    int64_t* indices;
    // High-water mark of the dense `entries` array, INCLUDING dead (tombstoned)
    // slots. New inserts append at `size`; deletes never shrink it (they mark
    // dead). dict_compact resets it to `used`.
    int64_t size;
    // Live entry count (size minus dead slots). This is what len() returns and
    // what every "count" decision uses. Kept in lockstep with the index table's
    // live entries.
    int64_t used;
    int64_t capacity;
    int64_t index_size;
    // 0 = int-keyed (the `key` slot holds an i64 cast to a pointer - NEVER
    // decref it); 1 = str-keyed (the dict OWNS one DragonString ref per key,
    // per the codegen contract in Assign.cpp, and must release it on removal/
    // destroy). Set only by the str setter, so int keys are never touched.
    uint8_t keys_are_ptr;
};

struct DragonTuple {
    DragonObjectHeader header;
    int64_t* data;
    int64_t length;
    uint8_t* elem_tags;
};

struct DragonSet {
    DragonObjectHeader header;
    int64_t* buckets;
    uint8_t* states;
    int64_t  capacity;
    int64_t  count;
    uint8_t  elem_tag;
};

struct DragonBytes {
    DragonObjectHeader header;
    int64_t len;
    uint8_t* data;
};

typedef struct DragonVThread {
    jmp_buf     exc_stack[DRAGON_EXC_STACK_SIZE];
    int         exc_sp;
    int         exc_type;
    const char* exc_msg;
    // Typed-field exception instance carried alongside type+msg. NULL for
    // built-in raises (`dragon_raise_exc`); set by `dragon_raise_exc_obj` when
    // the raise site constructed a user-class instance via its __init__. The
    // except handler reads it via `dragon_exc_get_obj` to bind `as e` to the
    // instance (with all typed fields intact) instead of just the message.
    void*       exc_obj;
    // Per-vthread unwind cleanup stack (see DragonCleanupStack). Mirrors the TLS
    // __dragon_cleanup so a vthread's owned heap locals are freed when a raise
    // unwinds its frames. cleanup_saved[i] holds the cleanup depth recorded when
    // exc frame i was pushed; the longjmp arrival frees back down to it.
    DragonCleanupStack cleanup;
    int32_t     cleanup_saved[DRAGON_EXC_STACK_SIZE];
    // Live exception-frame count for this vthread - the scheduler swaps it in/out
    // of the TLS __dragon_active_frames around mco_resume so the inline cleanup
    // gate stays correct when the vthread migrates workers across a yield.
    int         active_frames;
    mco_coro*   coro;
    int64_t     result;
    volatile int8_t done;
    volatile int8_t yield_reason;
    // I/O park handshake (see dragon_io_arm_park / dragon_io_wake /
    // scheduler worker loop). Resolves the "wakeup armed before the yield
    // completes" race: without it the reactor could scheduler_enqueue() a
    // vthread whose coroutine had not yet reached mco_yield (still MCO_RUNNING),
    // giving a lost wakeup (a worker drops a non-suspended coro) or a
    // double-enqueue (reactor + the resuming worker both re-queue). States:
    //   0 PARK_NONE  - not in an I/O/sleep wait (a cooperative yield stays here)
    //   1 PARK_ARMED - request posted, coro about to / in the middle of yielding
    //   2 PARK_PARKED- worker confirmed the coro is suspended; reactor may enqueue
    //   3 PARK_FIRED - reactor fired before the worker parked; worker enqueues
    // Exactly one of {worker, reactor} wins the ARMED transition, so the vthread
    // is enqueued exactly once, always after the coroutine is suspended.
    volatile int32_t park_state;
    // Set by the I/O reactor to 1 when a deadline-bearing fd watch fired due to
    // its timeout rather than the fd becoming ready (R1 idle/read timeout). The
    // waiter (dragon_nb_recv_timeout) clears it to 0 before each watch and reads
    // it after resuming to tell "timed out" from "data ready". Untouched by the
    // no-deadline watch paths, which never read it.
    volatile int8_t io_timed_out;
    // CAS'd 0->1 by dragon_vthread_join so only the first joiner destroys the
    // coroutine + frees the handle. A second join (UB double-mco_destroy +
    // double-free otherwise) returns the cached result. A Task handle is
    // single-owner in the type system, so a losing caller is a still-live
    // deliberately-shared handle.
    volatile int8_t joined;
    // Dual-ownership refcount (init 2 at spawn): one ref for the running coro
    // (the scheduler worker drops it on MCO_DEAD, gated by the done 0->1 CAS so a
    // re-enqueued-after-finish vthread can't double-drop) and one for the Task
    // handle (dropped by the join winner, or by dragon_vthread_detach for a
    // discarded fire-and-forget). The last release frees the struct + coroutine
    // stack - so a fire-and-forget vthread cannot leak.
    volatile int8_t refs;
    pthread_mutex_t join_lock;
    pthread_cond_t  join_cond;
    struct DragonVThread* next;
} DragonVThread;

struct DragonGenerator {
    DragonObjectHeader header;
    mco_coro*   coro;
    int64_t     yielded_value;
    int8_t      state;
    // D030: codegen-allocated typed args struct + per-callsite decref function.
    // The args buffer holds (Generator*, native-typed user args...). It outlives
    // the body (heap captures must survive abandonment) - destroy calls the
    // decref fn then frees args.
    void*       args;
    void      (*args_decref_fn)(void*);
    // Isolated exception context for the generator BODY (installed as
    // __dragon_exc_vt while the body runs - see dragon_generator_next). A
    // generator that yields inside a try keeps its setjmp frames here, separate
    // from the caller's exc stack, so the caller's frame push/pop across the
    // suspend cannot clobber them. Allocated lazily on first resume (a
    // created-but-never-iterated generator pays nothing). Used purely as an
    // exc-state container - its scheduler/join fields are never touched.
    DragonVThread* exc_vt;
    // Set by the trampoline's setjmp barrier when the body raised an exception
    // that was NOT caught inside the generator. dragon_generator_next reads it
    // after the resume (with the caller's exc context restored) and re-raises
    // the exception (type/msg/obj live in exc_vt) into the CALLER's frame -
    // instead of a cross-stack longjmp out of the coroutine (which would skip
    // generator_next's context restore and leave a dangling exc context).
    int8_t      pending_exc;
};

struct DragonClassDescriptor {
    DragonObjectHeader header;
    int64_t    class_id;
    const char* name;
    const char* doc;             // class docstring (NULL if absent); powers Cls.__doc__
    int64_t    parent;
    int64_t    constructor;
    int64_t*   ancestor_ids;
    int64_t    num_ancestors;
    // Field metadata for hasattr()/getattr() reflection
    const char** field_names;    // array of field name C-strings
    int64_t*     field_offsets;  // parallel array of BYTE offsets into the instance
    int64_t*     field_widths;   // parallel array of field byte widths (1/8/...)
    int64_t      num_fields;
    // D033: Method reflection - own (non-inherited) methods of this class.
    // Each class only stores its own methods; inheritance is resolved by
    // walking the `parent` chain in dragon_class_find_method / dragon_dir.
    // method_kinds entries: 0 = instance, 1 = static, 2 = classmethod.
    const char** method_names;
    void**       method_fn_ptrs;
    uint8_t*     method_kinds;
    int64_t      num_methods;
    // D033 Phase 3: parallel array of "bound thunks" - codegen-emitted
    // wrappers with closure ABI (user_args..., env: ptr) that unpack `self`
    // from env and call the underlying method. Used by getattr() to build
    // a bound DragonClosure without a runtime trampoline per call. Null
    // when the class has no own methods (set_methods skipped).
    void**       method_bound_thunks;
};

// D027/D030: Closure environment - heap-allocated, refcounted.
// The body layout (captured values) is owned by codegen as a per-lambda
// LLVM struct type with native field types. The runtime only sees the
// header + gc fn; the body is opaque memory immediately following.
//   sizeof(DragonEnv) = 24 bytes (16 header + 8 fn ptr) - codegen relies on
//   this offset to GEP into the body. Do not insert fields here.
//
// `gc_fn` is a MULTI-OP hook (one codegen-emitted fn per closure
// site) so the cycle collector can see through an env to the heap objects it
// captured. It replaces the old single-purpose `dealloc_fn` WITHOUT growing the
// struct (still one fn ptr; sizeof stays 24). The op selects the behavior:
//   DEALLOC  - decref each heap capture (the former dealloc_fn body).
//   TRAVERSE - call visit(capture, arg) for each capture that can close a cycle
//              (the cycle collector subtracts the env's internal ref to it).
//   CLEAR    - decref + NULL each heap capture slot (break the cycle; the later
//              env dealloc then sees emptied slots and double-frees nothing).
// NULL only for scalar-only envs (no heap captures) - those are never tracked.
#define DRAGON_ENV_OP_DEALLOC  0
#define DRAGON_ENV_OP_TRAVERSE 1
#define DRAGON_ENV_OP_CLEAR    2
// SHARED-mark every heap capture (str included - TRAVERSE deliberately sksips strings
// since they cannot close a cycle, but SHARED propagation must reach them).
#define DRAGON_ENV_OP_MARK_SHARED 3
struct DragonEnv {
    DragonObjectHeader header;
    void (*gc_fn)(void* env, int32_t op, void (*visit)(void*, void*), void* arg);
};

// D027: Closure wrapper - pairs a function pointer with its environment.
struct DragonClosure {
    DragonObjectHeader header;
    void*   fn_ptr;              // the actual lambda function (with trailing i8* env param)
    DragonEnv* env;              // captured environment (may be NULL for future use)
};

// Heap-boxed mutable binding for `nonlocal` semantics.
//   Layout: { 16B header, 8B value, 4B kind, 4B holds_heap } = 32 bytes.
//   `value` carries either a raw integer (int/bool zext) or a pointer-as-i64
//   (str/list/dict/instance/etc. via ptrtoint). `kind` records the
//   DragonValueTag so the cell's destructor knows how to drop the held ref.
//   `holds_heap` is set when `value` aliases a heap object (so RC discipline
//   applies on overwrite + dealloc); cleared for plain ints/bools/floats.
//
// One cell per nonlocal-declared variable. Both the owning function and
// every closure that captures it operate through `dragon_cell_get/set`,
// so reads chain and writes mutate through a single backing slot - the
// Python `nonlocal` story without the proxy-globals hack.
struct DragonCell {
    DragonObjectHeader header;
    int64_t value;
    int32_t kind;        // DragonValueTag
    int32_t holds_heap;  // 1 if `value` is a heap pointer requiring decref on overwrite/dealloc
};

// Deque - circular buffer for O(1) append/popleft
struct DragonDeque {
    DragonObjectHeader header;
    int64_t* data;      // circular buffer
    int64_t  capacity;
    int64_t  head;      // index of first element
    int64_t  size;      // number of elements
    int64_t  maxlen;    // bound: append past it discards the far end; -1 = unbounded
    uint8_t  elem_tag;  // DragonValueTag of elements - drives RC, equality, repr
};

//===----------------------------------------------------------------------===//
// Shared state - extern declarations (defined in runtime_core.cpp)
//===----------------------------------------------------------------------===//

// GC class tables
typedef void (*dragon_class_dealloc_fn)(void*);
typedef void (*dragon_class_clear_fn)(void*);
typedef void (*dragon_gc_visit_fn)(void* child, void* arg);
typedef void (*dragon_class_traverse_fn)(void*, dragon_gc_visit_fn, void*);
// Per-class shared-mark fn: visits every heap-typed field (incl. strings) and
// calls `dragon_mark_shared` (containers/instances) or `dragon_mark_shared_str`
// (strings) directly. Codegen-generated; registered via
// `dragon_class_register_mark_shared`. Receives an opaque worklist pointer so
// container/class children can be queued for further BFS traversal.
typedef void (*dragon_class_mark_shared_fn)(void* self, void* worklist);

#define DRAGON_MAX_CLASS_IDS 4096

extern dragon_class_dealloc_fn __class_dealloc_table[DRAGON_MAX_CLASS_IDS];
extern dragon_class_clear_fn __class_clear_table[DRAGON_MAX_CLASS_IDS];
extern dragon_class_traverse_fn __class_traverse_table[DRAGON_MAX_CLASS_IDS];
extern dragon_class_mark_shared_fn __class_mark_shared_table[DRAGON_MAX_CLASS_IDS];
extern int __next_class_id;

// Class-registration entrypoint for the per-class mark-shared fn (codegen
// emits one of these per class with heap fields).
int64_t dragon_class_register_mark_shared(int64_t class_id, void* fn);

// Worklist push for the per-class mark-shared fn - codegen calls this for
// container/class children so the BFS can continue from them. Idempotent:
// objects already SHARED skip the push.
void dragon_mark_shared_worklist_push(void* worklist, void* obj);

// GC tracking state
// Thread-safety:
//   - `gc_tracked`, `gc_tracked_size`, `gc_tracked_cap`: protected by `gc_lock`
//     (see runtime_core.cpp). All track/untrack/collect access takes the lock.
//   - `gc_alloc_counter`: accessed via `__atomic_*` (RELAXED). It's a soft trigger.
//   - `gc_threshold`: accessed via `__atomic_*` (RELAXED).
//   - `gc_collecting`: accessed via `__atomic_*` (ACQUIRE/RELEASE). Read by
//     `dragon_decref` / `dragon_decref_str` from any thread.
extern pthread_mutex_t gc_lock;
extern void** gc_tracked;
extern int32_t gc_tracked_size;
extern int32_t gc_tracked_cap;
extern int32_t gc_alloc_counter;
extern int32_t gc_threshold;
extern int gc_collecting;

// Concurrency latch. 0 = a single OS thread can touch the heap, so track /
// untrack / decref-to-zero / collect run WITHOUT gc_lock (the collector runs
// synchronously on that same thread between mutator ops, so there is no race).
// Latched to 1 - permanently - by dragon_gc_go_concurrent() at the first spawn
// of a second heap-mutating OS thread (scheduler workers, Thread/thread, I/O
// thread). No production GC takes a global lock per allocation; this keeps the
// uncontended single-threaded fast path lock-free. Accessed via __atomic_*.
extern int gc_concurrent;
// Set gc_concurrent=1 (idempotent). Call BEFORE pthread_create of any thread
// that can allocate/free Dragon heap objects.
void dragon_gc_go_concurrent(void);

//===----------------------------------------------------------------------===//
// Concurrent-mutation detector for SHARED containers (best-effort, Go-style)
//===----------------------------------------------------------------------===//
//
// Contract: reads of shared state are lock-free and safe; STRUCTURAL mutation
// of shared state must be serialized by the program (threading.Lock). An
// unlocked concurrent mutation corrupts memory silently ("realloc(): invalid
// next size", torn dense arrays). Rather than corrupt, the runtime detects
// the overlap and aborts loudly at the first colliding mutation, exactly like
// Go's "concurrent map writes" fatal error.
//
// Mechanism: structural mutators of dict/list/set/deque wrap their mutation
// section in begin/end. begin sets GC_FLAG_MUTATING; a second mutator seeing
// the bit already set has caught a real overlap (vthreads never yield inside
// a container op, so same-worker green threads can NEVER interleave inside a
// mutation window - only true OS-parallel mutators collide).
//
// Cost: gated first on the gc_concurrent latch (a single-OS-thread program
// pays one relaxed load + predicted branch, no RMW) and then on
// GC_FLAG_SHARED (request-local containers skip the RMW too). Only mutations
// of actually-shared containers in an actually-multi-threaded program pay
// the two flag RMWs, and those mutations are supposed to be under a Lock.
//
// Placement rules (each instrumented site verified individually):
//   - begin AFTER any validation that can raise, OR end explicitly on the
//     raise branch first: a longjmp does not run C++ destructors, and a
//     stranded MUTATING bit would false-abort the next mutation.
//   - end BEFORE decref-of-old sections where practical: today no user
//     finalizer can run inside a decref (class deallocs are generated
//     field-releases only), but if user __del__ ever lands, a finalizer
//     re-mutating the same container would self-collide with the guard.
//   - Wrappers that delegate to an instrumented core (dict_set, dict_update,
//     setdefault, list_sort) are NOT instrumented - nesting would self-abort.
#define GC_FLAG_MUTATING 0x10

// Report the collision and abort. Defined in runtime_core.cpp. Never returns.
void dragon_fatal_concurrent_mutation(const char* kind);

// Arm the detector for a structural mutation of `h`. Returns whether it was
// armed (caller must pass that to dragon_shared_mut_end on EVERY exit path
// that follows, except a longjmp-raise path, which must end BEFORE raising).
static inline bool dragon_shared_mut_begin(DragonObjectHeader* h, const char* kind) {
    if (!__atomic_load_n(&gc_concurrent, __ATOMIC_RELAXED)) return false;
    if (!(dragon_gc_flags_load(h) & GC_FLAG_SHARED)) return false;
    uint8_t prev = (uint8_t)__atomic_fetch_or(&h->gc_flags, GC_FLAG_MUTATING,
                                              __ATOMIC_ACQUIRE);
    if (prev & GC_FLAG_MUTATING) dragon_fatal_concurrent_mutation(kind);
    return true;
}

static inline void dragon_shared_mut_end(DragonObjectHeader* h, bool armed) {
    if (armed)
        __atomic_fetch_and(&h->gc_flags, (uint8_t)~GC_FLAG_MUTATING,
                           __ATOMIC_RELEASE);
}

// Per-OS-thread exception state (TLS)
extern __thread jmp_buf     __dragon_exc_stack[DRAGON_EXC_STACK_SIZE];
extern __thread int         __dragon_exc_sp;
extern __thread int         __dragon_exc_type;
extern __thread const char* __dragon_exc_msg;
// Typed-field exception instance - paired with __dragon_exc_msg. See the
// comment on DragonVThread::exc_obj. NULL when the raise site didn't
// construct a user-class instance (i.e. built-in raises through
// dragon_raise_exc).
extern __thread void*       __dragon_exc_obj;
extern __thread DragonVThread* __current_vthread;
// Exception-storage OVERRIDE. Normally NULL: exception state then lives in
// __current_vthread (a green thread) or the TLS globals (the bare OS thread).
// A running GENERATOR sets this to its own DragonVThread-shaped exc context
// (dragon_generator_next swaps it around mco_resume) so the generator body's
// try/except frames are ISOLATED from the caller's: a generator that yields
// inside a try keeps its setjmp frame alive across the suspend, instead of
// sharing the caller's exc stack (where the caller's per-iteration
// push/pop would overwrite the suspended generator's jmp_buf - a wrong-frame
// longjmp -> SIGSEGV). Kept separate from __current_vthread so it overrides
// ONLY exception storage; I/O / scheduler code keeps reading __current_vthread
// for the real green-thread identity.
extern __thread DragonVThread* __dragon_exc_vt;

// Per-OS-thread unwind cleanup stack + per-frame saved depths (TLS fallback for
// the main thread / non-vthread code; vthreads use the mirror inside their
// struct). See DragonCleanupStack.
extern __thread DragonCleanupStack __dragon_cleanup;
extern __thread int32_t            __dragon_cleanup_saved[DRAGON_EXC_STACK_SIZE];
// Live exception-frame count on the current context (see runtime_core.cpp). The
// gate codegen reads inline to skip unwind-cleanup registration when no frame is
// live. Saved/restored per vthread by the scheduler.
extern __thread int                __dragon_active_frames;

// Scheduler-aware socket I/O (defined in runtime_concurrency.cpp; ADR 038
// Phase 8). dragon_nb_connect is called from stdlib/socket.dr; the rest are
// used by the TLS BIO (runtime_tls.cpp) so TLS handshake/read/write yield to
// the green-thread scheduler on EAGAIN instead of blocking the carrier thread.
extern "C" {
int64_t dragon_nb_connect(int64_t fd, void* addr, int64_t addrlen);
int  dragon_io_wait_readable(int fd);   // 0 when readable, -1 on poll error/HUP
int  dragon_io_wait_writable(int fd);   // 0 when writable, -1 on poll error/HUP
// Deadline-bounded readability wait (R1): 0 readable, 1 timeout, -1 error.
// timeout_ms <= 0 means no deadline. Used by the TLS BIO so a stalled encrypted
// peer is dropped instead of pinning the green thread.
int  dragon_io_wait_readable_timeout(int fd, int64_t timeout_ms);
void dragon_set_nonblocking(int64_t fd);
// Binary (bytes) variants of the scheduler-aware recv/send - same yielding
// path as dragon_nb_recv/_send, but DragonBytes-typed so binary wire protocols
// (e.g. the Postgres/MySQL drivers, D032) are not forced through `str`.
DragonBytes* dragon_nb_recv_bytes(int64_t fd, int64_t max_len);
int64_t      dragon_nb_send_bytes(int64_t fd, DragonBytes* data);
}

// TLS atomic-context flag.
// Set non-zero by `dragon_decref_atomic` (and the atomic string variant) for
// the duration of a recursive dealloc - when we are tearing down a heap object
// whose final reference was dropped by an atomic decref, child decrefs must
// also use atomic variants because other threads may share those children.
// `dragon_dealloc` and per-type destroy functions consult this flag and route
// child decrefs through `dragon_decref_dispatch` / `dragon_decref_str_dispatch`.
//
// Save-and-restore semantics: nested atomic-context calls preserve the
// outer value (set to 1, restore old value on exit) - never unconditionally
// clear, so a non-atomic dealloc nested inside an atomic dealloc still uses
// the atomic path on its children.
extern __thread int __dragon_atomic_context;

//===----------------------------------------------------------------------===//
// Shared inline helpers
//===----------------------------------------------------------------------===//

static inline DragonString* dragon_string_from_data(const char* data) {
    return (DragonString*)((char*)data - offsetof(DragonString, data));
}

// Bounds of the executable image, used to range-gate the heap-string probe
// below.
// - ELF (Linux/BSD): linker-provided symbols (GNU ld default script; correct
//   under PIE after relocation).
// - Mach-O (macOS): no _end/__executable_start exist. runtime_core.cpp walks
//   the main image's LC_SEGMENT_64 load commands once at load time (via a
//   constructor) and publishes the span here. The startup defaults cover the
//   whole address space, which fails SAFE: until initialized, every pointer
//   classifies as "in image" (treated as a literal, never probed backwards),
//   never the reverse.
#ifdef __APPLE__
extern const char* __dragon_image_lo;
extern const char* __dragon_image_hi;
#define DRAGON_IMAGE_LO (__dragon_image_lo)
#define DRAGON_IMAGE_HI (__dragon_image_hi)
#else
extern char __executable_start[];
extern char _end[];
#define DRAGON_IMAGE_LO ((const char*)__executable_start)
#define DRAGON_IMAGE_HI ((const char*)_end)
#endif

/// Heap-vs-literal check for a `const char*` string pointer. Borrowed C
/// literals don't have a DragonObjectHeader; heap DragonStrings do, with
/// `type_tag == DRAGON_TAG_STR` and the GC_FLAG_HEAP_OBJ bit set.
///
/// The image range check MUST come before the header probe. A literal lives
/// in the binary's rodata, and probing the bytes BEFORE it is unsound: an
/// unlucky neighboring symbol can fake a valid header purely by layout
/// coincidence (not theoretical: a rodata layout shift once made an ssl.dr
/// literal's neighbor match tag+flags, and the next exception-slot decref
/// WROTE a refcount into the read-only page - SEGV in
/// test_ssl_roundtrip). Heap allocations can never live inside the image, so
/// the two compares are both sufficient and cheaper than the header loads
/// they replace on the literal path
static inline int dragon_str_is_heap(const char* s) {
    if (!s) return 0;
    if (s >= DRAGON_IMAGE_LO && s < DRAGON_IMAGE_HI) return 0;
    DragonString* ds = dragon_string_from_data(s);
    return ds->header.type_tag == DRAGON_TAG_STR &&
           (ds->header.gc_flags & GC_FLAG_HEAP_OBJ) ? 1 : 0;
}

/// Total byte length of a `const char*`'s underlying data buffer:
/// `len * kind` for heap strings, `strlen(s)` for literals.
static inline int64_t dragon_str_total_bytes(const char* s) {
    if (!s) return 0;
    if (dragon_str_is_heap(s)) {
        DragonString* ds = dragon_string_from_data(s);
        return ds->len * (int64_t)ds->kind;
    }
    return (int64_t)strlen(s);
}

// HashDoS defense: the dict/set string hash is keyed with a per-process random
// secret (seeded once at startup from the OS CSPRNG - dragon_hash_secret_init,
// runtime_platform.cpp). An unkeyed hash (the old FNV-1a) lets an attacker who
// knows the algorithm precompute keys that all land in one bucket, turning an
// O(1) dict into O(n) per probe and O(n^2) per request - a trivial DoS on any
// server that builds a dict/set from request data (JSON body, query params,
// headers). SipHash-1-3 is the same construction CPython and Rust's default
// HashMap use; keying it makes collisions unpredictable across processes.
extern uint64_t __dragon_hash_k0;
extern uint64_t __dragon_hash_k1;
void dragon_hash_secret_init(void);  // idempotent; called from a constructor

static inline uint64_t dragon_hash_read_le64(const unsigned char* p) {
    return (uint64_t)p[0]        | ((uint64_t)p[1] << 8)  |
           ((uint64_t)p[2] << 16) | ((uint64_t)p[3] << 24) |
           ((uint64_t)p[4] << 32) | ((uint64_t)p[5] << 40) |
           ((uint64_t)p[6] << 48) | ((uint64_t)p[7] << 56);
}

/// SipHash-1-3 (1 compression round, 3 finalization rounds) keyed by k0/k1.
static inline uint64_t dragon_siphash13(const void* in, size_t inlen,
                                        uint64_t k0, uint64_t k1) {
    uint64_t v0 = 0x736f6d6570736575ULL ^ k0;
    uint64_t v1 = 0x646f72616e646f6dULL ^ k1;
    uint64_t v2 = 0x6c7967656e657261ULL ^ k0;
    uint64_t v3 = 0x7465646279746573ULL ^ k1;
    const unsigned char* data = (const unsigned char*)in;
    const unsigned char* end = data + (inlen - (inlen % 8));
    uint64_t b = (uint64_t)inlen << 56;
#define DRAGON_SIPROUND \
    do { \
        v0 += v1; v1 = (v1 << 13) | (v1 >> 51); v1 ^= v0; v0 = (v0 << 32) | (v0 >> 32); \
        v2 += v3; v3 = (v3 << 16) | (v3 >> 48); v3 ^= v2; \
        v0 += v3; v3 = (v3 << 21) | (v3 >> 43); v3 ^= v0; \
        v2 += v1; v1 = (v1 << 17) | (v1 >> 47); v1 ^= v2; v2 = (v2 << 32) | (v2 >> 32); \
    } while (0)
    for (; data != end; data += 8) {
        uint64_t m = dragon_hash_read_le64(data);
        v3 ^= m;
        DRAGON_SIPROUND;            // c = 1 compression round
        v0 ^= m;
    }
    switch (inlen & 7) {
        case 7: b |= (uint64_t)data[6] << 48;  /* fallthrough */
        case 6: b |= (uint64_t)data[5] << 40;  /* fallthrough */
        case 5: b |= (uint64_t)data[4] << 32;  /* fallthrough */
        case 4: b |= (uint64_t)data[3] << 24;  /* fallthrough */
        case 3: b |= (uint64_t)data[2] << 16;  /* fallthrough */
        case 2: b |= (uint64_t)data[1] << 8;   /* fallthrough */
        case 1: b |= (uint64_t)data[0];        /* fallthrough */
        case 0: break;
    }
    v3 ^= b;
    DRAGON_SIPROUND;
    v0 ^= b;
    v2 ^= 0xff;
    DRAGON_SIPROUND; DRAGON_SIPROUND; DRAGON_SIPROUND;  // d = 3 finalization rounds
#undef DRAGON_SIPROUND
    return v0 ^ v1 ^ v2 ^ v3;
}

/// Keyed content hash of a Dragon string's underlying bytes. Uses
/// `dragon_str_total_bytes` so kind=4 strings hash all of their UCS-4 bytes
/// (not just up to the first internal NUL). Canonical-kind storage ensures
/// equal-content strings have equal byte representations.
static inline uint64_t dragon_str_content_hash(const char* s) {
    int64_t n = dragon_str_total_bytes(s);
    uint64_t h = dragon_siphash13(s, (size_t)n, __dragon_hash_k0, __dragon_hash_k1);
    return h | 1; // reserve 0
}

/// Byte-equality of two Dragon strings via canonical-kind storage. Same kind
/// + same byte length + memcmp. Use after a hash-collision short-circuit.
static inline int dragon_str_bytes_equal(const char* a, const char* b) {
    if (a == b) return 1;
    if (!a || !b) return 0;
    int64_t la = dragon_str_total_bytes(a);
    int64_t lb = dragon_str_total_bytes(b);
    if (la != lb) return 0;
    return memcmp(a, b, (size_t)la) == 0 ? 1 : 0;
}

static inline void dragon_incref_tagged(int64_t val, uint8_t tag);
static inline void dragon_decref_tagged(int64_t val, uint8_t tag);

//===----------------------------------------------------------------------===//
// Core runtime functions used across TUs
//===----------------------------------------------------------------------===//

// GC / refcounting
void dragon_incref(void* obj);
void dragon_decref(void* obj);
void dragon_incref_atomic(void* obj);
void dragon_decref_atomic(void* obj);
// TAG-GATED closure RC (defined in runtime_builtins.cpp). Safe on a bare
// fn ptr (no header) and null - used by dragon_{in,de}cref_tagged for
// TAG_CLOSURE elements so Callable containers RC closures without touching a
// headerless code pointer.
void dragon_incref_callable(void* p);
void dragon_decref_callable(void* p);
void dragon_make_immortal(void* obj);
int64_t dragon_is_immortal_obj(void* obj);
void dragon_gc_track(void* obj);
void dragon_gc_untrack(void* obj);
void dragon_gc_set_threshold(int64_t n);
int64_t dragon_gc_collect();

// Mark an object SHARED (atomic OR into gc_flags). Idempotent; safe under
// concurrent callers. Does NOT recurse into children. Used at the leaf of
// `dragon_mark_shared_deep` and inside RC-atomic entry points.
void dragon_mark_shared(void* obj);

// Mark `obj` and every transitively-reachable child SHARED. Walks via the
// per-type `dragon_traverse` table; visits each object at most once via the
// SHARED bit acting as a "seen" marker. Called from the fire-site (before
// atomic-incref'ing the heap arg) and from container write-barriers when a
// new value is stored into a SHARED parent.
void dragon_mark_shared_deep(void* obj);

// Same for str - `s` is a `const char*` pointer into a heap DragonString's
// data buffer. Skips string literals (no DragonObjectHeader) safely.
void dragon_mark_shared_str(const char* s);

// String refcounting
void dragon_incref_str(const char* s);
void dragon_decref_str(const char* s);
// Identity retain: incref + return s. Codegen's str(s)-of-a-str / single-part
// f"{s}" lowering - makes the identity result an owned +1 (see runtime_string.cpp).
const char* dragon_str_retain(const char* s);
void dragon_incref_str_atomic(const char* s);
void dragon_decref_str_atomic(const char* s);
const char* dragon_string_dup(const char* s);
// Dup a KNOWN plain C string without the DragonString header probe
// (safe for stack snprintf buffers - see runtime_string.cpp).
const char* dragon_string_dup_cstr(const char* s);
// Snapshot a message for re-raising only if it is a mortal heap string (else
// return it unchanged - no dup, no leak). See definition in runtime_string.cpp.
const char* dragon_exc_msg_preserve(const char* s);

// Force-free a heap string when its refcount has hit 0 inside the
// cycle collector's clear_refs phase. Bypasses `dragon_decref_str`'s
// `gc_collecting` guard, which would otherwise leave the string allocated
// because Phase 4 zeros the container's size and Phase 6's destroy iterates
// nothing. Safe because the string is owned exclusively by the unreachable
// container we're tearing down. Always honors the immortal sentinel.
void dragon_str_force_free_if_zero(const char* s);

// Container constructors (needed by sync wrappers, builtins, etc.)
DragonList* dragon_list_new(int64_t capacity);
DragonList* dragon_list_new_tagged(int64_t capacity, int64_t elem_tag);
void dragon_list_append(DragonList* list, int64_t value);
int64_t dragon_list_get(DragonList* list, int64_t index);
void dragon_list_set(DragonList* list, int64_t index, int64_t value);
int64_t dragon_list_len(DragonList* list);
void dragon_list_insert(DragonList* list, int64_t index, int64_t value);
void dragon_list_remove(DragonList* list, int64_t value);
int64_t dragon_list_pop(DragonList* list, int64_t index);
void dragon_list_delitem(DragonList* list, int64_t index);
void dragon_list_clear(DragonList* list);
void dragon_list_extend(DragonList* list, DragonList* other);
int64_t dragon_list_index(DragonList* list, int64_t value);
int64_t dragon_list_count(DragonList* list, int64_t value);
int64_t dragon_list_contains(DragonList* list, int64_t value);
// Container repr: return a DragonString (str() / f-string interpolation).
const char* dragon_list_to_str(DragonList* list);
const char* dragon_dict_to_str(DragonDict* d);
const char* dragon_dict_int_to_str(DragonDict* d);
const char* dragon_set_to_str(DragonSet* s);
const char* dragon_tuple_to_str(DragonTuple* t);
void dragon_list_sort(DragonList* list);
void dragon_list_sort_ex(DragonList* list, int64_t reverse);
void dragon_list_reverse(DragonList* list);
DragonList* dragon_list_copy(DragonList* list);
// dub (docs/002 2.7): spine copy + per-element deep copy by tag; every
// element arrives +1 (fresh container or identity-retained immutable).
DragonList* dragon_list_deep_copy(DragonList* list);
DragonDict* dragon_dict_deep_copy(DragonDict* d);
int64_t dragon_deep_copy_tagged(int64_t val, int64_t tag);
void* dragon_obj_retain(void* p);
DragonList* dragon_list_repeat(DragonList* src, int64_t count);
// list + list → fresh list (copy lhs, extend with rhs). Box-aware: dispatches
// to dragon_list_box_concat when either operand is a list[Any] (DragonListBox).
DragonList* dragon_list_concat(DragonList* a, DragonList* b);

// D030 Phase 3 - Monomorphized list ops. Codegen calls these by element kind;
// each returns / accepts the native type (double, void*) - no i64 funnel.
// Polymorphic destroy still dispatches off elem_tag (shared layout prefix).

// list[float] - native f64
DragonListF64* dragon_list_new_f64(int64_t capacity);
double         dragon_list_get_f64(DragonListF64* list, int64_t index);
void           dragon_list_set_f64(DragonListF64* list, int64_t index, double value);
void           dragon_list_append_f64(DragonListF64* list, double value);

// list[<heap>] - native ptr; ops are refcount-aware so codegen doesn't have to
// emit incref/decref around set/append.
DragonListPtr* dragon_list_new_ptr(int64_t capacity, int64_t elem_tag);
void*          dragon_list_get_ptr(DragonListPtr* list, int64_t index);
void           dragon_list_set_ptr(DragonListPtr* list, int64_t index, void* value);
void           dragon_list_append_ptr(DragonListPtr* list, void* value);

// D039 Phase 4: list[Any] box-storage runtime ops.
DragonListBox* dragon_list_box_new(int64_t capacity);
void           dragon_list_box_append(DragonListBox* list, int64_t tag, int64_t payload);
void           dragon_list_box_delitem(DragonListBox* list, int64_t index);

// Typed list[str] join - used by D017 Phase 4.B block-interp flatten and
// Phase 4.C `| join` filter / `!{*expr}` spread. Direct ptr-array walk; no
// per-element box decode, no tag dispatch. Sep may be NULL or "" to
// concatenate without a separator.
const char*    dragon_str_join_ptr(const char* sep, DragonListPtr* list);

DragonDict* dragon_dict_new(int64_t cap);
void dragon_dict_set(DragonDict* d, const char* key, int64_t value);
void dragon_dict_set_tagged(DragonDict* d, const char* key, int64_t value, int64_t tag);
int64_t dragon_dict_get(DragonDict* d, const char* key);
int64_t dragon_dict_get_tag(DragonDict* d, const char* key);
int64_t dragon_dict_get_checked(DragonDict* d, const char* key, int64_t expected_tag);
int64_t dragon_dict_len(DragonDict* d);
int64_t dragon_dict_has_key(DragonDict* d, const char* key);
// C9-B `**dict` spread: raise TypeError if any (str) key is not in `allowed`
// (the bindable parameter names). No-op for int-keyed/empty dicts.
void dragon_dict_reject_unknown_keys(DragonDict* d, const char** allowed,
                                     int64_t n, const char* func_name);
int64_t dragon_dict_get_default(DragonDict* d, const char* key, int64_t def);
void   dragon_dict_del(DragonDict* d, const char* key);
DragonList* dragon_dict_keys(DragonDict* d);

// D030 Phase 3.E - Typed dict ops for str-keyed monomorphic dicts. Codegen
// calls these so the value crosses the codegen↔runtime boundary at its
// native LLVM type instead of being i64-bashed.
double dragon_dict_get_str_f64(DragonDict* d, const char* key);
void   dragon_dict_set_str_f64(DragonDict* d, const char* key, double value);
void*  dragon_dict_get_str_ptr(DragonDict* d, const char* key, int64_t expected_tag);
void   dragon_dict_set_str_ptr(DragonDict* d, const char* key, void* value, int64_t tag);

// D030 Phase 3.G - Typed dict ops for int-keyed monomorphic dicts. Reuses
// the DragonDict layout: the i64 key is stored verbatim in the existing
// `const char* key` slot (8 bytes either way) and hashed via SplitMix64
// instead of FNV-1a. Codegen picks this family when the static dict type
// is `dict[int, V]`. Setters handle refcount semantics on overwrite the
// same way the str-keyed variants do.
void   dragon_dict_int_set(DragonDict* d, int64_t key, int64_t value);
void   dragon_dict_int_set_tagged(DragonDict* d, int64_t key, int64_t value, int64_t tag);
void   dragon_dict_int_set_f64(DragonDict* d, int64_t key, double value);
void   dragon_dict_int_set_str(DragonDict* d, int64_t key, const char* value);
void   dragon_dict_int_set_ptr(DragonDict* d, int64_t key, void* value, int64_t tag);
int64_t dragon_dict_int_get(DragonDict* d, int64_t key);
int64_t dragon_dict_int_get_tag(DragonDict* d, int64_t key);
int64_t dragon_dict_int_get_checked(DragonDict* d, int64_t key, int64_t expected_tag);
double dragon_dict_int_get_f64(DragonDict* d, int64_t key);
const char* dragon_dict_int_get_str(DragonDict* d, int64_t key);
void*  dragon_dict_int_get_ptr(DragonDict* d, int64_t key, int64_t expected_tag);
int64_t dragon_dict_int_get_default(DragonDict* d, int64_t key, int64_t def);
int64_t dragon_dict_int_has_key(DragonDict* d, int64_t key);
int64_t dragon_dict_int_pop(DragonDict* d, int64_t key);
int64_t dragon_dict_int_pop_default(DragonDict* d, int64_t key, int64_t def);
void   dragon_dict_int_del(DragonDict* d, int64_t key);
int64_t dragon_dict_int_setdefault(DragonDict* d, int64_t key, int64_t def);
DragonList* dragon_dict_int_keys(DragonDict* d);
void   dragon_print_dict_int(DragonDict* d);

DragonTuple* dragon_tuple_new(int64_t count);
void dragon_tuple_set(DragonTuple* t, int64_t index, int64_t val);
void dragon_tuple_set_tagged(DragonTuple* t, int64_t index, int64_t val, int64_t tag);
int64_t dragon_tuple_get(DragonTuple* t, int64_t index);
int64_t dragon_tuple_len(DragonTuple* t);

DragonSet* dragon_set_new();
DragonSet* dragon_set_from_list(DragonList* list);
DragonSet* dragon_set_new_tagged(int64_t elem_tag);
void dragon_set_adopt_tag(DragonSet* s, int64_t tag);
void dragon_set_add(DragonSet* s, int64_t val);
int64_t dragon_set_contains(DragonSet* s, int64_t val);

DragonBytes* dragon_bytes_new(const uint8_t* data, int64_t len);
DragonBytes* dragon_bytes_from_list(DragonList* list);

// Compression (gzip via libz, zstd via libzstd) - implemented in
// runtime_compress.cpp. Bytes-in / bytes-out one-shot; callers stream by
// chunking at the Dragon layer (stdlib/gzip.dr, stdlib/zstandard.dr).
DragonBytes* dragon_zlib_compress(DragonBytes* src, int64_t level);
DragonBytes* dragon_zlib_decompress(DragonBytes* src);
DragonBytes* dragon_zstd_compress(DragonBytes* src, int64_t level);
DragonBytes* dragon_zstd_decompress(DragonBytes* src);

// Bytes-aware whole-file IO (no UTF-8 codec). Used by gzip, tarfile, etc.
DragonBytes* dragon_read_file_bytes(const char* path);
int64_t dragon_write_file_bytes(const char* path, DragonBytes* data);

// Container destroy (called by dragon_dealloc in core)
void dragon_list_destroy(DragonList* l);
void dragon_list_box_destroy(DragonListBox* l);  // D039 Phase 4
void dragon_dict_destroy(DragonDict* d);
void dragon_tuple_destroy(DragonTuple* t);
void dragon_set_destroy(DragonSet* s);
void dragon_bytes_destroy(DragonBytes* b);
void dragon_deque_destroy(DragonDeque* d);
void dragon_generator_destroy(void* gen_ptr);
// Reclaim a generator abandoned mid-resume by a longjmp (its body raised). See
// the definition in runtime_concurrency.cpp (MINICORO_IMPL TU).
void dragon_generator_abandon(void* gen_ptr);

// Exception functions (used by generators, concurrency)
void dragon_raise_exc(int64_t type, const char* msg);
// Raise with a RAW C string message (rodata literal, stack snprintf buffer,
// errno/library text). Copies it into a fresh heap DragonString before the
// longjmp, so the exc_msg slot never holds a raw C pointer: the is_heap
// probe on the slot always reads a valid header (no OOB read 24 bytes before
// a literal), and a stack buffer can't dangle after the raising frame is
// unwound. Every runtime-internal raise site MUST use this; the plain
// dragon_raise_exc above is for codegen str-typed messages only.
void dragon_raise_exc_cstr(int64_t type, const char* msg);
// Consume variants: take an owned +1 message (freshly dup'd / allocated)
// into the slot instead of dup'ing a borrow. See dragon_exc_msg_set.
void dragon_raise_exc_consume(int64_t type, const char* msg);
void dragon_raise_exc_obj(int64_t type, void* obj, const char* msg);
void dragon_raise_exc_obj_consume(int64_t type, void* obj, const char* msg);
// `except ... as e` binding: returns the in-flight message with its own +1
// (mortal heap only; literal/immortal no-op) - handler scope cleanup drops it.
const char* dragon_exc_bind_msg(void);
int64_t dragon_exc_matches(int64_t raised, int64_t caught);

// Checked allocation. Callers were dereferencing a NULL malloc
// result on OOM -> SIGSEGV (e.g. chr() via dragon_string_alloc_ucs4), or
// overwriting the only pointer before checking realloc (-> NULL-write/leak).
// These centralize the contract:
//   *_x*   raise MemoryError (longjmp) - for user-reachable allocations. The
//          raise never returns, so a failed dragon_xrealloc leaves the caller's
//          OLD pointer intact (the realloc-into-temp pattern, built in).
//   *_or_abort  abort with a diagnostic - for the GC / exception machinery,
//          where raising would re-enter the very subsystem mid-failure.
// One branch, hinted unlikely, so the hot (success) path costs nothing - the
// same discipline dragon_string_alloc_raw already documents.
static inline void* dragon_xmalloc(size_t n) {
    void* p = malloc(n);
    if (__builtin_expect(p == nullptr, 0))
        dragon_raise_exc_cstr(43, "MemoryError: out of memory");
    return p;
}
static inline void* dragon_xrealloc(void* old, size_t n) {
    void* p = realloc(old, n);
    if (__builtin_expect(p == nullptr, 0))
        dragon_raise_exc_cstr(43, "MemoryError: out of memory");
    return p;
}
static inline void* dragon_xmalloc_or_abort(size_t n) {
    void* p = malloc(n);
    if (__builtin_expect(p == nullptr, 0)) {
        fprintf(stderr, "dragon: out of memory\n");
        abort();
    }
    return p;
}
static inline void* dragon_xrealloc_or_abort(void* old, size_t n) {
    void* p = realloc(old, n);
    if (__builtin_expect(p == nullptr, 0)) {
        fprintf(stderr, "dragon: out of memory\n");
        abort();
    }
    return p;
}

// Unwind cleanup stack (see DragonCleanupStack). Codegen emits push at each
// owned-heap-local declaration, update on reassignment, and reset-by-depth at
// normal scope exit; dragon_exc_cleanup_unwind runs at every longjmp arrival.
int32_t dragon_cleanup_push(int64_t val, int32_t kind, int32_t tag);
void    dragon_cleanup_update(int32_t slot, int64_t val, int32_t tag);
int32_t dragon_cleanup_depth(void);
void    dragon_cleanup_reset(int32_t depth);
void    dragon_exc_cleanup_unwind(void);

// D033: Method reflection setters and lookup. Called from codegen at module
// init time (after dragon_class_descriptor_create). dragon_class_find_method
// walks the parent chain via DragonClassDescriptor.parent so each class only
// needs to advertise its OWN methods.
void  dragon_class_descriptor_set_methods(int64_t descriptor,
                                          const char** method_names,
                                          void** method_fn_ptrs,
                                          uint8_t* method_kinds,
                                          int64_t num_methods);
void  dragon_class_descriptor_set_method_bound_thunks(int64_t descriptor,
                                                       void** bound_thunks);
void* dragon_class_find_method(int64_t descriptor, const char* name);
int64_t dragon_class_find_method_kind(int64_t descriptor, const char* name);
void* dragon_class_find_method_bound(int64_t descriptor, const char* name);

// D033 Phase 2: dir() builtin. Walks the instance's (or descriptor's) class
// MRO collecting field + method names; returns a sorted DragonListPtr[str]
// of refcounted heap strings. `is_descriptor` selects between instance ptr
// (look up class_id) and descriptor ptr.
DragonListPtr* dragon_dir(int64_t instance_or_desc, int64_t is_descriptor);

// String allocation helpers (used by string and other TUs)
const char* dragon_string_alloc(const char* src, int64_t len);
const char* dragon_str_intern(const char* utf8_bytes, int64_t byte_len);
DragonString* dragon_string_alloc_raw(int64_t len);
/// UTF-8 encode helper. For kind=1 / literals, returns NULL (caller uses
/// `s` directly). For kind=4, returns a freshly malloc'd UTF-8 buffer the
/// caller must free. `*out_byte_len` is set in both cases.
char* dragon_str_to_utf8_alloc(const char* s, int64_t* out_byte_len);

// Print helpers (used in collections printing)
void dragon_print_tagged(int64_t value, int64_t tag);

// String operations (used by builtins, collections)
const char* dragon_int_to_str(int64_t value);
const char* dragon_float_to_str(double value);
/// Python-repr float formatter shared by scalar print, str(), f-strings and
/// container repr (single source of truth). Writes into `buf` (>= 32 bytes),
/// returns length. Defined in runtime_string.cpp.
int dragon_format_double_into(double value, char* buf, size_t bufsz);
void dragon_slice_indices(int64_t len, int64_t* start, int64_t* stop, int64_t step);
void dragon_print_str(const char* s);

// Additional dict operations (used by syncdict in concurrency)
DragonList* dragon_dict_values(DragonDict* d);
DragonList* dragon_dict_items(DragonDict* d);
int64_t dragon_dict_pop(DragonDict* d, const char* key);
int64_t dragon_dict_pop_default(DragonDict* d, const char* key, int64_t def);
int64_t dragon_dict_popitem(DragonDict* d);
DragonDict* dragon_dict_fromkeys(DragonList* keys, int64_t value, int64_t tag);
void dragon_dict_clear(DragonDict* d);
void dragon_dict_update(DragonDict* d, DragonDict* other);
int64_t dragon_dict_setdefault(DragonDict* d, const char* key, int64_t def);
DragonDict* dragon_dict_copy(DragonDict* d);

// List operations (used by builtins)
void dragon_print_list_int(DragonList* list);
void dragon_print_list_str(DragonList* list);
void dragon_print_list_float(DragonList* list);
void dragon_print_list_bool(DragonList* list);
DragonList* dragon_list_slice(DragonList* l, int64_t start, int64_t stop, int64_t step);

// Tag-dispatched incref/decref (needs incref_str/decref_str/incref/decref declarations above)
static inline void dragon_incref_tagged(int64_t val, uint8_t tag) {
    if (!val) return;
    if (tag == TAG_STR)
        dragon_incref_str((const char*)(uintptr_t)val);
    else if (tag == TAG_LIST || tag == TAG_DICT || tag == TAG_BYTES)
        dragon_incref((void*)(uintptr_t)val);
    else if (tag == DRAGON_TAG_CLOSURE)  // Callable element (closure / bare fn)
        dragon_incref_callable((void*)(uintptr_t)val);
}

static inline void dragon_decref_tagged(int64_t val, uint8_t tag) {
    if (!val) return;
    if (tag == TAG_STR)
        dragon_decref_str((const char*)(uintptr_t)val);
    else if (tag == TAG_LIST || tag == TAG_DICT || tag == TAG_BYTES)
        dragon_decref((void*)(uintptr_t)val);
    else if (tag == DRAGON_TAG_CLOSURE)  // tag-gated (bare fn safe)
        dragon_decref_callable((void*)(uintptr_t)val);
}

// Route a child decref through the atomic variant if we're inside
// an atomic-context dealloc, otherwise use the normal variant. Per-type
// destroy functions invoked from `dragon_dealloc` MUST go through these.
static inline void dragon_decref_dispatch(void* obj) {
    if (__dragon_atomic_context) dragon_decref_atomic(obj);
    else dragon_decref(obj);
}

static inline void dragon_decref_str_dispatch(const char* s) {
    if (__dragon_atomic_context) dragon_decref_str_atomic(s);
    else dragon_decref_str(s);
}

#ifdef __cplusplus
}
#endif

#endif // DRAGON_RUNTIME_INTERNAL_H
