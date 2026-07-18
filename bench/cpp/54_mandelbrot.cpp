/* SPDX-License-Identifier: BSD-2-Clause */
/* Faithful C++ of bench/my/54_mandelbrot.my: sum of escape-iteration counts
 * over a 200x200 grid. Double math (MyLang float_type == double). The inner
 * `while` is a serial dependency chain (z = z*z + c), so it runs as written. */
#include "bench.h"

int main(int argc, char **argv)
{
    long scale = bench_scale(argc, argv);
    const int SIZE = 200;
    long MAXIT = 80L * scale;
    long total = 0;

    for (int py = 0; py < SIZE; py++) {
        double y0 = (py * 1.0 / SIZE) * 2.0 - 1.0;
        for (int px = 0; px < SIZE; px++) {
            double x0 = (px * 1.0 / SIZE) * 3.0 - 2.0;
            double zr = 0.0, zi = 0.0;
            long it = 0;
            while (it < MAXIT && zr * zr + zi * zi <= 4.0) {
                double nzr = zr * zr - zi * zi + x0;
                zi = 2.0 * zr * zi + y0;
                zr = nzr;
                it++;
            }
            total += it;
        }
    }
    printf("result: %ld\n", total);
    return 0;
}
