#include <iostream>

struct Point {
    long x, y;
    Point(long x, long y) : x(x), y(y) {}
};

int main() {
    long total = 0;
    for (int i = 0; i < 1000000; i++) {
        Point *p = new Point(i, i + 1);
        total += p->x + p->y;
        delete p;
    }
    std::cout << total << std::endl;
    return 0;
}
