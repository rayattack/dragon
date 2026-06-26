/* C has no standard hashmap, so we hand-roll open-addressing + FNV-1a.
   This measures a decent hand-written C map vs the others' built-ins. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CAP (1 << 20)  /* 1,048,576 slots; load factor < 0.2 for 200K keys */

typedef struct { char* key; long val; int used; } Slot;
static Slot table[CAP];

static unsigned long fnv1a(const char* s) {
    unsigned long h = 1469598103934665603UL;
    while (*s) { h ^= (unsigned char)*s++; h *= 1099511628211UL; }
    return h;
}

static long* slot_for(const char* key) {
    unsigned long h = fnv1a(key) & (CAP - 1);
    while (table[h].used) {
        if (strcmp(table[h].key, key) == 0) return &table[h].val;
        h = (h + 1) & (CAP - 1);
    }
    table[h].used = 1;
    table[h].key = strdup(key);
    table[h].val = 0;
    return &table[h].val;
}

int main(void) {
    long n = 3000000, k = 200000;
    char buf[32];
    for (long i = 0; i < n; i++) {
        sprintf(buf, "%ld", i % k);
        (*slot_for(buf))++;
    }
    long total = 0;
    for (long j = 0; j < k; j++) {
        sprintf(buf, "%ld", j);
        total += j * (*slot_for(buf));
    }
    printf("%ld\n", total);
    return 0;
}
