public class Parallel {
    static final int WORKERS = 8;
    static final long CHUNK = 30000000L, M = 1000000007L;

    static long work(long lo, long hi) {
        long acc = 0;
        for (long i = lo; i < hi; i++) acc = (acc + i) % M;
        return acc;
    }

    public static void main(String[] args) throws InterruptedException {
        Thread[] th = new Thread[WORKERS];
        long[] res = new long[WORKERS];
        for (int w = 0; w < WORKERS; w++) {
            final int ww = w;
            th[w] = new Thread(() -> res[ww] = work((long) ww * CHUNK, (long) ww * CHUNK + CHUNK));
            th[w].start();
        }
        long total = 0;
        for (int w = 0; w < WORKERS; w++) {
            th[w].join();
            total += res[w];
        }
        System.out.println(total);
    }
}
