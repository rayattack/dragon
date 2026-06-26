n = 3000000
k = 200000
counts = {}
for i in range(n):
    key = str(i % k)
    counts[key] = counts.get(key, 0) + 1
total = 0
for j in range(k):
    total += j * counts[str(j)]
print(total)
