#include <stdio.h>
#include <stdlib.h>

typedef struct {
    long x;
    long y;
} Point;

int main() {
    long total = 0;
    for (int i = 0; i < 1000000; i++) {
        Point *p = malloc(sizeof(Point));
        p->x = i;
        p->y = i + 1;
        total += p->x + p->y;
        free(p);
    }
    printf("%ld\n", total);
    return 0;
}
