#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>

int sieve(int limit) {
    bool *is_prime = malloc(limit + 1);
    memset(is_prime, 1, limit + 1);
    is_prime[0] = is_prime[1] = false;
    for (int i = 2; i * i <= limit; i++) {
        if (is_prime[i]) {
            for (int j = i * i; j <= limit; j += i) {
                is_prime[j] = false;
            }
        }
    }
    int count = 0;
    for (int i = 0; i <= limit; i++) {
        if (is_prime[i]) count++;
    }
    free(is_prime);
    return count;
}

int main() {
    printf("%d\n", sieve(1000000));
    return 0;
}
