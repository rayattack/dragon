import threading

WORKERS = 8
CHUNK = 30000000
M = 1000000007


def work(lo, hi, res, idx):
    acc = 0
    for i in range(lo, hi):
        acc = (acc + i) % M
    res[idx] = acc


res = [0] * WORKERS
threads = []
for w in range(WORKERS):
    t = threading.Thread(target=work, args=(w * CHUNK, w * CHUNK + CHUNK, res, w))
    t.start()
    threads.append(t)
for t in threads:
    t.join()
print(sum(res))
