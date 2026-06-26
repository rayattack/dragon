#include <cstdio>
#include <thread>
#include <vector>

const long WORKERS = 8, CHUNK = 30000000, M = 1000000007;

static long work(long lo, long hi) {
    long acc = 0;
    for (long i = lo; i < hi; i++) acc = (acc + i) % M;
    return acc;
}

int main() {
    std::vector<std::thread> th;
    std::vector<long> res(WORKERS);
    for (long w = 0; w < WORKERS; w++)
        th.emplace_back([w, &res] { res[w] = work(w * CHUNK, w * CHUNK + CHUNK); });
    long total = 0;
    for (long w = 0; w < WORKERS; w++) { th[w].join(); total += res[w]; }
    printf("%ld\n", total);
    return 0;
}
