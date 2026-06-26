#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main() {
    size_t len = 0;
    size_t cap = 1024;
    char *s = malloc(cap);
    s[0] = '\0';
    for (int i = 0; i < 10000; i++) {
        size_t need = len + 5;
        if (need >= cap) {
            cap = need * 2;
            s = realloc(s, cap);
        }
        memcpy(s + len, "hello", 5);
        len += 5;
    }
    s[len] = '\0';
    printf("%zu\n", len);
    free(s);
    return 0;
}
