// test/tools/malloc_fail.c - OOM fault-injection shim for verifying the
// allocation-hardening of dragon_xmalloc / dragon_xrealloc.
//
// OOM is not reachable from a normal .dr test, so allocation hardening is
// verified by interposing libc malloc/realloc and failing a chosen call. The
// statically-linked Dragon runtime calls libc's dynamic malloc, so LD_PRELOAD
// reaches it.
//
// Build:
//  cc -shared -fPIC -O2 test/tools/malloc_fail.c -o /tmp/malloc_fail.so -ldl
//
// Use (build a Dragon program, then run it under the shim):
//  DRAGON_FAIL_REPORT=1 LD_PRELOAD=/tmp/malloc_fail.so ./prog # count allocs
//  DRAGON_FAIL_ALLOC_AT=<N> LD_PRELOAD=/tmp/malloc_fail.so ./prog # fail the Nth
//
// Expected: before the fix, failing an allocation that a caller
// dereferences (e.g. chr() -> dragon_string_alloc_ucs4) SIGSEGVs (exit 139);
// after the fix it raises a catchable MemoryError. A `try/except MemoryError`
// around the operation should catch it and exit cleanly.
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdlib.h>
#include <stdio.h>

static void* (*real_malloc)(size_t);
static void* (*real_realloc)(void*, size_t);
static long counter, fail_at = -1;
static int inited, report;

static void report_total(void) {
    if (report) fprintf(stderr, "[fault] total allocs=%ld\n", counter);
}

static void init(void) {
    real_malloc  = (void* (*)(size_t))dlsym(RTLD_NEXT, "malloc");
    real_realloc = (void* (*)(void*, size_t))dlsym(RTLD_NEXT, "realloc");
    const char* e = getenv("DRAGON_FAIL_ALLOC_AT");
    fail_at = e ? atol(e) : -1;
    report = getenv("DRAGON_FAIL_REPORT") ? 1 : 0;
    atexit(report_total);
    inited = 1;
}

void* malloc(size_t n) {
    if (!inited) init();
    long c = ++counter;
    if (fail_at > 0 && c == fail_at) {
        fprintf(stderr, "[fault] malloc #%ld (size %zu) -> NULL\n", c, n);
        return NULL;
    }
    return real_malloc(n);
}

void* realloc(void* p, size_t n) {
    if (!inited) init();
    long c = ++counter;
    if (fail_at > 0 && c == fail_at) {
        fprintf(stderr, "[fault] realloc #%ld -> NULL\n", c);
        return NULL;
    }
    return real_realloc(p, n);
}
