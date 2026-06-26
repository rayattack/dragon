#include <cstdio>

struct Node { Node *left, *right; };

static Node* make(int depth) {
    Node* n = new Node();
    if (depth == 0) { n->left = nullptr; n->right = nullptr; }
    else { n->left = make(depth - 1); n->right = make(depth - 1); }
    return n;
}

static long check(Node* n) {
    if (n->left == nullptr) return 1;
    return 1 + check(n->left) + check(n->right);
}

static void freetree(Node* n) {
    if (n->left) { freetree(n->left); freetree(n->right); }
    delete n;
}

int main() {
    int min_depth = 4, max_depth = 14;
    long total = 0;
    Node* s = make(max_depth + 1);
    total += check(s);
    freetree(s);
    for (int depth = min_depth; depth <= max_depth; depth += 2) {
        long iterations = 1L << (max_depth - depth + min_depth);
        for (long i = 0; i < iterations; i++) {
            Node* t = make(depth);
            total += check(t);
            freetree(t);
        }
    }
    printf("%ld\n", total);
    return 0;
}
