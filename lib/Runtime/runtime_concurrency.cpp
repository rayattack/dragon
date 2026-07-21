/// Dragon Runtime - Concurrency: Threads, Green Threads, Scheduler, I/O, Locks, Sync
#define MINICORO_IMPL
#include "runtime_internal.h"
#include <errno.h>
#include <time.h>  // clock_gettime / timespec / nanosleep for timed lock acquire
// C++ standard library headers must be included before extern "C" - they
// declare C++ types/templates that don't make sense in C linkage. The Windows
// I/O loop below uses <vector>/<chrono>/<algorithm> for its WSAPoll dispatch.
// These stay Windows-only: on Linux/macOS the runtime archive is linked into
// Dragon programs with `cc` (no libstdc++), so the non-Windows paths must be
// pure C - the R1 deadline side list uses an intrusive list + clock_gettime,
// not std::vector/std::chrono, precisely to keep that link C-only.
#ifdef _WIN32
  #include <vector>
  #include <chrono>
  #include <algorithm>
  // winsock2.h was already included by runtime_internal.h. Provide the
  // POSIX-style names we use below.
  #include <io.h>
  #define poll WSAPoll
  // ssize_t is missing on MinGW for some headers but defined here for our use.
  #ifndef _SSIZE_T_DEFINED
    typedef intptr_t ssize_t;
    #define _SSIZE_T_DEFINED
  #endif
#else
  #include <sys/socket.h>
  #include <netinet/in.h>
  #include <arpa/inet.h>
  #include <poll.h>
  #include <semaphore.h>  // sem_t / sem_timedwait for Semaphore.acquire(timeout=)
  #ifdef __linux__
    #include <sys/epoll.h>
    #include <sys/timerfd.h>
    #include <fcntl.h>
  #elif defined(__APPLE__)
    #include <sys/event.h>
    #include <fcntl.h>
  #endif
#endif

extern "C" {

// SIGPIPE suppression for socket writes. A peer that resets its connection (a
// WebSocket/HTTP client closing with unread data) makes the server's next
// send(2) raise SIGPIPE, whose default disposition kills the whole process -
// one dropped client would take the server down. We suppress it PER SEND
// (MSG_NOSIGNAL on Linux, the SO_NOSIGPIPE socket option on macOS/BSD) rather
// than ignoring SIGPIPE process-wide: a process-wide SIG_IGN is inherited
// across fork/execve into child processes (e.g. a `popen("yes | head")`
// pipeline), suppressing the SIGPIPE that those children rely on to terminate.
// Per-send suppression affects only our own socket writes; send() then returns
// EPIPE, which dragon_nb_send surfaces as -1 and the caller handles gracefully.
#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0   // not defined on macOS/BSD; SO_NOSIGPIPE covers those
#endif

typedef struct {
    pthread_t tid;
    int64_t result;
    int8_t done;    // accessed via __atomic builtins for cross-thread visibility
    int8_t joined;  // CAS'd 0->1 in join to defeat double-join race (UB + double-free)
    int8_t started; // 1 iff pthread_create succeeded; join must not touch tid when 0
} DragonThread;

typedef struct {
    DragonThread* thread;
    void* fn;
    int64_t* args;
    int64_t nargs;
} DragonFireArgs;

static void* dragon_thread_entry(void* raw) {
    DragonFireArgs* fa = (DragonFireArgs*)raw;
    int64_t res = 0;
    // Dispatch by arity - all Dragon values are 8 bytes (i64 or i8*)
    typedef int64_t (*Fn0)();
    typedef int64_t (*Fn1)(int64_t);
    typedef int64_t (*Fn2)(int64_t, int64_t);
    typedef int64_t (*Fn3)(int64_t, int64_t, int64_t);
    typedef int64_t (*Fn4)(int64_t, int64_t, int64_t, int64_t);
    typedef int64_t (*Fn5)(int64_t, int64_t, int64_t, int64_t, int64_t);
    typedef int64_t (*Fn6)(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t);
    typedef int64_t (*Fn7)(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t);
    typedef int64_t (*Fn8)(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t);
    int64_t* a = fa->args;
    switch (fa->nargs) {
        case 0: res = ((Fn0)fa->fn)(); break;
        case 1: res = ((Fn1)fa->fn)(a[0]); break;
        case 2: res = ((Fn2)fa->fn)(a[0], a[1]); break;
        case 3: res = ((Fn3)fa->fn)(a[0], a[1], a[2]); break;
        case 4: res = ((Fn4)fa->fn)(a[0], a[1], a[2], a[3]); break;
        case 5: res = ((Fn5)fa->fn)(a[0], a[1], a[2], a[3], a[4]); break;
        case 6: res = ((Fn6)fa->fn)(a[0], a[1], a[2], a[3], a[4], a[5]); break;
        case 7: res = ((Fn7)fa->fn)(a[0], a[1], a[2], a[3], a[4], a[5], a[6]); break;
        case 8: res = ((Fn8)fa->fn)(a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7]); break;
        default:
            fprintf(stderr, "fire: too many arguments (%lld max 8)\n", (long long)fa->nargs);
            break;
    }
    fa->thread->result = res;
    __atomic_store_n(&fa->thread->done, (int8_t)1, __ATOMIC_RELEASE);
    free(fa->args);
    free(fa);
    return NULL;
}

DragonThread* dragon_thread_fire(void* fn, int64_t* args, int64_t nargs) {
    DragonThread* t = (DragonThread*)malloc(sizeof(DragonThread));
    t->result = 0;
    t->done = 0;
    t->joined = 0;
    t->started = 0;
    DragonFireArgs* fa = (DragonFireArgs*)malloc(sizeof(DragonFireArgs));
    fa->thread = t;
    fa->fn = fn;
    if (nargs > 0) {
        fa->args = (int64_t*)malloc(sizeof(int64_t) * nargs);
        memcpy(fa->args, args, sizeof(int64_t) * nargs);
    } else {
        fa->args = NULL;
    }
    fa->nargs = nargs;
    dragon_gc_go_concurrent();  // a heap-mutating OS thread is starting
    // Check pthread_create: on EAGAIN (thread-limit exhaustion, the exact
    // long-running-process failure mode) the entry never runs, so t->done is
    // never set and t->tid is garbage. The scoped-fire join would then
    // pthread_join a garbage tid (undefined behavior) and block forever on
    // done, and fa / fa->args / t all leak. On failure,
    // mark the handle done with an error result and free the args so the join
    // returns immediately instead of hanging on UB.
    int rc = pthread_create(&t->tid, NULL, dragon_thread_entry, fa);
    if (rc != 0) {
        if (fa->args) free(fa->args);
        free(fa);
        t->result = 0;
        __atomic_store_n(&t->done, 1, __ATOMIC_RELEASE);
    } else {
        t->started = 1;
    }
    return t;
}

int64_t dragon_thread_is_done(DragonThread* t) {
    return __atomic_load_n(&t->done, __ATOMIC_ACQUIRE) ? 1 : 0;
}

int64_t dragon_thread_join(DragonThread* t) {
    if (!t) return 0;
    // Defeat a double-join: pthread_join twice on one tid is UB and free(t)
    // twice is a double-free. Only the CAS winner joins + frees. A Task handle
    // is single-owner in the type system (the binding annotation is mandatory
    // and rebinding is rejected), so a losing caller can only be a deliberately
    // shared still-live handle; it returns the result without touching t again.
    int8_t expected = 0;
    if (!__atomic_compare_exchange_n(&t->joined, &expected, (int8_t)1, false,
                                     __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        return __atomic_load_n(&t->result, __ATOMIC_ACQUIRE);
    }
    // Only join a thread that actually started: on a pthread_create failure
    // t->tid is garbage, and pthread_join on it is undefined. The
    // failed handle already has done=1 and result=0.
    if (t->started)
        pthread_join(t->tid, NULL);
    int64_t result = t->result;
    free(t);
    return result;
}

//===----------------------------------------------------------------------===//
// OS Thread API (manual Thread class)
//===----------------------------------------------------------------------===//

typedef struct {
    pthread_t tid;
    int64_t result;
    int8_t done;     // accessed via __atomic builtins
    int8_t started;  // CAS'd in start() to defeat double-start race
    int8_t joined;   // CAS'd in join() to defeat double-join race
    void* fn;
    int64_t* args;
    int64_t nargs;
} DragonOSThread;

// Reuse same entry function pattern as dragon_thread_entry
static void* dragon_osthread_entry(void* raw) {
    DragonOSThread* t = (DragonOSThread*)raw;
    int64_t res = 0;
    typedef int64_t (*Fn0)();
    typedef int64_t (*Fn1)(int64_t);
    typedef int64_t (*Fn2)(int64_t, int64_t);
    typedef int64_t (*Fn3)(int64_t, int64_t, int64_t);
    typedef int64_t (*Fn4)(int64_t, int64_t, int64_t, int64_t);
    int64_t* a = t->args;
    switch (t->nargs) {
        case 0: res = ((Fn0)t->fn)(); break;
        case 1: res = ((Fn1)t->fn)(a[0]); break;
        case 2: res = ((Fn2)t->fn)(a[0], a[1]); break;
        case 3: res = ((Fn3)t->fn)(a[0], a[1], a[2]); break;
        case 4: res = ((Fn4)t->fn)(a[0], a[1], a[2], a[3]); break;
        default:
            fprintf(stderr, "Thread: too many arguments (%lld max 4)\n", (long long)t->nargs);
            break;
    }
    t->result = res;
    __atomic_store_n(&t->done, (int8_t)1, __ATOMIC_RELEASE);
    return NULL;
}

/// Create an OS thread handle (does not start yet)
void* dragon_osthread_new(void* fn, int64_t* args, int64_t nargs) {
    DragonOSThread* t = (DragonOSThread*)calloc(1, sizeof(DragonOSThread));
    t->fn = fn;
    t->done = 0;
    t->started = 0;
    if (nargs > 0 && args) {
        t->args = (int64_t*)malloc(sizeof(int64_t) * nargs);
        memcpy(t->args, args, sizeof(int64_t) * nargs);
    } else {
        t->args = NULL;
    }
    t->nargs = nargs;
    return t;
}

/// Start the OS thread. CAS on `started` ensures pthread_create runs at most once
/// even under concurrent .start() calls from multiple threads.
int64_t dragon_osthread_start(void* handle) {
    DragonOSThread* t = (DragonOSThread*)handle;
    if (!t) return -1;
    int8_t expected = 0;
    if (!__atomic_compare_exchange_n(&t->started, &expected, (int8_t)1,
                                     false,
                                     __ATOMIC_ACQ_REL,
                                     __ATOMIC_ACQUIRE)) {
        return -1;  // already started by another caller
    }
    dragon_gc_go_concurrent();  // a heap-mutating OS thread is starting
    return pthread_create(&t->tid, NULL, dragon_osthread_entry, t);
}

/// Join the OS thread, return its result
int64_t dragon_osthread_join(void* handle) {
    DragonOSThread* t = (DragonOSThread*)handle;
    if (!t || !__atomic_load_n(&t->started, __ATOMIC_ACQUIRE)) return 0;
    // Double-join guard: only the CAS winner joins + frees (see
    // dragon_thread_join for the single-owner rationale).
    int8_t expected = 0;
    if (!__atomic_compare_exchange_n(&t->joined, &expected, (int8_t)1, false,
                                     __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        return __atomic_load_n(&t->result, __ATOMIC_ACQUIRE);
    }
    pthread_join(t->tid, NULL);
    int64_t result = t->result;
    free(t->args);
    free(t);
    return result;
}

/// Check if the OS thread is still running
int64_t dragon_osthread_is_alive(void* handle) {
    DragonOSThread* t = (DragonOSThread*)handle;
    if (!t) return 0;
    int8_t started = __atomic_load_n(&t->started, __ATOMIC_ACQUIRE);
    int8_t done = __atomic_load_n(&t->done, __ATOMIC_ACQUIRE);
    return (started && !done) ? 1 : 0;
}

//===----------------------------------------------------------------------===//
// Green Thread Runtime (M:N scheduling via minicoro)
//===----------------------------------------------------------------------===//

// Forward: DragonVThread was declared near exception globals (Phase 1).
// We extend it here with the coroutine handle and run-queue linkage.

// D030: Vthread args are owned by codegen (per-callsite typed struct).
// Runtime treats them as opaque - see dragon_vthread_spawn_typed below.

typedef struct {
    DragonVThread*  head;
    DragonVThread*  tail;
    pthread_mutex_t lock;
    pthread_cond_t  not_empty;
    int             shutdown;
    int             num_workers;
    pthread_t*      workers;
} DragonScheduler;

static DragonScheduler* __scheduler = NULL;
static pthread_once_t   __scheduler_once = PTHREAD_ONCE_INIT;

static void scheduler_enqueue(DragonVThread* vt) {
    pthread_mutex_lock(&__scheduler->lock);
    vt->next = NULL;
    if (__scheduler->tail) {
        __scheduler->tail->next = vt;
    } else {
        __scheduler->head = vt;
    }
    __scheduler->tail = vt;
    pthread_cond_signal(&__scheduler->not_empty);
    pthread_mutex_unlock(&__scheduler->lock);
}

// --- I/O park handshake (see DragonVThread::park_state) ---------------------
// PARK states.
#define PARK_NONE   0
#define PARK_ARMED  1
#define PARK_PARKED 2
#define PARK_FIRED  3

// Called by every scheduler-parking path (via io_post_request) right BEFORE the
// request becomes visible to the reactor, marking that this vthread is about to
// suspend for I/O. Release so the reactor thread sees a coherent view.
static inline void dragon_io_arm_park(DragonVThread* vt) {
    __atomic_store_n(&vt->park_state, PARK_ARMED, __ATOMIC_RELEASE);
}

// Called by the reactor when a watched fd/timer fires, INSTEAD of enqueuing
// directly. Enqueues iff the worker has already confirmed the coro is parked;
// otherwise it hands the enqueue duty to the worker (which will observe FIRED
// when it finishes parking). Idempotent against a spurious second fire (a
// NONE/FIRED state simply does nothing). This is the reactor half of the
// exactly-once, park-then-enqueue guarantee.
static void dragon_io_wake(DragonVThread* vt) {
    for (;;) {
        int32_t st = __atomic_load_n(&vt->park_state, __ATOMIC_ACQUIRE);
        if (st == PARK_PARKED) {
            if (__atomic_compare_exchange_n(&vt->park_state, &st, PARK_NONE,
                                            false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
                vt->yield_reason = YIELD_COOP;
                scheduler_enqueue(vt);
                return;
            }
            continue;  // st reloaded; retry
        }
        if (st == PARK_ARMED) {
            if (__atomic_compare_exchange_n(&vt->park_state, &st, PARK_FIRED,
                                            false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
                return;  // worker will enqueue when it finishes parking
            }
            continue;
        }
        return;  // NONE or FIRED: nothing to do (guards a stray double-fire)
    }
}

// Called by the worker AFTER mco_resume returns and the coro is suspended, to
// finish the park for an I/O/sleep yield. Returns true if the worker must
// enqueue the vthread now (the reactor already fired during the yield window);
// false if the vthread is safely parked and the reactor will enqueue on fire.
static bool dragon_vthread_finish_park(DragonVThread* vt) {
    int32_t expected = PARK_ARMED;
    if (__atomic_compare_exchange_n(&vt->park_state, &expected, PARK_PARKED,
                                    false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        return false;  // parked; reactor owns the enqueue
    }
    // expected == PARK_FIRED: reactor fired before we parked -> we enqueue.
    __atomic_store_n(&vt->park_state, PARK_NONE, __ATOMIC_RELEASE);
    return true;
}

static DragonVThread* scheduler_dequeue() {
    // Caller must hold __scheduler->lock
    DragonVThread* vt = __scheduler->head;
    if (vt) {
        __scheduler->head = vt->next;
        if (!__scheduler->head) __scheduler->tail = NULL;
        vt->next = NULL;
    }
    return vt;
}

// D030: Generic vthread_entry deleted. Each spawn site now generates its own
// per-callsite trampoline that knows the exact native signature of its target,
// loads args from a typed struct via direct GEPs, and stores the result via
// dragon_vthread_set_result. No i64 funneling, no nargs switch, no Fn0..Fn8.

// Drop one reference on a vthread; the last one frees the struct + coroutine
// stack + lazily-grown unwind arrays. Used for both the worker's coro ref and
// the Task handle ref (join winner / detach), so neither side needs to know if
// it is last - the atomic decrement-to-zero decides.
static void vthread_release(DragonVThread* vt) {
    if (__atomic_sub_fetch(&vt->refs, 1, __ATOMIC_ACQ_REL) != 0) return;
    mco_destroy(vt->coro);
    pthread_mutex_destroy(&vt->join_lock);
    pthread_cond_destroy(&vt->join_cond);
    free(vt->cleanup.vals);
    free(vt->cleanup.kinds);
    free(vt->cleanup.tags);
    // The exc_msg slot owns its message (dragon_exc_msg_set); a vthread that
    // caught-and-finished still holds its last message - release it.
    dragon_decref_str_dispatch(vt->exc_msg);
    free(vt);
}

// A finished (MCO_DEAD) vthread: mark done, wake any joiner, drop the coro ref -
// EXACTLY ONCE, even if processed at MCO_DEAD more than once (the I/O reactor can
// re-enqueue a vthread that finished concurrently). The done 0->1 CAS is the
// single-fire gate: the second processing CASes a no-op and returns.
static void vthread_mark_done_and_release(DragonVThread* vt) {
    int8_t expected = 0;
    if (!__atomic_compare_exchange_n(&vt->done, &expected, (int8_t)1, false,
                                     __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
        return;
    pthread_mutex_lock(&vt->join_lock);
    pthread_cond_broadcast(&vt->join_cond);
    pthread_mutex_unlock(&vt->join_lock);
    vthread_release(vt);  // drop the coro ref
}

static void* scheduler_worker(void* arg) {
    (void)arg;
    while (1) {
        pthread_mutex_lock(&__scheduler->lock);
        while (!__scheduler->head && !__scheduler->shutdown) {
            pthread_cond_wait(&__scheduler->not_empty, &__scheduler->lock);
        }
        if (__scheduler->shutdown && !__scheduler->head) {
            pthread_mutex_unlock(&__scheduler->lock);
            break;
        }
        DragonVThread* vt = scheduler_dequeue();
        pthread_mutex_unlock(&__scheduler->lock);

        if (!vt) continue;

        // Set per-worker TLS so exception functions use this vthread's state.
        // Do NOT reset vt->exc_sp here: it is initialized to -1 once at spawn
        // (dragon_vthread_spawn_typed) and must PERSIST across I/O yields. A
        // green thread that yields inside a try/except (e.g. a blocking recv)
        // is resumed through this same path; resetting exc_sp would discard its
        // live exception frames, so the eventual raise would find no handler
        // (it would print "Unhandled exception" and unwind past the try). The
        // setjmp buffers live in vt->exc_stack and stay valid across yields
        // because the coroutine keeps its own stack.
        __current_vthread = vt;

        // Only ever resume a parked (SUSPENDED) coroutine. A vthread can be
        // enqueued by the I/O reactor when a watched fd becomes ready; under a
        // rare timing the same vthread can be enqueued for a readiness that
        // resolves as it is already finishing, so a worker may dequeue one that
        // has already reached MCO_DEAD. Resuming a non-suspended coroutine is a
        // minicoro error and would corrupt the scheduler - guard it. If it is
        // DEAD, make sure any joiner is woken (idempotent with the normal
        // completion path below) before dropping it.
        if (mco_status(vt->coro) != MCO_SUSPENDED) {
            if (mco_status(vt->coro) == MCO_DEAD) {
                vthread_mark_done_and_release(vt);
            }
            __current_vthread = NULL;
            continue;
        }

        // Swap in this vthread's live-frame count for the inline cleanup gate.
        // The worker thread itself runs no Dragon try-frames between vthreads, so
        // its TLS count is 0 here; save it, install the vthread's, and on
        // yield/return save the vthread's count back (it may resume on a
        // different worker, so the count must travel with the vthread, not the
        // OS thread). Balanced with push/pop_frame inside the body.
        int __saved_active_frames = __dragon_active_frames;
        __dragon_active_frames = vt->active_frames;

        mco_resume(vt->coro);

        vt->active_frames = __dragon_active_frames;
        __dragon_active_frames = __saved_active_frames;
        __current_vthread = NULL;

        if (mco_status(vt->coro) == MCO_DEAD) {
            // Vthread finished: mark done (RELEASE, pairs with the lock-free
            // is_alive / join reads), wake any joiner, and drop the coro ref -
            // which frees the vthread if its handle was already detached/joined.
            vthread_mark_done_and_release(vt);
        } else if (__atomic_load_n(&vt->park_state, __ATOMIC_ACQUIRE) == PARK_NONE) {
            // Cooperative yield (never armed for I/O) - re-enqueue immediately.
            // Keyed on park_state, NOT vt->yield_reason: the reactor overwrites
            // yield_reason to YIELD_COOP when it fires, so a yield_reason test
            // here raced with the reactor and could re-enqueue an I/O-parked
            // vthread the reactor had ALSO enqueued (the double-enqueue half of
            // the armed-before-yield bug).
            scheduler_enqueue(vt);
        } else {
            // I/O / sleep park. Complete the handshake now that the coro is
            // suspended. If the reactor already fired during the yield window,
            // WE enqueue (it deferred to us); otherwise the vthread is parked
            // and the reactor will enqueue exactly once when the fd/timer fires.
            if (dragon_vthread_finish_park(vt)) {
                scheduler_enqueue(vt);
            }
        }
    }
    return NULL;
}

static void scheduler_init() {
    // Scheduler workers are heap-mutating OS threads - switch GC track/untrack/
    // decref onto the locked path from now on (see gc_concurrent).
    dragon_gc_go_concurrent();
    __scheduler = (DragonScheduler*)calloc(1, sizeof(DragonScheduler));
    pthread_mutex_init(&__scheduler->lock, NULL);
    pthread_cond_init(&__scheduler->not_empty, NULL);
    __scheduler->head = NULL;
    __scheduler->tail = NULL;
    __scheduler->shutdown = 0;

    // Worker count: number of CPU cores, minimum 2
#ifdef _WIN32
    SYSTEM_INFO si; GetSystemInfo(&si);
    long ncpu = (long)si.dwNumberOfProcessors;
#else
    long ncpu = sysconf(_SC_NPROCESSORS_ONLN);
#endif
    if (ncpu < 2) ncpu = 2;
    const char* env = getenv("DRAGON_WORKER_THREADS");
    if (env) {
        long n = atol(env);
        if (n > 0) ncpu = n;
    }
    __scheduler->num_workers = (int)ncpu;
    __scheduler->workers = (pthread_t*)calloc(ncpu, sizeof(pthread_t));
    for (int i = 0; i < __scheduler->num_workers; i++) {
        pthread_create(&__scheduler->workers[i], NULL, scheduler_worker, NULL);
    }
}

/// D030: Spawn a green thread with a per-callsite typed args struct.
///   trampoline: codegen-synthesized; receives mco_coro* and unpacks args itself
///   args:       pointer to a stack-allocated typed args struct at the call site
///               (field 0 reserved for the DragonVThread*, patched here)
///   args_size:  byte size of the struct for malloc + memcpy (0 if no args)
/// The trampoline owns the heap copy and is responsible for free()ing it
/// before returning; dragon_vthread_join never touches args.
DragonVThread* dragon_vthread_spawn_typed(
    void (*trampoline)(mco_coro*), void* args, int64_t args_size) {
    pthread_once(&__scheduler_once, scheduler_init);

    DragonVThread* vt = (DragonVThread*)calloc(1, sizeof(DragonVThread));
    vt->exc_sp = -1;
    vt->done = 0;
    vt->yield_reason = YIELD_COOP;
    vt->result = 0;
    vt->next = NULL;
    vt->refs = 2;  // coro ref (worker drops on MCO_DEAD) + handle ref (join/detach)
    pthread_mutex_init(&vt->join_lock, NULL);
    pthread_cond_init(&vt->join_cond, NULL);

    // Heap-copy the codegen-built args struct so it outlives the spawn call.
    void* heap_args = NULL;
    if (args_size > 0 && args) {
        heap_args = malloc((size_t)args_size);
        memcpy(heap_args, args, (size_t)args_size);
        // Patch field 0 (DragonVThread*) so the trampoline can store the result.
        *(DragonVThread**)heap_args = vt;
    }

    mco_desc desc = mco_desc_init(trampoline, 0); // default stack (~2MB mmap, guarded - see MCO_USE_VMEM_ALLOCATOR)
    desc.user_data = heap_args;
    mco_result r = mco_create(&vt->coro, &desc);
    if (r != MCO_SUCCESS) {
        fprintf(stderr, "fire: failed to create green thread: %s\n", mco_result_description(r));
        if (heap_args) free(heap_args);
        free(vt);
        return NULL;
    }

    scheduler_enqueue(vt);
    return vt;
}

/// D030: Trampoline-side helper to record the green thread's return value.
/// Codegen emits this call inside the per-callsite trampoline after the
/// target returns. The store happens-before the worker's release-store of
/// vt->done (in scheduler_worker), so join's acquire-load sees a coherent
/// result without further sync.
void dragon_vthread_set_result(DragonVThread* vt, int64_t res) {
    if (vt) vt->result = res;
}

/// Join a green thread: block until done, return result, free resources.
/// D030: args buffer is owned by the per-callsite trampoline (free'd before
/// the coroutine returns) - join must never free args.
int64_t dragon_vthread_join(DragonVThread* vt) {
    if (!vt) return 0;

    // Double-join guard: only the CAS winner destroys + frees. A losing caller
    // must still see a completed result, so block on done first, then return
    // the cached result without touching the (possibly-freed) handle's owned
    // resources. See the `joined` field comment for the ownership rationale.
    int8_t expected = 0;
    bool winner = __atomic_compare_exchange_n(&vt->joined, &expected, (int8_t)1,
                                              false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);

    // Block until the vthread finishes
    pthread_mutex_lock(&vt->join_lock);
    while (!vt->done) {
        pthread_cond_wait(&vt->join_cond, &vt->join_lock);
    }
    pthread_mutex_unlock(&vt->join_lock);

    int64_t result = vt->result;
    // The CAS winner owns the handle ref and drops it (frees iff the coro is
    // also done). A losing caller (a deliberately-shared still-live handle)
    // holds no ref and must not release - it just returns the cached result.
    if (winner) vthread_release(vt);
    return result;
}

/// Detach a fire-and-forget vthread: a `fire fn()` / `fire { ... }` whose Task
/// handle is discarded (never awaited / joined). Claim the handle via the same
/// CAS join uses (so a stray later join is a no-op loser) and drop the handle
/// ref. The worker drops the coro ref on MCO_DEAD; the last release frees the
/// struct + its ~2MB coroutine stack - closing the fire-and-forget leak (#2).
void dragon_vthread_detach(DragonVThread* vt) {
    if (!vt) return;
    int8_t expected = 0;
    if (__atomic_compare_exchange_n(&vt->joined, &expected, (int8_t)1, false,
                                    __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
        vthread_release(vt);  // drop the handle ref
}

/// Check if a green thread is still running
int64_t dragon_vthread_is_alive(DragonVThread* vt) {
    if (!vt) return 0;
    return !__atomic_load_n(&vt->done, __ATOMIC_ACQUIRE) ? 1 : 0;
}

/// Reclaim a generator whose body raised an exception that propagated out.
///
/// Dragon's setjmp/longjmp exception model longjmps DIRECTLY out of the
/// generator's mco_resume (it does not unwind back through mco_yield), so the
/// coroutine is abandoned mid-run: it stays MCO_RUNNING and minicoro's
/// per-thread running-coroutine pointer (`mco_current_co`) is left dangling at
/// it - which both prevents mco_destroy from reclaiming the ~12KB coroutine
/// stack AND would corrupt the next resume on this thread. This undoes
/// _mco_prepare_jumpin's bookkeeping (restoring the resumer as the running
/// coroutine) and marks the abandoned coroutine MCO_DEAD so a subsequent
/// dragon_generator_destroy reclaims it cleanly. Idempotent / no-op unless the
/// coroutine is actually MCO_RUNNING. Must run on the same thread, at the
/// longjmp-arrival site, BEFORE any further coroutine is resumed.
///
/// Only the MINICORO_IMPL translation unit (this file) can see mco_current_co,
/// so this lives here rather than alongside dragon_generator_destroy.
/// The generator body's own owned heap locals are NOT freed here - they were
/// pushed onto the consumer's unwind cleanup stack and are freed by the
/// consumer's dragon_exc_cleanup_unwind at the same longjmp arrival.
void dragon_generator_abandon(void* gen_ptr) {
    DragonGenerator* gen = (DragonGenerator*)gen_ptr;
    if (!gen || !gen->coro) return;
    mco_coro* co = gen->coro;
    if (mco_status(co) != MCO_RUNNING) return;  // only the abandoned-mid-resume case
    mco_coro* resumer = co->prev_co;             // who called mco_resume(co)
    if (resumer) resumer->state = MCO_RUNNING;   // _mco_prepare_jumpin had set it NORMAL
    mco_current_co = resumer;                    // restore the running coroutine
    co->prev_co = NULL;
    co->state = MCO_DEAD;                        // now reclaimable by mco_destroy
}

//===----------------------------------------------------------------------===//
// Platform I/O Event Loop - dedicated thread for non-blocking I/O + timers
// Replaces libuv with raw epoll (Linux) / kqueue (macOS)
//===----------------------------------------------------------------------===//

// I/O event types for the event loop
enum IoEventType {
    IO_EVENT_FD_READ  = 1,   // fd is readable
    IO_EVENT_FD_WRITE = 2,   // fd is writable
    IO_EVENT_TIMER    = 3    // timer expired
};

// Pending I/O request - links a vthread to an fd or timer
typedef struct IoRequest {
    DragonVThread*    vt;
    int               fd;        // -1 for timer-only requests
    int               event_type;
    // For IO_EVENT_TIMER: the sleep duration in ms. For a deadline-bearing fd
    // watch (R1 recv timeout): the timeout in ms - the reactor converts it to an
    // absolute deadline. 0 on a plain fd watch means "no timeout, wait forever".
    int64_t           timer_ms;
    // Absolute monotonic deadline (ms) computed by the reactor when it registers
    // a deadline-bearing fd watch; 0 = none. Lets the reactor fire the watch on
    // timeout even if the fd never becomes ready (slowloris defense).
    int64_t           deadline_ms;
    struct IoRequest* next;     // pending-queue link (producer → reactor)
    // Intrusive link for the reactor's deadline side list (R1, non-Windows). A
    // plain pointer list instead of std::vector keeps the Linux/macOS runtime
    // free of libstdc++ so Dragon programs link it with `cc`. NULL when the
    // request is not on the deadline list.
    struct IoRequest* dl_next;
} IoRequest;

// Platform event loop state
static int            __io_epfd = -1;       // epoll/kqueue fd
#ifdef _WIN32
// Windows uses a self-pipe over a UDP socket pair (real OS pipe handles can't
// be polled by WSAPoll). Both ends are SOCKETs.
static SOCKET         __io_wakeup_pipe[2] = { INVALID_SOCKET, INVALID_SOCKET };
#else
static int            __io_wakeup_pipe[2];  // pipe to wake event loop from other threads
#endif
static pthread_t      __io_thread;
static pthread_once_t __io_once = PTHREAD_ONCE_INIT;
static volatile int   __io_shutdown = 0;

// Pending request queue - producers (worker threads) enqueue, I/O thread dequeues
static IoRequest*       __io_pending_head = NULL;
static pthread_mutex_t  __io_pending_lock = PTHREAD_MUTEX_INITIALIZER;

#ifdef _WIN32
// Initialize Winsock once for the whole runtime. Idempotent.
static void win_wsa_startup_once() {
    static int wsa_init = 0;
    static pthread_mutex_t wsa_lock = PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_lock(&wsa_lock);
    if (!wsa_init) {
        WSADATA wsa;
        WSAStartup(MAKEWORD(2, 2), &wsa);
        wsa_init = 1;
    }
    pthread_mutex_unlock(&wsa_lock);
}

// Public alias so other runtime TUs can ensure Winsock is up before any
// socket call.
void dragon_win_wsa_startup(void) { win_wsa_startup_once(); }

// Build a localhost UDP socket pair to use as a wakeup mechanism for WSAPoll.
// Returns 0 on success, -1 on failure.
static int win_make_socketpair(SOCKET out[2]) {
    out[0] = out[1] = INVALID_SOCKET;
    SOCKET listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listener == INVALID_SOCKET) return -1;
    struct sockaddr_in addr; memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    int alen = sizeof(addr);
    if (bind(listener, (struct sockaddr*)&addr, alen) == SOCKET_ERROR ||
        getsockname(listener, (struct sockaddr*)&addr, &alen) == SOCKET_ERROR ||
        listen(listener, 1) == SOCKET_ERROR) {
        closesocket(listener);
        return -1;
    }
    SOCKET cli = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (cli == INVALID_SOCKET) { closesocket(listener); return -1; }
    if (connect(cli, (struct sockaddr*)&addr, alen) == SOCKET_ERROR) {
        closesocket(cli); closesocket(listener); return -1;
    }
    SOCKET srv = accept(listener, NULL, NULL);
    closesocket(listener);
    if (srv == INVALID_SOCKET) { closesocket(cli); return -1; }
    out[0] = srv; out[1] = cli;
    return 0;
}
#endif

static void io_post_request(IoRequest* req) {
    // Arm the park BEFORE the request becomes visible to the reactor. Every
    // scheduler-parking path (fd watch, deadline watch, sleep) funnels through
    // here immediately before its mco_yield, so this is the one place that
    // marks "this vthread is about to suspend for I/O" for the park handshake.
    if (req->vt) dragon_io_arm_park(req->vt);
    pthread_mutex_lock(&__io_pending_lock);
    req->next = __io_pending_head;
    __io_pending_head = req;
    pthread_mutex_unlock(&__io_pending_lock);
    // Wake the event loop so it picks up the new request
    char c = 1;
#ifdef _WIN32
    (void)send(__io_wakeup_pipe[1], &c, 1, 0);
#else
    (void)write(__io_wakeup_pipe[1], &c, 1);
#endif
}

static IoRequest* io_drain_pending() {
    pthread_mutex_lock(&__io_pending_lock);
    IoRequest* list = __io_pending_head;
    __io_pending_head = NULL;
    pthread_mutex_unlock(&__io_pending_lock);
    return list;
}

#ifndef _WIN32
// --- Deadline-bearing fd watches (R1 recv timeout), epoll/kqueue platforms ---
//
// A normal fd watch parks a vthread until the fd is ready (or forever). A
// deadline-bearing watch additionally fires on a timeout, so a peer that goes
// silent mid-read can't pin the green thread (slowloris defense). epoll/kqueue
// have no per-fd timeout, so the reactor tracks these in a side list and scans
// it each loop tick, shortening the wait to the soonest deadline. Touched ONLY
// on the single reactor thread (io_process_pending + io_thread_entry both run
// there), so no lock is needed.
//
// The list is an INTRUSIVE singly-linked list (IoRequest.dl_next), not a
// std::vector: the Linux/macOS runtime archive is linked into Dragon programs
// with `cc`, so it must not pull in libstdc++. Windows uses its own __io_active
// (std::vector) instead - it links with a C++ toolchain.
static IoRequest* __io_deadline_head = NULL;

// Monotonic clock in ms - steady so a wall-clock jump can't skew a deadline.
static long long io_now_ms() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + (long long)ts.tv_nsec / 1000000;
}

// Push a deadline-bearing request onto the side list (front insert).
static void io_deadline_add(IoRequest* req) {
    req->dl_next = __io_deadline_head;
    __io_deadline_head = req;
}

// Unlink a resolved request from the deadline side list (by pointer).
static void io_deadline_remove(IoRequest* req) {
    IoRequest** pp = &__io_deadline_head;
    while (*pp) {
        if (*pp == req) {
            *pp = req->dl_next;
            req->dl_next = NULL;
            return;
        }
        pp = &(*pp)->dl_next;
    }
}

// Soonest deadline minus now, clamped to [0, cap] - the epoll/kqueue wait budget
// so a pending timeout fires promptly instead of waiting out the full tick.
static int io_deadline_wait_ms(int cap) {
    long long now = io_now_ms();
    int budget = cap;
    for (IoRequest* r = __io_deadline_head; r; r = r->dl_next) {
        long long d = r->deadline_ms - now;
        if (d < 0) d = 0;
        if ((int)d < budget) budget = (int)d;
    }
    return budget;
}
#endif // !_WIN32

#ifdef __linux__
// --- Linux: epoll + timerfd ---

static void io_process_pending() {
    IoRequest* req = io_drain_pending();
    while (req) {
        IoRequest* next = req->next;
        if (req->event_type == IO_EVENT_TIMER) {
            // Timer: create a timerfd, register with epoll
            int tfd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
            struct itimerspec its = {};
            int64_t ms = req->timer_ms;
            its.it_value.tv_sec  = ms / 1000;
            its.it_value.tv_nsec = (ms % 1000) * 1000000;
            timerfd_settime(tfd, 0, &its, NULL);
            req->fd = tfd; // store actual timerfd for unregistration
            struct epoll_event ev = {};
            ev.events = EPOLLIN | EPOLLONESHOT;
            ev.data.ptr = req;
            epoll_ctl(__io_epfd, EPOLL_CTL_ADD, tfd, &ev);
        } else {
            // Socket fd: register for read or write
            struct epoll_event ev = {};
            ev.events = (req->event_type == IO_EVENT_FD_READ ? EPOLLIN : EPOLLOUT)
                        | EPOLLONESHOT;
            ev.data.ptr = req;
            epoll_ctl(__io_epfd, EPOLL_CTL_ADD, req->fd, &ev);
            // Deadline-bearing watch (R1): track it so the loop can fire it on
            // timeout even if the fd never becomes ready.
            if (req->timer_ms > 0) {
                req->deadline_ms = io_now_ms() + req->timer_ms;
                io_deadline_add(req);
            }
        }
        req = next;
    }
}

static void* io_thread_entry(void*) {
    struct epoll_event events[64];
    while (!__io_shutdown) {
        // Wait at most until the soonest deadline (capped at the 100ms baseline
        // tick) so a recv timeout fires promptly rather than up to 100ms late.
        int wait_ms = io_deadline_wait_ms(100);
        int n = epoll_wait(__io_epfd, events, 64, wait_ms);
        for (int i = 0; i < n; i++) {
            IoRequest* req = (IoRequest*)events[i].data.ptr;
            if (req->fd == __io_wakeup_pipe[0]) {
                // Drain the wakeup pipe
                char buf[64];
                (void)read(__io_wakeup_pipe[0], buf, sizeof(buf));
            } else {
                // Timer or socket ready - remove from epoll and re-enqueue vthread
                epoll_ctl(__io_epfd, EPOLL_CTL_DEL, req->fd, NULL);
                if (req->event_type == IO_EVENT_TIMER) {
                    close(req->fd); // close timerfd
                } else if (req->timer_ms > 0) {
                    // Resolved by readiness before its deadline - drop the
                    // side-list entry so the expiry scan won't double-fire it.
                    io_deadline_remove(req);
                }
                // Park handshake: enqueue via dragon_io_wake, which enqueues only
                // once the coro is confirmed suspended (or hands off to the
                // parking worker). Direct scheduler_enqueue here raced the yield.
                DragonVThread* wv = req->vt;
                free(req);
                dragon_io_wake(wv);
            }
        }
        // Fire any deadline-bearing fd watches whose timeout has elapsed. The fd
        // never became ready in time: unregister it, flag the timeout, resume.
        if (__io_deadline_head) {
            long long now = io_now_ms();
            IoRequest** pp = &__io_deadline_head;
            while (*pp) {
                IoRequest* req = *pp;
                if (req->deadline_ms <= now) {
                    *pp = req->dl_next;            // unlink before freeing
                    epoll_ctl(__io_epfd, EPOLL_CTL_DEL, req->fd, NULL);
                    req->vt->io_timed_out = 1;
                    DragonVThread* wv = req->vt;
                    free(req);
                    dragon_io_wake(wv);
                } else {
                    pp = &req->dl_next;
                }
            }
        }
        // Process any new pending requests
        io_process_pending();
    }
    return NULL;
}

static void io_init() {
    __io_epfd = epoll_create1(EPOLL_CLOEXEC);
    pipe(__io_wakeup_pipe);
    // Make read end non-blocking
    fcntl(__io_wakeup_pipe[0], F_SETFL, O_NONBLOCK);
    // Register wakeup pipe with epoll
    struct epoll_event ev = {};
    ev.events = EPOLLIN;
    // Use a sentinel IoRequest for the wakeup pipe
    static IoRequest wakeup_sentinel = {NULL, 0, 0, 0, 0, NULL, NULL};
    wakeup_sentinel.fd = __io_wakeup_pipe[0];
    ev.data.ptr = &wakeup_sentinel;
    epoll_ctl(__io_epfd, EPOLL_CTL_ADD, __io_wakeup_pipe[0], &ev);
    dragon_gc_go_concurrent();  // I/O reactor is a separate OS thread
    pthread_create(&__io_thread, NULL, io_thread_entry, NULL);
}

#elif defined(__APPLE__)
// --- macOS: kqueue + EVFILT_TIMER ---

// On macOS we use a unique ident for each timer to avoid collisions
static volatile int64_t __kqueue_timer_id = 1;

static void io_process_pending() {
    IoRequest* req = io_drain_pending();
    while (req) {
        IoRequest* next = req->next;
        struct kevent kev;
        if (req->event_type == IO_EVENT_TIMER) {
            int64_t ms = req->timer_ms;
            uintptr_t timer_id = (uintptr_t)__sync_fetch_and_add(&__kqueue_timer_id, 1);
            req->fd = -1; // no real fd for kqueue timer
            EV_SET(&kev, timer_id, EVFILT_TIMER, EV_ADD | EV_ONESHOT,
                   NOTE_USECONDS, ms * 1000, req);
            kevent(__io_epfd, &kev, 1, NULL, 0, NULL);
        } else {
            int16_t filter = (req->event_type == IO_EVENT_FD_READ)
                             ? EVFILT_READ : EVFILT_WRITE;
            EV_SET(&kev, req->fd, filter, EV_ADD | EV_ONESHOT, 0, 0, req);
            kevent(__io_epfd, &kev, 1, NULL, 0, NULL);
            // Deadline-bearing watch (R1): track for timeout firing.
            if (req->timer_ms > 0) {
                req->deadline_ms = io_now_ms() + req->timer_ms;
                io_deadline_add(req);
            }
        }
        req = next;
    }
}

static void* io_thread_entry(void*) {
    struct kevent events[64];
    while (!__io_shutdown) {
        // Wait at most until the soonest deadline (capped at the 100ms tick).
        int wait_ms = io_deadline_wait_ms(100);
        struct timespec timeout = {wait_ms / 1000, (long)(wait_ms % 1000) * 1000000};
        int n = kevent(__io_epfd, NULL, 0, events, 64, &timeout);
        for (int i = 0; i < n; i++) {
            if ((int)events[i].ident == __io_wakeup_pipe[0]) {
                char buf[64];
                (void)read(__io_wakeup_pipe[0], buf, sizeof(buf));
            } else {
                IoRequest* req = (IoRequest*)events[i].udata;
                if (req->event_type != IO_EVENT_TIMER && req->timer_ms > 0) {
                    // Resolved by readiness before its deadline.
                    io_deadline_remove(req);
                }
                // Park handshake (see dragon_io_wake) - enqueue only once the
                // coro is confirmed parked. Was a direct scheduler_enqueue that
                // raced the yield.
                DragonVThread* wv = req->vt;
                free(req);
                dragon_io_wake(wv);
            }
        }
        // Fire deadline-bearing fd watches whose timeout elapsed: cancel the
        // kqueue filter, flag the timeout, resume the vthread.
        if (__io_deadline_head) {
            long long now = io_now_ms();
            IoRequest** pp = &__io_deadline_head;
            while (*pp) {
                IoRequest* req = *pp;
                if (req->deadline_ms <= now) {
                    *pp = req->dl_next;            // unlink before freeing
                    struct kevent dk;
                    int16_t filter = (req->event_type == IO_EVENT_FD_READ)
                                     ? EVFILT_READ : EVFILT_WRITE;
                    EV_SET(&dk, req->fd, filter, EV_DELETE, 0, 0, NULL);
                    kevent(__io_epfd, &dk, 1, NULL, 0, NULL);
                    req->vt->io_timed_out = 1;
                    DragonVThread* wv = req->vt;
                    free(req);
                    dragon_io_wake(wv);
                } else {
                    pp = &req->dl_next;
                }
            }
        }
        io_process_pending();
    }
    return NULL;
}

static void io_init() {
    __io_epfd = kqueue();
    pipe(__io_wakeup_pipe);
    fcntl(__io_wakeup_pipe[0], F_SETFL, O_NONBLOCK);
    // Register wakeup pipe with kqueue
    struct kevent kev;
    EV_SET(&kev, __io_wakeup_pipe[0], EVFILT_READ, EV_ADD, 0, 0, NULL);
    kevent(__io_epfd, &kev, 1, NULL, 0, NULL);
    dragon_gc_go_concurrent();  // I/O reactor is a separate OS thread
    pthread_create(&__io_thread, NULL, io_thread_entry, NULL);
}

#elif defined(_WIN32)
// --- Windows: WSAPoll + per-tick deadline scan for timers ---
//
// Windows has no epoll / kqueue / timerfd. WSAPoll is the modern equivalent of
// poll(2) for sockets and is available on Vista+. We track active fd watches
// and timer deadlines in two plain arrays under __io_pending_lock; each event
// loop tick we (1) snapshot pending requests, (2) build a pollfd array, (3)
// wait with the next timer's relative deadline, then (4) re-enqueue any
// fired/ready vthreads.
//
// This is good enough for v0.0.1 Windows preview. Performance is O(n) per
// tick rather than O(1) like epoll - switch to IOCP if it becomes a bottleneck.

// File-local C++ helper types. extern "C" controls linkage of declared
// functions, not the use of C++ types inside function bodies.
typedef struct WinIoEntry {
    IoRequest* req;
    long long deadline_ms;  // monotonic time at which timer fires (0 for fd watches)
} WinIoEntry;

static std::vector<WinIoEntry> __io_active;
static pthread_mutex_t __io_active_lock = PTHREAD_MUTEX_INITIALIZER;

static long long win_now_ms() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(
        steady_clock::now().time_since_epoch()).count();
}

static void io_process_pending() {
    IoRequest* req = io_drain_pending();
    long long now = win_now_ms();
    pthread_mutex_lock(&__io_active_lock);
    while (req) {
        IoRequest* next = req->next;
        WinIoEntry e{};
        e.req = req;
        // A timer, OR a deadline-bearing fd watch (R1 recv timeout: an fd watch
        // with timer_ms > 0), carries an absolute deadline; a plain fd watch has
        // none (waits forever for readiness).
        e.deadline_ms = (req->event_type == IO_EVENT_TIMER || req->timer_ms > 0)
                            ? (now + req->timer_ms) : 0;
        __io_active.push_back(e);
        req = next;
    }
    pthread_mutex_unlock(&__io_active_lock);
}

static void* io_thread_entry(void*) {
    while (!__io_shutdown) {
        io_process_pending();

        // Build pollfd array from active fd-watch entries; compute the next
        // timer deadline so we can size the WSAPoll timeout.
        long long now = win_now_ms();
        long long next_deadline = -1;
        std::vector<WSAPOLLFD> pfds;
        std::vector<size_t>    pfd_to_idx;

        pthread_mutex_lock(&__io_active_lock);
        // Always poll the wakeup socket.
        WSAPOLLFD wake = {};
        wake.fd = __io_wakeup_pipe[0];
        wake.events = POLLRDNORM;
        pfds.push_back(wake);
        pfd_to_idx.push_back((size_t)-1); // sentinel for wakeup

        for (size_t i = 0; i < __io_active.size(); i++) {
            auto& e = __io_active[i];
            if (e.req->event_type == IO_EVENT_TIMER) {
                if (next_deadline < 0 || e.deadline_ms < next_deadline)
                    next_deadline = e.deadline_ms;
            } else {
                WSAPOLLFD pf = {};
                pf.fd = (SOCKET)e.req->fd;
                pf.events = (e.req->event_type == IO_EVENT_FD_READ)
                                ? POLLRDNORM : POLLWRNORM;
                pfds.push_back(pf);
                pfd_to_idx.push_back(i);
                // A deadline-bearing fd watch also bounds the poll wait so its
                // timeout fires promptly even if the fd never becomes ready.
                if (e.deadline_ms > 0 &&
                    (next_deadline < 0 || e.deadline_ms < next_deadline))
                    next_deadline = e.deadline_ms;
            }
        }
        pthread_mutex_unlock(&__io_active_lock);

        int timeout_ms = 100; // baseline tick to allow shutdown checks
        if (next_deadline >= 0) {
            long long delta = next_deadline - now;
            if (delta < 0) delta = 0;
            if (delta < timeout_ms) timeout_ms = (int)delta;
        }

        int n = WSAPoll(pfds.data(), (ULONG)pfds.size(), timeout_ms);

        // Drain wakeup socket
        if (n > 0 && (pfds[0].revents & (POLLRDNORM | POLLERR | POLLHUP))) {
            char buf[64];
            (void)recv(__io_wakeup_pipe[0], buf, sizeof(buf), 0);
        }

        // Re-enqueue ready fd watches
        std::vector<size_t> to_remove;
        if (n > 0) {
            pthread_mutex_lock(&__io_active_lock);
            for (size_t k = 1; k < pfds.size(); k++) {
                if (pfds[k].revents == 0) continue;
                size_t idx = pfd_to_idx[k];
                if (idx >= __io_active.size()) continue;
                IoRequest* req = __io_active[idx].req;
                if (!req) continue;            // already taken this tick
                req->vt->io_timed_out = 0;     // resolved by readiness, not timeout
                // Park handshake (see dragon_io_wake): enqueue only once the
                // coro is confirmed parked. Was a direct enqueue that raced the yield.
                DragonVThread* wv = req->vt;
                free(req);
                __io_active[idx].req = nullptr; // tombstone so the expiry scan skips it
                to_remove.push_back(idx);
                dragon_io_wake(wv);
            }
            pthread_mutex_unlock(&__io_active_lock);
        }

        // Fire expired deadlines: sleep timers AND deadline-bearing fd watches
        // whose recv timeout elapsed before the fd became ready.
        now = win_now_ms();
        pthread_mutex_lock(&__io_active_lock);
        for (size_t i = 0; i < __io_active.size(); i++) {
            auto& e = __io_active[i];
            if (!e.req) continue;              // resolved by readiness above
            if (e.deadline_ms > 0 && e.deadline_ms <= now) {
                if (e.req->event_type != IO_EVENT_TIMER)
                    e.req->vt->io_timed_out = 1;   // fd watch timed out (R1)
                DragonVThread* wv = e.req->vt;
                free(e.req);
                e.req = nullptr;
                to_remove.push_back(i);
                dragon_io_wake(wv);
            }
        }
        // Remove fired/ready entries - sort descending so erase doesn't shift.
        std::sort(to_remove.begin(), to_remove.end(),
                  [](size_t a, size_t b) { return a > b; });
        size_t prev = (size_t)-1;
        for (size_t i : to_remove) {
            if (i == prev) continue;            // dedupe
            if (i < __io_active.size()) __io_active.erase(__io_active.begin() + i);
            prev = i;
        }
        pthread_mutex_unlock(&__io_active_lock);
    }
    return NULL;
}

static void io_init() {
    win_wsa_startup_once();
    if (win_make_socketpair(__io_wakeup_pipe) != 0) {
        fprintf(stderr, "dragon: failed to create wakeup socketpair\n");
        return;
    }
    // Make read end non-blocking
    u_long nb = 1;
    ioctlsocket(__io_wakeup_pipe[0], FIONBIO, &nb);
    dragon_gc_go_concurrent();  // I/O reactor is a separate OS thread
    pthread_create(&__io_thread, NULL, io_thread_entry, NULL);
}

#endif // platform

// --- Public API: watch an fd for readability/writability (used by nb_recv/nb_send) ---

void dragon_io_watch_fd(int fd, int event_type, DragonVThread* vt) {
    pthread_once(&__io_once, io_init);
    IoRequest* req = (IoRequest*)malloc(sizeof(IoRequest));
    req->vt = vt;
    req->fd = fd;
    req->event_type = event_type;
    req->timer_ms = 0;
    req->deadline_ms = 0;
    req->next = NULL;
    req->dl_next = NULL;
    vt->yield_reason = YIELD_IO;
    io_post_request(req);
}

// Watch `fd` for readiness with a timeout (R1). Like dragon_io_watch_fd, but the
// reactor also fires the watch after `timeout_ms` if the fd never becomes ready,
// setting vt->io_timed_out = 1 so the waiter can distinguish "timed out" from
// "ready" after it resumes. `timeout_ms <= 0` degrades to a plain forever watch.
void dragon_io_watch_fd_deadline(int fd, int event_type, DragonVThread* vt,
                                 int64_t timeout_ms) {
    pthread_once(&__io_once, io_init);
    IoRequest* req = (IoRequest*)malloc(sizeof(IoRequest));
    req->vt = vt;
    req->fd = fd;
    req->event_type = event_type;
    req->timer_ms = timeout_ms > 0 ? timeout_ms : 0;  // reactor → absolute deadline
    req->deadline_ms = 0;
    req->next = NULL;
    req->dl_next = NULL;
    vt->yield_reason = YIELD_IO;
    io_post_request(req);
}

// --- Timer-based sleep: yields vthread, resumes after ms milliseconds ---

/// Sleep for `ms` milliseconds - yields the current green thread
void dragon_vthread_sleep(int64_t ms) {
    pthread_once(&__io_once, io_init);

    DragonVThread* vt = __current_vthread;
    if (!vt || !vt->coro) {
        // Not inside a green thread - use blocking sleep as fallback
#ifdef _WIN32
        Sleep((DWORD)ms);
#else
        usleep((useconds_t)(ms * 1000));
#endif
        return;
    }

    // Post a timer request to the I/O thread
    IoRequest* req = (IoRequest*)malloc(sizeof(IoRequest));
    req->vt = vt;
    req->fd = -1;
    req->event_type = IO_EVENT_TIMER;
    req->timer_ms = ms;
    req->deadline_ms = 0;
    req->next = NULL;
    req->dl_next = NULL;
    vt->yield_reason = YIELD_SLEEP;
    io_post_request(req);

    // Yield - I/O thread will re-enqueue when timer expires
    mco_yield(vt->coro);
}

/// Yield the current green thread explicitly (cooperative scheduling)
void dragon_vthread_yield() {
    DragonVThread* vt = __current_vthread;
    if (vt && vt->coro) {
        mco_yield(vt->coro);
    }
}

//===----------------------------------------------------------------------===//
// Lock Functions
//===----------------------------------------------------------------------===//

void* dragon_lock_new() {
    pthread_mutex_t* m = (pthread_mutex_t*)malloc(sizeof(pthread_mutex_t));
    pthread_mutex_init(m, NULL);
    return m;
}

void dragon_lock_acquire(void* lock) {
    pthread_mutex_lock((pthread_mutex_t*)lock);
}

// Python threading.Lock.acquire(blocking, timeout) shape. Returns 1 if the
// lock was acquired, 0 otherwise:
//   blocking == 0           -> try once (timeout ignored, as in CPython)
//   blocking, timeout < 0   -> wait forever, always succeeds (returns 1)
//   blocking, timeout >= 0  -> wait up to `timeout` seconds, 0 on timeout
// Used for runtime/dynamic blocking|timeout; codegen fast-paths the no-arg
// (always-blocking, no-timeout) case straight to dragon_lock_acquire.
int64_t dragon_lock_acquire_ex(void* lock, int64_t blocking, double timeout) {
    pthread_mutex_t* m = (pthread_mutex_t*)lock;
    if (!blocking) {
        return pthread_mutex_trylock(m) == 0 ? 1 : 0;
    }
    if (timeout < 0) {
        pthread_mutex_lock(m);
        return 1;
    }
    // Absolute deadline = now + timeout (CLOCK_REALTIME, what timedlock uses).
    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    int64_t whole = (int64_t)timeout;
    deadline.tv_sec += (time_t)whole;
    deadline.tv_nsec += (long)((timeout - (double)whole) * 1e9);
    if (deadline.tv_nsec >= 1000000000L) {
        deadline.tv_sec += 1;
        deadline.tv_nsec -= 1000000000L;
    }
#if defined(__APPLE__)
    // macOS has no pthread_mutex_timedlock - poll with trylock until deadline.
    for (;;) {
        if (pthread_mutex_trylock(m) == 0) return 1;
        struct timespec now;
        clock_gettime(CLOCK_REALTIME, &now);
        if (now.tv_sec > deadline.tv_sec ||
            (now.tv_sec == deadline.tv_sec && now.tv_nsec >= deadline.tv_nsec))
            return 0;
        struct timespec nap = {0, 500000};  // 0.5 ms
        nanosleep(&nap, nullptr);
    }
#else
    return pthread_mutex_timedlock(m, &deadline) == 0 ? 1 : 0;
#endif
}

int64_t dragon_lock_try_acquire(void* lock) {
    return pthread_mutex_trylock((pthread_mutex_t*)lock) == 0 ? 1 : 0;
}

//===----------------------------------------------------------------------===//
// Timed acquire for RWLock / Semaphore - gives them the same
// acquire(blocking=True, timeout=T) shape as Lock. Build an absolute
// CLOCK_REALTIME deadline = now + `seconds` (what the POSIX timed primitives
// expect), then call the timed-wait variant. macOS ships neither
// pthread_rwlock_timed*lock nor a working sem_timedwait, so there we poll the
// try* variant until the deadline (same fallback as dragon_lock_acquire_ex).
// Each returns 1 if acquired, 0 on timeout. The caller (threading.dr) only
// reaches these when blocking && timeout >= 0.
//===----------------------------------------------------------------------===//

static void dragon__abs_deadline(double seconds, struct timespec* d) {
    clock_gettime(CLOCK_REALTIME, d);
    int64_t whole = (int64_t)seconds;
    d->tv_sec += (time_t)whole;
    d->tv_nsec += (long)((seconds - (double)whole) * 1e9);
    if (d->tv_nsec >= 1000000000L) { d->tv_sec += 1; d->tv_nsec -= 1000000000L; }
}

#if defined(__APPLE__)
static int dragon__deadline_passed(const struct timespec* d) {
    struct timespec now;
    clock_gettime(CLOCK_REALTIME, &now);
    return now.tv_sec > d->tv_sec ||
           (now.tv_sec == d->tv_sec && now.tv_nsec >= d->tv_nsec);
}
#endif

int dragon_rwlock_timedrdlock_sec(void* rw, double seconds) {
    pthread_rwlock_t* l = (pthread_rwlock_t*)rw;
    struct timespec d;
    dragon__abs_deadline(seconds, &d);
#if defined(__APPLE__)
    for (;;) {
        if (pthread_rwlock_tryrdlock(l) == 0) return 1;
        if (dragon__deadline_passed(&d)) return 0;
        struct timespec nap = {0, 500000};  // 0.5 ms
        nanosleep(&nap, nullptr);
    }
#else
    return pthread_rwlock_timedrdlock(l, &d) == 0 ? 1 : 0;
#endif
}

int dragon_rwlock_timedwrlock_sec(void* rw, double seconds) {
    pthread_rwlock_t* l = (pthread_rwlock_t*)rw;
    struct timespec d;
    dragon__abs_deadline(seconds, &d);
#if defined(__APPLE__)
    for (;;) {
        if (pthread_rwlock_trywrlock(l) == 0) return 1;
        if (dragon__deadline_passed(&d)) return 0;
        struct timespec nap = {0, 500000};  // 0.5 ms
        nanosleep(&nap, nullptr);
    }
#else
    return pthread_rwlock_timedwrlock(l, &d) == 0 ? 1 : 0;
#endif
}

int dragon_sem_timedwait_sec(void* sem, double seconds) {
    sem_t* s = (sem_t*)sem;
    struct timespec d;
    dragon__abs_deadline(seconds, &d);
#if defined(__APPLE__)
    for (;;) {
        if (sem_trywait(s) == 0) return 1;
        if (dragon__deadline_passed(&d)) return 0;
        struct timespec nap = {0, 500000};  // 0.5 ms
        nanosleep(&nap, nullptr);
    }
#else
    while (sem_timedwait(s, &d) != 0) {
        if (errno == EINTR) continue;  // interrupted by a signal - keep waiting
        return 0;                      // ETIMEDOUT or other error
    }
    return 1;
#endif
}

void dragon_lock_release(void* lock) {
    pthread_mutex_unlock((pthread_mutex_t*)lock);
}

void dragon_lock_destroy(void* lock) {
    pthread_mutex_destroy((pthread_mutex_t*)lock);
    free(lock);
}

//===----------------------------------------------------------------------===//
// SyncList - thread-safe list (DragonList + mutex)
//===----------------------------------------------------------------------===//

typedef struct {
    DragonList* list;
    pthread_mutex_t mtx;
} DragonSyncList;

DragonSyncList* dragon_synclist_new() {
    auto* sl = (DragonSyncList*)malloc(sizeof(DragonSyncList));
    sl->list = dragon_list_new(8);
    pthread_mutex_init(&sl->mtx, NULL);
    return sl;
}

void dragon_synclist_append(DragonSyncList* sl, int64_t val) {
    pthread_mutex_lock(&sl->mtx);
    dragon_list_append(sl->list, val);
    pthread_mutex_unlock(&sl->mtx);
}

int64_t dragon_synclist_get(DragonSyncList* sl, int64_t idx) {
    pthread_mutex_lock(&sl->mtx);
    int64_t v = dragon_list_get(sl->list, idx);
    pthread_mutex_unlock(&sl->mtx);
    return v;
}

void dragon_synclist_set(DragonSyncList* sl, int64_t idx, int64_t val) {
    pthread_mutex_lock(&sl->mtx);
    dragon_list_set(sl->list, idx, val);
    pthread_mutex_unlock(&sl->mtx);
}

int64_t dragon_synclist_pop(DragonSyncList* sl, int64_t idx) {
    pthread_mutex_lock(&sl->mtx);
    int64_t v = dragon_list_pop(sl->list, idx);
    pthread_mutex_unlock(&sl->mtx);
    return v;
}

int64_t dragon_synclist_len(DragonSyncList* sl) {
    pthread_mutex_lock(&sl->mtx);
    int64_t v = dragon_list_len(sl->list);
    pthread_mutex_unlock(&sl->mtx);
    return v;
}

void dragon_synclist_clear(DragonSyncList* sl) {
    pthread_mutex_lock(&sl->mtx);
    dragon_list_clear(sl->list);
    pthread_mutex_unlock(&sl->mtx);
}

void dragon_synclist_extend(DragonSyncList* sl, DragonList* other) {
    pthread_mutex_lock(&sl->mtx);
    dragon_list_extend(sl->list, other);
    pthread_mutex_unlock(&sl->mtx);
}

void dragon_synclist_remove(DragonSyncList* sl, int64_t val) {
    pthread_mutex_lock(&sl->mtx);
    dragon_list_remove(sl->list, val);
    pthread_mutex_unlock(&sl->mtx);
}

void dragon_synclist_insert(DragonSyncList* sl, int64_t idx, int64_t val) {
    pthread_mutex_lock(&sl->mtx);
    dragon_list_insert(sl->list, idx, val);
    pthread_mutex_unlock(&sl->mtx);
}

int64_t dragon_synclist_index(DragonSyncList* sl, int64_t val) {
    pthread_mutex_lock(&sl->mtx);
    int64_t v = dragon_list_index(sl->list, val);
    pthread_mutex_unlock(&sl->mtx);
    return v;
}

int64_t dragon_synclist_count(DragonSyncList* sl, int64_t val) {
    pthread_mutex_lock(&sl->mtx);
    int64_t v = dragon_list_count(sl->list, val);
    pthread_mutex_unlock(&sl->mtx);
    return v;
}

void dragon_synclist_sort(DragonSyncList* sl) {
    pthread_mutex_lock(&sl->mtx);
    dragon_list_sort(sl->list);
    pthread_mutex_unlock(&sl->mtx);
}

void dragon_synclist_reverse(DragonSyncList* sl) {
    pthread_mutex_lock(&sl->mtx);
    dragon_list_reverse(sl->list);
    pthread_mutex_unlock(&sl->mtx);
}

DragonSyncList* dragon_synclist_copy(DragonSyncList* sl) {
    pthread_mutex_lock(&sl->mtx);
    DragonSyncList* cp = (DragonSyncList*)malloc(sizeof(DragonSyncList));
    cp->list = dragon_list_copy(sl->list);
    pthread_mutex_init(&cp->mtx, NULL);
    pthread_mutex_unlock(&sl->mtx);
    return cp;
}

void dragon_synclist_destroy(DragonSyncList* sl) {
    pthread_mutex_destroy(&sl->mtx);
    dragon_decref(sl->list);  // untrack + decref elements + free
    free(sl);
}

//===----------------------------------------------------------------------===//
// SyncDict - thread-safe dict (DragonDict + rwlock)
//===----------------------------------------------------------------------===//

typedef struct {
    DragonDict* dict;
    pthread_rwlock_t rwl;
} DragonSyncDict;

DragonSyncDict* dragon_syncdict_new() {
    auto* sd = (DragonSyncDict*)malloc(sizeof(DragonSyncDict));
    sd->dict = dragon_dict_new(8);
    pthread_rwlock_init(&sd->rwl, NULL);
    return sd;
}

void dragon_syncdict_set(DragonSyncDict* sd, const char* key, int64_t val) {
    pthread_rwlock_wrlock(&sd->rwl);
    dragon_dict_set(sd->dict, key, val);
    pthread_rwlock_unlock(&sd->rwl);
}

int64_t dragon_syncdict_get(DragonSyncDict* sd, const char* key) {
    pthread_rwlock_rdlock(&sd->rwl);
    int64_t v = dragon_dict_get(sd->dict, key);
    pthread_rwlock_unlock(&sd->rwl);
    return v;
}

int64_t dragon_syncdict_get_default(DragonSyncDict* sd, const char* key, int64_t def) {
    pthread_rwlock_rdlock(&sd->rwl);
    int64_t v = dragon_dict_get_default(sd->dict, key, def);
    pthread_rwlock_unlock(&sd->rwl);
    return v;
}

int64_t dragon_syncdict_len(DragonSyncDict* sd) {
    pthread_rwlock_rdlock(&sd->rwl);
    int64_t v = dragon_dict_len(sd->dict);
    pthread_rwlock_unlock(&sd->rwl);
    return v;
}

int64_t dragon_syncdict_has_key(DragonSyncDict* sd, const char* key) {
    pthread_rwlock_rdlock(&sd->rwl);
    int64_t v = dragon_dict_has_key(sd->dict, key);
    pthread_rwlock_unlock(&sd->rwl);
    return v;
}

DragonList* dragon_syncdict_keys(DragonSyncDict* sd) {
    pthread_rwlock_rdlock(&sd->rwl);
    DragonList* v = dragon_dict_keys(sd->dict);
    pthread_rwlock_unlock(&sd->rwl);
    return v;
}

DragonList* dragon_syncdict_values(DragonSyncDict* sd) {
    pthread_rwlock_rdlock(&sd->rwl);
    DragonList* v = dragon_dict_values(sd->dict);
    pthread_rwlock_unlock(&sd->rwl);
    return v;
}

DragonList* dragon_syncdict_items(DragonSyncDict* sd) {
    pthread_rwlock_rdlock(&sd->rwl);
    DragonList* v = dragon_dict_items(sd->dict);
    pthread_rwlock_unlock(&sd->rwl);
    return v;
}

int64_t dragon_syncdict_pop(DragonSyncDict* sd, const char* key) {
    pthread_rwlock_wrlock(&sd->rwl);
    int64_t v = dragon_dict_pop(sd->dict, key);
    pthread_rwlock_unlock(&sd->rwl);
    return v;
}

int64_t dragon_syncdict_pop_default(DragonSyncDict* sd, const char* key, int64_t def) {
    pthread_rwlock_wrlock(&sd->rwl);
    int64_t v = dragon_dict_pop_default(sd->dict, key, def);
    pthread_rwlock_unlock(&sd->rwl);
    return v;
}

void dragon_syncdict_clear(DragonSyncDict* sd) {
    pthread_rwlock_wrlock(&sd->rwl);
    dragon_dict_clear(sd->dict);
    pthread_rwlock_unlock(&sd->rwl);
}

void dragon_syncdict_update(DragonSyncDict* sd, DragonDict* other) {
    pthread_rwlock_wrlock(&sd->rwl);
    dragon_dict_update(sd->dict, other);
    pthread_rwlock_unlock(&sd->rwl);
}

int64_t dragon_syncdict_setdefault(DragonSyncDict* sd, const char* key, int64_t def) {
    pthread_rwlock_wrlock(&sd->rwl);
    int64_t v = dragon_dict_setdefault(sd->dict, key, def);
    pthread_rwlock_unlock(&sd->rwl);
    return v;
}

DragonSyncDict* dragon_syncdict_copy(DragonSyncDict* sd) {
    pthread_rwlock_rdlock(&sd->rwl);
    DragonSyncDict* cp = (DragonSyncDict*)malloc(sizeof(DragonSyncDict));
    cp->dict = dragon_dict_copy(sd->dict);
    pthread_rwlock_init(&cp->rwl, NULL);
    pthread_rwlock_unlock(&sd->rwl);
    return cp;
}

void dragon_syncdict_destroy(DragonSyncDict* sd) {
    pthread_rwlock_destroy(&sd->rwl);
    dragon_decref(sd->dict);  // untrack + decref values + free
    free(sd);
}

// Socket Helpers are in runtime_platform.cpp

//===----------------------------------------------------------------------===//
// Non-Blocking Socket I/O (green-thread-aware)
// These yield the current green thread on EAGAIN, resume when fd is ready.
//===----------------------------------------------------------------------===//

static void make_nonblocking(int fd) {
#ifdef _WIN32
    u_long nb = 1;
    ioctlsocket((SOCKET)fd, FIONBIO, &nb);
#else
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags != -1) fcntl(fd, F_SETFL, flags | O_NONBLOCK);
#ifdef SO_NOSIGPIPE
    // macOS/BSD have no MSG_NOSIGNAL; this socket option is the per-socket
    // equivalent - writes to a reset peer return EPIPE instead of raising
    // SIGPIPE. Idempotent; harmless on listener/non-send fds.
    int nosigpipe = 1;
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &nosigpipe, sizeof(nosigpipe));
#endif
#endif
}

// When called outside a green thread, wait for the fd to become ready using
// poll() (POSIX) / WSAPoll (Windows). Returns 0 if the fd is ready, -1 on
// error (e.g. EBADF). Without this, EAGAIN/EWOULDBLOCK on a bad/closed fd
// causes a 100% CPU infinite usleep loop.
#ifdef _WIN32
static int nb_wait_fd(int fd, short events) {
    WSAPOLLFD pfd = {};
    pfd.fd = (SOCKET)fd;
    pfd.events = events;
    while (1) {
        int r = WSAPoll(&pfd, 1, -1);
        if (r > 0) {
            if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) return -1;
            return 0;
        }
        if (r < 0 && WSAGetLastError() == WSAEINTR) continue;
        return -1;
    }
}
// Bounded variant for the off-vthread recv-timeout fallback (R1). Returns 1 if
// ready, 0 on timeout, -1 on error.
static int nb_wait_fd_timeout(int fd, short events, int timeout_ms) {
    WSAPOLLFD pfd = {};
    pfd.fd = (SOCKET)fd;
    pfd.events = events;
    int r = WSAPoll(&pfd, 1, timeout_ms);
    if (r > 0) {
        if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) return -1;
        return 1;
    }
    return r == 0 ? 0 : -1;   // 0 = timeout, <0 = error
}
#else
static int nb_wait_fd(int fd, short events) {
    struct pollfd pfd = { fd, events, 0 };
    while (1) {
        int r = poll(&pfd, 1, -1);
        if (r > 0) {
            if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) return -1;
            return 0;
        }
        if (r < 0 && errno == EINTR) continue;
        return -1;
    }
}
// Bounded variant for the off-vthread recv-timeout fallback (R1). Returns 1 if
// ready, 0 on a genuine timeout, -1 on error. EINTR must NOT surface as a
// timeout - the sole caller treats 0 as "give up and close the connection", so
// collapsing a signal-interrupted wait into 0 would spuriously drop a healthy
// connection. Loop on EINTR with the remaining budget, matching the unbounded
// nb_wait_fd's EINTR discipline.
static int nb_wait_fd_timeout(int fd, short events, int timeout_ms) {
    struct pollfd pfd = { fd, events, 0 };
    struct timespec start;
    clock_gettime(CLOCK_MONOTONIC, &start);
    int remaining = timeout_ms;
    while (1) {
        int r = poll(&pfd, 1, remaining);
        if (r > 0) {
            if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) return -1;
            return 1;
        }
        if (r == 0) return 0;                 // genuine timeout
        if (errno != EINTR) return -1;        // real error
        // Interrupted: recompute the remaining budget and re-arm.
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        long elapsed = (now.tv_sec - start.tv_sec) * 1000
                     + (now.tv_nsec - start.tv_nsec) / 1000000;
        remaining = timeout_ms - (int)elapsed;
        if (remaining <= 0) return 0;         // budget exhausted → real timeout
    }
}
#endif

// Treat the socket call as "would have blocked" for both POSIX and Winsock.
static inline bool dragon_sock_wouldblock() {
#ifdef _WIN32
    int e = WSAGetLastError();
    return e == WSAEWOULDBLOCK || e == WSAEINPROGRESS;
#else
    return errno == EAGAIN || errno == EWOULDBLOCK;
#endif
}

/// Non-blocking accept: yields green thread until a connection is ready.
/// Returns client fd, or -1 on error.
int64_t dragon_nb_accept(int64_t server_fd, void* addr, void* addrlen) {
    make_nonblocking((int)server_fd);
    while (1) {
#ifdef _WIN32
        int client = (int)accept((SOCKET)server_fd,
                                 (struct sockaddr*)addr, (int*)addrlen);
#else
        int client = accept((int)server_fd, (struct sockaddr*)addr,
                            (socklen_t*)addrlen);
#endif
        if (client >= 0) return (int64_t)client;
        if (dragon_sock_wouldblock()) {
            DragonVThread* vt = __current_vthread;
            if (vt && vt->coro) {
                dragon_io_watch_fd((int)server_fd, IO_EVENT_FD_READ, vt);
                mco_yield(vt->coro);
                continue; // retry after wakeup
            }
            // Not in a green thread - block on poll() instead of spinning.
            if (nb_wait_fd((int)server_fd, POLLIN) < 0) return -1;
            continue;
        }
        return -1; // real error
    }
}

/// Non-blocking recv: yields green thread until data is available.
/// Returns bytes read, 0 on close, -1 on error.
int64_t dragon_nb_recv(int64_t fd, void* buf, int64_t max_len) {
    make_nonblocking((int)fd);
    while (1) {
#ifdef _WIN32
        int n = recv((SOCKET)fd, (char*)buf, (int)max_len, 0);
#else
        ssize_t n = recv((int)fd, buf, (size_t)max_len, 0);
#endif
        if (n >= 0) return (int64_t)n;
        if (dragon_sock_wouldblock()) {
            DragonVThread* vt = __current_vthread;
            if (vt && vt->coro) {
                dragon_io_watch_fd((int)fd, IO_EVENT_FD_READ, vt);
                mco_yield(vt->coro);
                continue;
            }
            if (nb_wait_fd((int)fd, POLLIN) < 0) return -1;
            continue;
        }
        return -1;
    }
}

/// Non-blocking send: yields green thread until socket is writable.
/// Returns bytes sent, -1 on error. Loops until all data is sent.
int64_t dragon_nb_send(int64_t fd, const char* buf, int64_t len) {
    make_nonblocking((int)fd);
    int64_t total = 0;
    while (total < len) {
#ifdef _WIN32
        int n = send((SOCKET)fd, buf + total, (int)(len - total), 0);
#else
        // MSG_NOSIGNAL: return EPIPE instead of raising SIGPIPE when the peer
        // has closed (no-op flag on platforms without it - macOS uses the
        // SO_NOSIGPIPE option set in make_nonblocking instead).
        ssize_t n = send((int)fd, buf + total, (size_t)(len - total), MSG_NOSIGNAL);
#endif
        if (n >= 0) {
            total += n;
            continue;
        }
        if (dragon_sock_wouldblock()) {
            DragonVThread* vt = __current_vthread;
            if (vt && vt->coro) {
                dragon_io_watch_fd((int)fd, IO_EVENT_FD_WRITE, vt);
                mco_yield(vt->coro);
                continue;
            }
            if (nb_wait_fd((int)fd, POLLOUT) < 0) return -1;
            continue;
        }
        return -1;
    }
    return total;
}

/// Non-blocking recv that returns a Dragon string. Reads up to max_len bytes.
const char* dragon_nb_recv_str(int64_t fd, int64_t max_len) {
    char* buf = (char*)malloc(max_len + 1);
    int64_t n = dragon_nb_recv(fd, buf, max_len);
    if (n < 0) n = 0;
    buf[n] = '\0';
    const char* result = dragon_string_alloc(buf, n);
    free(buf);
    return result;
}

/// Non-blocking recv returning DragonBytes (binary-safe - unlike _str it does
/// not depend on NUL termination). Reads up to max_len bytes via the same
/// scheduler-yielding path; empty bytes on EOF/error. For length-prefixed wire
/// protocols a caller loops this into a read-exact helper (recv may short-read).
DragonBytes* dragon_nb_recv_bytes(int64_t fd, int64_t max_len) {
    int64_t cap = max_len > 0 ? max_len : 1;
    uint8_t* buf = (uint8_t*)malloc((size_t)cap);
    int64_t n = dragon_nb_recv(fd, buf, max_len);
    if (n < 0) n = 0;
    DragonBytes* result = dragon_bytes_new(buf, n);
    free(buf);
    return result;
}

/// Non-blocking recv with an idle/read timeout (R1). Like dragon_nb_recv_bytes,
/// but if no data arrives within `timeout_ms` the wait is abandoned and EMPTY
/// bytes are returned - the same signal as EOF, so the HTTP request framer
/// closes the connection (a silent peer can't pin the green thread; slowloris
/// defense). `timeout_ms <= 0` means "no timeout" (identical to recv_bytes).
///
/// Returning empty for BOTH timeout and EOF is deliberate: in either case the
/// connection must be torn down, and the framer already does exactly that on an
/// empty read (clean keep-alive end between requests, or `bad`/truncated mid
/// body). The distinction matters only to metrics, not to control flow.
DragonBytes* dragon_nb_recv_timeout(int64_t fd, int64_t max_len, int64_t timeout_ms) {
    if (timeout_ms <= 0) return dragon_nb_recv_bytes(fd, max_len);
    make_nonblocking((int)fd);
    int64_t cap = max_len > 0 ? max_len : 1;
    uint8_t* buf = (uint8_t*)malloc((size_t)cap);
    int64_t n = 0;
    while (1) {
#ifdef _WIN32
        int r = recv((SOCKET)fd, (char*)buf, (int)max_len, 0);
#else
        ssize_t r = recv((int)fd, buf, (size_t)max_len, 0);
#endif
        if (r >= 0) { n = (int64_t)r; break; }      // data (or 0 = EOF)
        if (dragon_sock_wouldblock()) {
            DragonVThread* vt = __current_vthread;
            if (vt && vt->coro) {
                vt->io_timed_out = 0;
                dragon_io_watch_fd_deadline((int)fd, IO_EVENT_FD_READ, vt, timeout_ms);
                mco_yield(vt->coro);
                if (vt->io_timed_out) { n = 0; break; }  // idle timeout → empty
                continue;                                 // fd ready → retry recv
            }
            // Off a green thread: bounded poll fallback.
            int pr = nb_wait_fd_timeout((int)fd, POLLIN, (int)timeout_ms);
            if (pr <= 0) { n = 0; break; }               // timeout or error → empty
            continue;
        }
        n = 0; break;                                    // real error → empty (close)
    }
    DragonBytes* result = dragon_bytes_new(buf, n);
    free(buf);
    return result;
}

/// Non-blocking send of a DragonBytes' full contents (binary-safe). Reuses
/// dragon_nb_send, which yields until every byte is written. Returns bytes
/// sent, -1 on error.
int64_t dragon_nb_send_bytes(int64_t fd, DragonBytes* data) {
    if (!data || data->len == 0) return 0;
    return dragon_nb_send(fd, (const char*)data->data, data->len);
}

// connect(2) signals "in progress" differently from recv/send: EINPROGRESS on
// POSIX, WSAEWOULDBLOCK/WSAEINPROGRESS on Winsock (NOT EAGAIN), so it needs its
// own would-block test rather than dragon_sock_wouldblock().
static inline bool dragon_connect_in_progress() {
#ifdef _WIN32
    int e = WSAGetLastError();
    return e == WSAEWOULDBLOCK || e == WSAEINPROGRESS;
#else
    return errno == EINPROGRESS;
#endif
}

/// Non-blocking connect: yields the green thread until the connection completes
/// (or fails). Returns 0 on success, -1 on error. The socket becomes writable
/// when the connect resolves; SO_ERROR then distinguishes success from failure.
int64_t dragon_nb_connect(int64_t fd, void* addr, int64_t addrlen) {
    make_nonblocking((int)fd);
#ifdef _WIN32
    int rc = connect((SOCKET)fd, (struct sockaddr*)addr, (int)addrlen);
#else
    int rc = connect((int)fd, (struct sockaddr*)addr, (socklen_t)addrlen);
#endif
    if (rc == 0) return 0;                       // connected immediately (loopback)
    if (!dragon_connect_in_progress()) return -1; // real, immediate error

    DragonVThread* vt = __current_vthread;
    if (vt && vt->coro) {
        dragon_io_watch_fd((int)fd, IO_EVENT_FD_WRITE, vt);
        mco_yield(vt->coro);
    } else if (nb_wait_fd((int)fd, POLLOUT) < 0) {
        return -1;
    }

    int err = 0;
    socklen_t elen = sizeof(err);
#ifdef _WIN32
    getsockopt((SOCKET)fd, SOL_SOCKET, SO_ERROR, (char*)&err, &elen);
#else
    getsockopt((int)fd, SOL_SOCKET, SO_ERROR, &err, &elen);
#endif
    if (err != 0) {
#ifndef _WIN32
        errno = err;
#endif
        return -1;
    }
    return 0;
}

// Scheduler-aware wait for `fd` to reach readiness. On a green thread: park via
// the reactor + yield (resumed when ready). Off a green thread: block in poll().
// Returns 0 when ready, -1 on poll error/HUP (off-vthread only; the on-vthread
// path lets the caller's retried syscall surface the error). Used by the TLS
// BIO so mbedTLS yields instead of blocking the carrier OS thread.
static int dragon_io_wait(int fd, int event_type, short poll_events) {
    DragonVThread* vt = __current_vthread;
    if (vt && vt->coro) {
        dragon_io_watch_fd(fd, event_type, vt);
        mco_yield(vt->coro);
        return 0;
    }
    return nb_wait_fd(fd, poll_events);
}

int dragon_io_wait_readable(int fd) { return dragon_io_wait(fd, IO_EVENT_FD_READ, POLLIN); }
int dragon_io_wait_writable(int fd) { return dragon_io_wait(fd, IO_EVENT_FD_WRITE, POLLOUT); }

// Deadline-bounded readability wait (R1) - the TLS BIO's slowloris defense.
// Returns 0 readable, 1 timeout, -1 error. With no deadline it is exactly
// dragon_io_wait_readable. On a green thread it parks via the deadline reactor
// and reads vt->io_timed_out; off a green thread it falls back to a bounded poll.
int dragon_io_wait_readable_timeout(int fd, int64_t timeout_ms) {
    if (timeout_ms <= 0) return dragon_io_wait_readable(fd);
    DragonVThread* vt = __current_vthread;
    if (vt && vt->coro) {
        vt->io_timed_out = 0;
        dragon_io_watch_fd_deadline(fd, IO_EVENT_FD_READ, vt, timeout_ms);
        mco_yield(vt->coro);
        return vt->io_timed_out ? 1 : 0;
    }
    int pr = nb_wait_fd_timeout(fd, POLLIN, (int)timeout_ms);
    return pr > 0 ? 0 : (pr == 0 ? 1 : -1);
}

/// Set O_NONBLOCK on a socket fd. Exposed for the TLS BIO: an accepted fd does
/// NOT inherit O_NONBLOCK from its listener, so the TLS conn must set it itself
/// or its BIO recv/send would block the carrier thread instead of yielding.
void dragon_set_nonblocking(int64_t fd) { make_nonblocking((int)fd); }

/// Close a file descriptor (treats as socket on Windows since Dragon's nb_*
/// helpers operate on sockets - file descriptors and sockets are distinct on
/// Windows but Dragon currently routes both through this helper).
void dragon_close_fd(int64_t fd) {
#ifdef _WIN32
    closesocket((SOCKET)fd);
#else
    // Remove any pending reactor watch for this fd BEFORE closing it. The
    // kernel already drops a closed fd from the epoll/kqueue interest set once
    // its last reference goes away, but doing it explicitly here also covers
    // the case where a dup'd reference keeps the description alive: otherwise a
    // stale readiness event could resume the vthread that WAS waiting on this
    // fd after the fd number is recycled by a new connection - waking one
    // connection's handler with another connection's bytes (cross-tenant data
    // leak). Idempotent: deleting an unregistered fd returns ENOENT, ignored.
    if (__io_epfd >= 0) {
        #ifdef __linux__
        epoll_ctl(__io_epfd, EPOLL_CTL_DEL, (int)fd, NULL);
        #elif defined(__APPLE__)
        struct kevent kev;
        EV_SET(&kev, (uintptr_t)fd, EVFILT_READ, EV_DELETE, 0, 0, NULL);
        kevent(__io_epfd, &kev, 1, NULL, 0, NULL);
        EV_SET(&kev, (uintptr_t)fd, EVFILT_WRITE, EV_DELETE, 0, 0, NULL);
        kevent(__io_epfd, &kev, 1, NULL, 0, NULL);
        #endif
    }
    close((int)fd);
#endif
}

/// Set SO_REUSEADDR on a socket
void dragon_setsockopt_reuse(int64_t fd) {
    int opt = 1;
#ifdef _WIN32
    setsockopt((SOCKET)fd, SOL_SOCKET, SO_REUSEADDR,
               (const char*)&opt, sizeof(opt));
#else
    setsockopt((int)fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
#endif
}

//===----------------------------------------------------------------------===//
// HTTP Parsing (llhttp - bundled Node.js HTTP/1.1 parser)

} // extern "C"
