#include <cstdio>
#include <unordered_map>
#include <string>

int main() {
    long n = 3000000, k = 200000;
    std::unordered_map<std::string, long> counts;
    for (long i = 0; i < n; i++) counts[std::to_string(i % k)]++;
    long total = 0;
    for (long j = 0; j < k; j++) total += j * counts[std::to_string(j)];
    printf("%ld\n", total);
    return 0;
}
