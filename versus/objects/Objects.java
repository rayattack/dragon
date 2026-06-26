public class Objects {
    static class Point {
        long x, y;
        Point(long x, long y) { this.x = x; this.y = y; }
    }

    public static void main(String[] args) {
        long total = 0;
        for (int i = 0; i < 1000000; i++) {
            Point p = new Point(i, i + 1);
            total += p.x + p.y;
        }
        System.out.println(total);
    }
}
