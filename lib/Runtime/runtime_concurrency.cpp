#define MINICORO_IMPL
#include "runtime_internal.h"
#include <errno.h>
#include <time.h>
#include <cstdio>
#include <cstdlib>
#ifdef _WIN32
  #include <vector>
  #include <chrono>
  #include <algorithm>
  #include <io.h>
  #define poll WSAPoll
  #ifndef _SSIZE_T_DEFINED
    typedef intptr_t ssize_t;
    #define _SSIZE_T_DEFINED
  #endif
#else
  #include <sys/socket.h>
  #include <netinet/in.h>
  #include <arpa/inet.h>
  #include <poll.h>
  #include <semaphore.h>
  #ifdef __linux__
    #include <sys/epoll.h>
    #include <sys/timerfd.h>
    #include <fcntl.h>
  #elif defined(__APPLE__) || defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__)
    #include <sys/event.h>
    #include <fcntl.h>
  #endif
#endif

extern "C" {

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

typedef struct {
    pthread_t tid;
    int64_t result;
    int8_t done;
    int8_t joined;  // CAS'd 0->1 in join to defeat double-join race (UB + double-free)
    int8_t started;
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
    dragon_exc_thread_state_release();
    return NULL;
}

DragonThread* dragon_thread_fire(void* fn, int64_t* args, int64_t nargs) {
    DragonThread* t = (DragonThread*)dragon_xmalloc(sizeof(DragonThread));
    t->result = 0;
    t->done = 0;
    t->joined = 0;
    t->started = 0;
    DragonFireArgs* fa = (DragonFireArgs*)dragon_xmalloc(sizeof(DragonFireArgs));
    fa->thread = t;
    fa->fn = fn;
    if (nargs > 0) {
        fa->args = (int64_t*)dragon_xmalloc_n(nargs, sizeof(int64_t));
        memcpy(fa->args, args, sizeof(int64_t) * nargs);
    } else {
        fa->args = NULL;
    }
    fa->nargs = nargs;
    dragon_gc_go_concurrent();
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
    // Defeat a double-join (pthread_join twice is UB, free(t) twice is a double-free): only the CAS
    // winner joins+frees. Task handles are single-owner, so a losing caller just returns the cached result.
    int8_t expected = 0;
    if (!__atomic_compare_exchange_n(&t->joined, &expected, (int8_t)1, false,
                                     __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        return __atomic_load_n(&t->result, __ATOMIC_ACQUIRE);
    }
    if (t->started)
        pthread_join(t->tid, NULL);
    int64_t result = t->result;
    free(t);
    return result;
}

typedef struct {
    pthread_t tid;
    int64_t result;
    int8_t done;
    int8_t started;
    int8_t joined;
    void* fn;
    int64_t* args;
    int64_t nargs;
} DragonOSThread;

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
    dragon_exc_thread_state_release();
    return NULL;
}

void* dragon_osthread_new(void* fn, int64_t* args, int64_t nargs) {
    DragonOSThread* t = (DragonOSThread*)dragon_xcalloc_n(1, sizeof(DragonOSThread));
    t->fn = fn;
    t->done = 0;
    t->started = 0;
    if (nargs > 0 && args) {
        t->args = (int64_t*)dragon_xmalloc_n(nargs, sizeof(int64_t));
        memcpy(t->args, args, sizeof(int64_t) * nargs);
    } else {
        t->args = NULL;
    }
    t->nargs = nargs;
    return t;
}

int64_t dragon_osthread_start(void* handle) {
    DragonOSThread* t = (DragonOSThread*)handle;
    if (!t) return -1;
    int8_t expected = 0;
    if (!__atomic_compare_exchange_n(&t->started, &expected, (int8_t)1,
                                     false,
                                     __ATOMIC_ACQ_REL,
                                     __ATOMIC_ACQUIRE)) {
        return -1;
    }
    dragon_gc_go_concurrent();
    return pthread_create(&t->tid, NULL, dragon_osthread_entry, t);
}

int64_t dragon_osthread_join(void* handle) {
    DragonOSThread* t = (DragonOSThread*)handle;
    if (!t || !__atomic_load_n(&t->started, __ATOMIC_ACQUIRE)) return 0;
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

int64_t dragon_osthread_is_alive(void* handle) {
    DragonOSThread* t = (DragonOSThread*)handle;
    if (!t) return 0;
    int8_t started = __atomic_load_n(&t->started, __ATOMIC_ACQUIRE);
    int8_t done = __atomic_load_n(&t->done, __ATOMIC_ACQUIRE);
    return (started && !done) ? 1 : 0;
}

int64_t dragon_sizeof_mutex(void)  { return (int64_t)sizeof(pthread_mutex_t); }
int64_t dragon_sizeof_rwlock(void) { return (int64_t)sizeof(pthread_rwlock_t); }
int64_t dragon_sizeof_cond(void)   { return (int64_t)sizeof(pthread_cond_t); }
int64_t dragon_sizeof_sem(void)    { return (int64_t)sizeof(sem_t); }

typedef struct DragonBarrier {
    pthread_mutex_t mutex;
    pthread_cond_t  cond;
    uint64_t        generation;
    unsigned        threshold;
    unsigned        waiting;
} DragonBarrier;

void* dragon_barrier_new(int64_t count) {
    if (count <= 0) return nullptr;
    DragonBarrier* b = (DragonBarrier*)dragon_xmalloc(sizeof(DragonBarrier));
    if (pthread_mutex_init(&b->mutex, nullptr) != 0) {
        free(b);
        return nullptr;
    }
    if (pthread_cond_init(&b->cond, nullptr) != 0) {
        pthread_mutex_destroy(&b->mutex);
        free(b);
        return nullptr;
    }
    b->generation = 0;
    b->threshold = (unsigned)count;
    b->waiting = 0;
    return b;
}

int64_t dragon_barrier_wait(void* handle) {
    DragonBarrier* b = (DragonBarrier*)handle;
    if (!b) return -1;
    pthread_mutex_lock(&b->mutex);
    const uint64_t gen = b->generation;
    if (++b->waiting >= b->threshold) {
        b->generation++;
        b->waiting = 0;
        pthread_cond_broadcast(&b->cond);
        pthread_mutex_unlock(&b->mutex);
        return 1;
    }
    while (gen == b->generation) {
        pthread_cond_wait(&b->cond, &b->mutex);
    }
    pthread_mutex_unlock(&b->mutex);
    return 0;
}

int64_t dragon_barrier_destroy(void* handle) {
    DragonBarrier* b = (DragonBarrier*)handle;
    if (!b) return -1;
    pthread_cond_destroy(&b->cond);
    pthread_mutex_destroy(&b->mutex);
    free(b);
    return 0;
}

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

#define PARK_NONE   0
#define PARK_ARMED  1
#define PARK_PARKED 2
#define PARK_FIRED  3

static inline void dragon_io_arm_park(DragonVThread* vt) {
    __atomic_store_n(&vt->park_state, PARK_ARMED, __ATOMIC_RELEASE);
}

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
            continue;
        }
        if (st == PARK_ARMED) {
            if (__atomic_compare_exchange_n(&vt->park_state, &st, PARK_FIRED,
                                            false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
                return;
            }
            continue;
        }
        return;
    }
}

static bool dragon_vthread_finish_park(DragonVThread* vt) {
    int32_t expected = PARK_ARMED;
    if (__atomic_compare_exchange_n(&vt->park_state, &expected, PARK_PARKED,
                                    false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        return false;
    }
    __atomic_store_n(&vt->park_state, PARK_NONE, __ATOMIC_RELEASE);
    return true;
}

static DragonVThread* scheduler_dequeue() {
    DragonVThread* vt = __scheduler->head;
    if (vt) {
        __scheduler->head = vt->next;
        if (!__scheduler->head) __scheduler->tail = NULL;
        vt->next = NULL;
    }
    return vt;
}

void dragon_vthread_detach(DragonVThread* vt);

static void vthread_result_release_unclaimed(DragonVThread* vt) {
    if (vt->result_claimed || !vt->result) return;
    int64_t tag = vt->result_tag;
    void* p = (void*)(uintptr_t)vt->result;
    if (tag == TAG_TASK_HANDLE) dragon_vthread_detach((DragonVThread*)p);
    else if (tag == TAG_STR) dragon_decref_str_dispatch((const char*)p);
    else if (tag == (int8_t)DRAGON_TAG_CLOSURE) dragon_decref_callable(p);
    else if (tag >= TAG_LIST) dragon_decref_dispatch(p);
}

static void vthread_release(DragonVThread* vt) {
    if (__atomic_sub_fetch(&vt->refs, 1, __ATOMIC_ACQ_REL) != 0) return;
    vthread_result_release_unclaimed(vt);
    mco_destroy(vt->coro);
    pthread_mutex_destroy(&vt->join_lock);
    pthread_cond_destroy(&vt->join_cond);
    dragon_cleanup_stack_drain(&vt->cleanup, 0);
    free(vt->cleanup.vals);
    free(vt->cleanup.kinds);
    free(vt->cleanup.tags);
    dragon_decref_str_dispatch(vt->exc_msg);
    if (vt->exc_obj) dragon_decref_dispatch(vt->exc_obj);
    free(vt);
}

static void vthread_mark_done_and_release(DragonVThread* vt) {
    int8_t expected = 0;
    if (!__atomic_compare_exchange_n(&vt->done, &expected, (int8_t)1, false,
                                     __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
        return;
    pthread_mutex_lock(&vt->join_lock);
    pthread_cond_broadcast(&vt->join_cond);
    pthread_mutex_unlock(&vt->join_lock);
    vthread_release(vt);
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

        __current_vthread = vt;

        if (mco_status(vt->coro) != MCO_SUSPENDED) {
            if (mco_status(vt->coro) == MCO_DEAD) {
                vthread_mark_done_and_release(vt);
            }
            __current_vthread = NULL;
            continue;
        }

        int __saved_active_frames = __dragon_active_frames;
        __dragon_active_frames = vt->active_frames;

        mco_resume(vt->coro);

        vt->active_frames = __dragon_active_frames;
        __dragon_active_frames = __saved_active_frames;
        __current_vthread = NULL;

        if (mco_status(vt->coro) == MCO_DEAD) {
            vthread_mark_done_and_release(vt);
        } else if (__atomic_load_n(&vt->park_state, __ATOMIC_ACQUIRE) == PARK_NONE) {
            scheduler_enqueue(vt);
        } else {
            if (dragon_vthread_finish_park(vt)) {
                scheduler_enqueue(vt);
            }
        }
    }
    return NULL;
}

static void scheduler_init() {
    dragon_gc_go_concurrent();
    __scheduler = (DragonScheduler*)dragon_xcalloc_n(1, sizeof(DragonScheduler));
    pthread_mutex_init(&__scheduler->lock, NULL);
    pthread_cond_init(&__scheduler->not_empty, NULL);
    __scheduler->head = NULL;
    __scheduler->tail = NULL;
    __scheduler->shutdown = 0;

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
    __scheduler->workers = (pthread_t*)dragon_xcalloc_n_or_abort(ncpu, sizeof(pthread_t));
    for (int i = 0; i < __scheduler->num_workers; i++) {
        pthread_create(&__scheduler->workers[i], NULL, scheduler_worker, NULL);
    }
}

DragonVThread* dragon_vthread_spawn_typed(
    void (*trampoline)(mco_coro*), void* args, int64_t args_size) {
    pthread_once(&__scheduler_once, scheduler_init);

    DragonVThread* vt = (DragonVThread*)dragon_xcalloc_n(1, sizeof(DragonVThread));
    vt->exc_sp = -1;
    vt->done = 0;
    vt->yield_reason = YIELD_COOP;
    vt->result = 0;
    vt->next = NULL;
    vt->refs = 2;
    pthread_mutex_init(&vt->join_lock, NULL);
    pthread_cond_init(&vt->join_cond, NULL);

    void* heap_args = NULL;
    if (args_size > 0 && args) {
        heap_args = dragon_malloc_nullable((size_t)args_size);
        if (!heap_args) {
            pthread_mutex_destroy(&vt->join_lock);
            pthread_cond_destroy(&vt->join_cond);
            free(vt);
            dragon_raise_oom();
        }
        memcpy(heap_args, args, (size_t)args_size);
        *(DragonVThread**)heap_args = vt;
    }

    mco_desc desc = mco_desc_init(trampoline, 0);
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

void dragon_vthread_set_result(DragonVThread* vt, int64_t res, int64_t tag) {
    if (vt) {
        vt->result = res;
        vt->result_tag = tag;
    }
}

int64_t dragon_vthread_join(DragonVThread* vt) {
    if (!vt) return 0;

    int8_t expected = 0;
    bool winner = __atomic_compare_exchange_n(&vt->joined, &expected, (int8_t)1,
                                              false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);

    pthread_mutex_lock(&vt->join_lock);
    while (!vt->done) {
        pthread_cond_wait(&vt->join_cond, &vt->join_lock);
    }
    pthread_mutex_unlock(&vt->join_lock);

    int64_t result = vt->result;
    vt->result_claimed = 1;
    if (winner) vthread_release(vt);
    return result;
}

void dragon_vthread_detach(DragonVThread* vt) {
    if (!vt) return;
    int8_t expected = 0;
    if (__atomic_compare_exchange_n(&vt->joined, &expected, (int8_t)1, false,
                                    __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
        vthread_release(vt);
}

int64_t dragon_vthread_is_alive(DragonVThread* vt) {
    if (!vt) return 0;
    return !__atomic_load_n(&vt->done, __ATOMIC_ACQUIRE) ? 1 : 0;
}

void dragon_generator_abandon(void* gen_ptr) {
    DragonGenerator* gen = (DragonGenerator*)gen_ptr;
    if (!gen || !gen->coro) return;
    mco_coro* co = gen->coro;
    if (mco_status(co) != MCO_RUNNING) return;
    mco_coro* resumer = co->prev_co;
    if (resumer) resumer->state = MCO_RUNNING;
    mco_current_co = resumer;
    co->prev_co = NULL;
    co->state = MCO_DEAD;
}

enum IoEventType {
    IO_EVENT_FD_READ  = 1,
    IO_EVENT_FD_WRITE = 2,
    IO_EVENT_TIMER    = 3
};

typedef struct IoRequest {
    DragonVThread*    vt;
    int               fd;
    int               event_type;
    int64_t           timer_ms;
    int64_t           deadline_ms;
    struct IoRequest* next;
    struct IoRequest* dl_next;
} IoRequest;

static int            __io_epfd = -1;
#ifdef _WIN32
static SOCKET         __io_wakeup_pipe[2] = { INVALID_SOCKET, INVALID_SOCKET };
#else
static int            __io_wakeup_pipe[2];
#endif
static pthread_t      __io_thread;
static pthread_once_t __io_once = PTHREAD_ONCE_INIT;
static volatile int   __io_shutdown = 0;

static IoRequest*       __io_pending_head = NULL;
static pthread_mutex_t  __io_pending_lock = PTHREAD_MUTEX_INITIALIZER;

#ifdef _WIN32
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

void dragon_win_wsa_startup(void) { win_wsa_startup_once(); }

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
    if (req->vt) dragon_io_arm_park(req->vt);
    pthread_mutex_lock(&__io_pending_lock);
    req->next = __io_pending_head;
    __io_pending_head = req;
    pthread_mutex_unlock(&__io_pending_lock);
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
static IoRequest* __io_deadline_head = NULL;

static long long io_now_ms() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + (long long)ts.tv_nsec / 1000000;
}

static void io_deadline_add(IoRequest* req) {
    req->dl_next = __io_deadline_head;
    __io_deadline_head = req;
}

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
#endif

#ifdef __linux__

static void io_process_pending() {
    IoRequest* req = io_drain_pending();
    while (req) {
        IoRequest* next = req->next;
        if (req->event_type == IO_EVENT_TIMER) {
            int tfd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
            struct itimerspec its = {};
            int64_t ms = req->timer_ms;
            its.it_value.tv_sec  = ms / 1000;
            its.it_value.tv_nsec = (ms % 1000) * 1000000;
            timerfd_settime(tfd, 0, &its, NULL);
            req->fd = tfd;
            struct epoll_event ev = {};
            ev.events = EPOLLIN | EPOLLONESHOT;
            ev.data.ptr = req;
            epoll_ctl(__io_epfd, EPOLL_CTL_ADD, tfd, &ev);
        } else {
            struct epoll_event ev = {};
            ev.events = (req->event_type == IO_EVENT_FD_READ ? EPOLLIN : EPOLLOUT)
                        | EPOLLONESHOT;
            ev.data.ptr = req;
            epoll_ctl(__io_epfd, EPOLL_CTL_ADD, req->fd, &ev);
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
        int wait_ms = io_deadline_wait_ms(100);
        int n = epoll_wait(__io_epfd, events, 64, wait_ms);
        for (int i = 0; i < n; i++) {
            IoRequest* req = (IoRequest*)events[i].data.ptr;
            if (req->fd == __io_wakeup_pipe[0]) {
                char buf[64];
                (void)read(__io_wakeup_pipe[0], buf, sizeof(buf));
            } else {
                epoll_ctl(__io_epfd, EPOLL_CTL_DEL, req->fd, NULL);
                if (req->event_type == IO_EVENT_TIMER) {
                    close(req->fd);
                } else if (req->timer_ms > 0) {
                    io_deadline_remove(req);
                }
                DragonVThread* wv = req->vt;
                free(req);
                dragon_io_wake(wv);
            }
        }
        if (__io_deadline_head) {
            long long now = io_now_ms();
            IoRequest** pp = &__io_deadline_head;
            while (*pp) {
                IoRequest* req = *pp;
                if (req->deadline_ms <= now) {
                    *pp = req->dl_next;
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
        io_process_pending();
    }
    return NULL;
}

static void io_init() {
    __io_epfd = epoll_create1(EPOLL_CLOEXEC);
    pipe(__io_wakeup_pipe);
    fcntl(__io_wakeup_pipe[0], F_SETFL, O_NONBLOCK);
    struct epoll_event ev = {};
    ev.events = EPOLLIN;
    static IoRequest wakeup_sentinel = {NULL, 0, 0, 0, 0, NULL, NULL};
    wakeup_sentinel.fd = __io_wakeup_pipe[0];
    ev.data.ptr = &wakeup_sentinel;
    epoll_ctl(__io_epfd, EPOLL_CTL_ADD, __io_wakeup_pipe[0], &ev);
    dragon_gc_go_concurrent();
    pthread_create(&__io_thread, NULL, io_thread_entry, NULL);
}

#elif defined(__APPLE__) || defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__)

static volatile int64_t __kqueue_timer_id = 1;

static void io_process_pending() {
    IoRequest* req = io_drain_pending();
    while (req) {
        IoRequest* next = req->next;
        struct kevent kev;
        if (req->event_type == IO_EVENT_TIMER) {
            int64_t ms = req->timer_ms;
            uintptr_t timer_id = (uintptr_t)__sync_fetch_and_add(&__kqueue_timer_id, 1);
            req->fd = -1;
            #ifdef NOTE_USECONDS
            EV_SET(&kev, timer_id, EVFILT_TIMER, EV_ADD | EV_ONESHOT,
                   NOTE_USECONDS, ms * 1000, req);
            #else
            EV_SET(&kev, timer_id, EVFILT_TIMER, EV_ADD | EV_ONESHOT,
                   0, ms, req);
            #endif
            kevent(__io_epfd, &kev, 1, NULL, 0, NULL);
        } else {
            int16_t filter = (req->event_type == IO_EVENT_FD_READ)
                             ? EVFILT_READ : EVFILT_WRITE;
            EV_SET(&kev, req->fd, filter, EV_ADD | EV_ONESHOT, 0, 0, req);
            kevent(__io_epfd, &kev, 1, NULL, 0, NULL);
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
        int wait_ms = io_deadline_wait_ms(100);
        struct timespec timeout = {wait_ms / 1000, (long)(wait_ms % 1000) * 1000000};
        int n = kevent(__io_epfd, NULL, 0, events, 64, &timeout);
        for (int i = 0; i < n; i++) {
            if (events[i].udata == NULL) {
                char buf[64];
                (void)read(__io_wakeup_pipe[0], buf, sizeof(buf));
            } else {
                IoRequest* req = (IoRequest*)events[i].udata;
                if (req->event_type != IO_EVENT_TIMER && req->timer_ms > 0) {
                    io_deadline_remove(req);
                }
                DragonVThread* wv = req->vt;
                free(req);
                dragon_io_wake(wv);
            }
        }
        if (__io_deadline_head) {
            long long now = io_now_ms();
            IoRequest** pp = &__io_deadline_head;
            while (*pp) {
                IoRequest* req = *pp;
                if (req->deadline_ms <= now) {
                    *pp = req->dl_next;
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
    struct kevent kev;
    EV_SET(&kev, __io_wakeup_pipe[0], EVFILT_READ, EV_ADD, 0, 0, NULL);
    kevent(__io_epfd, &kev, 1, NULL, 0, NULL);
    dragon_gc_go_concurrent();
    pthread_create(&__io_thread, NULL, io_thread_entry, NULL);
}

#elif defined(_WIN32)

typedef struct WinIoEntry {
    IoRequest* req;
    long long deadline_ms;
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

        long long now = win_now_ms();
        long long next_deadline = -1;
        std::vector<WSAPOLLFD> pfds;
        std::vector<size_t>    pfd_to_idx;

        pthread_mutex_lock(&__io_active_lock);
        WSAPOLLFD wake = {};
        wake.fd = __io_wakeup_pipe[0];
        wake.events = POLLRDNORM;
        pfds.push_back(wake);
        pfd_to_idx.push_back((size_t)-1);

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
                if (e.deadline_ms > 0 &&
                    (next_deadline < 0 || e.deadline_ms < next_deadline))
                    next_deadline = e.deadline_ms;
            }
        }
        pthread_mutex_unlock(&__io_active_lock);

        int timeout_ms = 100;
        if (next_deadline >= 0) {
            long long delta = next_deadline - now;
            if (delta < 0) delta = 0;
            if (delta < timeout_ms) timeout_ms = (int)delta;
        }

        int n = WSAPoll(pfds.data(), (ULONG)pfds.size(), timeout_ms);

        if (n > 0 && (pfds[0].revents & (POLLRDNORM | POLLERR | POLLHUP))) {
            char buf[64];
            (void)recv(__io_wakeup_pipe[0], buf, sizeof(buf), 0);
        }

        std::vector<size_t> to_remove;
        if (n > 0) {
            pthread_mutex_lock(&__io_active_lock);
            for (size_t k = 1; k < pfds.size(); k++) {
                if (pfds[k].revents == 0) continue;
                size_t idx = pfd_to_idx[k];
                if (idx >= __io_active.size()) continue;
                IoRequest* req = __io_active[idx].req;
                if (!req) continue;
                req->vt->io_timed_out = 0;
                DragonVThread* wv = req->vt;
                free(req);
                __io_active[idx].req = nullptr;
                to_remove.push_back(idx);
                dragon_io_wake(wv);
            }
            pthread_mutex_unlock(&__io_active_lock);
        }

        now = win_now_ms();
        pthread_mutex_lock(&__io_active_lock);
        for (size_t i = 0; i < __io_active.size(); i++) {
            auto& e = __io_active[i];
            if (!e.req) continue;
            if (e.deadline_ms > 0 && e.deadline_ms <= now) {
                if (e.req->event_type != IO_EVENT_TIMER)
                    e.req->vt->io_timed_out = 1;
                DragonVThread* wv = e.req->vt;
                free(e.req);
                e.req = nullptr;
                to_remove.push_back(i);
                dragon_io_wake(wv);
            }
        }
        std::sort(to_remove.begin(), to_remove.end(),
                  [](size_t a, size_t b) { return a > b; });
        size_t prev = (size_t)-1;
        for (size_t i : to_remove) {
            if (i == prev) continue;
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
    u_long nb = 1;
    ioctlsocket(__io_wakeup_pipe[0], FIONBIO, &nb);
    dragon_gc_go_concurrent();
    pthread_create(&__io_thread, NULL, io_thread_entry, NULL);
}

#endif

void dragon_io_watch_fd(int fd, int event_type, DragonVThread* vt) {
    pthread_once(&__io_once, io_init);
    IoRequest* req = (IoRequest*)dragon_xmalloc(sizeof(IoRequest));
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

void dragon_io_watch_fd_deadline(int fd, int event_type, DragonVThread* vt,
                                 int64_t timeout_ms) {
    pthread_once(&__io_once, io_init);
    IoRequest* req = (IoRequest*)dragon_xmalloc(sizeof(IoRequest));
    req->vt = vt;
    req->fd = fd;
    req->event_type = event_type;
    req->timer_ms = timeout_ms > 0 ? timeout_ms : 0;
    req->deadline_ms = 0;
    req->next = NULL;
    req->dl_next = NULL;
    vt->yield_reason = YIELD_IO;
    io_post_request(req);
}

void dragon_vthread_sleep(int64_t ms) {
    pthread_once(&__io_once, io_init);

    DragonVThread* vt = __current_vthread;
    if (!vt || !vt->coro) {
#ifdef _WIN32
        Sleep((DWORD)ms);
#else
        usleep((useconds_t)(ms * 1000));
#endif
        return;
    }

    IoRequest* req = (IoRequest*)dragon_xmalloc(sizeof(IoRequest));
    req->vt = vt;
    req->fd = -1;
    req->event_type = IO_EVENT_TIMER;
    req->timer_ms = ms;
    req->deadline_ms = 0;
    req->next = NULL;
    req->dl_next = NULL;
    vt->yield_reason = YIELD_SLEEP;
    io_post_request(req);

    mco_yield(vt->coro);
}

void dragon_vthread_yield() {
    DragonVThread* vt = __current_vthread;
    if (vt && vt->coro) {
        mco_yield(vt->coro);
    }
}

void* dragon_lock_new() {
    pthread_mutex_t* m = (pthread_mutex_t*)dragon_xmalloc(sizeof(pthread_mutex_t));
    pthread_mutex_init(m, NULL);
    return m;
}

void dragon_lock_acquire(void* lock) {
    pthread_mutex_lock((pthread_mutex_t*)lock);
}

int64_t dragon_lock_acquire_ex(void* lock, int64_t blocking, double timeout) {
    pthread_mutex_t* m = (pthread_mutex_t*)lock;
    if (!blocking) {
        return pthread_mutex_trylock(m) == 0 ? 1 : 0;
    }
    if (timeout < 0) {
        pthread_mutex_lock(m);
        return 1;
    }
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
    for (;;) {
        if (pthread_mutex_trylock(m) == 0) return 1;
        struct timespec now;
        clock_gettime(CLOCK_REALTIME, &now);
        if (now.tv_sec > deadline.tv_sec ||
            (now.tv_sec == deadline.tv_sec && now.tv_nsec >= deadline.tv_nsec))
            return 0;
        struct timespec nap = {0, 500000};
        nanosleep(&nap, nullptr);
    }
#else
    return pthread_mutex_timedlock(m, &deadline) == 0 ? 1 : 0;
#endif
}

int64_t dragon_lock_try_acquire(void* lock) {
    return pthread_mutex_trylock((pthread_mutex_t*)lock) == 0 ? 1 : 0;
}

static void dragon__abs_deadline(double seconds, struct timespec* d) {
    clock_gettime(CLOCK_REALTIME, d);
    int64_t whole = (int64_t)seconds;
    d->tv_sec += (time_t)whole;
    d->tv_nsec += (long)((seconds - (double)whole) * 1e9);
    if (d->tv_nsec >= 1000000000L) { d->tv_sec += 1; d->tv_nsec -= 1000000000L; }
}

static int dragon__deadline_passed(const struct timespec* d) {
    struct timespec now;
    clock_gettime(CLOCK_REALTIME, &now);
    return now.tv_sec > d->tv_sec ||
           (now.tv_sec == d->tv_sec && now.tv_nsec >= d->tv_nsec);
}

int dragon_rwlock_timedrdlock_sec(void* rw, double seconds) {
    pthread_rwlock_t* l = (pthread_rwlock_t*)rw;
    struct timespec d;
    dragon__abs_deadline(seconds, &d);
#if defined(__APPLE__)
    for (;;) {
        if (pthread_rwlock_tryrdlock(l) == 0) return 1;
        if (dragon__deadline_passed(&d)) return 0;
        struct timespec nap = {0, 500000};
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
        struct timespec nap = {0, 500000};
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
        struct timespec nap = {0, 500000};
        nanosleep(&nap, nullptr);
    }
#else
    while (sem_timedwait(s, &d) != 0) {
        if (errno == EINTR) continue;
        return 0;
    }
    return 1;
#endif
}

typedef struct DragonSem {
    pthread_mutex_t mutex;
    pthread_cond_t  cond;
    int64_t         permits;
} DragonSem;

void* dragon_sem_new(int64_t value) {
    if (value < 0) return nullptr;
    DragonSem* s = (DragonSem*)dragon_xmalloc(sizeof(DragonSem));
    if (pthread_mutex_init(&s->mutex, nullptr) != 0) {
        free(s);
        return nullptr;
    }
    if (pthread_cond_init(&s->cond, nullptr) != 0) {
        pthread_mutex_destroy(&s->mutex);
        free(s);
        return nullptr;
    }
    s->permits = value;
    return s;
}

int64_t dragon_sem_acquire(void* handle) {
    DragonSem* s = (DragonSem*)handle;
    if (!s) return -1;
    pthread_mutex_lock(&s->mutex);
    while (s->permits == 0) {
        pthread_cond_wait(&s->cond, &s->mutex);
    }
    s->permits--;
    pthread_mutex_unlock(&s->mutex);
    return 0;
}

int64_t dragon_sem_tryacquire(void* handle) {
    DragonSem* s = (DragonSem*)handle;
    if (!s) return 0;
    pthread_mutex_lock(&s->mutex);
    int64_t taken = 0;
    if (s->permits > 0) {
        s->permits--;
        taken = 1;
    }
    pthread_mutex_unlock(&s->mutex);
    return taken;
}

int64_t dragon_sem_timedacquire_sec(void* handle, double seconds) {
    DragonSem* s = (DragonSem*)handle;
    if (!s) return 0;
    struct timespec deadline;
    dragon__abs_deadline(seconds, &deadline);
    pthread_mutex_lock(&s->mutex);
    while (s->permits == 0) {
        const int rc = pthread_cond_timedwait(&s->cond, &s->mutex, &deadline);
        if (rc == ETIMEDOUT) {
            pthread_mutex_unlock(&s->mutex);
            return 0;
        }
        if (rc != 0 && dragon__deadline_passed(&deadline)) {
            pthread_mutex_unlock(&s->mutex);
            return 0;
        }
    }
    s->permits--;
    pthread_mutex_unlock(&s->mutex);
    return 1;
}

int64_t dragon_sem_release(void* handle) {
    DragonSem* s = (DragonSem*)handle;
    if (!s) return -1;
    pthread_mutex_lock(&s->mutex);
    s->permits++;
    pthread_cond_signal(&s->cond);
    pthread_mutex_unlock(&s->mutex);
    return 0;
}

int64_t dragon_sem_free(void* handle) {
    DragonSem* s = (DragonSem*)handle;
    if (!s) return -1;
    pthread_cond_destroy(&s->cond);
    pthread_mutex_destroy(&s->mutex);
    free(s);
    return 0;
}

void dragon_lock_release(void* lock) {
    pthread_mutex_unlock((pthread_mutex_t*)lock);
}

void dragon_lock_destroy(void* lock) {
    pthread_mutex_destroy((pthread_mutex_t*)lock);
    free(lock);
}

typedef struct {
    DragonList* list;
    pthread_mutex_t mtx;
} DragonSyncList;

DragonSyncList* dragon_synclist_new() {
    auto* sl = (DragonSyncList*)dragon_xmalloc(sizeof(DragonSyncList));
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
    DragonSyncList* cp = (DragonSyncList*)dragon_xmalloc(sizeof(DragonSyncList));
    cp->list = dragon_list_copy(sl->list);
    pthread_mutex_init(&cp->mtx, NULL);
    pthread_mutex_unlock(&sl->mtx);
    return cp;
}

void dragon_synclist_destroy(DragonSyncList* sl) {
    pthread_mutex_destroy(&sl->mtx);
    dragon_decref(sl->list);
    free(sl);
}

typedef struct {
    DragonDict* dict;
    pthread_rwlock_t rwl;
} DragonSyncDict;

DragonSyncDict* dragon_syncdict_new() {
    auto* sd = (DragonSyncDict*)dragon_xmalloc(sizeof(DragonSyncDict));
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
    DragonSyncDict* cp = (DragonSyncDict*)dragon_xmalloc(sizeof(DragonSyncDict));
    cp->dict = dragon_dict_copy(sd->dict);
    pthread_rwlock_init(&cp->rwl, NULL);
    pthread_rwlock_unlock(&sd->rwl);
    return cp;
}

void dragon_syncdict_destroy(DragonSyncDict* sd) {
    pthread_rwlock_destroy(&sd->rwl);
    dragon_decref(sd->dict);
    free(sd);
}

static void make_nonblocking(int fd) {
#ifdef _WIN32
    u_long nb = 1;
    ioctlsocket((SOCKET)fd, FIONBIO, &nb);
#else
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags != -1) fcntl(fd, F_SETFL, flags | O_NONBLOCK);
#ifdef SO_NOSIGPIPE
    int nosigpipe = 1;
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &nosigpipe, sizeof(nosigpipe));
#endif
#endif
}

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
static int nb_wait_fd_timeout(int fd, short events, int timeout_ms) {
    WSAPOLLFD pfd = {};
    pfd.fd = (SOCKET)fd;
    pfd.events = events;
    int r = WSAPoll(&pfd, 1, timeout_ms);
    if (r > 0) {
        if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) return -1;
        return 1;
    }
    return r == 0 ? 0 : -1;
}
#else
static int nb_wait_fd(int fd, short events) {
    struct pollfd pfd = { fd, events, 0 };
    while (1) {
        int r = poll(&pfd, 1, -1);
        if (r > 0) {
            // POLLHUP/POLLERR still mean "the syscall will not block": macOS raises
            // POLLHUP on peer close with data still buffered, so only POLLNVAL fails here.
            if (pfd.revents & POLLNVAL) return -1;
            return 0;
        }
        if (r < 0 && errno == EINTR) continue;
        return -1;
    }
}
static int nb_wait_fd_timeout(int fd, short events, int timeout_ms) {
    struct pollfd pfd = { fd, events, 0 };
    struct timespec start;
    clock_gettime(CLOCK_MONOTONIC, &start);
    int remaining = timeout_ms;
    while (1) {
        int r = poll(&pfd, 1, remaining);
        if (r > 0) {
            if (pfd.revents & POLLNVAL) return -1;
            return 1;
        }
        if (r == 0) return 0;
        if (errno != EINTR) return -1;
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        long elapsed = (now.tv_sec - start.tv_sec) * 1000
                     + (now.tv_nsec - start.tv_nsec) / 1000000;
        remaining = timeout_ms - (int)elapsed;
        if (remaining <= 0) return 0;
    }
}
#endif

static int nb_debug_enabled() {
    static int v = -1;
    if (v < 0) {
        const char* e = getenv("DRAGON_NB_DEBUG");
        v = (e && *e) ? 1 : 0;
    }
    return v;
}

#define NB_DBG(...) do { if (nb_debug_enabled()) fprintf(stderr, __VA_ARGS__); } while (0)

static inline bool dragon_sock_wouldblock() {
#ifdef _WIN32
    int e = WSAGetLastError();
    return e == WSAEWOULDBLOCK || e == WSAEINPROGRESS;
#else
    return errno == EAGAIN || errno == EWOULDBLOCK;
#endif
}

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
        if (client >= 0) {
            NB_DBG("[nb] accept fd=%lld -> client=%d\n", (long long)server_fd, client);
            return (int64_t)client;
        }
        if (dragon_sock_wouldblock()) {
            DragonVThread* vt = __current_vthread;
            if (vt && vt->coro) {
                NB_DBG("[nb] accept fd=%lld park\n", (long long)server_fd);
                dragon_io_watch_fd((int)server_fd, IO_EVENT_FD_READ, vt);
                mco_yield(vt->coro);
                NB_DBG("[nb] accept fd=%lld resume timed_out=%d\n",
                       (long long)server_fd, (int)vt->io_timed_out);
                continue;
            }
            if (nb_wait_fd((int)server_fd, POLLIN) < 0) return -1;
            continue;
        }
        NB_DBG("[nb] accept fd=%lld err errno=%d\n", (long long)server_fd, errno);
        return -1;
    }
}

int64_t dragon_nb_recv(int64_t fd, void* buf, int64_t max_len) {
    make_nonblocking((int)fd);
    while (1) {
#ifdef _WIN32
        int n = recv((SOCKET)fd, (char*)buf, (int)max_len, 0);
#else
        ssize_t n = recv((int)fd, buf, (size_t)max_len, 0);
#endif
        if (n >= 0) {
            NB_DBG("[nb] recv fd=%lld n=%lld\n", (long long)fd, (long long)n);
            return (int64_t)n;
        }
        if (dragon_sock_wouldblock()) {
            DragonVThread* vt = __current_vthread;
            if (vt && vt->coro) {
                NB_DBG("[nb] recv fd=%lld park\n", (long long)fd);
                dragon_io_watch_fd((int)fd, IO_EVENT_FD_READ, vt);
                mco_yield(vt->coro);
                NB_DBG("[nb] recv fd=%lld resume timed_out=%d\n",
                       (long long)fd, (int)vt->io_timed_out);
                continue;
            }
            if (nb_wait_fd((int)fd, POLLIN) < 0) return -1;
            continue;
        }
        NB_DBG("[nb] recv fd=%lld err errno=%d\n", (long long)fd, errno);
        return -1;
    }
}

int64_t dragon_nb_send(int64_t fd, const char* buf, int64_t len) {
    make_nonblocking((int)fd);
    int64_t total = 0;
    while (total < len) {
#ifdef _WIN32
        int n = send((SOCKET)fd, buf + total, (int)(len - total), 0);
#else
        ssize_t n = send((int)fd, buf + total, (size_t)(len - total), MSG_NOSIGNAL);
#endif
        if (n >= 0) {
            total += n;
            continue;
        }
        if (dragon_sock_wouldblock()) {
            DragonVThread* vt = __current_vthread;
            if (vt && vt->coro) {
                NB_DBG("[nb] send fd=%lld park total=%lld\n", (long long)fd, (long long)total);
                dragon_io_watch_fd((int)fd, IO_EVENT_FD_WRITE, vt);
                mco_yield(vt->coro);
                continue;
            }
            if (nb_wait_fd((int)fd, POLLOUT) < 0) return -1;
            continue;
        }
        NB_DBG("[nb] send fd=%lld err errno=%d total=%lld\n",
               (long long)fd, errno, (long long)total);
        return -1;
    }
    NB_DBG("[nb] send fd=%lld done total=%lld\n", (long long)fd, (long long)total);
    return total;
}

const char* dragon_nb_recv_str(int64_t fd, int64_t max_len) {
    int64_t cap = max_len > 0 ? max_len : 1;
    char* buf = (char*)dragon_xmalloc_ex(cap, 1, 1);
    int64_t n = dragon_nb_recv(fd, buf, max_len);
    if (n < 0) n = 0;
    buf[n] = '\0';
    int32_t clbase = dragon_cleanup_depth();
    dragon_cleanup_push((int64_t)(uintptr_t)buf, DCLEAN_FREE, 0);
    const char* result = dragon_string_alloc(buf, n);
    dragon_cleanup_reset(clbase);
    free(buf);
    return result;
}

DragonBytes* dragon_nb_recv_bytes(int64_t fd, int64_t max_len) {
    int64_t cap = max_len > 0 ? max_len : 1;
    uint8_t* buf = (uint8_t*)dragon_xmalloc_n(cap, 1);
    int64_t n = dragon_nb_recv(fd, buf, max_len);
    if (n < 0) n = 0;
    DragonBytes* result = dragon_bytes_new(buf, n);
    free(buf);
    return result;
}

DragonBytes* dragon_nb_recv_timeout(int64_t fd, int64_t max_len, int64_t timeout_ms) {
    if (timeout_ms <= 0) return dragon_nb_recv_bytes(fd, max_len);
    make_nonblocking((int)fd);
    int64_t cap = max_len > 0 ? max_len : 1;
    uint8_t* buf = (uint8_t*)dragon_xmalloc_n(cap, 1);
    int64_t n = 0;
    while (1) {
#ifdef _WIN32
        int r = recv((SOCKET)fd, (char*)buf, (int)max_len, 0);
#else
        ssize_t r = recv((int)fd, buf, (size_t)max_len, 0);
#endif
        if (r >= 0) { n = (int64_t)r; break; }
        if (dragon_sock_wouldblock()) {
            DragonVThread* vt = __current_vthread;
            if (vt && vt->coro) {
                vt->io_timed_out = 0;
                dragon_io_watch_fd_deadline((int)fd, IO_EVENT_FD_READ, vt, timeout_ms);
                mco_yield(vt->coro);
                if (vt->io_timed_out) { n = 0; break; }
                continue;
            }
            int pr = nb_wait_fd_timeout((int)fd, POLLIN, (int)timeout_ms);
            if (pr <= 0) { n = 0; break; }
            continue;
        }
        n = 0; break;
    }
    DragonBytes* result = dragon_bytes_new(buf, n);
    free(buf);
    return result;
}

int64_t dragon_nb_send_bytes(int64_t fd, DragonBytes* data) {
    if (!data || data->len == 0) return 0;
    return dragon_nb_send(fd, (const char*)data->data, data->len);
}

static inline bool dragon_connect_in_progress() {
#ifdef _WIN32
    int e = WSAGetLastError();
    return e == WSAEWOULDBLOCK || e == WSAEINPROGRESS;
#else
    return errno == EINPROGRESS;
#endif
}

int64_t dragon_nb_connect(int64_t fd, void* addr, int64_t addrlen) {
    make_nonblocking((int)fd);
#ifdef _WIN32
    int rc = connect((SOCKET)fd, (struct sockaddr*)addr, (int)addrlen);
#else
    int rc = connect((int)fd, (struct sockaddr*)addr, (socklen_t)addrlen);
#endif
    if (rc == 0) return 0;
    if (!dragon_connect_in_progress()) return -1;

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
        NB_DBG("[nb] connect fd=%lld err so_error=%d\n", (long long)fd, err);
        return -1;
    }
    NB_DBG("[nb] connect fd=%lld ok\n", (long long)fd);
    return 0;
}

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

void dragon_set_nonblocking(int64_t fd) { make_nonblocking((int)fd); }

void dragon_close_fd(int64_t fd) {
#ifdef _WIN32
    closesocket((SOCKET)fd);
#else
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

void dragon_setsockopt_reuse(int64_t fd) {
    int opt = 1;
#ifdef _WIN32
    setsockopt((SOCKET)fd, SOL_SOCKET, SO_REUSEADDR,
               (const char*)&opt, sizeof(opt));
#else
    setsockopt((int)fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
#endif
}

}
