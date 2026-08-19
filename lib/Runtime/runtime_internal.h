#ifndef DRAGON_RUNTIME_INTERNAL_H
#define DRAGON_RUNTIME_INTERNAL_H

#include <cmath>
#include <csetjmp>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <stdlib.h>
#include <cstring>
#ifdef _WIN32
  #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
  #endif
  #include <winsock2.h>
  #include <ws2tcpip.h>
  #include <windows.h>
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
#include <pthread.h>
#include <unistd.h>

#ifndef MCO_USE_VMEM_ALLOCATOR
#define MCO_USE_VMEM_ALLOCATOR
#endif
#include "minicoro.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int64_t  refcount;
    uint8_t  type_tag;
    uint8_t  gc_flags;
    uint16_t class_id;
    int32_t  gc_track_idx;
} DragonObjectHeader;

#define GC_FLAG_TRACKED   0x01
#define GC_FLAG_REACHABLE 0x02
#define GC_FLAG_SHARED    0x04
#define GC_FLAG_IN_TO_FREE 0x08
#define GC_FLAG_HEAP_OBJ  0x80

#define DRAGON_IMMORTAL_REFCOUNT ((int64_t)0x4000000000000000LL)

static inline bool dragon_is_immortal(void* obj) {
    return obj && ((DragonObjectHeader*)obj)->refcount >= DRAGON_IMMORTAL_REFCOUNT;
}

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
    DRAGON_TAG_CELL      = 13,
    DRAGON_TAG_LIST_BOX  = 14,
};

static inline void dragon_obj_init(DragonObjectHeader* h, uint8_t tag) {
    h->refcount = 1;
    h->type_tag = tag;
    h->gc_flags = GC_FLAG_HEAP_OBJ;
    h->class_id = 0;
    h->gc_track_idx = -1;
}

#include "dragon/ValueTags.h"
static_assert(TAG_CALLABLE == (int8_t)DRAGON_TAG_CLOSURE,
              "value-tag slot 10 must stay aligned with the closure object tag");

static inline bool dragon_value_tag_is_traceable(int8_t tag) {
    return tag == TAG_LIST || tag == TAG_DICT || tag == TAG_BYTES ||
           tag == (int8_t)DRAGON_TAG_CLOSURE;
}

#define DRAGON_EXC_STACK_SIZE 32
#define DRAGON_EXC_MAX_USER 256

typedef struct DragonCleanupStack {
    int64_t* vals;
    int32_t* kinds;
    int32_t* tags;
    int32_t  sp;
    int32_t  cap;
} DragonCleanupStack;

enum DragonCleanupKind {
    DCLEAN_STR      = 1,
    DCLEAN_CALLABLE = 2,
    DCLEAN_OBJ      = 3,
    DCLEAN_UNION    = 4,
    DCLEAN_DEFER_CALL = 5,
    DCLEAN_FREE = 6,
};

enum VThreadYieldReason {
    YIELD_COOP  = 0,
    YIELD_IO    = 1,
    YIELD_SLEEP = 2
};

typedef struct {
    DragonObjectHeader header;
    int64_t len;
    uint8_t kind;
    uint8_t _pad[3];
    int32_t cap;
    char    data[];
} DragonString;

static inline int64_t dragon_str_byte_len(const DragonString* s) {
    return s->len * (int64_t)s->kind;
}

static inline int32_t dragon_cap_clamp(int64_t bytes) {
    return bytes > 0x7fffffff ? (int32_t)0x7fffffff : (int32_t)bytes;
}

struct DragonList {
    DragonObjectHeader header;
    void*    data;
    int64_t  size;
    int64_t  capacity;
    uint8_t  elem_tag;
    uint8_t  elem_size;
};

struct DragonBox {
    int64_t tag;
    int64_t payload;
};

struct DragonListF64 {
    DragonObjectHeader header;
    double*  data;
    int64_t  size;
    int64_t  capacity;
    uint8_t  elem_tag;
    uint8_t  elem_size;
};

struct DragonListPtr {
    DragonObjectHeader header;
    void**   data;
    int64_t  size;
    int64_t  capacity;
    uint8_t  elem_tag;
    uint8_t  elem_size;
};

struct DragonListBoxElem {
    int64_t tag;
    int64_t payload;
};

struct DragonListBox {
    DragonObjectHeader header;
    DragonListBoxElem* data;
    int64_t size;
    int64_t capacity;
};

static inline uint8_t dragon_list_size_for_tag(uint8_t tag) {
    return (tag == TAG_BOOL) ? 1 : 8;
}

static inline int64_t dragon_list_load(const struct DragonList* l, int64_t i) {
    if (l->elem_size == 1) return (int64_t)((const uint8_t*)l->data)[i];
    return ((const int64_t*)l->data)[i];
}

static inline void dragon_list_store(struct DragonList* l, int64_t i, int64_t v) {
    if (l->elem_size == 1) ((uint8_t*)l->data)[i] = (uint8_t)(v & 0xFF);
    else ((int64_t*)l->data)[i] = v;
}

struct DictEntry {
    uint64_t hash;
    const char* key;
    int64_t value;
    int8_t tag;
    int8_t dead;
};

struct DragonDict {
    DragonObjectHeader header;
    DictEntry* entries;
    int64_t* indices;
    int64_t size;
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
    void*       exc_obj;
    DragonCleanupStack cleanup;
    int32_t     cleanup_saved[DRAGON_EXC_STACK_SIZE];
    int         active_frames;
    mco_coro*   coro;
    int64_t     result;
    int64_t     result_tag;
    volatile int8_t result_claimed;
    volatile int8_t done;
    volatile int8_t yield_reason;
    volatile int32_t park_state;
    volatile int8_t io_timed_out;
    // CAS'd 0->1 by dragon_vthread_join so only the first joiner destroys the coroutine and
    // frees the handle (else a double-mco_destroy/double-free); a second join returns the cached result.
    volatile int8_t joined;
    volatile int8_t refs;
    pthread_mutex_t join_lock;
    pthread_cond_t  join_cond;
    struct DragonVThread* next;
} DragonVThread;

struct DragonGenerator {
    DragonObjectHeader header;
    mco_coro*   coro;
    int64_t     yielded_value;
    int64_t     yielded_tag;
    int8_t      state;
    void*       args;
    void      (*args_decref_fn)(void*);
    DragonVThread* exc_vt;
    int8_t      pending_exc;
};

struct DragonClassDescriptor {
    DragonObjectHeader header;
    int64_t    class_id;
    const char* name;
    const char* doc;
    int64_t    parent;
    int64_t    constructor;
    int64_t*   ancestor_ids;
    int64_t    num_ancestors;
    const char** field_names;
    int64_t*     field_offsets;
    int64_t*     field_widths;
    int64_t      num_fields;
    const char** method_names;
    void**       method_fn_ptrs;
    uint8_t*     method_kinds;
    int64_t      num_methods;
    void**       method_bound_thunks;
};

#define DRAGON_ENV_OP_DEALLOC  0
#define DRAGON_ENV_OP_TRAVERSE 1
#define DRAGON_ENV_OP_CLEAR    2
#define DRAGON_ENV_OP_MARK_SHARED 3
struct DragonEnv {
    DragonObjectHeader header;
    void (*gc_fn)(void* env, int32_t op, void (*visit)(void*, void*), void* arg);
};

struct DragonClosure {
    DragonObjectHeader header;
    void*   fn_ptr;
    DragonEnv* env;
};

struct DragonCell {
    DragonObjectHeader header;
    int64_t value;
    int32_t kind;
    int32_t holds_heap;
};

struct DragonDeque {
    DragonObjectHeader header;
    int64_t* data;
    int64_t  capacity;
    int64_t  head;
    int64_t  size;
    int64_t  maxlen;
    uint8_t  elem_tag;
};

typedef void (*dragon_class_dealloc_fn)(void*);
typedef void (*dragon_class_clear_fn)(void*);
typedef void (*dragon_gc_visit_fn)(void* child, void* arg);
typedef void (*dragon_class_traverse_fn)(void*, dragon_gc_visit_fn, void*);
typedef void (*dragon_class_mark_shared_fn)(void* self, void* worklist);

#define DRAGON_MAX_CLASS_IDS 4096

extern dragon_class_dealloc_fn __class_dealloc_table[DRAGON_MAX_CLASS_IDS];
extern dragon_class_clear_fn __class_clear_table[DRAGON_MAX_CLASS_IDS];
extern dragon_class_traverse_fn __class_traverse_table[DRAGON_MAX_CLASS_IDS];
extern dragon_class_mark_shared_fn __class_mark_shared_table[DRAGON_MAX_CLASS_IDS];
extern int __next_class_id;

int64_t dragon_class_register_mark_shared(int64_t class_id, void* fn);

void dragon_mark_shared_worklist_push(void* worklist, void* obj);

extern pthread_mutex_t gc_lock;
extern void** gc_tracked;
extern int32_t gc_tracked_size;
extern int32_t gc_tracked_cap;
extern int32_t gc_alloc_counter;
extern int32_t gc_threshold;
extern int gc_collecting;

extern int gc_concurrent;
void dragon_gc_go_concurrent(void);

#define GC_FLAG_MUTATING 0x10

void dragon_fatal_concurrent_mutation(const char* kind);

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

extern __thread jmp_buf     __dragon_exc_stack[DRAGON_EXC_STACK_SIZE];
extern __thread int         __dragon_exc_sp;
extern __thread int         __dragon_exc_type;
extern __thread const char* __dragon_exc_msg;
extern __thread void*       __dragon_exc_obj;
extern __thread DragonVThread* __current_vthread;
extern __thread DragonVThread* __dragon_exc_vt;

extern __thread DragonCleanupStack __dragon_cleanup;
extern __thread int32_t            __dragon_cleanup_saved[DRAGON_EXC_STACK_SIZE];
extern __thread int                __dragon_active_frames;

extern "C" {
int64_t dragon_nb_connect(int64_t fd, void* addr, int64_t addrlen);
int  dragon_io_wait_readable(int fd);
int  dragon_io_wait_writable(int fd);
int  dragon_io_wait_readable_timeout(int fd, int64_t timeout_ms);
void dragon_set_nonblocking(int64_t fd);
DragonBytes* dragon_nb_recv_bytes(int64_t fd, int64_t max_len);
int64_t      dragon_nb_send_bytes(int64_t fd, DragonBytes* data);
}

extern __thread int __dragon_atomic_context;

static inline DragonString* dragon_string_from_data(const char* data) {
    return (DragonString*)((char*)data - offsetof(DragonString, data));
}

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

static inline int dragon_str_is_heap(const char* s) {
    if (!s) return 0;
    if (s >= DRAGON_IMAGE_LO && s < DRAGON_IMAGE_HI) return 0;
    DragonString* ds = dragon_string_from_data(s);
    return ds->header.type_tag == DRAGON_TAG_STR &&
           (ds->header.gc_flags & GC_FLAG_HEAP_OBJ) ? 1 : 0;
}

static inline int64_t dragon_str_total_bytes(const char* s) {
    if (!s) return 0;
    if (dragon_str_is_heap(s)) {
        DragonString* ds = dragon_string_from_data(s);
        return ds->len * (int64_t)ds->kind;
    }
    return (int64_t)strlen(s);
}

extern uint64_t __dragon_hash_k0;
extern uint64_t __dragon_hash_k1;
void dragon_hash_secret_init(void);

static inline uint64_t dragon_hash_read_le64(const unsigned char* p) {
    return (uint64_t)p[0]        | ((uint64_t)p[1] << 8)  |
           ((uint64_t)p[2] << 16) | ((uint64_t)p[3] << 24) |
           ((uint64_t)p[4] << 32) | ((uint64_t)p[5] << 40) |
           ((uint64_t)p[6] << 48) | ((uint64_t)p[7] << 56);
}

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
        DRAGON_SIPROUND;
        v0 ^= m;
    }
    switch (inlen & 7) {
        case 7: b |= (uint64_t)data[6] << 48;
        case 6: b |= (uint64_t)data[5] << 40;
        case 5: b |= (uint64_t)data[4] << 32;
        case 4: b |= (uint64_t)data[3] << 24;
        case 3: b |= (uint64_t)data[2] << 16;
        case 2: b |= (uint64_t)data[1] << 8;
        case 1: b |= (uint64_t)data[0];
        case 0: break;
    }
    v3 ^= b;
    DRAGON_SIPROUND;
    v0 ^= b;
    v2 ^= 0xff;
    DRAGON_SIPROUND; DRAGON_SIPROUND; DRAGON_SIPROUND;
#undef DRAGON_SIPROUND
    return v0 ^ v1 ^ v2 ^ v3;
}

static inline uint64_t dragon_str_content_hash(const char* s) {
    int64_t n = dragon_str_total_bytes(s);
    uint64_t h = dragon_siphash13(s, (size_t)n, __dragon_hash_k0, __dragon_hash_k1);
    return h | 1;
}

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

void dragon_incref(void* obj);
void dragon_decref(void* obj);
void dragon_incref_atomic(void* obj);
void dragon_decref_atomic(void* obj);
void dragon_incref_callable(void* p);
void dragon_decref_callable(void* p);
void dragon_make_immortal(void* obj);
int64_t dragon_is_immortal_obj(void* obj);
void dragon_gc_track(void* obj);
void dragon_gc_untrack(void* obj);
void dragon_gc_set_threshold(int64_t n);
int64_t dragon_gc_collect();
int64_t dragon_gc_tracked_count();

void dragon_mark_shared(void* obj);

void dragon_mark_shared_deep(void* obj);

void dragon_mark_shared_str(const char* s);

void dragon_incref_str(const char* s);
void dragon_decref_str(const char* s);
const char* dragon_str_retain(const char* s);
void dragon_incref_str_atomic(const char* s);
void dragon_decref_str_atomic(const char* s);
const char* dragon_string_dup(const char* s);
const char* dragon_string_dup_cstr(const char* s);

static inline void dragon_hex_encode(char* dst, const unsigned char* src, int64_t n) {
    static const char* hexd = "0123456789abcdef";
    for (int64_t i = 0; i < n; i++) {
        dst[i * 2] = hexd[src[i] >> 4];
        dst[i * 2 + 1] = hexd[src[i] & 0xF];
    }
}
const char* dragon_exc_msg_preserve(const char* s);

void dragon_exc_thread_state_release(void);

void dragon_cleanup_stack_drain(DragonCleanupStack* cs, int32_t target);

void dragon_str_force_free_if_zero(const char* s);

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
const char* dragon_list_to_str(DragonList* list);
const char* dragon_bytes_to_str(DragonBytes* b);
const char* dragon_dict_to_str(DragonDict* d);
const char* dragon_dict_int_to_str(DragonDict* d);
const char* dragon_set_to_str(DragonSet* s);
const char* dragon_tuple_to_str(DragonTuple* t);
void dragon_list_sort(DragonList* list);
void dragon_list_sort_ex(DragonList* list, int64_t reverse);
void dragon_list_reverse(DragonList* list);
DragonList* dragon_list_copy(DragonList* list);
DragonList* dragon_list_deep_copy(DragonList* list);
DragonDict* dragon_dict_deep_copy(DragonDict* d);
int64_t dragon_deep_copy_tagged(int64_t val, int64_t tag);
void* dragon_obj_retain(void* p);
DragonList* dragon_list_repeat(DragonList* src, int64_t count);
DragonList* dragon_list_concat(DragonList* a, DragonList* b);

DragonListF64* dragon_list_new_f64(int64_t capacity);
double         dragon_list_get_f64(DragonListF64* list, int64_t index);
void           dragon_list_set_f64(DragonListF64* list, int64_t index, double value);
void           dragon_list_append_f64(DragonListF64* list, double value);

DragonListPtr* dragon_list_new_ptr(int64_t capacity, int64_t elem_tag);
void*          dragon_list_get_ptr(DragonListPtr* list, int64_t index);
void           dragon_list_set_ptr(DragonListPtr* list, int64_t index, void* value);
void           dragon_list_append_ptr(DragonListPtr* list, void* value);

DragonListBox* dragon_list_box_new(int64_t capacity);
void           dragon_list_box_append(DragonListBox* list, int64_t tag, int64_t payload);
void           dragon_list_box_delitem(DragonListBox* list, int64_t index);

const char*    dragon_str_join_ptr(const char* sep, DragonListPtr* list);

DragonDict* dragon_dict_new(int64_t cap);
void dragon_dict_set(DragonDict* d, const char* key, int64_t value);
void dragon_dict_set_tagged(DragonDict* d, const char* key, int64_t value, int64_t tag);
int64_t dragon_dict_get(DragonDict* d, const char* key);
int64_t dragon_dict_get_tag(DragonDict* d, const char* key);
int64_t dragon_dict_get_checked(DragonDict* d, const char* key, int64_t expected_tag);
int64_t dragon_dict_len(DragonDict* d);
int64_t dragon_dict_has_key(DragonDict* d, const char* key);
void dragon_dict_reject_unknown_keys(DragonDict* d, const char** allowed,
                                     int64_t n, const char* func_name);
int64_t dragon_dict_get_default(DragonDict* d, const char* key, int64_t def);
void   dragon_dict_del(DragonDict* d, const char* key);
DragonList* dragon_dict_keys(DragonDict* d);

double dragon_dict_get_str_f64(DragonDict* d, const char* key);
void   dragon_dict_set_str_f64(DragonDict* d, const char* key, double value);
void*  dragon_dict_get_str_ptr(DragonDict* d, const char* key, int64_t expected_tag);
void   dragon_dict_set_str_ptr(DragonDict* d, const char* key, void* value, int64_t tag);

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

DragonBytes* dragon_zlib_compress(DragonBytes* src, int64_t level);
DragonBytes* dragon_zlib_decompress(DragonBytes* src);
DragonBytes* dragon_zstd_compress(DragonBytes* src, int64_t level);
DragonBytes* dragon_zstd_decompress(DragonBytes* src);

DragonBytes* dragon_read_file_bytes(const char* path);
int64_t dragon_write_file_bytes(const char* path, DragonBytes* data);

void dragon_list_destroy(DragonList* l);
void dragon_list_box_destroy(DragonListBox* l);
void dragon_dict_destroy(DragonDict* d);
void dragon_tuple_destroy(DragonTuple* t);
void dragon_set_destroy(DragonSet* s);
void dragon_bytes_destroy(DragonBytes* b);
void dragon_deque_destroy(DragonDeque* d);
void dragon_generator_destroy(void* gen_ptr);
void dragon_generator_abandon(void* gen_ptr);

void dragon_raise_exc(int64_t type, const char* msg);
void dragon_raise_exc_cstr(int64_t type, const char* msg);
void dragon_raise_oom(void);
void dragon_raise_exc_consume(int64_t type, const char* msg);
void dragon_raise_exc_obj(int64_t type, void* obj, const char* msg);
void dragon_raise_exc_obj_consume(int64_t type, void* obj, const char* msg);
const char* dragon_exc_bind_msg(void);
int64_t dragon_exc_matches(int64_t raised, int64_t caught);

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

static inline bool dragon_alloc_bytes_try(int64_t count, size_t elem_size,
                                          size_t extra, size_t* out) {
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

int32_t dragon_cleanup_push(int64_t val, int32_t kind, int32_t tag);
void    dragon_cleanup_update(int32_t slot, int64_t val, int32_t tag);
int32_t dragon_cleanup_depth(void);
void    dragon_cleanup_reset(int32_t depth);
void    dragon_exc_cleanup_unwind(void);

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

DragonListPtr* dragon_dir(int64_t instance_or_desc, int64_t is_descriptor);

const char* dragon_string_alloc(const char* src, int64_t len);
const char* dragon_str_intern(const char* utf8_bytes, int64_t byte_len);
DragonString* dragon_string_alloc_raw(int64_t len);
char* dragon_str_to_utf8_alloc(const char* s, int64_t* out_byte_len);

void dragon_print_tagged(int64_t value, int64_t tag);

const char* dragon_int_to_str(int64_t value);
const char* dragon_float_to_str(double value);
int dragon_format_double_into(double value, char* buf, size_t bufsz);
void dragon_slice_indices(int64_t len, int64_t* start, int64_t* stop, int64_t step);
void dragon_print_str(const char* s);

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

void dragon_print_list_int(DragonList* list);
void dragon_print_list_str(DragonList* list);
void dragon_print_list_float(DragonList* list);
void dragon_print_list_bool(DragonList* list);
DragonList* dragon_list_slice(DragonList* l, int64_t start, int64_t stop, int64_t step);

static inline void dragon_incref_tagged(int64_t val, uint8_t tag) {
    if (!val) return;
    if (tag == TAG_STR)
        dragon_incref_str((const char*)(uintptr_t)val);
    else if (tag == TAG_LIST || tag == TAG_DICT || tag == TAG_BYTES)
        dragon_incref((void*)(uintptr_t)val);
    else if (tag == DRAGON_TAG_CLOSURE)
        dragon_incref_callable((void*)(uintptr_t)val);
}

static inline void dragon_decref_tagged(int64_t val, uint8_t tag) {
    if (!val) return;
    if (tag == TAG_STR)
        dragon_decref_str((const char*)(uintptr_t)val);
    else if (tag == TAG_LIST || tag == TAG_DICT || tag == TAG_BYTES)
        dragon_decref((void*)(uintptr_t)val);
    else if (tag == DRAGON_TAG_CLOSURE)
        dragon_decref_callable((void*)(uintptr_t)val);
}

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

#endif
