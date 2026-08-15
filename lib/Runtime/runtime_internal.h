/// Dragon Runtime - Shared Internal Header
/// Types, macros, enums, and extern declarations used across runtime TUs.
#ifndef DRAGON_RUNTIME_INTERNAL_H
#define DRAGON_RUNTIME_INTERNAL_H

#include <cmath>
#include <csetjmp>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <stdlib.h>
#include <cstring>
// winsock2 must be included before windows.h (and before MinGW's <unistd.h>) to pin the v2 API.
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
// MinGW ships winpthreads (pthread.h) and unistd.h for the POSIX subset we use; MSVC is not a target.
#include <pthread.h>
#include <unistd.h>
// Atomics: use __atomic_* builtins only. Do NOT include <stdatomic.h>; it is a
// C11 header that GCC < 13 rejects in C++ TUs (breaks the ubuntu-22.04 CI).

// minicoro decls only (MINICORO_IMPL in runtime_concurrency.cpp). VMEM allocator guards each
// green-thread's ~2MB stack so overflow faults instead of corrupting a neighbour's refcount header (was exploitable). Must precede every minicoro.h include.
#ifndef MCO_USE_VMEM_ALLOCATOR
#define MCO_USE_VMEM_ALLOCATOR
#endif
#include "minicoro.h"

#ifdef __cplusplus
extern "C" {
#endif

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
// Set on objects that escaped to another OS thread (via fire/thread or a SHARED parent); gates
// dragon_incref/decref to the atomic variants. Lives in refcount's cache line so the check is free.
#define GC_FLAG_SHARED    0x04
// Set by the cycle collector on objects queued into `to_free` after trial-deletion; cleared
// implicitly on next dragon_obj_init. Mutator decref checks this (not bare gc_collecting) so only collector-owned objects are skipped, keeping gc_tracked from bloating with refcount-0 orphans (was an O(N^2) GC slowdown under multi-threaded allocation).
#define GC_FLAG_IN_TO_FREE 0x08
#define GC_FLAG_HEAP_OBJ  0x80

#define DRAGON_IMMORTAL_REFCOUNT ((int64_t)0x4000000000000000LL)

static inline bool dragon_is_immortal(void* obj) {
    return obj && ((DragonObjectHeader*)obj)->refcount >= DRAGON_IMMORTAL_REFCOUNT;
}

// Header fields another thread may write atomically (mark-shared's fetch_or on gc_flags; atomic
// refcount ops). RELAXED load emits the same instruction as a plain read but isn't a data race; ordering comes from the protocol (SHARED-marked before publishing to another thread).
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

// Value tags for dict entries and container elements: shared with codegen via
// include/dragon/ValueTags.h (single source of truth for the tag ABI).
#include "dragon/ValueTags.h"
static_assert(TAG_CALLABLE == (int8_t)DRAGON_TAG_CLOSURE,
              "value-tag slot 10 must stay aligned with the closure object tag");

// A value-tag is "traceable" iff the cycle collector must follow it: list, dict, bytes, or
// Callable (tag 10; a bare fn ptr is a safe hash-miss no-op). Single source of truth shared by the traverse fns and the acyclic-skip allocation gates so they can't diverge (a gap here once meant closure-only dicts/tuples were never enrolled and their cycles never collected).
static inline bool dragon_value_tag_is_traceable(int8_t tag) {
    return tag == TAG_LIST || tag == TAG_DICT || tag == TAG_BYTES ||
           tag == (int8_t)DRAGON_TAG_CLOSURE;
}

#define DRAGON_EXC_STACK_SIZE 32
#define DRAGON_EXC_MAX_USER 256

// Side channel for longjmp unwinding, which skips codegen's scope-cleanup decrefs and would
// otherwise leak owned heap locals. Codegen pushes/updates a snapshot per local; per-context (TLS + per-vthread) since interleaved green threads would corrupt a shared LIFO.
typedef struct DragonCleanupStack {
    int64_t* vals;   // snapshot of the owned heap pointer (i64-shaped)
    int32_t* kinds;  // DragonCleanupKind - which decref to call on unwind
    int32_t* tags;   // box value-tag for DCLEAN_UNION; 0 otherwise
    int32_t  sp;     // next free slot (0 = empty)
    int32_t  cap;    // allocated capacity
} DragonCleanupStack;

// How to release a cleanup entry on unwind, mirrors emitScopeCleanupFor's per-VarKind dispatch.
// Kept distinct from DragonValueTag/DragonTypeTag so the codegen/runtime contract is explicit.
enum DragonCleanupKind {
    DCLEAN_STR      = 1,  // dragon_decref_str(val)
    DCLEAN_CALLABLE = 2,  // dragon_decref_callable(val)  (tag-gated: no-ops on bare fn ptr)
    DCLEAN_OBJ      = 3,  // dragon_decref(val)           (containers, instances, cells, deque, file)
    DCLEAN_UNION    = 4,  // tag-conditional, using the entry's `tag` (mirrors emitUnionDecref)
    // Pending `defer` call (defer.md): val is a codegen thunk `void(*)(int64_t*)`, tag is its
    // snapshot-arg count; those entries carry the snapshot values and drain per-kind so defers run before the borrowed values release.
    DCLEAN_DEFER_CALL = 5,
    // Raw malloc'd scratch inside a runtime function whose callee can raise: free(val) on
    // unwind. Runtime-internal only; codegen never emits it.
    DCLEAN_FREE = 6,
};

enum VThreadYieldReason {
    YIELD_COOP  = 0,
    YIELD_IO    = 1,
    YIELD_SLEEP = 2
};

/// Dragon string, PEP 393-lite layout. kind=1: ASCII/Latin-1, NUL-terminated C string.
/// kind=4: UCS-4 code points, not NUL-terminated. `len` is code-point count, not byte length (byte len = len*kind).
typedef struct {
    DragonObjectHeader header;
    int64_t len;          // code-point count
    uint8_t kind;         // 1 or 4
    uint8_t _pad[3];      // pad to 4-byte boundary for `cap`
    int32_t cap;          // byte capacity of `data` (excl NUL), for amortized append;
                          // packed into former padding so header stays 32B/8-aligned. 2 GiB cap; larger strings fall back.
    char    data[];       // len * kind bytes; kind=1 NUL-terminated
} DragonString;

/// Byte length of a DragonString's data buffer (excluding any NUL terminator).
static inline int64_t dragon_str_byte_len(const DragonString* s) {
    return s->len * (int64_t)s->kind;
}

/// Clamp a byte count to the int32 `cap` range. Strings over 2GiB skip the in-place append
/// fast path; `cap` then understates the buffer (safe: forces realloc) but never overstates it.
static inline int32_t dragon_cap_clamp(int64_t bytes) {
    return bytes > 0x7fffffff ? (int32_t)0x7fffffff : (int32_t)bytes;
}

/// Packed-element storage. elem_size is 1 for list[bool] (TAG_BOOL), 8 otherwise; callers
/// MUST use dragon_list_load/store. Bool packing dropped a 1M-element is_prime sieve from 8MB (L2-spilling) to 1MB (L2-resident).
struct DragonList {
    DragonObjectHeader header;
    void*    data;        // bytes; stride = elem_size
    int64_t  size;        // # of elements
    int64_t  capacity;    // capacity in elements
    uint8_t  elem_tag;    // TAG_INT, TAG_STR, TAG_BOOL, ...
    uint8_t  elem_size;   // 1 (bool) or 8 (int/float/ptr/etc.)
};

/// Native Any/Union value: layout matches `%dragon.box = {i64,i64}` in LLVM (D039), passed in
/// two registers (SysV ABI, <=16B structs). Single source of truth for every TU using a box.
struct DragonBox {
    int64_t tag;
    int64_t payload;
};

/// D030 Phase 3: monomorphized list family. DragonList (above) is the I64 variant (int/bool,
/// packed bool). DragonListF64/Ptr share its exact layout/offsets (only `data`'s static type differs) so polymorphic ops can cast between them; codegen picks by list[T]: int/bool->DragonList, float->F64, heap types->Ptr, Any->DragonListBox.

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

/// D039 Phase 4: list[Any], per-element {tag, payload} 16-byte box storage (Go's []interface{}
/// analog). List owns +1 on each refcounted payload; set/append/destroy own the RC accounting so codegen is free to borrow.
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

/// Write element `i` of a DragonList: truncates to a byte for kind=1, writes the full slot
/// for kind=8. Byte-only; caller handles RC on the previous value.
static inline void dragon_list_store(struct DragonList* l, int64_t i, int64_t v) {
    if (l->elem_size == 1) ((uint8_t*)l->data)[i] = (uint8_t)(v & 0xFF);
    else ((int64_t*)l->data)[i] = v;
}

struct DictEntry {
    uint64_t hash;
    const char* key;
    int64_t value;
    int8_t tag;
    // Lazy-delete tombstone (audit 4.2/4.3): a removed entry is marked dead in place instead of
    // shifting the dense array (was O(n) per delete, O(n^2) bulk - an algorithmic DoS). Skipped by scans, reclaimed by dict_compact; fits existing padding.
    int8_t dead;
};

struct DragonDict {
    DragonObjectHeader header;
    DictEntry* entries;
    int64_t* indices;
    // High-water mark of dense `entries`, including dead slots. Inserts append at `size`;
    // deletes never shrink it (mark dead); dict_compact resets it to `used`.
    int64_t size;
    // Live entry count (size minus dead slots); what len() returns.
    int64_t used;
    int64_t capacity;
    int64_t index_size;
    uint8_t key_kind;
};

enum DragonDictKeyKind : uint8_t {
    DRAGON_DICT_KEY_INT = 0,
    DRAGON_DICT_KEY_STR = 1,
    DRAGON_DICT_KEY_FLOAT = 2,
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
    // Typed exception instance alongside type+msg; NULL for built-in raises, set by
    // dragon_raise_exc_obj when the raise site constructed a user-class instance. Read by the except handler to bind `as e` to the instance.
    void*       exc_obj;
    // Per-vthread unwind cleanup stack (mirrors TLS __dragon_cleanup) so a raise frees a
    // vthread's owned heap locals. cleanup_saved[i] is the depth at frame i's push; longjmp arrival frees back down to it.
    DragonCleanupStack cleanup;
    int32_t     cleanup_saved[DRAGON_EXC_STACK_SIZE];
    // Live exception-frame count; the scheduler swaps it with TLS __dragon_active_frames
    // around mco_resume so the inline cleanup gate stays correct across a worker migration.
    int         active_frames;
    mco_coro*   coro;
    int64_t     result;
    volatile int8_t done;
    volatile int8_t yield_reason;
    // I/O park handshake (dragon_io_arm_park/wake): resolves a lost-wakeup/double-enqueue race
    // between the reactor and worker. States: 0 NONE, 1 ARMED, 2 PARKED (reactor may enqueue), 3 FIRED (worker enqueues); exactly one side wins the ARMED transition.
    volatile int32_t park_state;
    // Set to 1 by the I/O reactor when a deadline-bearing fd watch timed out rather than became
    // ready; dragon_nb_recv_timeout clears it before each watch and reads it after resuming to tell timeout from data-ready.
    volatile int8_t io_timed_out;
    // CAS'd 0->1 by dragon_vthread_join so only the first joiner destroys the coroutine and
    // frees the handle (else a double-mco_destroy/double-free); a second join returns the cached result.
    volatile int8_t joined;
    // Dual-ownership refcount (init 2 at spawn): one for the running coro (dropped on MCO_DEAD,
    // gated by the done CAS), one for the Task handle (dropped by the join winner or dragon_vthread_detach). Last release frees the struct + coroutine stack.
    volatile int8_t refs;
    pthread_mutex_t join_lock;
    pthread_cond_t  join_cond;
    struct DragonVThread* next;
} DragonVThread;

struct DragonGenerator {
    DragonObjectHeader header;
    mco_coro*   coro;
    int64_t     yielded_value;
    // DragonValueTag of yielded_value; heap tags mean the slot OWNS a +1,
    // released on the next yield / at destroy (0 for non-heap yields).
    int64_t     yielded_tag;
    int8_t      state;
    // D030: codegen-allocated typed args struct + per-callsite decref fn. Buffer holds
    // (Generator*, user args...) and outlives the body; destroy calls the decref fn then frees args.
    void*       args;
    void      (*args_decref_fn)(void*);
    // Isolated exception context for the generator body (installed as __dragon_exc_vt during
    // resume): a generator yielding inside a try keeps its setjmp frames here so the caller's push/pop can't clobber them. Lazily allocated; only its exc fields are used.
    DragonVThread* exc_vt;
    // Set by the trampoline's setjmp barrier on an uncaught exception in the body.
    // dragon_generator_next reads it post-resume and re-raises (type/msg/obj in exc_vt) into the caller's frame, instead of a cross-stack longjmp that would skip the context restore.
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
    // D033: own (non-inherited) methods; inheritance resolved by walking `parent` in
    // dragon_class_find_method/dragon_dir. method_kinds: 0=instance, 1=static, 2=classmethod.
    const char** method_names;
    void**       method_fn_ptrs;
    uint8_t*     method_kinds;
    int64_t      num_methods;
    // D033 Phase 3: "bound thunks", codegen wrappers (user_args..., env: ptr) that unpack
    // `self` and call the method. Lets getattr() build a bound DragonClosure with no runtime trampoline; NULL when the class has no own methods.
    void**       method_bound_thunks;
};

// D027/D030: heap-allocated refcounted closure env; body is a per-lambda LLVM struct following
// header+gc_fn. sizeof=24 (codegen GEPs this offset; never add fields). gc_fn ops: DEALLOC decrefs captures, TRAVERSE visits cycle-closing captures (skips strings, which can't cycle), CLEAR decrefs+nulls slots (breaks cycles), MARK_SHARED marks every capture incl. strings; NULL only for scalar-only envs.
#define DRAGON_ENV_OP_DEALLOC  0
#define DRAGON_ENV_OP_TRAVERSE 1
#define DRAGON_ENV_OP_CLEAR    2
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

// Heap-boxed mutable binding for `nonlocal`: {16B header, 8B value, 4B kind, 4B holds_heap} = 32B.
// `value` is a raw int or ptr-as-i64; `kind` (DragonValueTag) drives the destructor, `holds_heap` gates RC. One cell per nonlocal var; owner + capturing closures share it via dragon_cell_get/set.
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

// GC class tables
typedef void (*dragon_class_dealloc_fn)(void*);
typedef void (*dragon_class_clear_fn)(void*);
typedef void (*dragon_gc_visit_fn)(void* child, void* arg);
typedef void (*dragon_class_traverse_fn)(void*, dragon_gc_visit_fn, void*);
// Per-class shared-mark fn: visits every heap-typed field (incl. strings), calling
// dragon_mark_shared/_str directly. Codegen-generated, registered via dragon_class_register_mark_shared; the worklist arg lets children queue for further BFS.
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

// Worklist push for the per-class mark-shared fn, called for container/class children so
// BFS continues from them. Idempotent: already-SHARED objects skip the push.
void dragon_mark_shared_worklist_push(void* worklist, void* obj);

// GC tracking state. Thread-safety: gc_tracked/_size/_cap are protected by gc_lock; gc_alloc_counter
// and gc_threshold use __atomic_* RELAXED; gc_collecting uses ACQUIRE/RELEASE (read by dragon_decref/_str from any thread).
extern pthread_mutex_t gc_lock;
extern void** gc_tracked;
extern int32_t gc_tracked_size;
extern int32_t gc_tracked_cap;
extern int32_t gc_alloc_counter;
extern int32_t gc_threshold;
extern int gc_collecting;

// Concurrency latch: 0 means a single OS thread touches the heap, so track/untrack/decref-to-zero/
// collect skip gc_lock entirely (lock-free fast path). Latched to 1 permanently by dragon_gc_go_concurrent() at the first second heap-mutating thread spawn. Accessed via __atomic_*.
extern int gc_concurrent;
// Set gc_concurrent=1 (idempotent); call before pthread_create of any heap-touching thread.
void dragon_gc_go_concurrent(void);

// Concurrent-mutation detector (best-effort, Go-style): structural mutators of a SHARED
// dict/list/set/deque set GC_FLAG_MUTATING on begin and abort loudly if a second mutator finds it already set, catching unlocked concurrent structural writes (which corrupt memory silently) instead. Gated on gc_concurrent + GC_FLAG_SHARED so single-threaded/unshared paths pay nothing. A raise between begin/end must end the guard BEFORE longjmp, since unwind skips the end call.
#define GC_FLAG_MUTATING 0x10

// Report the collision and abort. Defined in runtime_core.cpp. Never returns.
void dragon_fatal_concurrent_mutation(const char* kind);

// Arm the detector for a structural mutation of `h`; returns whether armed. Caller must pass
// that to dragon_shared_mut_end on every exit path except a longjmp-raise, which must end BEFORE raising.
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
// Typed exception instance paired with __dragon_exc_msg (see DragonVThread::exc_obj); NULL
// when the raise site didn't construct a user-class instance (built-in raises via dragon_raise_exc).
extern __thread void*       __dragon_exc_obj;
extern __thread DragonVThread* __current_vthread;
// Exception-storage override, normally NULL (state then lives in __current_vthread or the TLS
// globals). A running generator sets its own exc context here (swapped by dragon_generator_next around mco_resume) so its try/except frames stay isolated from the caller's, else overlapping push/pop could longjmp a wrong frame -> SIGSEGV. Kept separate from __current_vthread so scheduler/I/O code still sees the real green-thread identity there.
extern __thread DragonVThread* __dragon_exc_vt;

// Per-OS-thread unwind cleanup stack + saved depths (TLS fallback for non-vthread code;
// vthreads mirror this inside their struct). See DragonCleanupStack.
extern __thread DragonCleanupStack __dragon_cleanup;
extern __thread int32_t            __dragon_cleanup_saved[DRAGON_EXC_STACK_SIZE];
// Live exception-frame count on the current context; codegen's inline gate reads it to skip
// unwind-cleanup registration when no frame is live. Saved/restored per vthread by the scheduler.
extern __thread int                __dragon_active_frames;

// Scheduler-aware socket I/O (ADR 038 Phase 8, impl in runtime_concurrency.cpp). dragon_nb_connect
// is called from stdlib/socket.dr; the rest back the TLS BIO so handshake/read/write yield to the scheduler on EAGAIN instead of blocking the carrier thread.
extern "C" {
int64_t dragon_nb_connect(int64_t fd, void* addr, int64_t addrlen);
int  dragon_io_wait_readable(int fd);   // 0 when readable, -1 on poll error/HUP
int  dragon_io_wait_writable(int fd);   // 0 when writable, -1 on poll error/HUP
// Deadline-bounded readability wait (R1): 0 readable, 1 timeout, -1 error; timeout_ms<=0 means
// no deadline. Used by the TLS BIO so a stalled encrypted peer is dropped instead of pinning the green thread.
int  dragon_io_wait_readable_timeout(int fd, int64_t timeout_ms);
void dragon_set_nonblocking(int64_t fd);
// Binary (bytes) variants of the scheduler-aware recv/send, same yielding path as
// dragon_nb_recv/_send but DragonBytes-typed so wire protocols (Postgres/MySQL, D032) aren't forced through `str`.
DragonBytes* dragon_nb_recv_bytes(int64_t fd, int64_t max_len);
int64_t      dragon_nb_send_bytes(int64_t fd, DragonBytes* data);
}

// TLS atomic-context flag: set by dragon_decref_atomic (and the atomic str variant) during a
// recursive dealloc so child decrefs also route atomic (via dragon_decref_dispatch/_str_dispatch), since other threads may share those children. Nested calls save/restore rather than clear, so a non-atomic dealloc nested inside an atomic one still uses the atomic path.
extern __thread int __dragon_atomic_context;

static inline DragonString* dragon_string_from_data(const char* data) {
    return (DragonString*)((char*)data - offsetof(DragonString, data));
}

// Bounds of the executable image, used to range-gate the heap-string probe below. ELF uses
// linker symbols; Mach-O has no _end/__executable_start, so runtime_core.cpp walks LC_SEGMENT_64 load commands at startup. Until initialized, every pointer fails SAFE as "in image" (treated as literal, never probed backwards).
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

/// Heap-vs-literal check: literals have no DragonObjectHeader, heap DragonStrings have
/// type_tag==DRAGON_TAG_STR + GC_FLAG_HEAP_OBJ. Image-range check MUST precede the header probe: probing rodata bytes before a literal is unsound (a layout shift once faked a valid header and SEGV'd in test_ssl_roundtrip via a write into read-only memory).
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

// HashDoS defense: dict/set string hash is keyed with a per-process random secret (seeded
// from the OS CSPRNG at startup, dragon_hash_secret_init). An unkeyed hash (old FNV-1a) let an attacker precompute keys landing in one bucket, turning O(1) dict ops into O(n)/O(n^2) DoS; SipHash-1-3 (like CPython/Rust) keyed makes collisions unpredictable.
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

/// Keyed content hash of a Dragon string's bytes. Uses dragon_str_total_bytes so kind=4 strings
/// hash all UCS-4 bytes, not just up to the first internal NUL.
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

// Container-walk guard shared by the repr/json/print recursion (runtime_collections.cpp):
// detects cycles (self-referential containers SIGSEGV'd via unbounded recursion) and
// caps depth so pathological nesting cannot overflow the C stack.
#define DRAGON_WALK_MAX 256
extern thread_local const void* __dragon_walk_seen[DRAGON_WALK_MAX];
extern thread_local int __dragon_walk_depth;

enum { DRAGON_WALK_OK = 0, DRAGON_WALK_CYCLE = 1, DRAGON_WALK_TOO_DEEP = 2 };

static inline int dragon_walk_enter(const void* obj) {
    for (int i = 0; i < __dragon_walk_depth; i++)
        if (__dragon_walk_seen[i] == obj) return DRAGON_WALK_CYCLE;
    if (__dragon_walk_depth >= DRAGON_WALK_MAX) return DRAGON_WALK_TOO_DEEP;
    __dragon_walk_seen[__dragon_walk_depth++] = obj;
    return DRAGON_WALK_OK;
}

static inline void dragon_walk_exit(void) {
    if (__dragon_walk_depth > 0) __dragon_walk_depth--;
}

static inline void dragon_walk_reset(void) { __dragon_walk_depth = 0; }

// GC / refcounting
void dragon_incref(void* obj);
void dragon_decref(void* obj);
void dragon_incref_atomic(void* obj);
void dragon_decref_atomic(void* obj);
// Tag-gated closure RC (runtime_builtins.cpp); safe on a bare fn ptr (no header) and null. Used
// by dragon_{in,de}cref_tagged for TAG_CLOSURE elements so Callable containers RC closures without touching a headerless code pointer.
void dragon_incref_callable(void* p);
void dragon_decref_callable(void* p);
void dragon_make_immortal(void* obj);
int64_t dragon_is_immortal_obj(void* obj);
void dragon_gc_track(void* obj);
void dragon_gc_untrack(void* obj);
void dragon_gc_set_threshold(int64_t n);
int64_t dragon_gc_collect();
int64_t dragon_gc_tracked_count();

// Mark an object SHARED (atomic OR into gc_flags); idempotent, safe under concurrent callers,
// does not recurse into children. Used at the leaf of dragon_mark_shared_deep and inside RC-atomic entry points.
void dragon_mark_shared(void* obj);

// Mark `obj` and every transitively-reachable child SHARED, walking the per-type dragon_traverse
// table (SHARED bit doubles as "seen"). Called from the fire-site and from container write-barriers when a value is stored into a SHARED parent.
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

static inline void dragon_hex_encode(char* dst, const unsigned char* src, int64_t n) {
    static const char* hexd = "0123456789abcdef";
    for (int64_t i = 0; i < n; i++) {
        dst[i * 2] = hexd[src[i] >> 4];
        dst[i * 2 + 1] = hexd[src[i] & 0xF];
    }
}
// Snapshot a message for re-raising only if it is a mortal heap string (else
// return it unchanged - no dup, no leak). See definition in runtime_string.cpp.
const char* dragon_exc_msg_preserve(const char* s);

// Free this OS thread's TLS exception state (cleanup-stack arrays, owned exc
// message and instance). Call as a thread entry returns; the vthread twin is
// vthread_release.
void dragon_exc_thread_state_release(void);

// Free every owned entry above `target` on `cs` and rewind to it. Teardown of an
// abandoned context (generator / vthread) must drain before freeing the arrays,
// else the registered locals leak.
void dragon_cleanup_stack_drain(DragonCleanupStack* cs, int32_t target);

// Force-free a heap string at refcount 0 inside the cycle collector's clear_refs phase,
// bypassing dragon_decref_str's gc_collecting guard (else Phase 4/6 would leave it allocated). Safe since the string is owned exclusively by the unreachable container being torn down; still honors the immortal sentinel.
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

// D030 Phase 3: monomorphized list ops. Codegen calls these by element kind, each returning/
// accepting its native type (double, void*), no i64 funnel; polymorphic destroy still dispatches off elem_tag (shared layout prefix).

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

// Typed list[str] join (D017 Phase 4.B block-interp flatten, 4.C `| join`/`!{*expr}` spread).
// Direct ptr-array walk, no per-element box decode; sep may be NULL/"" for no separator.
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

// D030 Phase 3.E: typed dict ops for str-keyed monomorphic dicts, so the value crosses the
// codegen/runtime boundary at its native LLVM type instead of being i64-bashed.
double dragon_dict_get_str_f64(DragonDict* d, const char* key);
void   dragon_dict_set_str_f64(DragonDict* d, const char* key, double value);
void*  dragon_dict_get_str_ptr(DragonDict* d, const char* key, int64_t expected_tag);
void   dragon_dict_set_str_ptr(DragonDict* d, const char* key, void* value, int64_t tag);

// D030 Phase 3.G: typed dict ops for int-keyed monomorphic dicts. Reuses the DragonDict layout
// (i64 key stored verbatim in the `key` slot, hashed via SplitMix64 instead of FNV-1a); codegen picks this family for `dict[int, V]`. Setters handle overwrite RC like the str-keyed variants.
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

// Compression (gzip via libz, zstd via libzstd; runtime_compress.cpp). Bytes-in/bytes-out
// one-shot; callers stream by chunking at the Dragon layer (stdlib/gzip.dr, stdlib/zstandard.dr).
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
// Raise with a raw C string (literal/stack buffer/errno text): copies it into a fresh heap
// DragonString before the longjmp, so exc_msg never holds a raw C pointer (no OOB header probe, no dangling stack buffer). Every runtime-internal raise site must use this; dragon_raise_exc is for codegen str-typed messages only.
void dragon_raise_exc_cstr(int64_t type, const char* msg);
// Allocation-free MemoryError raise for OOM branches: _cstr dups its message through
// dragon_xmalloc, so raising OOM there would recurse to stack death. Every other raise still must use _cstr.
void dragon_raise_oom(void);
// Consume variants: take an owned +1 message (freshly dup'd / allocated)
// into the slot instead of dup'ing a borrow. See dragon_exc_msg_set.
void dragon_raise_exc_consume(int64_t type, const char* msg);
void dragon_raise_exc_obj(int64_t type, void* obj, const char* msg);
void dragon_raise_exc_obj_consume(int64_t type, void* obj, const char* msg);
// `except ... as e` binding: returns the in-flight message with its own +1
// (mortal heap only; literal/immortal no-op) - handler scope cleanup drops it.
const char* dragon_exc_bind_msg(void);
int64_t dragon_exc_matches(int64_t raised, int64_t caught);

// Checked allocation, fixing NULL-malloc derefs on OOM and NULL-write/leaks from overwriting a
// pointer before checking realloc. `_x*` raises MemoryError (longjmp; failed xrealloc leaves the old pointer intact) for user-reachable allocations; `_or_abort` aborts with a diagnostic for GC/exception code where raising would re-enter the failing subsystem. Hot path costs nothing (hinted unlikely branch).
static inline void* dragon_xmalloc(size_t n) {
    void* p = malloc(n);
    if (__builtin_expect(p == nullptr, 0))
        dragon_raise_oom();
    return p;
}
static inline void* dragon_xrealloc(void* old, size_t n) {
    void* p = realloc(old, n);
    if (__builtin_expect(p == nullptr, 0))
        dragon_raise_oom();
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

// Count-based variants: the count*elem_size multiply belongs here, not at the call site. A-B
// proven: `[1]*n` with n=2^61+2 wrapped dragon_list_repeat's guard and the byte multiply to a 16-byte malloc, then memcpy'd 2.3 quintillion elements (controlled heap overflow). `extra` covers NUL/header trailing bytes so `n+1` is checked too; __builtin_mul_overflow replaces six hand-rolled guards scattered across the runtime.
static inline bool dragon_alloc_bytes_try(int64_t count, size_t elem_size,
                                          size_t extra, size_t* out) {
    // elem_size == 0 would make any count "fit" and yield a 0-byte buffer that
    // callers then index. No current caller passes it; trap rather than trust.
    if (__builtin_expect(count < 0 || elem_size == 0, 0)) return false;
    size_t bytes;
    if (__builtin_expect(__builtin_mul_overflow((size_t)count, elem_size, &bytes), 0))
        return false;
    if (__builtin_expect(__builtin_add_overflow(bytes, extra, &bytes), 0))
        return false;
    *out = bytes;
    return true;
}

static inline size_t dragon_alloc_bytes_ex(int64_t count, size_t elem_size,
                                           size_t extra) {
    size_t bytes;
    if (__builtin_expect(!dragon_alloc_bytes_try(count, elem_size, extra, &bytes), 0))
        dragon_raise_exc_cstr(43, "MemoryError: allocation size overflow");
    return bytes;
}
static inline size_t dragon_alloc_bytes(int64_t count, size_t elem_size) {
    return dragon_alloc_bytes_ex(count, elem_size, 0);
}
static inline void* dragon_xmalloc_n(int64_t count, size_t elem_size) {
    return dragon_xmalloc(dragon_alloc_bytes_ex(count, elem_size, 0));
}
static inline void* dragon_xmalloc_ex(int64_t count, size_t elem_size, size_t extra) {
    return dragon_xmalloc(dragon_alloc_bytes_ex(count, elem_size, extra));
}
static inline void* dragon_xrealloc_n(void* old, int64_t count, size_t elem_size) {
    return dragon_xrealloc(old, dragon_alloc_bytes_ex(count, elem_size, 0));
}
// calloc had no helper despite 24 direct call sites; its product check returns NULL rather
// than truncating, but every site then wrote through that NULL. Routes the count through the same trap so overflow vs OOM report differently instead of both SIGSEGV-ing.
static inline void* dragon_xcalloc_n(int64_t count, size_t elem_size) {
    size_t bytes;
    if (__builtin_expect(!dragon_alloc_bytes_try(count, elem_size, 0, &bytes), 0))
        dragon_raise_exc_cstr(43, "MemoryError: allocation size overflow");
    void* p = calloc((size_t)count, elem_size);
    if (__builtin_expect(p == nullptr && bytes != 0, 0))
        dragon_raise_oom();
    return p;
}
static inline void* dragon_xcalloc(size_t n) { return dragon_xcalloc_n((int64_t)n, 1); }

// Non-raising siblings for container GROW paths: a longjmp between dragon_shared_mut_begin/end
// would strand GC_FLAG_MUTATING and false-trip dragon_fatal_concurrent_mutation later, so grow paths keep abort-on-failure and only gain the overflow trap. Use _or_abort when: inside an armed mut_begin window, inside the GC/exception machinery, or after freeing/half-swapping a buffer a live object still points at; use the raising form otherwise.
static inline size_t dragon_alloc_bytes_ex_or_abort(int64_t count, size_t elem_size,
                                                    size_t extra) {
    size_t bytes;
    if (__builtin_expect(!dragon_alloc_bytes_try(count, elem_size, extra, &bytes), 0)) {
        fprintf(stderr, "dragon: allocation size overflow\n");
        abort();
    }
    return bytes;
}
static inline size_t dragon_alloc_bytes_or_abort(int64_t count, size_t elem_size) {
    return dragon_alloc_bytes_ex_or_abort(count, elem_size, 0);
}
static inline void* dragon_xmalloc_n_or_abort(int64_t count, size_t elem_size) {
    return dragon_xmalloc_or_abort(dragon_alloc_bytes_ex_or_abort(count, elem_size, 0));
}
static inline void* dragon_xrealloc_n_or_abort(void* old, int64_t count,
                                               size_t elem_size) {
    return dragon_xrealloc_or_abort(old,
        dragon_alloc_bytes_ex_or_abort(count, elem_size, 0));
}
static inline void* dragon_xcalloc_n_or_abort(int64_t count, size_t elem_size) {
    size_t bytes = dragon_alloc_bytes_ex_or_abort(count, elem_size, 0);
    void* p = calloc((size_t)count, elem_size);
    if (__builtin_expect(p == nullptr && bytes != 0, 0)) {
        fprintf(stderr, "dragon: out of memory\n");
        abort();
    }
    return p;
}

static inline void* dragon_malloc_nullable(size_t n) { return malloc(n); }
static inline void* dragon_realloc_nullable(void* old, size_t n) {
    return realloc(old, n);
}
static inline void* dragon_calloc_nullable(size_t count, size_t elem_size) {
    return calloc(count, elem_size);
}

// Unwind cleanup stack (DragonCleanupStack). Codegen pushes at each owned-heap-local decl,
// updates on reassignment, resets-by-depth at normal scope exit; dragon_exc_cleanup_unwind runs at every longjmp arrival.
int32_t dragon_cleanup_push(int64_t val, int32_t kind, int32_t tag);
void    dragon_cleanup_update(int32_t slot, int64_t val, int32_t tag);
int32_t dragon_cleanup_depth(void);
void    dragon_cleanup_reset(int32_t depth);
void    dragon_exc_cleanup_unwind(void);

// D033: method reflection setters/lookup, called from codegen at module init time (after
// dragon_class_descriptor_create). dragon_class_find_method walks `parent` so each class only advertises its own methods.
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

// D033 Phase 2: dir() builtin. Walks the class MRO collecting field+method names into a sorted
// DragonListPtr[str]; `is_descriptor` selects between an instance ptr (look up class_id) and a descriptor ptr.
DragonListPtr* dragon_dir(int64_t instance_or_desc, int64_t is_descriptor);

// String allocation helpers (used by string and other TUs)
const char* dragon_string_alloc(const char* src, int64_t len);
const char* dragon_str_intern(const char* utf8_bytes, int64_t byte_len);
DragonString* dragon_string_alloc_raw(int64_t len);
/// UTF-8 encode helper: kind=1/literals return NULL (caller uses `s` directly); kind=4 returns
/// a freshly malloc'd buffer the caller must free. `*out_byte_len` is set either way.
char* dragon_str_to_utf8_alloc(const char* s, int64_t* out_byte_len);

// Print helpers (used in collections printing)
void dragon_print_tagged(int64_t value, int64_t tag);

// String operations (used by builtins, collections)
const char* dragon_int_to_str(int64_t value);
const char* dragon_float_to_str(double value);
/// Python-repr float formatter shared by scalar print, str(), f-strings, and container repr
/// (single source of truth). Writes into `buf` (>=32 bytes), returns length.
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

// Route a child decref through the atomic variant inside an atomic-context dealloc, else the
// normal variant. Per-type destroy functions invoked from dragon_dealloc must go through these.
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

#pragma GCC poison malloc realloc calloc

#endif // DRAGON_RUNTIME_INTERNAL_H
