import java.util.HashMap;

public class Dicts {
    public static void main(String[] args) {
        long n = 3000000, k = 200000;
        HashMap<String, Long> counts = new HashMap<>();
        for (long i = 0; i < n; i++) {
            counts.merge(Long.toString(i % k), 1L, Long::sum);
        }
        long total = 0;
        for (long j = 0; j < k; j++) {
            total += j * counts.get(Long.toString(j));
        }
        System.out.println(total);
    }
}
