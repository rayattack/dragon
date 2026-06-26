public class BinaryTrees {
    static final class Node { Node left, right; }

    static Node make(int depth) {
        Node n = new Node();
        if (depth > 0) {
            n.left = make(depth - 1);
            n.right = make(depth - 1);
        }
        return n;
    }

    static long check(Node n) {
        if (n.left == null) return 1;
        return 1 + check(n.left) + check(n.right);
    }

    public static void main(String[] args) {
        int minDepth = 4, maxDepth = 14;
        long total = 0;
        total += check(make(maxDepth + 1));
        for (int depth = minDepth; depth <= maxDepth; depth += 2) {
            long iterations = 1L << (maxDepth - depth + minDepth);
            for (long i = 0; i < iterations; i++) {
                total += check(make(depth));
            }
        }
        System.out.println(total);
    }
}
