#include <stdio.h>

int main(void) {
    int width = 1600, height = 1600, maxiter = 100;
    long count = 0;
    for (int py = 0; py < height; py++) {
        double ci = 2.0 * py / height - 1.0;
        for (int px = 0; px < width; px++) {
            double cr = 2.0 * px / width - 1.5;
            double zr = 0.0, zi = 0.0, zr2 = 0.0, zi2 = 0.0;
            int it = 0;
            while (it < maxiter && zr2 + zi2 <= 4.0) {
                zi = 2.0 * zr * zi + ci;
                zr = zr2 - zi2 + cr;
                zr2 = zr * zr;
                zi2 = zi * zi;
                it++;
            }
            if (zr2 + zi2 <= 4.0) count++;
        }
    }
    printf("%ld\n", count);
    return 0;
}
