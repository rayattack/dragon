#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#define MINICORO_IMPL
#include "runtime_internal.h"
#include <errno.h>
#include <time.h>
#ifdef _WIN32
  #include <vector>
  #include <chrono>
  #include <algorithm>
  #include <io.h>
  #include <semaphore.h>
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
  #include <sys/mman.h>
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

void dragon_vthread_yield(void);

static void vthread_wake(DragonVThread* vt);
static void vthread_arm_park(DragonVThread* vt);
static DragonVThread* dragon_green_self(void);
static void osthread_exit_wake_green_joiner(void** slot);
static void osthread_green_join_wait(void** slot, volatile int8_t* done);

typedef struct {
    pthread_t tid;
    int64_t result;
    int8_t done;
    int8_t joined;  // CAS'd 0->1 in join to defeat double-join race (UB + double-free)
    int8_t started;
    void* green_waiter;
} DragonThread;

typedef struct {
    DragonThread* thread;
    void* fn;
    int64_t* args;
    int64_t nargs;
} DragonFireArgs;

static void* dragon_thread_entry(void* raw) {
    dragon_gc_mutator_register();
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
    osthread_exit_wake_green_joiner(&fa->thread->green_waiter);
    free(fa->args);
    free(fa);
    dragon_exc_thread_state_release();
    dragon_gc_mutator_unregister();
    return NULL;
}

DragonThread* dragon_thread_fire(void* fn, int64_t* args, int64_t nargs) {
    DragonThread* t = (DragonThread*)dragon_xmalloc(sizeof(DragonThread));
    t->result = 0;
    t->done = 0;
    t->joined = 0;
    t->started = 0;
    t->green_waiter = NULL;
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
    if (t->started) {
        osthread_green_join_wait(&t->green_waiter, &t->done);
        dragon_gc_safe_begin();
        pthread_join(t->tid, NULL);
        dragon_gc_safe_end();
    }
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
    void* green_waiter;
} DragonOSThread;

static void* dragon_osthread_entry(void* raw) {
    dragon_gc_mutator_register();
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
    osthread_exit_wake_green_joiner(&t->green_waiter);
    dragon_exc_thread_state_release();
    dragon_gc_mutator_unregister();
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
    osthread_green_join_wait(&t->green_waiter, &t->done);
    dragon_gc_safe_begin();
    pthread_join(t->tid, NULL);
    dragon_gc_safe_end();
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

typedef struct {
    DragonVThread*  head;
    DragonVThread*  tail;
    pthread_mutex_t lock;
    pthread_cond_t  not_empty;
    pthread_cond_t  spare_cond;
    int64_t         spares;
    int64_t         spare_wakeups;
    int64_t         total_carriers;
    int64_t         carrier_cap;
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

static void vthread_arm_park(DragonVThread* vt) {
    __atomic_store_n(&vt->park_state, PARK_ARMED, __ATOMIC_RELEASE);
}

static void vthread_wake(DragonVThread* vt) {
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

static DragonVThread* dragon_green_self(void) {
    DragonVThread* vt = __current_vthread;
    return (vt && vt->coro) ? vt : NULL;
}

static void vthread_waitq_push(DragonVThreadQueue* q, DragonVThread* vt) {
    vt->park_next = NULL;
    if (q->tail) {
        q->tail->park_next = vt;
    } else {
        q->head = vt;
    }
    q->tail = vt;
}

static DragonVThread* vthread_waitq_pop(DragonVThreadQueue* q) {
    DragonVThread* vt = q->head;
    if (vt) {
        q->head = vt->park_next;
        if (!q->head) q->tail = NULL;
        vt->park_next = NULL;
    }
    return vt;
}

static DragonVThread* vthread_waitq_steal(DragonVThreadQueue* q) {
    DragonVThread* head = q->head;
    q->head = NULL;
    q->tail = NULL;
    return head;
}

static void vthread_wake_stolen(DragonVThread* head) {
    while (head) {
        DragonVThread* next = head->park_next;
        head->park_next = NULL;
        vthread_wake(head);
        head = next;
    }
}

static void green_park(DragonVThreadQueue* q, DragonVThread* vt,
                       pthread_mutex_t* guard) {
    vthread_waitq_push(q, vt);
    vthread_arm_park(vt);
    pthread_mutex_unlock(guard);
    mco_yield(vt->coro);
    pthread_mutex_lock(guard);
}

static void osthread_exit_wake_green_joiner(void** slot) {
    void* prev = __atomic_exchange_n(slot, (void*)(uintptr_t)1, __ATOMIC_ACQ_REL);
    if ((uintptr_t)prev > 1) vthread_wake((DragonVThread*)prev);
}

static void osthread_green_join_wait(void** slot, volatile int8_t* done) {
    DragonVThread* self = dragon_green_self();
    if (!self || __atomic_load_n(done, __ATOMIC_ACQUIRE)) return;
    vthread_arm_park(self);
    void* expected = NULL;
    if (__atomic_compare_exchange_n(slot, &expected, (void*)self, false,
                                    __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        mco_yield(self->coro);
    } else {
        __atomic_store_n(&self->park_state, PARK_NONE, __ATOMIC_RELEASE);
    }
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

typedef struct DragonCarrier {
    volatile uint32_t tick;
    volatile int8_t   retaken;
    uint32_t          sysmon_last;
    struct DragonCarrier* reg_next;
} DragonCarrier;

static __thread DragonCarrier* __current_carrier = NULL;
static DragonCarrier* __carrier_list = NULL;

void dragon_extern_enter(void) {
    DragonCarrier* c = __current_carrier;
    if (!c || !__current_vthread) return;
    __atomic_store_n(&c->tick, c->tick + 1, __ATOMIC_RELEASE);
}

void dragon_extern_exit(void) {
    DragonCarrier* c = __current_carrier;
    if (!c || !__current_vthread) return;
    __atomic_store_n(&c->tick, c->tick + 1, __ATOMIC_RELEASE);
}

void dragon_foreign_enter(void) {
    dragon_gc_safe_begin();
    dragon_extern_enter();
}

void dragon_foreign_exit(void) {
    dragon_extern_exit();
    dragon_gc_safe_end();
}

int64_t dragon_safe_region_suspend(void) {
    DragonMutator* m = __dragon_mutator;
    if (!m) {
        dragon_gc_mutator_register_foreign();
        m = __dragon_mutator;
    }
    if (!m || m->safe_depth == 0) return 0;
    int32_t saved = m->safe_depth;
    m->safe_depth = 1;
    dragon_gc_safe_end();
    return (int64_t)saved;
}

void dragon_safe_region_resume(int64_t token) {
    if (token <= 0) return;
    dragon_gc_safe_begin();
    DragonMutator* m = __dragon_mutator;
    if (m) m->safe_depth = (int32_t)token;
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
    dragon_lsan_unroot_whole_coroutine(vt->coro);
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

static int64_t __dragon_vthread_live = 0;

int64_t dragon_vthread_live_count(void) {
    return __atomic_load_n(&__dragon_vthread_live, __ATOMIC_ACQUIRE);
}

static void vthread_mark_done_and_release(DragonVThread* vt) {
    int8_t expected = 0;
    if (!__atomic_compare_exchange_n(&vt->done, &expected, (int8_t)1, false,
                                     __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
        return;
    __atomic_fetch_sub(&__dragon_vthread_live, 1, __ATOMIC_ACQ_REL);
    pthread_mutex_lock(&vt->join_lock);
    DragonVThread* joiners = vthread_waitq_steal(&vt->join_waiters);
    pthread_cond_broadcast(&vt->join_cond);
    pthread_mutex_unlock(&vt->join_lock);
    vthread_wake_stolen(joiners);
    vthread_release(vt);
}

static DragonCarrier* carrier_register(void) {
    DragonCarrier* c = (DragonCarrier*)dragon_xcalloc_n(1, sizeof(DragonCarrier));
    pthread_mutex_lock(&__scheduler->lock);
    c->reg_next = __carrier_list;
    __atomic_store_n(&__carrier_list, c, __ATOMIC_RELEASE);
    pthread_mutex_unlock(&__scheduler->lock);
    __current_carrier = c;
    dragon_gc_mutator_register();
    return c;
}

static void carrier_park_as_spare(DragonCarrier* c) {
    dragon_gc_safe_begin();
    pthread_mutex_lock(&__scheduler->lock);
    __scheduler->spares++;
    while (__scheduler->spare_wakeups == 0) {
        pthread_cond_wait(&__scheduler->spare_cond, &__scheduler->lock);
    }
    __scheduler->spare_wakeups--;
    __scheduler->spares--;
    pthread_mutex_unlock(&__scheduler->lock);
    dragon_gc_safe_end();
    __atomic_store_n(&c->retaken, 0, __ATOMIC_RELEASE);
}

static void* scheduler_worker(void* arg) {
    (void)arg;
    DragonCarrier* carrier = carrier_register();
    while (1) {
        if (__atomic_load_n(&carrier->retaken, __ATOMIC_ACQUIRE)) {
            carrier_park_as_spare(carrier);
        }
        dragon_gc_safe_begin();
        pthread_mutex_lock(&__scheduler->lock);
        while (!__scheduler->head && !__scheduler->shutdown) {
            pthread_cond_wait(&__scheduler->not_empty, &__scheduler->lock);
        }
        if (__scheduler->shutdown && !__scheduler->head) {
            pthread_mutex_unlock(&__scheduler->lock);
            dragon_gc_safe_end();
            break;
        }
        DragonVThread* vt = scheduler_dequeue();
        pthread_mutex_unlock(&__scheduler->lock);
        dragon_gc_safe_end();

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

        dragon_gc_assert_running("scheduler resume");
        mco_resume(vt->coro);
        dragon_gc_safe_region_reset();

        vt->active_frames = __dragon_active_frames;
        __dragon_active_frames = __saved_active_frames;
        __current_vthread = NULL;
        dragon_gc_poll();

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
    dragon_gc_mutator_unregister();
    return NULL;
}

static void sysmon_retake(DragonCarrier* c) {
    pthread_mutex_lock(&__scheduler->lock);
    if (__scheduler->spares > __scheduler->spare_wakeups) {
        __scheduler->spare_wakeups++;
        __atomic_store_n(&c->retaken, 1, __ATOMIC_RELEASE);
        pthread_cond_signal(&__scheduler->spare_cond);
    } else if (__scheduler->total_carriers < __scheduler->carrier_cap) {
        dragon_gc_go_concurrent();
        pthread_t tid;
        if (pthread_create(&tid, NULL, scheduler_worker, NULL) == 0) {
            pthread_detach(tid);
            __scheduler->total_carriers++;
            __atomic_store_n(&c->retaken, 1, __ATOMIC_RELEASE);
        }
    }
    pthread_mutex_unlock(&__scheduler->lock);
}

#define DRAGON_SYSMON_SCAN_INTERVAL 100
#define DRAGON_SYSMON_IDLE_CEILING 10000

static void* sysmon_thread(void* arg) {
    (void)arg;
    useconds_t interval = DRAGON_SYSMON_SCAN_INTERVAL;
    while (1) {
#ifdef _WIN32
        Sleep(interval / 1000 > 0 ? interval / 1000 : 1);
#else
        usleep(interval);
#endif
        int in_extern = 0;
        for (DragonCarrier* c = __atomic_load_n(&__carrier_list, __ATOMIC_ACQUIRE);
             c; c = c->reg_next) {
            const uint32_t t = __atomic_load_n(&c->tick, __ATOMIC_ACQUIRE);
            if ((t & 1u) == 0) {
                c->sysmon_last = t;
                continue;
            }
            in_extern = 1;
            if (t == c->sysmon_last &&
                !__atomic_load_n(&c->retaken, __ATOMIC_ACQUIRE)) {
                sysmon_retake(c);
            }
            c->sysmon_last = t;
        }
        if (in_extern) {
            interval = DRAGON_SYSMON_SCAN_INTERVAL;
        } else if (interval < DRAGON_SYSMON_IDLE_CEILING) {
            interval *= 2;
            if (interval > DRAGON_SYSMON_IDLE_CEILING)
                interval = DRAGON_SYSMON_IDLE_CEILING;
        }
    }
    return NULL;
}

static void scheduler_init() {
    dragon_gc_go_concurrent();
    __scheduler = (DragonScheduler*)dragon_xcalloc_n(1, sizeof(DragonScheduler));
    pthread_mutex_init(&__scheduler->lock, NULL);
    pthread_cond_init(&__scheduler->not_empty, NULL);
    pthread_cond_init(&__scheduler->spare_cond, NULL);
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
    long cap = 256;
    const char* capEnv = getenv("DRAGON_MAX_CARRIERS");
    if (capEnv && atol(capEnv) > 0) cap = atol(capEnv);
    if (cap < ncpu) cap = ncpu;
    __scheduler->carrier_cap = cap;
    __scheduler->total_carriers = ncpu;
    __scheduler->num_workers = (int)ncpu;
    __scheduler->workers = (pthread_t*)dragon_xcalloc_n_or_abort(ncpu, sizeof(pthread_t));
    for (int i = 0; i < __scheduler->num_workers; i++) {
        pthread_create(&__scheduler->workers[i], NULL, scheduler_worker, NULL);
    }
    pthread_t sysmon_tid;
    if (pthread_create(&sysmon_tid, NULL, sysmon_thread, NULL) == 0) {
        pthread_detach(sysmon_tid);
    }
}

#define DRAGON_FIRE_POOL_MAX 64

static pthread_mutex_t __coro_pool_lock = PTHREAD_MUTEX_INITIALIZER;
static void*   __coro_pool_head = NULL;
static int64_t __coro_pool_count = 0;
static size_t  __coro_pool_block_size = 0;

static void* coro_block_map(size_t size) {
#ifdef _WIN32
    return VirtualAlloc(NULL, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
#else
    void* p = mmap(NULL, size, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    return p != MAP_FAILED ? p : NULL;
#endif
}

static void coro_block_unmap(void* p, size_t size) {
#ifdef _WIN32
    (void)size;
    VirtualFree(p, 0, MEM_RELEASE);
#else
    munmap(p, size);
#endif
}

static void* coro_block_alloc(size_t size, void* allocator_data) {
    (void)allocator_data;
    if (size == __atomic_load_n(&__coro_pool_block_size, __ATOMIC_ACQUIRE)) {
        pthread_mutex_lock(&__coro_pool_lock);
        void* blk = __coro_pool_head;
        if (blk) {
            __coro_pool_head = *(void**)blk;
            __coro_pool_count--;
            dragon_asan_unpoison_region(blk, size);
        }
        pthread_mutex_unlock(&__coro_pool_lock);
        if (blk) return blk;
    }
    return coro_block_map(size);
}

static void coro_block_free(void* ptr, size_t size, void* allocator_data) {
    (void)allocator_data;
    if (size == __atomic_load_n(&__coro_pool_block_size, __ATOMIC_ACQUIRE)) {
        pthread_mutex_lock(&__coro_pool_lock);
        if (__coro_pool_count < DRAGON_FIRE_POOL_MAX) {
            dragon_asan_unpoison_region(ptr, size);
            *(void**)ptr = __coro_pool_head;
            __coro_pool_head = ptr;
            __coro_pool_count++;
            dragon_asan_poison_region((char*)ptr + sizeof(void*),
                                      size - sizeof(void*));
            pthread_mutex_unlock(&__coro_pool_lock);
            return;
        }
        pthread_mutex_unlock(&__coro_pool_lock);
    }
    coro_block_unmap(ptr, size);
}

mco_desc dragon_coro_desc_init(void (*entry)(mco_coro*)) {
    mco_desc desc = mco_desc_init(entry, 0);
    size_t unclaimed = 0;
    __atomic_compare_exchange_n(&__coro_pool_block_size, &unclaimed,
                                desc.coro_size, false,
                                __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
    desc.alloc_cb = coro_block_alloc;
    desc.dealloc_cb = coro_block_free;
    return desc;
}

int64_t dragon_fire_pool_size(void) {
    pthread_mutex_lock(&__coro_pool_lock);
    int64_t n = __coro_pool_count;
    pthread_mutex_unlock(&__coro_pool_lock);
    return n;
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

    mco_desc desc = dragon_coro_desc_init(trampoline);
    desc.user_data = heap_args;
    mco_result r = mco_create(&vt->coro, &desc);
    if (r != MCO_SUCCESS) {
        fprintf(stderr, "fire: failed to create green thread: %s\n", mco_result_description(r));
        if (heap_args) free(heap_args);
        free(vt);
        return NULL;
    }

    dragon_lsan_root_whole_coroutine(vt->coro);
    __atomic_fetch_add(&__dragon_vthread_live, 1, __ATOMIC_ACQ_REL);
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

    DragonVThread* self = dragon_green_self();
    int safe = 0;
    pthread_mutex_lock(&vt->join_lock);
    while (!vt->done) {
        if (self) {
            green_park(&vt->join_waiters, self, &vt->join_lock);
            continue;
        }
        if (!safe) {
            safe = 1;
            dragon_gc_safe_begin();
        }
        pthread_cond_wait(&vt->join_cond, &vt->join_lock);
    }
    pthread_mutex_unlock(&vt->join_lock);
    if (safe) dragon_gc_safe_end();

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
    if (req->vt) vthread_arm_park(req->vt);
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
    if (!__io_deadline_head) return -1;
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
    while (1) {
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
                vthread_wake(wv);
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
                    vthread_wake(wv);
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
    while (1) {
        int wait_ms = io_deadline_wait_ms(100);
        struct timespec timeout = {0, 0};
        const struct timespec* deadline = NULL;
        if (wait_ms >= 0) {
            timeout.tv_sec = wait_ms / 1000;
            timeout.tv_nsec = (long)(wait_ms % 1000) * 1000000;
            deadline = &timeout;
        }
        int n = kevent(__io_epfd, NULL, 0, events, 64, deadline);
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
                vthread_wake(wv);
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
                    vthread_wake(wv);
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
    while (1) {
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
                vthread_wake(wv);
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
                vthread_wake(wv);
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

#ifndef _WIN32
static void os_sleep_whole_duration(int64_t ms) {
    struct timespec remaining;
    remaining.tv_sec = (time_t)(ms / 1000);
    remaining.tv_nsec = (long)((ms % 1000) * 1000000);
    while (nanosleep(&remaining, &remaining) != 0 && errno == EINTR) {
    }
}
#endif

void dragon_vthread_sleep(int64_t ms) {
    pthread_once(&__io_once, io_init);

    DragonVThread* vt = __current_vthread;
    if (!vt || !vt->coro) {
        if (ms <= 0) return;
        dragon_gc_safe_begin();
#ifdef _WIN32
        Sleep((DWORD)ms);
#else
        os_sleep_whole_duration(ms);
#endif
        dragon_gc_safe_end();
        return;
    }

    if (ms <= 0) {
        dragon_vthread_yield();
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
        dragon_gc_assert_running("vthread yield");
    }
}

typedef struct DragonLock {
    pthread_mutex_t m;
    pthread_cond_t  c;
    DragonVThreadQueue waiters;
    int64_t os_waiters;
    int8_t  locked;
} DragonLock;

void* dragon_lock_new() {
    DragonLock* l = (DragonLock*)dragon_xcalloc_n(1, sizeof(DragonLock));
    pthread_mutex_init(&l->m, NULL);
    pthread_cond_init(&l->c, NULL);
    return l;
}

void dragon_lock_acquire(void* lock) {
    DragonLock* l = (DragonLock*)lock;
    DragonVThread* vt = dragon_green_self();
    int safe = 0;
    pthread_mutex_lock(&l->m);
    while (l->locked) {
        if (vt) {
            green_park(&l->waiters, vt, &l->m);
            continue;
        }
        if (!safe) {
            safe = 1;
            dragon_gc_safe_begin();
        }
        l->os_waiters++;
        pthread_cond_wait(&l->c, &l->m);
        l->os_waiters--;
    }
    l->locked = 1;
    pthread_mutex_unlock(&l->m);
    if (safe) dragon_gc_safe_end();
}

int64_t dragon_lock_try_acquire(void* lock) {
    DragonLock* l = (DragonLock*)lock;
    pthread_mutex_lock(&l->m);
    int64_t got = 0;
    if (!l->locked) {
        l->locked = 1;
        got = 1;
    }
    pthread_mutex_unlock(&l->m);
    return got;
}

static int64_t lock_timed_wait_os(DragonLock* l, const struct timespec* deadline) {
    pthread_mutex_lock(&l->m);
    while (l->locked) {
        l->os_waiters++;
        const int rc = pthread_cond_timedwait(&l->c, &l->m, deadline);
        l->os_waiters--;
        if (l->locked && (rc == ETIMEDOUT || dragon__deadline_passed(deadline))) {
            pthread_mutex_unlock(&l->m);
            return 0;
        }
    }
    l->locked = 1;
    pthread_mutex_unlock(&l->m);
    return 1;
}

int64_t dragon_lock_acquire_ex(void* lock, int64_t blocking, double timeout) {
    if (!blocking) return dragon_lock_try_acquire(lock);
    if (timeout < 0) {
        dragon_lock_acquire(lock);
        return 1;
    }
    if (dragon_lock_try_acquire(lock)) return 1;
    struct timespec deadline;
    dragon__abs_deadline(timeout, &deadline);
    dragon_extern_enter();
    const int64_t got = lock_timed_wait_os((DragonLock*)lock, &deadline);
    dragon_extern_exit();
    return got;
}

void dragon_lock_release(void* lock) {
    DragonLock* l = (DragonLock*)lock;
    pthread_mutex_lock(&l->m);
    l->locked = 0;
    DragonVThread* waiter = vthread_waitq_pop(&l->waiters);
    if (!waiter && l->os_waiters > 0) pthread_cond_signal(&l->c);
    pthread_mutex_unlock(&l->m);
    if (waiter) vthread_wake(waiter);
}

void dragon_lock_destroy(void* lock) {
    if (!lock) return;
    DragonLock* l = (DragonLock*)lock;
    pthread_cond_destroy(&l->c);
    pthread_mutex_destroy(&l->m);
    free(l);
}

typedef struct DragonCondVar {
    pthread_mutex_t m;
    pthread_cond_t  c;
    DragonVThreadQueue waiters;
    int64_t os_waiters;
    int64_t os_signals;
} DragonCondVar;

void* dragon_condvar_new() {
    DragonCondVar* cv = (DragonCondVar*)dragon_xcalloc_n(1, sizeof(DragonCondVar));
    pthread_mutex_init(&cv->m, NULL);
    pthread_cond_init(&cv->c, NULL);
    return cv;
}

void dragon_condvar_free(void* cond) {
    if (!cond) return;
    DragonCondVar* cv = (DragonCondVar*)cond;
    pthread_cond_destroy(&cv->c);
    pthread_mutex_destroy(&cv->m);
    free(cv);
}

void dragon_condvar_wait(void* cond, void* lock) {
    DragonCondVar* cv = (DragonCondVar*)cond;
    DragonVThread* vt = dragon_green_self();
    pthread_mutex_lock(&cv->m);
    if (vt) {
        vthread_waitq_push(&cv->waiters, vt);
        vthread_arm_park(vt);
        pthread_mutex_unlock(&cv->m);
        dragon_lock_release(lock);
        mco_yield(vt->coro);
    } else {
        cv->os_waiters++;
        dragon_lock_release(lock);
        dragon_gc_safe_begin();
        while (cv->os_signals == 0) {
            pthread_cond_wait(&cv->c, &cv->m);
        }
        cv->os_signals--;
        cv->os_waiters--;
        pthread_mutex_unlock(&cv->m);
        dragon_gc_safe_end();
    }
    dragon_lock_acquire(lock);
}

void dragon_condvar_signal(void* cond) {
    DragonCondVar* cv = (DragonCondVar*)cond;
    pthread_mutex_lock(&cv->m);
    DragonVThread* waiter = vthread_waitq_pop(&cv->waiters);
    if (!waiter && cv->os_waiters > cv->os_signals) {
        cv->os_signals++;
        pthread_cond_signal(&cv->c);
    }
    pthread_mutex_unlock(&cv->m);
    if (waiter) vthread_wake(waiter);
}

void dragon_condvar_broadcast(void* cond) {
    DragonCondVar* cv = (DragonCondVar*)cond;
    pthread_mutex_lock(&cv->m);
    DragonVThread* stolen = vthread_waitq_steal(&cv->waiters);
    if (cv->os_waiters > cv->os_signals) {
        cv->os_signals = cv->os_waiters;
        pthread_cond_broadcast(&cv->c);
    }
    pthread_mutex_unlock(&cv->m);
    vthread_wake_stolen(stolen);
}

typedef struct DragonRWLock {
    pthread_mutex_t m;
    pthread_cond_t  c;
    DragonVThreadQueue reader_waiters;
    DragonVThreadQueue writer_waiters;
    int64_t os_waiters;
    int64_t active_readers;
    int8_t  writer_active;
} DragonRWLock;

void* dragon_rwlock_new() {
    DragonRWLock* l = (DragonRWLock*)dragon_xcalloc_n(1, sizeof(DragonRWLock));
    pthread_mutex_init(&l->m, NULL);
    pthread_cond_init(&l->c, NULL);
    return l;
}

void dragon_rwlock_free(void* rw) {
    if (!rw) return;
    DragonRWLock* l = (DragonRWLock*)rw;
    pthread_cond_destroy(&l->c);
    pthread_mutex_destroy(&l->m);
    free(l);
}

void dragon_rwlock_rdlock(void* rw) {
    DragonRWLock* l = (DragonRWLock*)rw;
    DragonVThread* vt = dragon_green_self();
    int safe = 0;
    pthread_mutex_lock(&l->m);
    while (l->writer_active) {
        if (vt) {
            green_park(&l->reader_waiters, vt, &l->m);
            continue;
        }
        if (!safe) {
            safe = 1;
            dragon_gc_safe_begin();
        }
        l->os_waiters++;
        pthread_cond_wait(&l->c, &l->m);
        l->os_waiters--;
    }
    l->active_readers++;
    pthread_mutex_unlock(&l->m);
    if (safe) dragon_gc_safe_end();
}

void dragon_rwlock_wrlock(void* rw) {
    DragonRWLock* l = (DragonRWLock*)rw;
    DragonVThread* vt = dragon_green_self();
    pthread_mutex_lock(&l->m);
    int safe = 0;
    while (l->writer_active || l->active_readers > 0) {
        if (vt) {
            green_park(&l->writer_waiters, vt, &l->m);
            continue;
        }
        if (!safe) {
            safe = 1;
            dragon_gc_safe_begin();
        }
        l->os_waiters++;
        pthread_cond_wait(&l->c, &l->m);
        l->os_waiters--;
    }
    l->writer_active = 1;
    pthread_mutex_unlock(&l->m);
    if (safe) dragon_gc_safe_end();
}

int64_t dragon_rwlock_tryrdlock(void* rw) {
    DragonRWLock* l = (DragonRWLock*)rw;
    pthread_mutex_lock(&l->m);
    int64_t got = 0;
    if (!l->writer_active) {
        l->active_readers++;
        got = 1;
    }
    pthread_mutex_unlock(&l->m);
    return got;
}

int64_t dragon_rwlock_trywrlock(void* rw) {
    DragonRWLock* l = (DragonRWLock*)rw;
    pthread_mutex_lock(&l->m);
    int64_t got = 0;
    if (!l->writer_active && l->active_readers == 0) {
        l->writer_active = 1;
        got = 1;
    }
    pthread_mutex_unlock(&l->m);
    return got;
}

void dragon_rwlock_unlock(void* rw) {
    DragonRWLock* l = (DragonRWLock*)rw;
    pthread_mutex_lock(&l->m);
    if (l->writer_active) {
        l->writer_active = 0;
    } else if (l->active_readers > 0) {
        l->active_readers--;
    }
    DragonVThread* writer = NULL;
    DragonVThread* readers = NULL;
    if (l->active_readers == 0) writer = vthread_waitq_pop(&l->writer_waiters);
    if (!writer) readers = vthread_waitq_steal(&l->reader_waiters);
    if (l->os_waiters > 0) pthread_cond_broadcast(&l->c);
    pthread_mutex_unlock(&l->m);
    if (writer) vthread_wake(writer);
    vthread_wake_stolen(readers);
}

static int64_t rwlock_timedrd_wait_os(DragonRWLock* l,
                                      const struct timespec* deadline) {
    pthread_mutex_lock(&l->m);
    while (l->writer_active) {
        l->os_waiters++;
        const int rc = pthread_cond_timedwait(&l->c, &l->m, deadline);
        l->os_waiters--;
        if (l->writer_active && (rc == ETIMEDOUT || dragon__deadline_passed(deadline))) {
            pthread_mutex_unlock(&l->m);
            return 0;
        }
    }
    l->active_readers++;
    pthread_mutex_unlock(&l->m);
    return 1;
}

static int64_t rwlock_timedwr_wait_os(DragonRWLock* l,
                                      const struct timespec* deadline) {
    pthread_mutex_lock(&l->m);
    while (l->writer_active || l->active_readers > 0) {
        l->os_waiters++;
        const int rc = pthread_cond_timedwait(&l->c, &l->m, deadline);
        l->os_waiters--;
        if ((l->writer_active || l->active_readers > 0) &&
            (rc == ETIMEDOUT || dragon__deadline_passed(deadline))) {
            pthread_mutex_unlock(&l->m);
            return 0;
        }
    }
    l->writer_active = 1;
    pthread_mutex_unlock(&l->m);
    return 1;
}

int dragon_rwlock_timedrdlock_sec(void* rw, double seconds) {
    if (dragon_rwlock_tryrdlock(rw)) return 1;
    struct timespec deadline;
    dragon__abs_deadline(seconds, &deadline);
    dragon_extern_enter();
    const int64_t got = rwlock_timedrd_wait_os((DragonRWLock*)rw, &deadline);
    dragon_extern_exit();
    return (int)got;
}

int dragon_rwlock_timedwrlock_sec(void* rw, double seconds) {
    if (dragon_rwlock_trywrlock(rw)) return 1;
    struct timespec deadline;
    dragon__abs_deadline(seconds, &deadline);
    dragon_extern_enter();
    const int64_t got = rwlock_timedwr_wait_os((DragonRWLock*)rw, &deadline);
    dragon_extern_exit();
    return (int)got;
}

typedef struct DragonSem {
    pthread_mutex_t m;
    pthread_cond_t  c;
    DragonVThreadQueue waiters;
    int64_t os_waiters;
    int64_t permits;
} DragonSem;

void* dragon_sem_new(int64_t value) {
    if (value < 0) return nullptr;
    DragonSem* s = (DragonSem*)dragon_xcalloc_n(1, sizeof(DragonSem));
    if (pthread_mutex_init(&s->m, nullptr) != 0) {
        free(s);
        return nullptr;
    }
    if (pthread_cond_init(&s->c, nullptr) != 0) {
        pthread_mutex_destroy(&s->m);
        free(s);
        return nullptr;
    }
    s->permits = value;
    return s;
}

int64_t dragon_sem_acquire(void* handle) {
    DragonSem* s = (DragonSem*)handle;
    if (!s) return -1;
    DragonVThread* vt = dragon_green_self();
    pthread_mutex_lock(&s->m);
    int safe = 0;
    while (s->permits == 0) {
        if (vt) {
            green_park(&s->waiters, vt, &s->m);
            continue;
        }
        if (!safe) {
            safe = 1;
            dragon_gc_safe_begin();
        }
        s->os_waiters++;
        pthread_cond_wait(&s->c, &s->m);
        s->os_waiters--;
    }
    s->permits--;
    pthread_mutex_unlock(&s->m);
    if (safe) dragon_gc_safe_end();
    return 0;
}

int64_t dragon_sem_tryacquire(void* handle) {
    DragonSem* s = (DragonSem*)handle;
    if (!s) return 0;
    pthread_mutex_lock(&s->m);
    int64_t taken = 0;
    if (s->permits > 0) {
        s->permits--;
        taken = 1;
    }
    pthread_mutex_unlock(&s->m);
    return taken;
}

static int64_t sem_timed_wait_os(DragonSem* s, const struct timespec* deadline) {
    pthread_mutex_lock(&s->m);
    while (s->permits == 0) {
        s->os_waiters++;
        const int rc = pthread_cond_timedwait(&s->c, &s->m, deadline);
        s->os_waiters--;
        if (s->permits == 0 && (rc == ETIMEDOUT || dragon__deadline_passed(deadline))) {
            pthread_mutex_unlock(&s->m);
            return 0;
        }
    }
    s->permits--;
    pthread_mutex_unlock(&s->m);
    return 1;
}

int64_t dragon_sem_timedacquire_sec(void* handle, double seconds) {
    DragonSem* s = (DragonSem*)handle;
    if (!s) return 0;
    if (dragon_sem_tryacquire(handle)) return 1;
    struct timespec deadline;
    dragon__abs_deadline(seconds, &deadline);
    dragon_extern_enter();
    const int64_t got = sem_timed_wait_os(s, &deadline);
    dragon_extern_exit();
    return got;
}

int64_t dragon_sem_release(void* handle) {
    DragonSem* s = (DragonSem*)handle;
    if (!s) return -1;
    pthread_mutex_lock(&s->m);
    s->permits++;
    DragonVThread* waiter = vthread_waitq_pop(&s->waiters);
    if (!waiter && s->os_waiters > 0) pthread_cond_signal(&s->c);
    pthread_mutex_unlock(&s->m);
    if (waiter) vthread_wake(waiter);
    return 0;
}

int64_t dragon_sem_free(void* handle) {
    DragonSem* s = (DragonSem*)handle;
    if (!s) return -1;
    pthread_cond_destroy(&s->c);
    pthread_mutex_destroy(&s->m);
    free(s);
    return 0;
}

typedef struct DragonBarrier {
    pthread_mutex_t m;
    pthread_cond_t  c;
    DragonVThreadQueue waiters;
    uint64_t generation;
    int64_t  threshold;
    int64_t  waiting;
} DragonBarrier;

void* dragon_barrier_new(int64_t count) {
    if (count <= 0) return nullptr;
    DragonBarrier* b = (DragonBarrier*)dragon_xcalloc_n(1, sizeof(DragonBarrier));
    if (pthread_mutex_init(&b->m, nullptr) != 0) {
        free(b);
        return nullptr;
    }
    if (pthread_cond_init(&b->c, nullptr) != 0) {
        pthread_mutex_destroy(&b->m);
        free(b);
        return nullptr;
    }
    b->threshold = count;
    return b;
}

int64_t dragon_barrier_wait(void* handle) {
    DragonBarrier* b = (DragonBarrier*)handle;
    if (!b) return -1;
    DragonVThread* vt = dragon_green_self();
    pthread_mutex_lock(&b->m);
    const uint64_t gen = b->generation;
    if (++b->waiting >= b->threshold) {
        b->generation++;
        b->waiting = 0;
        DragonVThread* stolen = vthread_waitq_steal(&b->waiters);
        pthread_cond_broadcast(&b->c);
        pthread_mutex_unlock(&b->m);
        vthread_wake_stolen(stolen);
        return 1;
    }
    int safe = 0;
    while (gen == b->generation) {
        if (vt) {
            green_park(&b->waiters, vt, &b->m);
            continue;
        }
        if (!safe) {
            safe = 1;
            dragon_gc_safe_begin();
        }
        pthread_cond_wait(&b->c, &b->m);
    }
    pthread_mutex_unlock(&b->m);
    if (safe) dragon_gc_safe_end();
    return 0;
}

int64_t dragon_barrier_destroy(void* handle) {
    DragonBarrier* b = (DragonBarrier*)handle;
    if (!b) return -1;
    pthread_cond_destroy(&b->c);
    pthread_mutex_destroy(&b->m);
    free(b);
    return 0;
}

void* dragon_cond_new() {
    pthread_cond_t* c = (pthread_cond_t*)dragon_xmalloc(sizeof(pthread_cond_t));
    pthread_cond_init(c, NULL);
    return c;
}

void dragon_cond_free(void* cond) {
    if (!cond) return;
    pthread_cond_destroy((pthread_cond_t*)cond);
    free(cond);
}

void* dragon_mutex_new() {
    pthread_mutex_t* m =
        (pthread_mutex_t*)dragon_xmalloc(sizeof(pthread_mutex_t));
    pthread_mutex_init(m, NULL);
    return m;
}

void dragon_mutex_free(void* mtx) {
    if (!mtx) return;
    pthread_mutex_destroy((pthread_mutex_t*)mtx);
    free(mtx);
}

void* dragon_flag_new() {
    int64_t* f = (int64_t*)dragon_xmalloc(sizeof(int64_t));
    __atomic_store_n(f, (int64_t)0, __ATOMIC_RELEASE);
    return f;
}

void dragon_flag_free(void* flag) {
    free(flag);
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

#ifdef __linux__
#define DRAGON_NONBLOCK_PER_CALL 1
#define DRAGON_MSG_NB MSG_DONTWAIT
#else
#define DRAGON_NONBLOCK_PER_CALL 0
#define DRAGON_MSG_NB 0
#endif

static void make_nonblocking(int fd) {
#ifdef _WIN32
    u_long nb = 1;
    ioctlsocket((SOCKET)fd, FIONBIO, &nb);
#else
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags != -1 && !(flags & O_NONBLOCK))
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);
#ifdef SO_NOSIGPIPE
    int nosigpipe = 1;
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &nosigpipe, sizeof(nosigpipe));
#endif
#endif
}

static inline void arm_stream_nonblocking(int fd) {
#if DRAGON_NONBLOCK_PER_CALL
    (void)fd;
#else
    make_nonblocking(fd);
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
            if (pfd.revents & POLLNVAL) { WSASetLastError(WSAENOTSOCK); return -1; }
            if (pfd.revents & (POLLERR | POLLHUP)) { WSASetLastError(WSAECONNRESET); return -1; }
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
    dragon_gc_safe_begin();
    int r = WSAPoll(&pfd, 1, timeout_ms);
    dragon_gc_safe_end();
    if (r > 0) {
        if (pfd.revents & POLLNVAL) { WSASetLastError(WSAENOTSOCK); return -1; }
        if (pfd.revents & (POLLERR | POLLHUP)) { WSASetLastError(WSAECONNRESET); return -1; }
        return 1;
    }
    return r == 0 ? 0 : -1;
}
#else
static int nb_wait_fd(int fd, short events) {
    struct pollfd pfd = { fd, events, 0 };
    while (1) {
        dragon_gc_safe_begin();
        int r = poll(&pfd, 1, -1);
        int poll_errno = errno;
        dragon_gc_safe_end();
        if (r > 0) {
            // POLLHUP/POLLERR still mean "the syscall will not block": macOS raises
            // POLLHUP on peer close with data still buffered, so only POLLNVAL fails here.
            if (pfd.revents & POLLNVAL) { errno = EBADF; return -1; }
            return 0;
        }
        if (r < 0 && poll_errno == EINTR) continue;
        errno = poll_errno;
        return -1;
    }
}
static int nb_wait_fd_timeout(int fd, short events, int timeout_ms) {
    struct pollfd pfd = { fd, events, 0 };
    struct timespec start;
    clock_gettime(CLOCK_MONOTONIC, &start);
    int remaining = timeout_ms;
    while (1) {
        dragon_gc_safe_begin();
        int r = poll(&pfd, 1, remaining);
        int poll_errno = errno;
        dragon_gc_safe_end();
        if (r > 0) {
            if (pfd.revents & POLLNVAL) { errno = EBADF; return -1; }
            return 1;
        }
        if (r == 0) return 0;
        if (poll_errno != EINTR) { errno = poll_errno; return -1; }
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        long elapsed = (now.tv_sec - start.tv_sec) * 1000
                     + (now.tv_nsec - start.tv_nsec) / 1000000;
        remaining = timeout_ms - (int)elapsed;
        if (remaining <= 0) return 0;
    }
}
#endif

static inline bool dragon_sock_wouldblock() {
#ifdef _WIN32
    int e = WSAGetLastError();
    return e == WSAEWOULDBLOCK || e == WSAEINPROGRESS;
#else
    return errno == EAGAIN || errno == EWOULDBLOCK;
#endif
}

static inline int dragon_sock_last_error() {
#ifdef _WIN32
    return WSAGetLastError();
#else
    return errno;
#endif
}

static void dragon_raise_sock_error(int err, const char* what) {
    int64_t code;
    const char* name;
#ifdef _WIN32
    switch (err) {
        case WSAECONNRESET:   code = 61; name = "ConnectionResetError"; break;
        case WSAECONNABORTED: code = 59; name = "ConnectionAbortedError"; break;
        case WSAECONNREFUSED: code = 60; name = "ConnectionRefusedError"; break;
        case WSAESHUTDOWN:    code = 58; name = "BrokenPipeError"; break;
        case WSAETIMEDOUT:    code = 56; name = "TimeoutError"; break;
        default:              code = 50; name = "OSError"; break;
    }
    char msg[224];
    snprintf(msg, sizeof(msg), "%s: %s failed: winsock error %d", name, what, err);
#else
    switch (err) {
        case ECONNRESET:   code = 61; name = "ConnectionResetError"; break;
        case ECONNABORTED: code = 59; name = "ConnectionAbortedError"; break;
        case ECONNREFUSED: code = 60; name = "ConnectionRefusedError"; break;
        case EPIPE:        code = 58; name = "BrokenPipeError"; break;
        case ETIMEDOUT:    code = 56; name = "TimeoutError"; break;
        default:           code = 50; name = "OSError"; break;
    }
    char msg[224];
    snprintf(msg, sizeof(msg), "%s: %s failed: %s", name, what, strerror(err));
#endif
    dragon_raise_exc_cstr(code, msg);
}

int64_t dragon_nb_accept(int64_t server_fd, void* addr, void* addrlen) {
    make_nonblocking((int)server_fd);
    while (1) {
#ifdef _WIN32
        int client = (int)accept((SOCKET)server_fd,
                                 (struct sockaddr*)addr, (int*)addrlen);
#elif defined(__linux__) || defined(__FreeBSD__)
        int client = accept4((int)server_fd, (struct sockaddr*)addr,
                             (socklen_t*)addrlen, SOCK_NONBLOCK);
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
                continue;
            }
            if (nb_wait_fd((int)server_fd, POLLIN) < 0) return -1;
            continue;
        }
        return -1;
    }
}

static int64_t dragon_recv_len_or_raise(int64_t max_len) {
    if (__builtin_expect(max_len < 0, 0))
        dragon_raise_exc_cstr(90, "ValueError: receive size must not be negative");
    if (__builtin_expect(max_len > DRAGON_MAX_RECV_BYTES, 0))
        dragon_raise_exc_cstr(43, "MemoryError: receive size exceeds the 1 GiB per-call limit");
    return max_len;
}

static int64_t nb_recv_into(int64_t fd, void* buf, int64_t max_len, int* err_out) {
    *err_out = 0;
    if (max_len <= 0) return 0;
    arm_stream_nonblocking((int)fd);
    while (1) {
#ifdef _WIN32
        int n = recv((SOCKET)fd, (char*)buf, (int)max_len, 0);
#else
        ssize_t n = recv((int)fd, buf, (size_t)max_len, DRAGON_MSG_NB);
#endif
        if (n >= 0) return (int64_t)n;
        if (dragon_sock_wouldblock()) {
            DragonVThread* vt = __current_vthread;
            if (vt && vt->coro) {
                dragon_io_watch_fd((int)fd, IO_EVENT_FD_READ, vt);
                mco_yield(vt->coro);
                continue;
            }
            if (nb_wait_fd((int)fd, POLLIN) < 0) {
                *err_out = dragon_sock_last_error();
                return -1;
            }
            continue;
        }
        *err_out = dragon_sock_last_error();
        return -1;
    }
}

int64_t dragon_nb_recv(int64_t fd, void* buf, int64_t max_len) {
    int err = 0;
    return nb_recv_into(fd, buf, max_len, &err);
}

int64_t dragon_nb_send(int64_t fd, const char* buf, int64_t len) {
    arm_stream_nonblocking((int)fd);
    int64_t total = 0;
    while (total < len) {
#ifdef _WIN32
        int n = send((SOCKET)fd, buf + total, (int)(len - total), 0);
#else
        ssize_t n = send((int)fd, buf + total, (size_t)(len - total),
                         MSG_NOSIGNAL | DRAGON_MSG_NB);
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

const char* dragon_nb_recv_str(int64_t fd, int64_t max_len) {
    dragon_recv_len_or_raise(max_len);
    int64_t cap = max_len > 0 ? max_len : 1;
    char* buf = (char*)dragon_xmalloc_ex(cap, 1, 1);
    int err = 0;
    int64_t n = nb_recv_into(fd, buf, max_len, &err);
    if (n < 0) {
        free(buf);
        dragon_raise_sock_error(err, "receive");
    }
    buf[n] = '\0';
    int32_t clbase = dragon_cleanup_depth();
    dragon_cleanup_push((int64_t)(uintptr_t)buf, DCLEAN_FREE, 0);
    const char* result = dragon_string_alloc(buf, n);
    dragon_cleanup_reset(clbase);
    free(buf);
    return result;
}

DragonBytes* dragon_nb_recv_bytes(int64_t fd, int64_t max_len) {
    dragon_recv_len_or_raise(max_len);
    int64_t cap = max_len > 0 ? max_len : 1;
    uint8_t* buf = (uint8_t*)dragon_xmalloc_n(cap, 1);
    int err = 0;
    int64_t n = nb_recv_into(fd, buf, max_len, &err);
    if (n < 0) {
        free(buf);
        dragon_raise_sock_error(err, "receive");
    }
    DragonBytes* result = dragon_bytes_new(buf, n);
    free(buf);
    return result;
}

enum RecvParkOutcome {
    RECV_PARK_READY     = 0,
    RECV_PARK_TIMED_OUT = 1,
    RECV_PARK_FAILED    = 2
};

static int dragon_recv_park_deadline(int64_t fd, int64_t timeout_ms, int* err_out) {
    DragonVThread* vt = __current_vthread;
    if (vt && vt->coro) {
        vt->io_timed_out = 0;
        dragon_io_watch_fd_deadline((int)fd, IO_EVENT_FD_READ, vt, timeout_ms);
        mco_yield(vt->coro);
        return vt->io_timed_out ? RECV_PARK_TIMED_OUT : RECV_PARK_READY;
    }
    int pr = nb_wait_fd_timeout((int)fd, POLLIN, (int)timeout_ms);
    if (pr > 0) return RECV_PARK_READY;
    if (pr == 0) return RECV_PARK_TIMED_OUT;
    *err_out = dragon_sock_last_error();
    return RECV_PARK_FAILED;
}

DragonBytes* dragon_nb_recv_timeout(int64_t fd, int64_t max_len, int64_t timeout_ms) {
    dragon_recv_len_or_raise(max_len);
    if (timeout_ms <= 0) return dragon_nb_recv_bytes(fd, max_len);
    if (max_len == 0) return dragon_bytes_new(nullptr, 0);
    arm_stream_nonblocking((int)fd);
    int64_t cap = max_len > 0 ? max_len : 1;
    uint8_t* buf = (uint8_t*)dragon_xmalloc_n(cap, 1);
    int64_t n = 0;
    int err = 0;
    while (1) {
#ifdef _WIN32
        int r = recv((SOCKET)fd, (char*)buf, (int)max_len, 0);
#else
        ssize_t r = recv((int)fd, buf, (size_t)max_len, DRAGON_MSG_NB);
#endif
        if (r >= 0) { n = (int64_t)r; break; }
        if (!dragon_sock_wouldblock()) { err = dragon_sock_last_error(); break; }
        int parked = dragon_recv_park_deadline(fd, timeout_ms, &err);
        if (parked == RECV_PARK_READY) continue;
        n = 0;
        break;
    }
    if (err != 0) {
        free(buf);
        dragon_raise_sock_error(err, "receive");
    }
    DragonBytes* result = dragon_bytes_new(buf, n);
    free(buf);
    return result;
}

static int64_t dragon_nb_recv_deadline_raw(int64_t fd, void* buf, int64_t max_len,
                                           int64_t timeout_ms, int* timed_out,
                                           int* err_out) {
    *timed_out = 0;
    *err_out = 0;
    if (max_len <= 0) return 0;
    if (timeout_ms <= 0) return nb_recv_into(fd, buf, max_len, err_out);
    arm_stream_nonblocking((int)fd);
    while (1) {
#ifdef _WIN32
        int r = recv((SOCKET)fd, (char*)buf, (int)max_len, 0);
#else
        ssize_t r = recv((int)fd, buf, (size_t)max_len, DRAGON_MSG_NB);
#endif
        if (r >= 0) return (int64_t)r;
        if (!dragon_sock_wouldblock()) {
            *err_out = dragon_sock_last_error();
            return -1;
        }
        int parked = dragon_recv_park_deadline(fd, timeout_ms, err_out);
        if (parked == RECV_PARK_READY) continue;
        *timed_out = (parked == RECV_PARK_TIMED_OUT);
        return *timed_out ? 0 : -1;
    }
}

const char* dragon_nb_recv_str_deadline(int64_t fd, int64_t max_len, int64_t timeout_ms) {
    dragon_recv_len_or_raise(max_len);
    int64_t cap = max_len > 0 ? max_len : 1;
    char* buf = (char*)dragon_xmalloc_ex(cap, 1, 1);
    int timed_out = 0;
    int err = 0;
    int64_t n = dragon_nb_recv_deadline_raw(fd, buf, max_len, timeout_ms, &timed_out, &err);
    if (timed_out) {
        free(buf);
        dragon_raise_exc_cstr(56, "TimeoutError: receive timed out");
    }
    if (n < 0) {
        free(buf);
        dragon_raise_sock_error(err, "receive");
    }
    buf[n] = '\0';
    int32_t clbase = dragon_cleanup_depth();
    dragon_cleanup_push((int64_t)(uintptr_t)buf, DCLEAN_FREE, 0);
    const char* result = dragon_string_alloc(buf, n);
    dragon_cleanup_reset(clbase);
    free(buf);
    return result;
}

DragonBytes* dragon_nb_recv_bytes_deadline(int64_t fd, int64_t max_len, int64_t timeout_ms) {
    dragon_recv_len_or_raise(max_len);
    int64_t cap = max_len > 0 ? max_len : 1;
    uint8_t* buf = (uint8_t*)dragon_xmalloc_n(cap, 1);
    int timed_out = 0;
    int err = 0;
    int64_t n = dragon_nb_recv_deadline_raw(fd, buf, max_len, timeout_ms, &timed_out, &err);
    if (timed_out) {
        free(buf);
        dragon_raise_exc_cstr(56, "TimeoutError: receive timed out");
    }
    if (n < 0) {
        free(buf);
        dragon_raise_sock_error(err, "receive");
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
        return -1;
    }
    return 0;
}

int64_t dragon_nb_connect_timeout(int64_t fd, void* addr, int64_t addrlen,
                                  int64_t timeout_ms) {
    if (timeout_ms <= 0) return dragon_nb_connect(fd, addr, addrlen);
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
        vt->io_timed_out = 0;
        dragon_io_watch_fd_deadline((int)fd, IO_EVENT_FD_WRITE, vt, timeout_ms);
        mco_yield(vt->coro);
        if (vt->io_timed_out) return -2;
    } else {
        int pr = nb_wait_fd_timeout((int)fd, POLLOUT, (int)timeout_ms);
        if (pr == 0) return -2;
        if (pr < 0) return -1;
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
