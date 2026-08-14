/// Dragon Runtime - Core: GC, Reference Counting, Object Lifecycle
/// Defines all shared state (GC tables, TLS exception state, etc.)

#include "runtime_internal.h"

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
    // Image 0 is always the main executable.
    const struct mach_header_64* mh =
        (const struct mach_header_64*)_dyld_get_image_header(0);
    intptr_t slide = _dyld_get_image_vmaddr_slide(0);
    uintptr_t lo = UINTPTR_MAX, hi = 0;
    const struct load_command* lc = (const struct load_command*)(mh + 1);
    for (uint32_t i = 0; i < mh->ncmds; i++) {
        if (lc->cmd == LC_SEGMENT_64) {
            const struct segment_command_64* seg =
                (const struct segment_command_64*)lc;
            // __PAGEZERO is the 4GB reserved null region, not image content.
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
#endif

extern "C" {

dragon_class_dealloc_fn __class_dealloc_table[DRAGON_MAX_CLASS_IDS];
dragon_class_clear_fn __class_clear_table[DRAGON_MAX_CLASS_IDS];
dragon_class_traverse_fn __class_traverse_table[DRAGON_MAX_CLASS_IDS];
dragon_class_mark_shared_fn __class_mark_shared_table[DRAGON_MAX_CLASS_IDS];
int __next_class_id = 1;  // 0 = unregistered

// gc_lock protects gc_tracked/_size/_cap and the file-static gc_in_progress flag; gc_alloc_counter,
// gc_threshold, gc_collecting are accessed atomically.
pthread_mutex_t gc_lock = PTHREAD_MUTEX_INITIALIZER;
void** gc_tracked = nullptr;
int32_t gc_tracked_size = 0;
int32_t gc_tracked_cap = 0;
int32_t gc_alloc_counter = 0;
// Baseline (and floor) for the cycle-collection trigger; adaptive from here (see the tail of
// dragon_gc_collect): backs off when a pass reclaims nothing, snaps back when one does, so acyclic churn stops paying for full tracked-set scans.
static const int32_t GC_BASE_THRESHOLD = 700;
int32_t gc_threshold = GC_BASE_THRESHOLD;
int gc_collecting = 0;

// Concurrency latch (see runtime_internal.h): 0 until a second heap-mutating OS thread starts,
// then 1 forever. While 0, track/untrack/decref-to-zero skip gc_lock since the collector runs synchronously on this same thread.
int gc_concurrent = 0;

void dragon_gc_go_concurrent(void) {
    __atomic_store_n(&gc_concurrent, 1, __ATOMIC_RELEASE);
}

// Concurrent-mutation detector trip wire (see runtime_internal.h): a second vthread entered a
// structural mutation of the same SHARED container while another was still inside its own. Continuing would corrupt the heap silently; stop at the first overlap with an actionable message.
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

// Tracked-set array helpers (defined below near gc_track). Caller must hold
// exclusion: gc_lock in concurrent mode, or be the sole mutator thread.
static inline void gc_tracked_append(DragonObjectHeader* h, void* obj);
static inline void gc_tracked_remove(DragonObjectHeader* h);

// Re-entrancy + thread-safety guard: one collection at a time across all threads. Set under
// gc_lock at the start of dragon_gc_collect, cleared under gc_lock at the end.
static int gc_in_progress = 0;

__thread jmp_buf     __dragon_exc_stack[DRAGON_EXC_STACK_SIZE];
__thread int         __dragon_exc_sp   = -1;
__thread int         __dragon_exc_type = 0;
__thread const char* __dragon_exc_msg  = "";
__thread void*       __dragon_exc_obj  = nullptr;
__thread DragonVThread* __current_vthread = NULL;
__thread DragonVThread* __dragon_exc_vt = NULL;

// Unwind cleanup stack TLS (main thread / non-vthread). Zero-initialized:
// vals/kinds/tags = NULL, sp = cap = 0 - lazily grown on first push.
__thread DragonCleanupStack __dragon_cleanup = {nullptr, nullptr, nullptr, 0, 0};
__thread int32_t            __dragon_cleanup_saved[DRAGON_EXC_STACK_SIZE] = {0};

// Live exception-frame count on the current context (try/with/for-iter/fire/generator-resume);
// push/pop is balanced (emitExcFramePops), saved/restored per vthread across yields. Codegen gates unwind-cleanup registration on it: no live frame means no local can ever be longjmp-unwound, so the hot path costs only a predicted-untaken branch.
__thread int __dragon_active_frames = 0;

// TLS atomic-context flag. See runtime_internal.h for semantics.
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

// D027: Forward declarations for closure/env dealloc (defined in runtime_builtins.cpp)
void dragon_closure_dealloc(DragonClosure* cls);
void dragon_env_dealloc(DragonEnv* env);

static void dragon_dealloc(void* obj) {
    if (!obj) return;
    DragonObjectHeader* h = (DragonObjectHeader*)obj;
    if (h->gc_flags & GC_FLAG_TRACKED) dragon_gc_untrack(obj);
    switch (h->type_tag) {
        case DRAGON_TAG_LIST:  dragon_list_destroy((struct DragonList*)obj); break;
        case DRAGON_TAG_LIST_BOX: dragon_list_box_destroy((struct DragonListBox*)obj); break;
        case DRAGON_TAG_DICT:  dragon_dict_destroy((struct DragonDict*)obj); break;
        case DRAGON_TAG_TUPLE: dragon_tuple_destroy((struct DragonTuple*)obj); break;
        case DRAGON_TAG_SET:   dragon_set_destroy((struct DragonSet*)obj); break;
        case DRAGON_TAG_BYTES: dragon_bytes_destroy((struct DragonBytes*)obj); break;
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
            // No free(obj): class descriptors are always immortal (dragon_class_descriptor_create
            // calls dragon_make_immortal), so this case is unreachable today. A future mortal TAG_TYPE would need to free the shell (name/doc are rodata borrows, ancestor_ids is malloc'd).
            break;
        case DRAGON_TAG_CLOSURE:
            dragon_closure_dealloc((DragonClosure*)obj);
            return;  // dealloc calls free
        case DRAGON_TAG_ENV:
            dragon_env_dealloc((DragonEnv*)obj);
            return;  // dealloc calls free
        case DRAGON_TAG_CELL: {
            // Drop the held heap reference (if any), then free the cell shell. Uses _dispatch
            // variants so a cell freed from an atomic-context worker (e.g. a fire'd vthread's env) routes children through the atomic decref path, matching the closure dealloc convention.
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
    // SHARED objects must use atomic ops since vthread bodies on different OS threads can race
    // on the refcount. The bit shares refcount's cache line, so the check is essentially free.
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
        // SHARED objects route to the atomic path, which already serializes
        // dealloc with GC under gc_lock and uses __ATOMIC_ACQ_REL ordering.
        if (dragon_gc_flags_load(h) & GC_FLAG_SHARED) {
            dragon_decref_atomic(obj);
            return;
        }
        // Fast path: refcount doesn't reach 0. No GC coordination needed.
        if (h->refcount > 1) {
            h->refcount--;
            return;
        }
        // Decref-to-zero of a tracked object races with GC (which could capture refcount==0 and
        // schedule dealloc concurrently -> double-free). Serialize through gc_lock: either we decrement+dealloc under the lock, or the lock blocks us until GC finishes and clears TRACKED.
        bool tracked = (h->gc_flags & GC_FLAG_TRACKED) != 0;
        if (!tracked) {
            // Not tracked - no GC interference possible. Plain decref.
            if (--h->refcount == 0) dragon_dealloc(obj);
            return;
        }
        // Cycle-collector ownership guard: the collector marks unreachable objects IN_TO_FREE
        // before clear_refs + dealloc; decrefs reaching 0 here must NOT dealloc (UAF/double-free) - only the collector's loop does. Bare gc_collecting is too coarse (bloats gc_tracked with refcount-0 orphans on reachable objects). Mirrors dragon_decref_str.
        if (__atomic_load_n(&h->gc_flags, __ATOMIC_ACQUIRE) & GC_FLAG_IN_TO_FREE) {
            if (h->refcount > 0) --h->refcount;
            return;
        }
        // Single-threaded fast path: the collector runs synchronously on this same thread
        // between mutator ops, so it can't be mid-collection here; no lock needed, inline untrack+dealloc.
        if (!__atomic_load_n(&gc_concurrent, __ATOMIC_ACQUIRE)) {
            if (--h->refcount == 0) {
                gc_tracked_remove(h);
                // dragon_dealloc sees GC_FLAG_TRACKED cleared and skips untrack.
                dragon_dealloc(obj);
            }
            return;
        }
        pthread_mutex_lock(&gc_lock);
        // Re-check TRACKED flag under lock: GC may have already claimed
        // this object and untracked it (and will free it after unlock).
        if (!(h->gc_flags & GC_FLAG_TRACKED)) {
            // GC owns it - don't touch refcount (GC already decided this
            // object is unreachable). Drop our reference silently.
            pthread_mutex_unlock(&gc_lock);
            return;
        }
        // GC hasn't claimed it: decrement under the lock so a concurrent GC snapshot sees the
        // final value, then dealloc if 0. Lock stays held across dealloc's untrack call to keep gc_tracked mutation linearizable.
        if (--h->refcount == 0) {
            gc_tracked_remove(h);  // inline untrack under the lock
            pthread_mutex_unlock(&gc_lock);
            // Dealloc (type-specific destroy) without the lock so recursive child decrefs can
            // re-acquire as needed; dragon_dealloc sees TRACKED already cleared and skips untrack.
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
    // Atomic-incref means this object is escaping to another OS thread; set SHARED so plain
    // incref/decref route atomic (idempotent under concurrent callers). The add stays RELAXED, like libstdc++ shared_ptr - dealloc ordering lives in decref's ACQ_REL sub.
    if (!(dragon_gc_flags_load(h) & GC_FLAG_SHARED))
        __atomic_fetch_or(&h->gc_flags, GC_FLAG_SHARED, __ATOMIC_RELAXED);
    __atomic_fetch_add(&h->refcount, 1, __ATOMIC_RELAXED);
}

void dragon_decref_atomic(void* obj) {
    if (obj) {
        DragonObjectHeader* h = (DragonObjectHeader*)obj;
        if (dragon_refcount_load(h) >= DRAGON_IMMORTAL_REFCOUNT) return;
        // Mark SHARED on entry - same rationale as dragon_incref_atomic.
        if (!(dragon_gc_flags_load(h) & GC_FLAG_SHARED))
            __atomic_fetch_or(&h->gc_flags, GC_FLAG_SHARED, __ATOMIC_RELAXED);
        if (__atomic_sub_fetch(&h->refcount, 1, __ATOMIC_ACQ_REL) == 0) {
            // Cycle-collector ownership guard: if IN_TO_FREE is set (pass 1 of dragon_gc_collect),
            // the collector owns the dealloc; re-entering dragon_dealloc here would UAF/double-free, and also sidesteps a gc_lock self-deadlock. Narrower than gc_collecting: reachable/never-collected objects still free eagerly, so gc_tracked doesn't bloat with orphans.
            if (__atomic_load_n(&h->gc_flags, __ATOMIC_ACQUIRE) & GC_FLAG_IN_TO_FREE) {
                return;
            }
            // Race with GC: take gc_lock to serialize our dealloc decision with any concurrent
            // collection; skip if GC already untracked and claimed this object.
            if (h->gc_flags & GC_FLAG_TRACKED) {
                pthread_mutex_lock(&gc_lock);
                if (!(h->gc_flags & GC_FLAG_TRACKED)) {
                    // GC claimed it.
                    pthread_mutex_unlock(&gc_lock);
                    return;
                }
                // Inline untrack under the lock.
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
            // Enter atomic-context for the dealloc: per-type destroy fns consult
            // __dragon_atomic_context and route child decrefs atomic (children may be shared). Save-and-restore so nested atomic deallocs preserve the outer flag.
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

// Append obj to the tracked set. Caller must hold exclusion (gc_lock in
// concurrent mode, or be the sole mutator thread in single-threaded mode).
static inline void gc_tracked_append(DragonObjectHeader* h, void* obj) {
    if (gc_tracked_size >= gc_tracked_cap) {
        // Grow in int64 then clamp: a plain `int32 *= 2` wraps to INT32_MIN past 2^31, feeding
        // realloc a ~16 EiB request. Hard-bounded by INT32_MAX anyway (gc_track_idx is int32).
        int64_t new_cap = gc_tracked_cap ? (int64_t)gc_tracked_cap * 2 : 256;
        if (new_cap > INT32_MAX) new_cap = INT32_MAX;
        if (new_cap <= gc_tracked_cap) {
            // Already at the ceiling and still full: 2.1B tracked objects is
            // far past any real workload - treat as fatal OOM, not a wrap.
            fprintf(stderr, "dragon: GC tracked-set exhausted (INT32_MAX objects)\n");
            abort();
        }
        void** grown = (void**)realloc(gc_tracked, (size_t)new_cap * sizeof(void*));
        if (!grown) {
            fprintf(stderr, "dragon: out of memory growing GC tracked-set\n");
            abort();  // old gc_tracked is intact but we cannot proceed.
        }
        gc_tracked = grown;
        gc_tracked_cap = (int32_t)new_cap;
    }
    h->gc_track_idx = gc_tracked_size;
    h->gc_flags |= GC_FLAG_TRACKED;
    gc_tracked[gc_tracked_size++] = obj;
}

// Swap-remove obj from the tracked set + clear its TRACKED flag. Caller must
// hold exclusion and have already confirmed GC_FLAG_TRACKED is set.
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

// Age the cyclic GC by one tracked allocation, collecting if the threshold is crossed. Without
// this, a class-only cyclic workload (`a.other=b; b.other=a` in a loop) never bumps gc_alloc_counter and leaks unbounded (binary-trees/L24); dragon_gc_track is the single choke point every cycle-capable object passes through.
static void dragon_gc_age_one() {
    if (__atomic_add_fetch(&gc_alloc_counter, 1, __ATOMIC_RELAXED)
        >= __atomic_load_n(&gc_threshold, __ATOMIC_RELAXED)) {
        dragon_gc_collect();
    }
}

void dragon_gc_track(void* obj) {
    if (!obj) return;
    auto* h = (DragonObjectHeader*)obj;
    // Quick lock-free early-out: GC_FLAG_TRACKED is only set under the lock, and only this
    // thread (the allocator) can race to set it, so an already-set flag is safe to skip.
    if (h->gc_flags & GC_FLAG_TRACKED) return;

    // Single-threaded fast path: no other thread, collector runs synchronously
    // on this same thread, so no lock is needed (see gc_concurrent).
    if (!__atomic_load_n(&gc_concurrent, __ATOMIC_ACQUIRE)) {
        gc_tracked_append(h, obj);
        dragon_gc_age_one();
        return;
    }

    pthread_mutex_lock(&gc_lock);
    // Re-check under the lock to handle concurrent track of the same object.
    if (h->gc_flags & GC_FLAG_TRACKED) {
        pthread_mutex_unlock(&gc_lock);
        return;
    }
    gc_tracked_append(h, obj);
    pthread_mutex_unlock(&gc_lock);
    // Age AFTER releasing gc_lock: dragon_gc_collect takes gc_lock itself.
    dragon_gc_age_one();
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
    // Re-check under the lock - another thread may have just untracked.
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
        // TAG_INT entries are raw integers, not pointers; visiting them would let an
        // attacker-supplied value alias a tracked address (trial-deletion corruption). Traceable tags only (value_tag_is_traceable, incl. tag-10 Callable, #11: dict[K,Callable] can cycle back; bare fn ptr is deref-safe).
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
            // #11: tuple Callable elements (tag 10) are covered by the shared
            // predicate; visit is deref-safe on a bare fn ptr (hash-miss).
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
    // TAG_INT set elements are raw integers, never pointers, so must not be traversed (aliasing
    // risk). Traceable tags only; #11: set[Callable] (tag 10) is covered by the shared predicate, deref-safe on a bare fn ptr.
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

// Deque elements live in the circular window [head, head+size); elem_tag drives dispatch like
// the list traverse. Without this, a deque is invisible to the cycle collector and leaks unconditionally in a cycle.
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
            // The cell holds at most one heap reference; visit it so cycles
            // through nonlocal-mutated containers are reachable to the GC.
            DragonCell* c = (DragonCell*)obj;
            if (c->holds_heap && c->value) {
                visit((void*)(intptr_t)c->value, arg);
            }
            break;
        }
        case DRAGON_TAG_CLOSURE: {
            // A closure owns exactly one ref on its env; visit it so the collector subtracts
            // that internal edge. Only trackable-env closures reach gc_tracked, so env is a real DragonEnv or NULL here.
            DragonClosure* cls = (DragonClosure*)obj;
            if (cls->env) visit(cls->env, arg);
            break;
        }
        case DRAGON_TAG_ENV: {
            // The env's captures live inline in a per-site layout only the codegen-emitted gc_fn
            // understands; delegate the walk to it (TRAVERSE visits each cycle-capable capture once).
            DragonEnv* env = (DragonEnv*)obj;
            if (env->gc_fn) env->gc_fn(env, DRAGON_ENV_OP_TRAVERSE, visit, arg);
            break;
        }
        default: break;
    }
}

// Clear functions: break cycles by setting contained refs to NULL and decref'ing.

static void dragon_list_clear_refs(void* obj) {
    auto* l = (DragonList*)obj;
    if (!l || !l->data) return;
    uint8_t tag = l->elem_tag;
    if (tag == TAG_STR) {
        // Force-free bypass: skips dragon_decref_str's gc_collecting guard so strings dropped
        // to refcount==0 are actually freed (Phase 6's dragon_list_destroy iterates size=0 and would leak them otherwise).
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
        // Breaks a list->closure->env->list cycle. The element may be a real DragonClosure
        // (IN_TO_FREE guard makes this a plain decrement; the dealloc loop frees it) or a bare fn ptr; dragon_decref_callable is tag-gated so it never writes a refcount into .text.
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
            // Bypass gc_collecting guard - see list_clear_refs.
            dragon_str_force_free_if_zero((const char*)(uintptr_t)val);
        } else if (val && (tag == TAG_LIST || tag == TAG_DICT || tag == TAG_BYTES)) {
            dragon_decref((void*)(uintptr_t)val);
        } else if (val && tag == DRAGON_TAG_CLOSURE) {
            // #11: tag-gated - frees a real closure (IN_TO_FREE-guarded), no-ops
            // on a bare fn ptr (never writes a refcount into .text).
            dragon_decref_callable((void*)(uintptr_t)val);
        }
        d->entries[i].value = 0;
        // Release the owned str key (force-free bypasses gc_collecting, like the value above),
        // then null it so the later dragon_dict_destroy (size now 0) can't re-release.
        if (d->keys_are_ptr && d->entries[i].key)
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
                // Bypass gc_collecting guard - see list_clear_refs.
                dragon_str_force_free_if_zero((const char*)(uintptr_t)val);
            } else if (val && (tag == TAG_LIST || tag == TAG_DICT || tag == TAG_BYTES)) {
                dragon_decref((void*)(uintptr_t)val);
            } else if (val && tag == DRAGON_TAG_CLOSURE) {
                dragon_decref_callable((void*)(uintptr_t)val);  // #11: tag-gated
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
            // Bypass gc_collecting guard - see list_clear_refs.
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
                dragon_decref_callable((void*)(uintptr_t)s->buckets[i]);  // #11
            s->buckets[i] = 0;
            s->states[i] = 0;
        }
    }
    s->count = 0;
}

// list[Any] cycle-break: drop refcounted child references in place. Strings use force_free
// (gc_collecting bypass), other heap tags decref normally; plain values just zero out.
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
        // No TAG_CLOSURE arm: the box does not own closure refs today - see
        // the WALL note in dragon_listbox_decref_elem (runtime_list.cpp).
        l->data[i].payload = 0;
        l->data[i].tag = 0;
    }
    l->size = 0;
}

// Deque cycle-break over the circular window, mirrors dragon_list_clear_refs (str force-free,
// heap decref, tag-gated closure decref), then empties the window so dragon_deque_destroy can't re-release.
static void dragon_deque_clear_refs(void* obj) {
    auto* d = (DragonDeque*)obj;
    if (!d || !d->data) return;
    uint8_t tag = d->elem_tag;
    for (int64_t i = 0; i < d->size; i++) {
        int64_t idx = (d->head + i) % d->capacity;
        int64_t v = d->data[idx];
        if (v) {
            if (tag == TAG_STR) {
                // Bypass gc_collecting guard - see list_clear_refs.
                dragon_str_force_free_if_zero((const char*)(uintptr_t)v);
            } else if (tag == TAG_LIST || tag == TAG_DICT || tag == TAG_BYTES) {
                dragon_decref((void*)(uintptr_t)v);
            } else if (tag == DRAGON_TAG_CLOSURE) {
                dragon_decref_callable((void*)(uintptr_t)v);  // tag-gated
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
            // Drop the closure's ref on its env and NULL the slot so the later dragon_closure_dealloc
            // sees env==NULL and doesn't double-decref; dragon_decref sees the env's IN_TO_FREE bit and only decrements.
            DragonClosure* cls = (DragonClosure*)obj;
            if (cls->env) { dragon_decref(cls->env); cls->env = nullptr; }
            break;
        }
        case DRAGON_TAG_ENV: {
            // The gc_fn CLEAR op decrefs+NULLs each heap capture slot, so the subsequent env
            // dealloc sees emptied slots exactly once, like the per-class clear.
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

static void* gc_alloc_or_abort(size_t bytes, int zero) {
    void* p = zero ? calloc(1, bytes) : malloc(bytes);
    if (!p && bytes) {
        fprintf(stderr, "dragon: out of memory during gc\n");
        abort();
    }
    return p;
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

static void gc_visit_subtract(void* child, void* /*arg*/) {
    int32_t idx = gc_ht_lookup(__gc_ht, __gc_ht_mask, child);
    if (idx >= 0) __gc_refs[idx]--;
}

static void gc_visit_reachable(void* child, void* arg) {
    int32_t idx = gc_ht_lookup(__gc_ht, __gc_ht_mask, child);
    if (idx >= 0) {
        auto* h = (DragonObjectHeader*)child;
        // Atomic RMW: a concurrent mark_shared fetch_or's GC_FLAG_SHARED into the same byte, so
        // a plain |= here could lose that bit (was a formal data race); only this thread sets REACHABLE so the load-then-or dedup itself can't race.
        if (!(dragon_gc_flags_load(h) & GC_FLAG_REACHABLE)) {
            __atomic_fetch_or(&h->gc_flags, GC_FLAG_REACHABLE, __ATOMIC_RELAXED);
            auto* queue = (int32_t*)arg;
            int32_t wpos = queue[0]++;
            queue[1 + wpos] = idx;
        }
    }
}

int64_t dragon_gc_collect() {
    pthread_mutex_lock(&gc_lock);
    // Only one collection at a time, across all threads: bail out immediately if another thread
    // is collecting, or we re-entered via __dealloc__ crossing gc_threshold while the lock was dropped.
    if (gc_in_progress) {
        pthread_mutex_unlock(&gc_lock);
        return 0;
    }
    gc_in_progress = 1;

    __atomic_store_n(&gc_alloc_counter, 0, __ATOMIC_RELAXED);
    int32_t n = gc_tracked_size;
    if (n == 0) {
        gc_in_progress = 0;
        pthread_mutex_unlock(&gc_lock);
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
    auto* ht = (GCHashEntry*)gc_alloc_or_abort((size_t)ht_cap * sizeof(GCHashEntry), 1);

    auto* refs = (int64_t*)gc_alloc_or_abort((size_t)n * sizeof(int64_t), 0);
    for (int32_t i = 0; i < n; i++) {
        DragonObjectHeader* h = (DragonObjectHeader*)gc_tracked[i];
        // Atomic snapshot read: other threads run atomic fetch_add/sub on refcount concurrently;
        // a plain read here was the audit 2.1/2.2 data race (torn snapshot feeding trial deletion). One load, then clamp the local copy.
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

    auto* queue = (int32_t*)gc_alloc_or_abort(((size_t)n + 1) * sizeof(int32_t), 0);
    queue[0] = 0;

    for (int32_t i = 0; i < n; i++) {
        if (refs[i] > 0) {
            auto* h = (DragonObjectHeader*)gc_tracked[i];
            // Atomic for the same reason as gc_visit_reachable: don't lose a
            // concurrent mark_shared SHARED bit.
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
    void** to_free = (void**)gc_alloc_or_abort((size_t)n * sizeof(void*), 0);
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
        // Atomic clear: plain &= could lose a concurrent SHARED fetch_or.
        __atomic_fetch_and(&((DragonObjectHeader*)gc_tracked[i])->gc_flags,
                           (uint8_t)~GC_FLAG_REACHABLE, __ATOMIC_RELAXED);
    }

    // TLS pointers no longer needed; clear before dropping the lock so a
    // re-entrant traverse (from __dealloc__) on this thread sees nullptrs.
    __gc_refs = nullptr;
    __gc_ht = nullptr;

    // Drop gc_lock around dealloc invocations: user __dealloc__ may allocate, and those
    // allocations need gc_lock (gc_in_progress still blocks re-entering collect). gc_collecting stays 1 across the loop so mutator decrefs on STILL-TRACKED objects skip their own dealloc (no double-free), reset to 0 only after all to_free objects are freed.
    pthread_mutex_unlock(&gc_lock);

    for (int32_t i = 0; i < to_free_count; i++) {
        dragon_dealloc(to_free[i]);
    }

    free(to_free);
    free(refs);
    free(ht);
    free(queue);

    pthread_mutex_lock(&gc_lock);
    // Adaptive trigger for the next collection: acyclic churn (freed promptly by refcounting)
    // shouldn't pay for a full O(live) scan that finds nothing, so back off geometrically on a nothing-reclaimed pass and snap back to baseline when one reclaims cycles, capped at GC_BASE+8*live. Changes only WHEN the collector runs, never correctness.
    {
        int32_t live = gc_tracked_size;
        int64_t next;
        if (collected == 0) {
            next = (int64_t)gc_threshold * 2;          // found nothing -> back off
        } else {
            next = GC_BASE_THRESHOLD;                   // found cycles -> stay eager
        }
        int64_t cap = (int64_t)GC_BASE_THRESHOLD + (int64_t)live * 8;
        if (next > cap) next = cap;
        if (next < GC_BASE_THRESHOLD) next = GC_BASE_THRESHOLD;
        if (next > 2147483647LL) next = 2147483647LL;   // clamp to int32
        __atomic_store_n(&gc_threshold, (int32_t)next, __ATOMIC_RELAXED);
    }
    __atomic_store_n(&gc_collecting, 0, __ATOMIC_RELEASE);
    gc_in_progress = 0;
    pthread_mutex_unlock(&gc_lock);
    return collected;
}

// Live tracked-container count, for leak probes: tracked objects are pinned by gc_tracked and
// invisible to LSan, so churn asserts count-before==count-after instead. Locked read: a torn size during a concurrent collect would fake or hide a leak.
int64_t dragon_gc_tracked_count() {
    pthread_mutex_lock(&gc_lock);
    int64_t n = gc_tracked_size;
    pthread_mutex_unlock(&gc_lock);
    return n;
}

// D018 (memory/d018-shared-refcount.md): dragon_mark_shared_deep walks an object graph via
// per-type/per-class child enumerators and sets GC_FLAG_SHARED on every reachable object, so plain incref/decref route atomic only where actually needed. Strings are leaves (marked directly, no BFS push); containers/classes are pushed and walked when popped.

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
        w->cap = w->cap ? w->cap * 2 : 32;
        // Abort (not raise) on OOM: runs inside mark-shared/GC, where raising would re-enter at
        // an arbitrary point. xrealloc_or_abort also fixes the self-assign NULL-write.
        w->entries = (void**)dragon_xrealloc_or_abort(w->entries, w->cap * sizeof(void*));
    }
    w->entries[w->size++] = obj;
}

void dragon_mark_shared(void* obj) {
    if (!obj) return;
    DragonObjectHeader* h = (DragonObjectHeader*)obj;
    // Skip non-heap pointers (e.g. raw integers cast to void*). The HEAP_OBJ
    // sentinel is set by `dragon_obj_init` for every legitimate heap header.
    if (!(h->gc_flags & GC_FLAG_HEAP_OBJ)) return;
    if (h->gc_flags & GC_FLAG_SHARED) return;
    __atomic_fetch_or(&h->gc_flags, GC_FLAG_SHARED, __ATOMIC_RELAXED);
}

void dragon_mark_shared_str(const char* s) {
    if (!s) return;
    DragonString* ds = dragon_string_from_data(s);
    if (ds->header.type_tag != DRAGON_TAG_STR) return;
    if (!(ds->header.gc_flags & GC_FLAG_HEAP_OBJ)) return;
    // Immortal strings (literals + interned non-ASCII) are never refcounted, so marking them
    // SHARED is meaningless; skipping also avoids cross-thread cache-line contention and a fault on an atomic OR into a read-only literal.
    if (dragon_is_immortal(ds)) return;
    if (ds->header.gc_flags & GC_FLAG_SHARED) return;
    __atomic_fetch_or(&ds->header.gc_flags, GC_FLAG_SHARED, __ATOMIC_RELAXED);
}

// Codegen-callable worklist push. Marks SHARED + queues for further BFS.
// Called from `__dragon_mark_shared_X` for container / class-instance fields.
void dragon_mark_shared_worklist_push(void* worklist, void* obj) {
    if (!obj) return;
    auto* w = (DragonSharedWorklist*)worklist;
    DragonObjectHeader* h = (DragonObjectHeader*)obj;
    if (!(h->gc_flags & GC_FLAG_HEAP_OBJ)) return;
    if (h->gc_flags & GC_FLAG_SHARED) return;
    __atomic_fetch_or(&h->gc_flags, GC_FLAG_SHARED, __ATOMIC_RELAXED);
    shared_worklist_push_internal(w, obj);
}

// Tag-gated shared-marking for a Callable: a bare fn ptr (no header) vs a DragonClosure (header,
// type_tag==DRAGON_TAG_CLOSURE). The generic worklist push writes gc_flags at offset 9, which for a bare fn ptr is a write into .text (SIGSEGV); gate on type_tag first (like dragon_incref_callable) so a bare fn ptr is a clean no-op.
void dragon_mark_shared_callable(void* worklist, void* obj) {
    if (!obj) return;
    DragonObjectHeader* h = (DragonObjectHeader*)obj;
    if (h->type_tag != DRAGON_TAG_CLOSURE) return;  // bare fn ptr / non-closure
    dragon_mark_shared_worklist_push(worklist, obj);
}

// Per-type child walkers for SHARED marking, distinct from cycle-collector traverse: these visit
// TAG_STR elements too (cycle traverse skips them) and mark+push directly instead of an indirect visit-fn call.

// TAG_INT entries are raw integers for list[int]/dict[K,int], not pointers; dereferencing them
// would segfault. Cycle-traverse tolerates TAG_INT (hash-lookup-only visitor) but the SHARED-mark visitor dereferences gc_flags, so it must only walk known-heap tags (list[Any]/dict[str,Any] packing pointers under TAG_INT are a follow-up, not yet handled here).
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
        // list[Callable]: tag-gated (element may be a bare fn pointer).
        for (int64_t i = 0; i < l->size; i++) {
            int64_t v = dragon_list_load(l, i);
            if (v) dragon_mark_shared_callable(w, (void*)(uintptr_t)v);
        }
    }
}

static void shared_walk_dict(DragonDict* d, DragonSharedWorklist* w) {
    if (!d || d->size == 0) return;
    for (int64_t i = 0; i < d->size; i++) {
        if (d->entries[i].dead) continue;  // tombstone: key/value cleared
        int8_t tag = d->entries[i].tag;
        int64_t v = d->entries[i].value;
        if (v) {
            if (tag == TAG_STR) dragon_mark_shared_str((const char*)(uintptr_t)v);
            else if (tag == TAG_LIST || tag == TAG_DICT || tag == TAG_BYTES)
                dragon_mark_shared_worklist_push(w, (void*)(uintptr_t)v);
            else if (tag == DRAGON_TAG_CLOSURE)
                dragon_mark_shared_callable(w, (void*)(uintptr_t)v);
        }
        // Keys are DragonStrings only when keys_are_ptr==1; an int-keyed dict casts the i64 key
        // into the pointer slot, so treating it as a string header reads wild memory (CodeGenE2E.DictEqIntKeyed).
        if (d->keys_are_ptr && d->entries[i].key)
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

// list[Any] SHARED propagation: per-element tag dispatches to string-mark or worklist-push like
// shared_walk_dict. Heap-tag-only; TAG_INT entries are raw integers, never pointers.
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

// Deque elements live in the circular window [head, head+size); elem_tag drives dispatch like
// the list walker. Previously a gap: a shared global deque's elements were never marked, so reading one out of a handler increfed an unmarked object (the same torn-refcount class the list/dict walkers close).
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
            // A closure's captures live in its env; walk via the codegen gc_fn (MARK_SHARED
            // visits every heap capture incl. strings, unlike cycle TRAVERSE). Previously a gap: a shared closure's captured str/list read inside a handler increfed an unmarked object.
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
            // Strings, bytes, generators, types: leaves for SHARED
            // propagation (no heap children to mark).
            default: break;
        }
    }
    shared_worklist_free(&w);
}

// SHARED-mark a boxed {tag, payload} value stored into a module-global Union/Any slot, mirroring
// emitUnionIncref's dispatch: tag 1 (STR) is a leaf; tag 10 (CLOSURE) gates on type_tag first since it may be a bare fn ptr; tag>=5 is header-carrying so the deep mark is safe.
void dragon_mark_shared_boxed(int64_t tag, int64_t payload) {
    if (!payload) return;
    if (tag == TAG_STR) {
        dragon_mark_shared_str((const char*)(uintptr_t)payload);
        return;
    }
    if (tag == DRAGON_TAG_CLOSURE) {
        DragonObjectHeader* h = (DragonObjectHeader*)(uintptr_t)payload;
        if (h->type_tag != DRAGON_TAG_CLOSURE) return;  // bare fn ptr: no-op
        // Deep, not single: the closure's env captures must be marked too
        // (mark_shared_deep's TAG_CLOSURE case walks them via the env gc_fn).
        dragon_mark_shared_deep(h);
        return;
    }
    if (tag >= TAG_LIST) dragon_mark_shared_deep((void*)(uintptr_t)payload);
}

// del (docs/002 ADR): the debug executable assertion of the ownership proof. OwnershipCheck
// proved the deleted binding is the value's sole owner, so refcount must be exactly 1 at the del; disagreement names the exact line instead of costing an ASan A/B hunt. Debug builds only (-O0); release lowers del to a plain scope-exit release.

static void dragon_del_violation(const char* file, int64_t line, int64_t rc) {
    fprintf(stderr,
            "dragon: del at %s:%lld: refcount is %lld, not 1 - a reference "
            "escaped the compiler's ownership proof (codegen refcount bug, or "
            "a callee retained a borrowed argument)\n",
            file ? file : "<unknown>", (long long)line, (long long)rc);
    abort();
}

// dub (docs/002 2.7): the priced deep copy. Immutable payloads (str, bytes) keep honest copy
// semantics via a free identity retain; mutable containers copy their spine and deep-copy elements by tag. Every entry returns an owned +1, so consumers adopt it like any owned call result.

/// Identity retain for header-carrying heap objects (bytes, tuples): the dub of an immutable
/// is the object itself, +1. Mirrors dragon_str_retain's calls-return-owned convention.
void* dragon_obj_retain(void* p) {
    dragon_incref(p);
    return p;
}

/// Deep-copy one element by its container tag; returns an owned +1 value.
int64_t dragon_deep_copy_tagged(int64_t val, int64_t tag) {
    if (!val) return val;
    switch (tag) {
        case TAG_STR:
            dragon_incref_str((const char*)(uintptr_t)val);
            return val;
        case TAG_BYTES:  // immutable: identity
            dragon_incref((void*)(uintptr_t)val);
            return val;
        case TAG_LIST:
            return (int64_t)(uintptr_t)dragon_list_deep_copy(
                (DragonList*)(uintptr_t)val);
        case TAG_DICT:
            return (int64_t)(uintptr_t)dragon_dict_deep_copy(
                (DragonDict*)(uintptr_t)val);
        default:
            // Scalars carry no refs; anything else (closure etc.) is E11 at
            // compile time - defensively share+retain rather than corrupt.
            dragon_incref_tagged(val, (uint8_t)tag);
            return val;
    }
}

/// cls 0 = generic heap object (pointer IS the DragonObjectHeader base);
/// cls 1 = string DATA pointer (header behind it; literals skipped).
void dragon_del_assert_unique(void* p, int64_t cls, const char* file,
                              int64_t line) {
    if (!p) return;
    int64_t rc;
    if (cls == 1) {
        const char* s = (const char*)p;
        if (!dragon_str_is_heap(s)) return;  // literal: nothing to count
        rc = dragon_string_from_data(s)->header.refcount;
    } else {
        rc = ((DragonObjectHeader*)p)->refcount;
    }
    if (rc >= DRAGON_IMMORTAL_REFCOUNT) return;  // immortal: exempt by design
    if (rc != 1) dragon_del_violation(file, line, rc);
}

/// Boxed {tag, payload} variant: dispatch by runtime tag; scalar tags and
/// bare-fn-ptr closures carry no refcount and are exempt.
void dragon_del_assert_unique_box(int64_t tag, int64_t payload,
                                  const char* file, int64_t line) {
    if (!payload) return;
    if (tag == TAG_STR) {
        dragon_del_assert_unique((void*)(uintptr_t)payload, 1, file, line);
        return;
    }
    if (tag == DRAGON_TAG_CLOSURE) {
        DragonObjectHeader* h = (DragonObjectHeader*)(uintptr_t)payload;
        if (h->type_tag != DRAGON_TAG_CLOSURE) return;  // bare fn ptr
        int64_t rc = h->refcount;
        if (rc < DRAGON_IMMORTAL_REFCOUNT && rc != 1)
            dragon_del_violation(file, line, rc);
        return;
    }
    if (tag >= TAG_LIST)
        dragon_del_assert_unique((void*)(uintptr_t)payload, 0, file, line);
}

} // extern "C"
