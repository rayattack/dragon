#include <stdio.h>
#include <pthread.h>

#define WORKERS 8
#define CHUNK 30000000L
#define M 1000000007L

typedef struct { long lo, hi, result; } Arg;

static void* work(void* p) {
    Arg* a = (Arg*)p;
    long acc = 0;
    for (long i = a->lo; i < a->hi; i++) acc = (acc + i) % M;
    a->result = acc;
    return NULL;
}

int main(void) {
    pthread_t th[WORKERS];
    Arg args[WORKERS];
    for (int w = 0; w < WORKERS; w++) {
        args[w].lo = (long)w * CHUNK;
        args[w].hi = args[w].lo + CHUNK;
        pthread_create(&th[w], NULL, work, &args[w]);
    }
    long total = 0;
    for (int w = 0; w < WORKERS; w++) {
        pthread_join(th[w], NULL);
        total += args[w].result;
    }
    printf("%ld\n", total);
    return 0;
}
