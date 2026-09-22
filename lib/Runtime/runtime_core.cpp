#include "runtime_internal.h"
#include <errno.h>
#include <time.h>

thread_local const void* __dragon_walk_seen[DRAGON_WALK_MAX];
thread_local int __dragon_walk_depth = 0;

#ifdef __APPLE__
// Mach-O has no _end/__executable_start; computes the main image's span once at load time for
// the dragon_str_is_heap range gate. Defaults fail SAFE (uninitialized pointer reads as "in image"), preventing the A/B-proven rodata-write SEGV this gate exists to guard against.
#include <mach-o/dyld.h>
#include <mach-o/loader.h>
#include <string.h>

const char* __dragon_image_lo = (const char*)0;
const char* __dragon_image_hi = (const char*)UINTPTR_MAX;
__attribute__((constructor))
static void dragon_image_bounds_init(void) {
    const struct mach_header_64* mh =
        (const struct mach_header_64*)_dyld_get_image_header(0);
    intptr_t slide = _dyld_get_image_vmaddr_slide(0);
    uintptr_t lo = UINTPTR_MAX, hi = 0;
    const struct load_command* lc = (const struct load_command*)(mh + 1);
    for (uint32_t i = 0; i < mh->ncmds; i++) {
        if (lc->cmd == LC_SEGMENT_64) {
            const struct segment_command_64* seg =
                (const struct segment_command_64*)lc;
            if (strcmp(seg->segname, "__PAGEZERO") != 0) {
                uintptr_t s = (uintptr_t)seg->vmaddr + (uintptr_t)slide;
                uintptr_t e = s + (uintptr_t)seg->vmsize;
                if (s < lo) lo = s;
                if (e > hi) hi = e;
            }
        }
        lc = (const struct load_command*)((const char*)lc + lc->cmdsize);
    }
    if (lo < hi) {
        __dragon_image_lo = (const char*)lo;
        __dragon_image_hi = (const char*)hi;
    }
}
#elif defined(_WIN32)
// PE has no _end/__executable_start either. __ImageBase is the linker-provided
// base of the module the runtime is linked into; SizeOfImage in that module's
// PE headers gives the span. Same fail-SAFE defaults as the Mach-O path above:
// an uninitialized read reports "inside the image", which keeps
// dragon_str_is_heap conservative rather than letting it write to rodata.
extern "C" IMAGE_DOS_HEADER __ImageBase;

const char* __dragon_image_lo = (const char*)0;
const char* __dragon_image_hi = (const char*)UINTPTR_MAX;
__attribute__((constructor))
static void dragon_image_bounds_init(void) {
    const char* base = (const char*)&__ImageBase;
    if (__ImageBase.e_magic != IMAGE_DOS_SIGNATURE) return;
    const IMAGE_NT_HEADERS* nt =
        (const IMAGE_NT_HEADERS*)(base + __ImageBase.e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return;
    const uintptr_t span = (uintptr_t)nt->OptionalHeader.SizeOfImage;
    if (span == 0) return;
    __dragon_image_lo = base;
    __dragon_image_hi = base + span;
}
#endif

extern "C" {

dragon_class_dealloc_fn __class_dealloc_table[DRAGON_MAX_CLASS_IDS];
dragon_class_clear_fn __class_clear_table[DRAGON_MAX_CLASS_IDS];
dragon_class_traverse_fn __class_traverse_table[DRAGON_MAX_CLASS_IDS];
dragon_class_mark_shared_fn __class_mark_shared_table[DRAGON_MAX_CLASS_IDS];
int __next_class_id = 1;

pthread_mutex_t gc_lock = PTHREAD_MUTEX_INITIALIZER;
void** gc_tracked = nullptr;
int32_t gc_tracked_size = 0;
int32_t gc_tracked_cap = 0;
int32_t gc_alloc_counter = 0;
static const int32_t GC_BASE_THRESHOLD = 700;
int32_t gc_threshold = GC_BASE_THRESHOLD;
int gc_collecting = 0;

int gc_concurrent = 0;

void dragon_gc_go_concurrent(void) {
    __atomic_store_n(&gc_concurrent, 1, __ATOMIC_RELEASE);
}

int gc_stop_requested = 0;
__thread DragonMutator* __dragon_mutator = nullptr;

static pthread_mutex_t gc_stop_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  gc_stop_cond = PTHREAD_COND_INITIALIZER;
static pthread_cond_t  gc_safe_cond = PTHREAD_COND_INITIALIZER;
static DragonMutator*  gc_mutator_list = nullptr;

static int64_t gc_stat_collections = 0;
static int64_t gc_stat_deferred = 0;
static int64_t gc_stat_max_pause_ns = 0;
static int64_t gc_stat_last_pause_ns = 0;

static const int64_t GC_STOP_WAIT_NS = 20000000;

void dragon_gc_mutator_register(void) {
    if (__dragon_mutator) return;
    DragonMutator* m = (DragonMutator*)dragon_xcalloc_n(1, sizeof(DragonMutator));
    m->state = DRAGON_MUTATOR_RUNNING;
    pthread_mutex_lock(&gc_stop_lock);
    while (gc_stop_requested) pthread_cond_wait(&gc_stop_cond, &gc_stop_lock);
    m->next = gc_mutator_list;
    gc_mutator_list = m;
    pthread_mutex_unlock(&gc_stop_lock);
    __dragon_mutator = m;
}

void dragon_gc_mutator_register_foreign(void) {
    if (__dragon_mutator) return;
    dragon_gc_go_concurrent();
    DragonMutator* m = (DragonMutator*)dragon_xcalloc_n(1, sizeof(DragonMutator));
    m->state = DRAGON_MUTATOR_SAFE;
    m->safe_depth = 1;
    pthread_mutex_lock(&gc_stop_lock);
    m->next = gc_mutator_list;
    gc_mutator_list = m;
    pthread_mutex_unlock(&gc_stop_lock);
    __dragon_mutator = m;
}

void dragon_gc_mutator_unregister(void) {
    DragonMutator* m = __dragon_mutator;
    if (!m) return;
    __dragon_mutator = nullptr;
    pthread_mutex_lock(&gc_stop_lock);
    DragonMutator** link = &gc_mutator_list;
    while (*link && *link != m) link = &(*link)->next;
    if (*link) *link = m->next;
    pthread_cond_signal(&gc_safe_cond);
    pthread_mutex_unlock(&gc_stop_lock);
    free(m);
}

__attribute__((constructor))
static void dragon_gc_register_main_thread(void) {
#if !defined(__APPLE__) && !defined(_WIN32)
    pthread_condattr_t attr;
    pthread_condattr_init(&attr);
    pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
    pthread_cond_init(&gc_safe_cond, &attr);
    pthread_condattr_destroy(&attr);
#endif
    dragon_gc_mutator_register();
}

void dragon_fatal_unregistered_mutation(const char* where) {
    fprintf(stderr,
        "DRAGON FATAL: unregistered thread mutated the object graph (%s)\n"
        "\n"
        "A thread the runtime never created ran Dragon code without going\n"
        "through dragon_safe_region_suspend, so the collector cannot stop it.\n"
        "Wrap every C-to-Dragon callback with the suspend/resume pair.\n",
        where);
    fflush(stderr);
    abort();
}

void dragon_fatal_mutation_in_safe_region(const char* where) {
    fprintf(stderr,
        "DRAGON FATAL: container mutation inside a safe region (%s)\n"
        "\n"
        "This carrier is counted stopped by the collector and mutated the\n"
        "object graph anyway. A SAFE region may only wrap foreign code or a\n"
        "blocking call that touches no Dragon object.\n",
        where);
    fflush(stderr);
    abort();
}

void dragon_fatal_safe_region_yield(const char* where) {
    fprintf(stderr,
        "DRAGON FATAL: safe region spans a yield (%s)\n"
        "\n"
        "A carrier resumed Dragon code while the collector counted it as\n"
        "stopped. A runtime safe region must never span an mco_yield: the\n"
        "depth lives on the carrier, the green thread migrates.\n",
        where);
    fflush(stderr);
    abort();
}

void dragon_gc_safepoint(void) {
    DragonMutator* m = __dragon_mutator;
    if (!m || m->is_collector || m->safe_depth) return;
    pthread_mutex_lock(&gc_stop_lock);
    if (gc_stop_requested) {
        __atomic_store_n(&m->state, DRAGON_MUTATOR_HELD, __ATOMIC_RELEASE);
        pthread_cond_signal(&gc_safe_cond);
        while (__atomic_load_n(&m->state, __ATOMIC_ACQUIRE) == DRAGON_MUTATOR_HELD)
            pthread_cond_wait(&gc_stop_cond, &gc_stop_lock);
        __atomic_store_n(&m->state, DRAGON_MUTATOR_RUNNING, __ATOMIC_RELEASE);
    }
    pthread_mutex_unlock(&gc_stop_lock);
}

void dragon_gc_safe_begin_slow(DragonMutator* m) {
    (void)m;
    pthread_mutex_lock(&gc_stop_lock);
    pthread_cond_signal(&gc_safe_cond);
    pthread_mutex_unlock(&gc_stop_lock);
}

void dragon_gc_safe_end_slow(DragonMutator* m) {
    pthread_mutex_lock(&gc_stop_lock);
    for (;;) {
        int32_t st = __atomic_load_n(&m->state, __ATOMIC_ACQUIRE);
        if (st == DRAGON_MUTATOR_HELD) {
            pthread_cond_wait(&gc_stop_cond, &gc_stop_lock);
            continue;
        }
        if (!gc_stop_requested || m->is_collector) break;
        __atomic_store_n(&m->state, DRAGON_MUTATOR_HELD, __ATOMIC_RELEASE);
        pthread_cond_signal(&gc_safe_cond);
    }
    __atomic_store_n(&m->state, DRAGON_MUTATOR_RUNNING, __ATOMIC_RELEASE);
    pthread_mutex_unlock(&gc_stop_lock);
}

static int gc_claim_stopped(DragonMutator* self) {
    int all_stopped = 1;
    for (DragonMutator* m = gc_mutator_list; m; m = m->next) {
        if (m == self) continue;
        int32_t st = __atomic_load_n(&m->state, __ATOMIC_ACQUIRE);
        if (st == DRAGON_MUTATOR_HELD) continue;
        if (st == DRAGON_MUTATOR_SAFE &&
            __atomic_compare_exchange_n(&m->state, &st, DRAGON_MUTATOR_HELD, false,
                                        __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
            continue;
        all_stopped = 0;
    }
    return all_stopped;
}

static void gc_release_world(DragonMutator* self) {
    __atomic_store_n(&gc_stop_requested, 0, __ATOMIC_SEQ_CST);
    self->is_collector = 0;
    for (DragonMutator* m = gc_mutator_list; m; m = m->next) {
        if (__atomic_load_n(&m->state, __ATOMIC_ACQUIRE) == DRAGON_MUTATOR_HELD)
            __atomic_store_n(&m->state, DRAGON_MUTATOR_SAFE, __ATOMIC_RELEASE);
    }
    pthread_cond_broadcast(&gc_stop_cond);
}

static void gc_retune_threshold(int64_t collected) {
    int32_t live = gc_tracked_size;
    int64_t next = (collected == 0) ? (int64_t)gc_threshold * 2 : GC_BASE_THRESHOLD;
    int64_t cap = (int64_t)GC_BASE_THRESHOLD + (int64_t)live * 8;
    if (next > cap) next = cap;
    if (next < GC_BASE_THRESHOLD) next = GC_BASE_THRESHOLD;
    if (next > INT32_MAX) next = INT32_MAX;
    __atomic_store_n(&gc_threshold, (int32_t)next, __ATOMIC_RELAXED);
}

static int64_t gc_now_ns(void) {
    struct timespec t;
    clock_gettime(CLOCK_REALTIME, &t);
    return (int64_t)t.tv_sec * 1000000000LL + (int64_t)t.tv_nsec;
}

static int gc_stop_the_world(DragonMutator* self) {
    struct timespec deadline;
#if !defined(__APPLE__) && !defined(_WIN32)
    clock_gettime(CLOCK_MONOTONIC, &deadline);
#else
    clock_gettime(CLOCK_REALTIME, &deadline);
#endif
    deadline.tv_nsec += (long)GC_STOP_WAIT_NS;
    if (deadline.tv_nsec >= 1000000000L) {
        deadline.tv_sec += 1;
        deadline.tv_nsec -= 1000000000L;
    }

    pthread_mutex_lock(&gc_stop_lock);
    while (gc_stop_requested) pthread_cond_wait(&gc_stop_cond, &gc_stop_lock);
    __atomic_store_n(&gc_stop_requested, 1, __ATOMIC_SEQ_CST);
    self->is_collector = 1;

    int stopped = gc_claim_stopped(self);
    while (!stopped) {
        int rc = pthread_cond_timedwait(&gc_safe_cond, &gc_stop_lock, &deadline);
        stopped = gc_claim_stopped(self);
        if (!stopped && rc == ETIMEDOUT) break;
    }
    if (!stopped) {
        gc_release_world(self);
        gc_stat_deferred++;
    }
    pthread_mutex_unlock(&gc_stop_lock);
    return stopped;
}

static void gc_resume_the_world(DragonMutator* self, int64_t pause_ns) {
    pthread_mutex_lock(&gc_stop_lock);
    gc_release_world(self);
    gc_stat_collections++;
    gc_stat_last_pause_ns = pause_ns;
    if (pause_ns > gc_stat_max_pause_ns) gc_stat_max_pause_ns = pause_ns;
    pthread_mutex_unlock(&gc_stop_lock);
}

static void gc_defer_the_world(DragonMutator* self) {
    pthread_mutex_lock(&gc_stop_lock);
    if (self) gc_release_world(self);
    gc_stat_deferred++;
    pthread_mutex_unlock(&gc_stop_lock);
}

int64_t dragon_gc_collections() {
    return __atomic_load_n(&gc_stat_collections, __ATOMIC_RELAXED);
}

int64_t dragon_gc_deferred_collections() {
    return __atomic_load_n(&gc_stat_deferred, __ATOMIC_RELAXED);
}

int64_t dragon_gc_max_pause_ns() {
    return __atomic_load_n(&gc_stat_max_pause_ns, __ATOMIC_RELAXED);
}

int64_t dragon_gc_last_pause_ns() {
    return __atomic_load_n(&gc_stat_last_pause_ns, __ATOMIC_RELAXED);
}

void dragon_fatal_concurrent_mutation(const char* kind) {
    fprintf(stderr,
        "DRAGON FATAL: concurrent mutation of a shared %s\n"
        "\n"
        "Two vthreads mutated the same globally-reachable %s at the same\n"
        "time. Reads of shared state are always safe and lock-free, but\n"
        "unsynchronized concurrent MUTATION corrupts memory, so the runtime\n"
        "stops at the first overlap it detects. Guard every mutation of\n"
        "shared state with a Lock:\n"
        "\n"
        "    from threading import Lock\n"
        "    lock: Lock = Lock()\n"
        "\n"
        "    with lock {\n"
        "        SHARED[key] = value\n"
        "    }\n",
        kind, kind);
    fflush(stderr);
    abort();
}

static inline bool gc_tracked_append(DragonObjectHeader* h, void* obj);
static inline void gc_tracked_remove(DragonObjectHeader* h);

static int gc_in_progress = 0;

__thread jmp_buf     __dragon_exc_stack[DRAGON_EXC_STACK_SIZE];
__thread int         __dragon_exc_sp   = -1;
__thread int         __dragon_exc_type = 0;
__thread const char* __dragon_exc_msg  = "";
__thread void*       __dragon_exc_obj  = nullptr;
__thread DragonVThread* __current_vthread = NULL;
__thread DragonVThread* __dragon_exc_vt = NULL;

__thread DragonCleanupStack __dragon_cleanup = {nullptr, nullptr, nullptr, 0, 0};
__thread int32_t            __dragon_cleanup_saved[DRAGON_EXC_STACK_SIZE] = {0};

__thread int __dragon_active_frames = 0;

__thread int __dragon_atomic_context = 0;

int64_t dragon_class_register_dealloc(void* fn) {
    int id = __atomic_fetch_add(&__next_class_id, 1, __ATOMIC_SEQ_CST);
    if (id < DRAGON_MAX_CLASS_IDS) {
        __class_dealloc_table[id] = (dragon_class_dealloc_fn)fn;
    }
    return (int64_t)id;
}

int64_t dragon_class_register_clear(int64_t class_id, void* fn) {
    if (class_id > 0 && class_id < DRAGON_MAX_CLASS_IDS) {
        __class_clear_table[class_id] = (dragon_class_clear_fn)fn;
    }
    return class_id;
}

int64_t dragon_class_register_traverse(int64_t class_id, void* fn) {
    if (class_id > 0 && class_id < DRAGON_MAX_CLASS_IDS) {
        __class_traverse_table[class_id] = (dragon_class_traverse_fn)fn;
    }
    return class_id;
}

int64_t dragon_class_register_mark_shared(int64_t class_id, void* fn) {
    if (class_id > 0 && class_id < DRAGON_MAX_CLASS_IDS) {
        __class_mark_shared_table[class_id] = (dragon_class_mark_shared_fn)fn;
    }
    return class_id;
}

void dragon_closure_dealloc(DragonClosure* cls);
void dragon_env_dealloc(DragonEnv* env);

static void dragon_dealloc(void* obj) {
    if (!obj) return;
    dragon_gc_assert_mutable("dealloc");
    DragonObjectHeader* h = (DragonObjectHeader*)obj;
    if (h->gc_flags & GC_FLAG_TRACKED) dragon_gc_untrack(obj);
    switch (h->type_tag) {
        case DRAGON_TAG_LIST:  dragon_list_destroy((struct DragonList*)obj); break;
        case DRAGON_TAG_LIST_BOX: dragon_list_box_destroy((struct DragonListBox*)obj); break;
        case DRAGON_TAG_DICT:  dragon_dict_destroy((struct DragonDict*)obj); break;
        case DRAGON_TAG_TUPLE: dragon_tuple_destroy((struct DragonTuple*)obj); break;
        case DRAGON_TAG_SET:   dragon_set_destroy((struct DragonSet*)obj); break;
        case DRAGON_TAG_BYTES:
        case DRAGON_TAG_BYTEARRAY: dragon_bytes_destroy((struct DragonBytes*)obj); break;
        case DRAGON_TAG_DEQUE: dragon_deque_destroy((struct DragonDeque*)obj); break;
        case DRAGON_TAG_STR:   free(obj); break;
        case DRAGON_TAG_CLASS: {
            uint16_t cid = h->class_id;
            if (cid > 0 && cid < DRAGON_MAX_CLASS_IDS && __class_dealloc_table[cid]) {
                __class_dealloc_table[cid](obj);
            }
            free(obj);
            break;
        }
        case DRAGON_TAG_GENERATOR:
            dragon_generator_destroy(obj);
            break;
        case DRAGON_TAG_TYPE:
            break;
        case DRAGON_TAG_CLOSURE:
            dragon_closure_dealloc((DragonClosure*)obj);
            return;
        case DRAGON_TAG_ENV:
            dragon_env_dealloc((DragonEnv*)obj);
            return;
        case DRAGON_TAG_CELL: {
            DragonCell* c = (DragonCell*)obj;
            if (c->holds_heap && c->value) {
                if (c->kind == TAG_STR) {
                    dragon_decref_str_dispatch((char*)(intptr_t)c->value);
                } else {
                    dragon_decref_dispatch((void*)(intptr_t)c->value);
                }
            }
            free(obj);
            return;
        }
    }
}

void dragon_incref(void* obj) {
    if (!obj) return;
    DragonObjectHeader* h = (DragonObjectHeader*)obj;
    if (dragon_refcount_load(h) >= DRAGON_IMMORTAL_REFCOUNT) return;
    if (dragon_gc_flags_load(h) & GC_FLAG_SHARED) {
        __atomic_fetch_add(&h->refcount, 1, __ATOMIC_RELAXED);
        return;
    }
    h->refcount++;
}

void dragon_decref(void* obj) {
    if (obj) {
        DragonObjectHeader* h = (DragonObjectHeader*)obj;
        if (dragon_refcount_load(h) >= DRAGON_IMMORTAL_REFCOUNT) return;
        if (dragon_gc_flags_load(h) & GC_FLAG_SHARED) {
            dragon_decref_atomic(obj);
            return;
        }
        if (h->refcount > 1) {
            h->refcount--;
            return;
        }
        // Decref-to-zero of a tracked object races with GC (which could capture refcount==0 and
        // schedule dealloc concurrently -> double-free). Serialize through gc_lock: either we decrement+dealloc under the lock, or the lock blocks us until GC finishes and clears TRACKED.
        bool tracked = (h->gc_flags & GC_FLAG_TRACKED) != 0;
        if (!tracked) {
            if (--h->refcount != 0) return;
            dragon_gc_poll();
            dragon_dealloc(obj);
            return;
        }
        // Cycle-collector ownership guard: the collector marks unreachable objects IN_TO_FREE
        // before clear_refs + dealloc; decrefs reaching 0 here must NOT dealloc (UAF/double-free) - only the collector's loop does. Bare gc_collecting is too coarse (bloats gc_tracked with refcount-0 orphans on reachable objects). Mirrors dragon_decref_str.
        if (__atomic_load_n(&h->gc_flags, __ATOMIC_ACQUIRE) & GC_FLAG_IN_TO_FREE) {
            if (h->refcount > 0) --h->refcount;
            return;
        }
        if (!__atomic_load_n(&gc_concurrent, __ATOMIC_ACQUIRE)) {
            if (--h->refcount == 0) {
                gc_tracked_remove(h);
                dragon_dealloc(obj);
            }
            return;
        }
        pthread_mutex_lock(&gc_lock);
        if (!(h->gc_flags & GC_FLAG_TRACKED)) {
            pthread_mutex_unlock(&gc_lock);
            return;
        }
        if (--h->refcount == 0) {
            gc_tracked_remove(h);
            pthread_mutex_unlock(&gc_lock);
            dragon_gc_poll();
            dragon_dealloc(obj);
            return;
        }
        pthread_mutex_unlock(&gc_lock);
    }
}

void dragon_incref_atomic(void* obj) {
    if (!obj) return;
    DragonObjectHeader* h = (DragonObjectHeader*)obj;
    if (dragon_refcount_load(h) >= DRAGON_IMMORTAL_REFCOUNT) return;
    if (!(dragon_gc_flags_load(h) & GC_FLAG_SHARED))
        __atomic_fetch_or(&h->gc_flags, GC_FLAG_SHARED, __ATOMIC_RELAXED);
    __atomic_fetch_add(&h->refcount, 1, __ATOMIC_RELAXED);
}

void dragon_decref_atomic(void* obj) {
    if (obj) {
        DragonObjectHeader* h = (DragonObjectHeader*)obj;
        if (dragon_refcount_load(h) >= DRAGON_IMMORTAL_REFCOUNT) return;
        if (!(dragon_gc_flags_load(h) & GC_FLAG_SHARED))
            __atomic_fetch_or(&h->gc_flags, GC_FLAG_SHARED, __ATOMIC_RELAXED);
        if (__atomic_sub_fetch(&h->refcount, 1, __ATOMIC_ACQ_REL) == 0) {
            // Cycle-collector ownership guard: if IN_TO_FREE is set (pass 1 of dragon_gc_collect),
            // the collector owns the dealloc; re-entering dragon_dealloc here would UAF/double-free, and also sidesteps a gc_lock self-deadlock. Narrower than gc_collecting: reachable/never-collected objects still free eagerly, so gc_tracked doesn't bloat with orphans.
            if (__atomic_load_n(&h->gc_flags, __ATOMIC_ACQUIRE) & GC_FLAG_IN_TO_FREE) {
                return;
            }
            if (h->gc_flags & GC_FLAG_TRACKED) {
                pthread_mutex_lock(&gc_lock);
                if (!(h->gc_flags & GC_FLAG_TRACKED)) {
                    pthread_mutex_unlock(&gc_lock);
                    return;
                }
                int32_t idx = h->gc_track_idx;
                int32_t last = gc_tracked_size - 1;
                if (idx >= 0 && idx <= last) {
                    if (idx != last) {
                        gc_tracked[idx] = gc_tracked[last];
                        ((DragonObjectHeader*)gc_tracked[idx])->gc_track_idx = idx;
                    }
                    gc_tracked_size--;
                }
                h->gc_flags &= ~GC_FLAG_TRACKED;
                h->gc_track_idx = -1;
                pthread_mutex_unlock(&gc_lock);
            }
            dragon_gc_poll();
            int saved = __dragon_atomic_context;
            __dragon_atomic_context = 1;
            dragon_dealloc(obj);
            __dragon_atomic_context = saved;
        }
    }
}

void dragon_make_immortal(void* obj) {
    if (obj) ((DragonObjectHeader*)obj)->refcount = DRAGON_IMMORTAL_REFCOUNT;
}

int64_t dragon_is_immortal_obj(void* obj) {
    return (int64_t)dragon_is_immortal(obj);
}

static inline bool gc_tracked_append(DragonObjectHeader* h, void* obj) {
    dragon_gc_assert_mutable("gc track");
    if (gc_tracked_size >= gc_tracked_cap) {
        int64_t new_cap = gc_tracked_cap ? (int64_t)gc_tracked_cap * 2 : 256;
        if (new_cap > INT32_MAX) new_cap = INT32_MAX;
        if (new_cap <= gc_tracked_cap) {
            fprintf(stderr, "dragon: GC tracked-set exhausted (INT32_MAX objects)\n");
            abort();
        }
        void** grown = (void**)dragon_realloc_nullable(
            gc_tracked, dragon_alloc_bytes_or_abort(new_cap, sizeof(void*)));
        if (!grown) return false;
        gc_tracked = grown;
        gc_tracked_cap = (int32_t)new_cap;
    }
    h->gc_track_idx = gc_tracked_size;
    h->gc_flags |= GC_FLAG_TRACKED;
    gc_tracked[gc_tracked_size++] = obj;
    return true;
}

static inline void gc_tracked_remove(DragonObjectHeader* h) {
    int32_t idx = h->gc_track_idx;
    int32_t last = gc_tracked_size - 1;
    if (idx >= 0 && idx <= last) {
        if (idx != last) {
            gc_tracked[idx] = gc_tracked[last];
            ((DragonObjectHeader*)gc_tracked[idx])->gc_track_idx = idx;
        }
        gc_tracked_size--;
    }
    h->gc_flags &= ~GC_FLAG_TRACKED;
    h->gc_track_idx = -1;
}

static void dragon_gc_age_one() {
    if (__atomic_add_fetch(&gc_alloc_counter, 1, __ATOMIC_RELAXED)
        >= __atomic_load_n(&gc_threshold, __ATOMIC_RELAXED)) {
        dragon_gc_collect();
    }
}

bool dragon_gc_try_track(void* obj) {
    if (!obj) return true;
    auto* h = (DragonObjectHeader*)obj;
    if (h->gc_flags & GC_FLAG_TRACKED) return true;

    if (!__atomic_load_n(&gc_concurrent, __ATOMIC_ACQUIRE)) {
        if (!gc_tracked_append(h, obj)) return false;
        dragon_gc_age_one();
        return true;
    }

    pthread_mutex_lock(&gc_lock);
    if (h->gc_flags & GC_FLAG_TRACKED) {
        pthread_mutex_unlock(&gc_lock);
        return true;
    }
    bool appended = gc_tracked_append(h, obj);
    pthread_mutex_unlock(&gc_lock);
    if (!appended) return false;
    dragon_gc_poll();
    dragon_gc_age_one();
    return true;
}

void dragon_gc_track(void* obj) {
    if (dragon_gc_try_track(obj)) return;
    fprintf(stderr, "dragon: out of memory growing GC tracked-set\n");
    abort();
}

void dragon_gc_untrack(void* obj) {
    if (!obj) return;
    auto* h = (DragonObjectHeader*)obj;
    if (!(h->gc_flags & GC_FLAG_TRACKED)) return;

    if (!__atomic_load_n(&gc_concurrent, __ATOMIC_ACQUIRE)) {
        gc_tracked_remove(h);
        return;
    }

    pthread_mutex_lock(&gc_lock);
    if (!(h->gc_flags & GC_FLAG_TRACKED)) {
        pthread_mutex_unlock(&gc_lock);
        return;
    }
    gc_tracked_remove(h);
    pthread_mutex_unlock(&gc_lock);
}

void dragon_gc_set_threshold(int64_t n) {
    __atomic_store_n(&gc_threshold, (int32_t)n, __ATOMIC_RELAXED);
}

static void dragon_list_traverse(void* obj, dragon_gc_visit_fn visit, void* arg) {
    auto* l = (DragonList*)obj;
    if (!l || !l->data || l->size == 0) return;
    // Only follows heap-child tags (value_tag_is_traceable). TAG_INT must NOT be traversed: an
    // attacker-supplied integer aliasing a tracked address could subtract a ref during trial-deletion (premature free/UAF). list[Callable] (tag 10) is included since a closure can capture the list back (real cycle); a bare fn ptr is a safe hash-miss no-op since visit only dereferences tracked children.
    if (dragon_value_tag_is_traceable((int8_t)l->elem_tag)) {
        for (int64_t i = 0; i < l->size; i++) {
            int64_t v = dragon_list_load(l, i);
            if (v) visit((void*)(uintptr_t)v, arg);
        }
    }
}

static void dragon_dict_traverse(void* obj, dragon_gc_visit_fn visit, void* arg) {
    auto* d = (DragonDict*)obj;
    if (!d || d->size == 0) return;
    for (int64_t i = 0; i < d->size; i++) {
        if (dragon_value_tag_is_traceable(d->entries[i].tag) &&
            d->entries[i].value)
            visit((void*)(uintptr_t)d->entries[i].value, arg);
    }
}

static void dragon_tuple_traverse(void* obj, dragon_gc_visit_fn visit, void* arg) {
    auto* t = (DragonTuple*)obj;
    if (!t || !t->data || t->length == 0) return;
    if (t->elem_tags) {
        for (int64_t i = 0; i < t->length; i++) {
            uint8_t tag = t->elem_tags[i];
            if (dragon_value_tag_is_traceable((int8_t)tag) && t->data[i])
                visit((void*)(uintptr_t)t->data[i], arg);
        }
    } else {
        for (int64_t i = 0; i < t->length; i++) {
            if (t->data[i]) visit((void*)(uintptr_t)t->data[i], arg);
        }
    }
}

static void dragon_set_traverse(void* obj, dragon_gc_visit_fn visit, void* arg) {
    auto* s = (DragonSet*)obj;
    if (!s || !s->buckets || s->count == 0) return;
    if (dragon_value_tag_is_traceable((int8_t)s->elem_tag)) {
        for (int64_t i = 0; i < s->capacity; i++) {
            if (s->states[i] == 1 && s->buckets[i])
                visit((void*)(uintptr_t)s->buckets[i], arg);
        }
    }
}

// list[Any] per-element {tag, payload}; visit only heap-tagged children (mirrors dragon_dict_traverse).
// Deliberately NOT the shared traceable predicate: tag-10 closures are excluded since the box doesn't own closure refs (WALL note, dragon_listbox_decref_elem) - subtracting a non-owned edge could free a live closure (UAF).
static void dragon_list_box_traverse(void* obj, dragon_gc_visit_fn visit, void* arg) {
    auto* l = (DragonListBox*)obj;
    if (!l || !l->data || l->size == 0) return;
    for (int64_t i = 0; i < l->size; i++) {
        int64_t tag = l->data[i].tag;
        int64_t v = l->data[i].payload;
        if (v && (tag == TAG_LIST || tag == TAG_DICT || tag == TAG_BYTES))
            visit((void*)(uintptr_t)v, arg);
    }
}

static void dragon_deque_traverse(void* obj, dragon_gc_visit_fn visit, void* arg) {
    auto* d = (DragonDeque*)obj;
    if (!d || !d->data || d->size == 0) return;
    if (dragon_value_tag_is_traceable((int8_t)d->elem_tag)) {
        for (int64_t i = 0; i < d->size; i++) {
            int64_t v = d->data[(d->head + i) % d->capacity];
            if (v) visit((void*)(uintptr_t)v, arg);
        }
    }
}

static void dragon_traverse(void* obj, dragon_gc_visit_fn visit, void* arg) {
    if (!obj) return;
    auto* h = (DragonObjectHeader*)obj;
    switch (h->type_tag) {
        case DRAGON_TAG_LIST:  dragon_list_traverse(obj, visit, arg); break;
        case DRAGON_TAG_LIST_BOX: dragon_list_box_traverse(obj, visit, arg); break;
        case DRAGON_TAG_DICT:  dragon_dict_traverse(obj, visit, arg); break;
        case DRAGON_TAG_TUPLE: dragon_tuple_traverse(obj, visit, arg); break;
        case DRAGON_TAG_SET:   dragon_set_traverse(obj, visit, arg); break;
        case DRAGON_TAG_DEQUE: dragon_deque_traverse(obj, visit, arg); break;
        case DRAGON_TAG_CLASS: {
            uint16_t cid = h->class_id;
            if (cid > 0 && cid < DRAGON_MAX_CLASS_IDS && __class_traverse_table[cid])
                __class_traverse_table[cid](obj, visit, arg);
            break;
        }
        case DRAGON_TAG_CELL: {
            DragonCell* c = (DragonCell*)obj;
            if (c->holds_heap && c->value) {
                visit((void*)(intptr_t)c->value, arg);
            }
            break;
        }
        case DRAGON_TAG_CLOSURE: {
            DragonClosure* cls = (DragonClosure*)obj;
            if (cls->env) visit(cls->env, arg);
            break;
        }
        case DRAGON_TAG_ENV: {
            DragonEnv* env = (DragonEnv*)obj;
            if (env->gc_fn) env->gc_fn(env, DRAGON_ENV_OP_TRAVERSE, visit, arg);
            break;
        }
        default: break;
    }
}

static void dragon_list_clear_refs(void* obj) {
    auto* l = (DragonList*)obj;
    if (!l || !l->data) return;
    uint8_t tag = l->elem_tag;
    if (tag == TAG_STR) {
        for (int64_t i = 0; i < l->size; i++) {
            int64_t v = dragon_list_load(l, i);
            if (v) dragon_str_force_free_if_zero((const char*)(uintptr_t)v);
            dragon_list_store(l, i, 0);
        }
    } else if (tag == TAG_LIST || tag == TAG_DICT || tag == TAG_BYTES) {
        for (int64_t i = 0; i < l->size; i++) {
            int64_t v = dragon_list_load(l, i);
            if (v) dragon_decref((void*)(uintptr_t)v);
            dragon_list_store(l, i, 0);
        }
    } else if (tag == DRAGON_TAG_CLOSURE) {
        for (int64_t i = 0; i < l->size; i++) {
            int64_t v = dragon_list_load(l, i);
            if (v) dragon_decref_callable((void*)(uintptr_t)v);
            dragon_list_store(l, i, 0);
        }
    }
    l->size = 0;
}

static void dragon_dict_clear_refs(void* obj) {
    auto* d = (DragonDict*)obj;
    if (!d) return;
    for (int64_t i = 0; i < d->size; i++) {
        int8_t tag = d->entries[i].tag;
        int64_t val = d->entries[i].value;
        if (val && tag == TAG_STR) {
            dragon_str_force_free_if_zero((const char*)(uintptr_t)val);
        } else if (val && (tag == TAG_LIST || tag == TAG_DICT || tag == TAG_BYTES)) {
            dragon_decref((void*)(uintptr_t)val);
        } else if (val && tag == DRAGON_TAG_CLOSURE) {
            dragon_decref_callable((void*)(uintptr_t)val);
        }
        d->entries[i].value = 0;
        if (d->key_kind == DRAGON_DICT_KEY_STR && d->entries[i].key)
            dragon_str_force_free_if_zero(d->entries[i].key);
        d->entries[i].key = nullptr;
    }
    d->size = 0;
    for (int64_t i = 0; i < d->index_size; i++) d->indices[i] = -1;
}

static void dragon_tuple_clear_refs(void* obj) {
    auto* t = (DragonTuple*)obj;
    if (!t || !t->data) return;
    if (t->elem_tags) {
        for (int64_t i = 0; i < t->length; i++) {
            uint8_t tag = t->elem_tags[i];
            int64_t val = t->data[i];
            if (val && tag == TAG_STR) {
                dragon_str_force_free_if_zero((const char*)(uintptr_t)val);
            } else if (val && (tag == TAG_LIST || tag == TAG_DICT || tag == TAG_BYTES)) {
                dragon_decref((void*)(uintptr_t)val);
            } else if (val && tag == DRAGON_TAG_CLOSURE) {
                dragon_decref_callable((void*)(uintptr_t)val);
            }
            t->data[i] = 0;
        }
    }
}

static void dragon_set_clear_refs(void* obj) {
    auto* s = (DragonSet*)obj;
    if (!s || !s->buckets) return;
    uint8_t tag = s->elem_tag;
    if (tag == TAG_STR) {
        for (int64_t i = 0; i < s->capacity; i++) {
            if (s->states[i] == 1 && s->buckets[i])
                dragon_str_force_free_if_zero((const char*)(uintptr_t)s->buckets[i]);
            s->buckets[i] = 0;
            s->states[i] = 0;
        }
    } else if (tag == TAG_LIST || tag == TAG_DICT || tag == TAG_BYTES) {
        for (int64_t i = 0; i < s->capacity; i++) {
            if (s->states[i] == 1 && s->buckets[i])
                dragon_decref((void*)(uintptr_t)s->buckets[i]);
            s->buckets[i] = 0;
            s->states[i] = 0;
        }
    } else if (tag == DRAGON_TAG_CLOSURE) {
        for (int64_t i = 0; i < s->capacity; i++) {
            if (s->states[i] == 1 && s->buckets[i])
                dragon_decref_callable((void*)(uintptr_t)s->buckets[i]);
            s->buckets[i] = 0;
            s->states[i] = 0;
        }
    }
    s->count = 0;
}

static void dragon_list_box_clear_refs(void* obj) {
    auto* l = (DragonListBox*)obj;
    if (!l || !l->data) return;
    for (int64_t i = 0; i < l->size; i++) {
        int64_t tag = l->data[i].tag;
        int64_t v = l->data[i].payload;
        if (v && tag == TAG_STR) {
            dragon_str_force_free_if_zero((const char*)(uintptr_t)v);
        } else if (v && (tag == TAG_LIST || tag == TAG_DICT || tag == TAG_BYTES)) {
            dragon_decref((void*)(uintptr_t)v);
        }
        l->data[i].payload = 0;
        l->data[i].tag = 0;
    }
    l->size = 0;
}

static void dragon_deque_clear_refs(void* obj) {
    auto* d = (DragonDeque*)obj;
    if (!d || !d->data) return;
    uint8_t tag = d->elem_tag;
    for (int64_t i = 0; i < d->size; i++) {
        int64_t idx = (d->head + i) % d->capacity;
        int64_t v = d->data[idx];
        if (v) {
            if (tag == TAG_STR) {
                dragon_str_force_free_if_zero((const char*)(uintptr_t)v);
            } else if (tag == TAG_LIST || tag == TAG_DICT || tag == TAG_BYTES) {
                dragon_decref((void*)(uintptr_t)v);
            } else if (tag == DRAGON_TAG_CLOSURE) {
                dragon_decref_callable((void*)(uintptr_t)v);
            }
        }
        d->data[idx] = 0;
    }
    d->size = 0;
    d->head = 0;
}

static void dragon_clear_refs(void* obj) {
    if (!obj) return;
    auto* h = (DragonObjectHeader*)obj;
    switch (h->type_tag) {
        case DRAGON_TAG_LIST:  dragon_list_clear_refs(obj); break;
        case DRAGON_TAG_LIST_BOX: dragon_list_box_clear_refs(obj); break;
        case DRAGON_TAG_DICT:  dragon_dict_clear_refs(obj); break;
        case DRAGON_TAG_TUPLE: dragon_tuple_clear_refs(obj); break;
        case DRAGON_TAG_SET:   dragon_set_clear_refs(obj); break;
        case DRAGON_TAG_DEQUE: dragon_deque_clear_refs(obj); break;
        case DRAGON_TAG_CLASS: {
            uint16_t cid = h->class_id;
            if (cid > 0 && cid < DRAGON_MAX_CLASS_IDS && __class_clear_table[cid])
                __class_clear_table[cid](obj);
            break;
        }
        case DRAGON_TAG_CELL: {
            DragonCell* c = (DragonCell*)obj;
            if (c->holds_heap && c->value) {
                if (c->kind == TAG_STR) {
                    dragon_str_force_free_if_zero((const char*)(intptr_t)c->value);
                } else {
                    dragon_decref((void*)(intptr_t)c->value);
                }
                c->value = 0;
                c->holds_heap = 0;
            }
            break;
        }
        case DRAGON_TAG_CLOSURE: {
            DragonClosure* cls = (DragonClosure*)obj;
            if (cls->env) { dragon_decref(cls->env); cls->env = nullptr; }
            break;
        }
        case DRAGON_TAG_ENV: {
            DragonEnv* env = (DragonEnv*)obj;
            if (env->gc_fn) env->gc_fn(env, DRAGON_ENV_OP_CLEAR, nullptr, nullptr);
            break;
        }
        default: break;
    }
}

struct GCHashEntry { void* key; int32_t idx; };

static int32_t gc_ht_lookup(GCHashEntry* ht, int32_t mask, void* key) {
    auto slot = (int32_t)((uintptr_t)key >> 4) & mask;
    for (;;) {
        if (ht[slot].key == key) return ht[slot].idx;
        if (ht[slot].key == nullptr) return -1;
        slot = (slot + 1) & mask;
    }
}

static void* gc_scratch_alloc(int64_t count, size_t elem_size, int zero) {
    size_t bytes;
    if (!dragon_alloc_bytes_try(count, elem_size, 0, &bytes)) return nullptr;
    return zero ? dragon_calloc_nullable(1, bytes) : dragon_malloc_nullable(bytes);
}

static void gc_ht_insert(GCHashEntry* ht, int32_t mask, void* key, int32_t idx) {
    auto slot = (int32_t)((uintptr_t)key >> 4) & mask;
    while (ht[slot].key != nullptr) slot = (slot + 1) & mask;
    ht[slot].key = key;
    ht[slot].idx = idx;
}

static __thread int64_t* __gc_refs = nullptr;
static __thread GCHashEntry* __gc_ht = nullptr;
static __thread int32_t __gc_ht_mask = 0;

static void gc_visit_subtract(void* child, void* ) {
    int32_t idx = gc_ht_lookup(__gc_ht, __gc_ht_mask, child);
    if (idx >= 0) __gc_refs[idx]--;
}

static void gc_visit_reachable(void* child, void* arg) {
    int32_t idx = gc_ht_lookup(__gc_ht, __gc_ht_mask, child);
    if (idx >= 0) {
        auto* h = (DragonObjectHeader*)child;
        if (!(dragon_gc_flags_load(h) & GC_FLAG_REACHABLE)) {
            __atomic_fetch_or(&h->gc_flags, GC_FLAG_REACHABLE, __ATOMIC_RELAXED);
            auto* queue = (int32_t*)arg;
            int32_t wpos = queue[0]++;
            queue[1 + wpos] = idx;
        }
    }
}

int64_t dragon_gc_collect() {
    DragonMutator* stw_self = nullptr;
    if (__atomic_load_n(&gc_concurrent, __ATOMIC_ACQUIRE)) {
        pthread_mutex_lock(&gc_lock);
        if (gc_in_progress) {
            pthread_mutex_unlock(&gc_lock);
            return 0;
        }
        gc_in_progress = 1;
        pthread_mutex_unlock(&gc_lock);
        dragon_gc_mutator_register();
        stw_self = __dragon_mutator;
        if (!gc_stop_the_world(stw_self)) {
            pthread_mutex_lock(&gc_lock);
            __atomic_store_n(&gc_alloc_counter, 0, __ATOMIC_RELAXED);
            gc_retune_threshold(0);
            gc_in_progress = 0;
            pthread_mutex_unlock(&gc_lock);
            return 0;
        }
    }
    const int64_t pause_start = gc_now_ns();

    pthread_mutex_lock(&gc_lock);
    if (!stw_self && gc_in_progress) {
        pthread_mutex_unlock(&gc_lock);
        return 0;
    }
    gc_in_progress = 1;

    __atomic_store_n(&gc_alloc_counter, 0, __ATOMIC_RELAXED);
    int32_t n = gc_tracked_size;
    if (n == 0) {
        gc_in_progress = 0;
        pthread_mutex_unlock(&gc_lock);
        if (stw_self) gc_resume_the_world(stw_self, gc_now_ns() - pause_start);
        return 0;
    }

    int64_t ht_cap64 = 1;
    while (ht_cap64 < (int64_t)n * 4) ht_cap64 <<= 1;
    if (ht_cap64 > INT32_MAX) {
        fprintf(stderr, "dragon: gc hash table exceeds int32 capacity\n");
        abort();
    }
    int32_t ht_cap = (int32_t)ht_cap64;
    int32_t ht_mask = ht_cap - 1;
    auto* ht = (GCHashEntry*)gc_scratch_alloc(ht_cap, sizeof(GCHashEntry), 1);
    auto* refs = (int64_t*)gc_scratch_alloc(n, sizeof(int64_t), 0);
    auto* queue = (int32_t*)gc_scratch_alloc((int64_t)n + 1, sizeof(int32_t), 0);
    auto* to_free = (void**)gc_scratch_alloc(n, sizeof(void*), 0);
    if (!ht || !refs || !queue || !to_free) {
        free(to_free);
        free(queue);
        free(refs);
        free(ht);
        gc_retune_threshold(0);
        gc_in_progress = 0;
        pthread_mutex_unlock(&gc_lock);
        gc_defer_the_world(stw_self);
        return 0;
    }

    for (int32_t i = 0; i < n; i++) {
        DragonObjectHeader* h = (DragonObjectHeader*)gc_tracked[i];
        int64_t rc = dragon_refcount_load(h);
        refs[i] = (rc >= DRAGON_IMMORTAL_REFCOUNT) ? DRAGON_IMMORTAL_REFCOUNT : rc;
        gc_ht_insert(ht, ht_mask, gc_tracked[i], i);
    }

    __gc_refs = refs;
    __gc_ht = ht;
    __gc_ht_mask = ht_mask;

    for (int32_t i = 0; i < n; i++) {
        dragon_traverse(gc_tracked[i], gc_visit_subtract, nullptr);
    }

    queue[0] = 0;

    for (int32_t i = 0; i < n; i++) {
        if (refs[i] > 0) {
            auto* h = (DragonObjectHeader*)gc_tracked[i];
            __atomic_fetch_or(&h->gc_flags, GC_FLAG_REACHABLE, __ATOMIC_RELAXED);
            queue[1 + queue[0]++] = i;
        }
    }

    int32_t rpos = 0;
    while (rpos < queue[0]) {
        int32_t idx = queue[1 + rpos++];
        dragon_traverse(gc_tracked[idx], gc_visit_reachable, queue);
    }

    __atomic_store_n(&gc_collecting, 1, __ATOMIC_RELEASE);
    int64_t collected = 0;
    int32_t to_free_count = 0;
    // Two-pass tear-down: Pass 1 marks every unreachable object IN_TO_FREE so Pass 2's clear_refs
    // and any concurrent mutator decref both skip dealloc (owned by the loop below). The flag must precede any clear_refs decref into to_free children, else a sibling decref races the flag-set -> UAF.
    for (int32_t i = 0; i < n; i++) {
        auto* h = (DragonObjectHeader*)gc_tracked[i];
        if (!(dragon_gc_flags_load(h) & GC_FLAG_REACHABLE)) {
            __atomic_fetch_or(&h->gc_flags, GC_FLAG_IN_TO_FREE, __ATOMIC_RELEASE);
            to_free[to_free_count++] = gc_tracked[i];
            collected++;
        }
    }
    for (int32_t i = 0; i < to_free_count; i++) {
        dragon_clear_refs(to_free[i]);
    }
    for (int32_t i = 0; i < gc_tracked_size; i++) {
        __atomic_fetch_and(&((DragonObjectHeader*)gc_tracked[i])->gc_flags,
                           (uint8_t)~GC_FLAG_REACHABLE, __ATOMIC_RELAXED);
    }

    __gc_refs = nullptr;
    __gc_ht = nullptr;

    // Drop gc_lock around dealloc invocations: user __dealloc__ may allocate, and those
    // allocations need gc_lock (gc_in_progress still blocks re-entering collect). gc_collecting stays 1 across the loop so mutator decrefs on STILL-TRACKED objects skip their own dealloc (no double-free), reset to 0 only after all to_free objects are freed.
    pthread_mutex_unlock(&gc_lock);

    if (stw_self) gc_resume_the_world(stw_self, gc_now_ns() - pause_start);

    for (int32_t i = 0; i < to_free_count; i++) {
        dragon_dealloc(to_free[i]);
    }

    free(to_free);
    free(refs);
    free(ht);
    free(queue);

    pthread_mutex_lock(&gc_lock);
    gc_retune_threshold(collected);
    __atomic_store_n(&gc_collecting, 0, __ATOMIC_RELEASE);
    gc_in_progress = 0;
    pthread_mutex_unlock(&gc_lock);
    return collected;
}

int64_t dragon_gc_tracked_count() {
    pthread_mutex_lock(&gc_lock);
    int64_t n = gc_tracked_size;
    pthread_mutex_unlock(&gc_lock);
    return n;
}

struct DragonSharedWorklist {
    void**  entries;
    int32_t size;
    int32_t cap;
};

static void shared_worklist_init(DragonSharedWorklist* w) {
    w->entries = nullptr;
    w->size = 0;
    w->cap = 0;
}

static void shared_worklist_free(DragonSharedWorklist* w) {
    if (w->entries) free(w->entries);
}

static void shared_worklist_push_internal(DragonSharedWorklist* w, void* obj) {
    if (w->size >= w->cap) {
        if (w->cap > INT32_MAX / 2) {
            fprintf(stderr, "dragon: shared worklist exceeds int32 capacity\n");
            abort();
        }
        w->cap = w->cap ? w->cap * 2 : 32;
        w->entries = (void**)dragon_xrealloc_n_or_abort(w->entries, w->cap, sizeof(void*));
    }
    w->entries[w->size++] = obj;
}

void dragon_mark_shared(void* obj) {
    if (!obj) return;
    DragonObjectHeader* h = (DragonObjectHeader*)obj;
    if (!(h->gc_flags & GC_FLAG_HEAP_OBJ)) return;
    if (dragon_is_immortal(obj)) return;
    if (h->gc_flags & GC_FLAG_SHARED) return;
    __atomic_fetch_or(&h->gc_flags, GC_FLAG_SHARED, __ATOMIC_RELAXED);
}

void dragon_mark_shared_str(const char* s) {
    if (!s) return;
    DragonString* ds = dragon_string_from_data(s);
    if (ds->header.type_tag != DRAGON_TAG_STR) return;
    if (!(ds->header.gc_flags & GC_FLAG_HEAP_OBJ)) return;
    if (dragon_is_immortal(ds)) return;
    if (ds->header.gc_flags & GC_FLAG_SHARED) return;
    __atomic_fetch_or(&ds->header.gc_flags, GC_FLAG_SHARED, __ATOMIC_RELAXED);
}

void dragon_mark_shared_worklist_push(void* worklist, void* obj) {
    if (!obj) return;
    auto* w = (DragonSharedWorklist*)worklist;
    DragonObjectHeader* h = (DragonObjectHeader*)obj;
    if (!(h->gc_flags & GC_FLAG_HEAP_OBJ)) return;
    if (dragon_is_immortal(obj)) return;
    if (h->gc_flags & GC_FLAG_SHARED) return;
    __atomic_fetch_or(&h->gc_flags, GC_FLAG_SHARED, __ATOMIC_RELAXED);
    shared_worklist_push_internal(w, obj);
}

void dragon_mark_shared_callable(void* worklist, void* obj) {
    if (!obj) return;
    DragonObjectHeader* h = (DragonObjectHeader*)obj;
    if (h->type_tag != DRAGON_TAG_CLOSURE) return;
    dragon_mark_shared_worklist_push(worklist, obj);
}

static void shared_walk_list(DragonList* l, DragonSharedWorklist* w) {
    if (!l || !l->data || l->size == 0) return;
    uint8_t tag = l->elem_tag;
    if (tag == TAG_STR) {
        for (int64_t i = 0; i < l->size; i++) {
            int64_t v = dragon_list_load(l, i);
            if (v) dragon_mark_shared_str((const char*)(uintptr_t)v);
        }
    } else if (tag == TAG_LIST || tag == TAG_DICT || tag == TAG_BYTES) {
        for (int64_t i = 0; i < l->size; i++) {
            int64_t v = dragon_list_load(l, i);
            if (v) dragon_mark_shared_worklist_push(w, (void*)(uintptr_t)v);
        }
    } else if (tag == DRAGON_TAG_CLOSURE) {
        for (int64_t i = 0; i < l->size; i++) {
            int64_t v = dragon_list_load(l, i);
            if (v) dragon_mark_shared_callable(w, (void*)(uintptr_t)v);
        }
    }
}

static void shared_walk_dict(DragonDict* d, DragonSharedWorklist* w) {
    if (!d || d->size == 0) return;
    for (int64_t i = 0; i < d->size; i++) {
        if (d->entries[i].dead) continue;
        int8_t tag = d->entries[i].tag;
        int64_t v = d->entries[i].value;
        if (v) {
            if (tag == TAG_STR) dragon_mark_shared_str((const char*)(uintptr_t)v);
            else if (tag == TAG_LIST || tag == TAG_DICT || tag == TAG_BYTES)
                dragon_mark_shared_worklist_push(w, (void*)(uintptr_t)v);
            else if (tag == DRAGON_TAG_CLOSURE)
                dragon_mark_shared_callable(w, (void*)(uintptr_t)v);
        }
        if (d->key_kind == DRAGON_DICT_KEY_STR && d->entries[i].key)
            dragon_mark_shared_str(d->entries[i].key);
    }
}

static void shared_walk_tuple(DragonTuple* t, DragonSharedWorklist* w) {
    if (!t || !t->data || t->length == 0) return;
    if (t->elem_tags) {
        for (int64_t i = 0; i < t->length; i++) {
            uint8_t tag = t->elem_tags[i];
            int64_t v = t->data[i];
            if (!v) continue;
            if (tag == TAG_STR) dragon_mark_shared_str((const char*)(uintptr_t)v);
            else if (tag == TAG_LIST || tag == TAG_DICT || tag == TAG_BYTES)
                dragon_mark_shared_worklist_push(w, (void*)(uintptr_t)v);
            else if (tag == DRAGON_TAG_CLOSURE)
                dragon_mark_shared_callable(w, (void*)(uintptr_t)v);
        }
    }
}

static void shared_walk_list_box(DragonListBox* l, DragonSharedWorklist* w) {
    if (!l || !l->data || l->size == 0) return;
    for (int64_t i = 0; i < l->size; i++) {
        int64_t tag = l->data[i].tag;
        int64_t v = l->data[i].payload;
        if (!v) continue;
        if (tag == TAG_STR) dragon_mark_shared_str((const char*)(uintptr_t)v);
        else if (tag == TAG_LIST || tag == TAG_DICT || tag == TAG_BYTES)
            dragon_mark_shared_worklist_push(w, (void*)(uintptr_t)v);
        else if (tag == DRAGON_TAG_CLOSURE)
            dragon_mark_shared_callable(w, (void*)(uintptr_t)v);
    }
}

static void shared_walk_set(DragonSet* s, DragonSharedWorklist* w) {
    if (!s || !s->buckets || s->count == 0) return;
    uint8_t tag = s->elem_tag;
    if (tag == TAG_STR) {
        for (int64_t i = 0; i < s->capacity; i++)
            if (s->states[i] == 1 && s->buckets[i])
                dragon_mark_shared_str((const char*)(uintptr_t)s->buckets[i]);
    } else if (tag == TAG_LIST || tag == TAG_DICT || tag == TAG_BYTES) {
        for (int64_t i = 0; i < s->capacity; i++)
            if (s->states[i] == 1 && s->buckets[i])
                dragon_mark_shared_worklist_push(w, (void*)(uintptr_t)s->buckets[i]);
    } else if (tag == DRAGON_TAG_CLOSURE) {
        // set[Callable]: tag-gated. Without this arm an un-marked-SHARED closure in a set
        // crossing threads does non-atomic refcounting (torn-refcount/UAF-by-race).
        for (int64_t i = 0; i < s->capacity; i++)
            if (s->states[i] == 1 && s->buckets[i])
                dragon_mark_shared_callable(w, (void*)(uintptr_t)s->buckets[i]);
    }
}

static void shared_walk_deque(DragonDeque* d, DragonSharedWorklist* w) {
    if (!d || !d->data || d->size == 0) return;
    uint8_t tag = d->elem_tag;
    for (int64_t i = 0; i < d->size; i++) {
        int64_t v = d->data[(d->head + i) % d->capacity];
        if (!v) continue;
        if (tag == TAG_STR) dragon_mark_shared_str((const char*)(uintptr_t)v);
        else if (tag == TAG_LIST || tag == TAG_DICT || tag == TAG_BYTES)
            dragon_mark_shared_worklist_push(w, (void*)(uintptr_t)v);
        else if (tag == DRAGON_TAG_CLOSURE)
            dragon_mark_shared_callable(w, (void*)(uintptr_t)v);
    }
}

void dragon_mark_shared_cell(void* worklist, void* cellPtr) {
    if (!cellPtr) return;
    DragonCell* c = (DragonCell*)cellPtr;
    if (c->header.type_tag != DRAGON_TAG_CELL) return;
    __atomic_fetch_or(&c->header.gc_flags, GC_FLAG_SHARED, __ATOMIC_RELAXED);
    if (!c->holds_heap || !c->value) return;
    if (c->kind == TAG_STR)
        dragon_mark_shared_str((const char*)(intptr_t)c->value);
    else if (c->kind == DRAGON_TAG_CLOSURE)
        dragon_mark_shared_callable(worklist, (void*)(intptr_t)c->value);
    else
        dragon_mark_shared_worklist_push(worklist, (void*)(intptr_t)c->value);
}

void dragon_mark_shared_deep(void* obj) {
    if (!obj) return;
    DragonObjectHeader* h = (DragonObjectHeader*)obj;
    if (!(h->gc_flags & GC_FLAG_HEAP_OBJ)) return;
    if (h->gc_flags & GC_FLAG_SHARED) return;

    DragonSharedWorklist w;
    shared_worklist_init(&w);
    __atomic_fetch_or(&h->gc_flags, GC_FLAG_SHARED, __ATOMIC_RELAXED);
    shared_worklist_push_internal(&w, obj);

    while (w.size > 0) {
        void* cur = w.entries[--w.size];
        DragonObjectHeader* ch = (DragonObjectHeader*)cur;
        switch (ch->type_tag) {
            case DRAGON_TAG_LIST:  shared_walk_list((DragonList*)cur, &w); break;
            case DRAGON_TAG_LIST_BOX: shared_walk_list_box((DragonListBox*)cur, &w); break;
            case DRAGON_TAG_DICT:  shared_walk_dict((DragonDict*)cur, &w); break;
            case DRAGON_TAG_TUPLE: shared_walk_tuple((DragonTuple*)cur, &w); break;
            case DRAGON_TAG_SET:   shared_walk_set((DragonSet*)cur, &w); break;
            case DRAGON_TAG_CLASS: {
                uint16_t cid = ch->class_id;
                if (cid > 0 && cid < DRAGON_MAX_CLASS_IDS && __class_mark_shared_table[cid])
                    __class_mark_shared_table[cid](cur, &w);
                break;
            }
            case DRAGON_TAG_DEQUE:
                shared_walk_deque((DragonDeque*)cur, &w);
                break;
            case DRAGON_TAG_BYTES: {
                DragonString* owner = dragon_bytes_borrowed_owner((DragonBytes*)cur);
                if (owner) dragon_mark_shared_str(owner->data);
                break;
            }
            case DRAGON_TAG_CLOSURE: {
                DragonClosure* cl = (DragonClosure*)cur;
                DragonEnv* env = cl->env;
                if (env) {
                    uint8_t prev = (uint8_t)__atomic_fetch_or(
                        &env->header.gc_flags, GC_FLAG_SHARED, __ATOMIC_RELAXED);
                    if (!(prev & GC_FLAG_SHARED) && env->gc_fn)
                        env->gc_fn(env, DRAGON_ENV_OP_MARK_SHARED, nullptr, &w);
                }
                break;
            }
            case DRAGON_TAG_ENV: {
                DragonEnv* env = (DragonEnv*)cur;
                if (env->gc_fn)
                    env->gc_fn(env, DRAGON_ENV_OP_MARK_SHARED, nullptr, &w);
                break;
            }
            default: break;
        }
    }
    shared_worklist_free(&w);
}

void dragon_incref_boxed(int64_t tag, int64_t payload) {
    if (!payload) return;
    if (tag == TAG_STR) {
        dragon_incref_str((const char*)(uintptr_t)payload);
        return;
    }
    if (tag == TAG_CALLABLE) {
        dragon_incref_callable((void*)(uintptr_t)payload);
        return;
    }
    if (tag >= TAG_LIST) dragon_incref((void*)(uintptr_t)payload);
}

void dragon_mark_shared_boxed(int64_t tag, int64_t payload) {
    if (!payload) return;
    if (tag == TAG_STR) {
        dragon_mark_shared_str((const char*)(uintptr_t)payload);
        return;
    }
    if (tag == DRAGON_TAG_CLOSURE) {
        DragonObjectHeader* h = (DragonObjectHeader*)(uintptr_t)payload;
        if (h->type_tag != DRAGON_TAG_CLOSURE) return;
        dragon_mark_shared_deep(h);
        return;
    }
    if (tag >= TAG_LIST) dragon_mark_shared_deep((void*)(uintptr_t)payload);
}

// del: the debug executable assertion of the ownership proof. OwnershipCheck
// proved the deleted binding is the value's sole owner, so refcount must be exactly 1 at the del; disagreement names the exact line instead of costing an ASan A/B hunt. Debug builds only (-O0); release lowers del to a plain scope-exit release.

static void dragon_del_violation(const char* file, int64_t line, int64_t rc) {
    fprintf(stderr,
            "dragon: del at %s:%lld: refcount is %lld, not 1 - a reference "
            "escaped the compiler's ownership proof (codegen refcount bug, or "
            "a callee retained a borrowed argument)\n",
            file ? file : "<unknown>", (long long)line, (long long)rc);
    abort();
}

void* dragon_obj_retain(void* p) {
    dragon_incref(p);
    return p;
}

int64_t dragon_deep_copy_tagged(int64_t val, int64_t tag) {
    if (!val) return val;
    switch (tag) {
        case TAG_STR:
            dragon_incref_str((const char*)(uintptr_t)val);
            return val;
        case TAG_BYTES:
            dragon_incref((void*)(uintptr_t)val);
            return val;
        case TAG_LIST:
            return (int64_t)(uintptr_t)dragon_list_deep_copy(
                (DragonList*)(uintptr_t)val);
        case TAG_DICT:
            return (int64_t)(uintptr_t)dragon_dict_deep_copy(
                (DragonDict*)(uintptr_t)val);
        default:
            dragon_incref_tagged(val, (uint8_t)tag);
            return val;
    }
}

DragonBox dragon_box_deep_copy(int64_t tag, int64_t payload) {
    DragonBox copy;
    copy.tag = tag;
    copy.payload = dragon_deep_copy_tagged(payload, tag);
    return copy;
}

void dragon_del_assert_unique(void* p, int64_t cls, const char* file,
                              int64_t line) {
    if (!p) return;
    int64_t rc;
    if (cls == 1) {
        const char* s = (const char*)p;
        if (!dragon_str_is_heap(s)) return;
        rc = dragon_string_from_data(s)->header.refcount;
    } else {
        rc = ((DragonObjectHeader*)p)->refcount;
    }
    if (rc >= DRAGON_IMMORTAL_REFCOUNT) return;
    if (rc != 1) dragon_del_violation(file, line, rc);
}

void dragon_del_assert_unique_box(int64_t tag, int64_t payload,
                                  const char* file, int64_t line) {
    if (!payload) return;
    if (tag == TAG_STR) {
        dragon_del_assert_unique((void*)(uintptr_t)payload, 1, file, line);
        return;
    }
    if (tag == DRAGON_TAG_CLOSURE) {
        DragonObjectHeader* h = (DragonObjectHeader*)(uintptr_t)payload;
        if (h->type_tag != DRAGON_TAG_CLOSURE) return;
        int64_t rc = h->refcount;
        if (rc < DRAGON_IMMORTAL_REFCOUNT && rc != 1)
            dragon_del_violation(file, line, rc);
        return;
    }
    if (tag >= TAG_LIST)
        dragon_del_assert_unique((void*)(uintptr_t)payload, 0, file, line);
}

}
