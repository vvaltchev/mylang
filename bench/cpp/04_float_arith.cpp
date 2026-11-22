/* SPDX-License-Identifier: BSD-2-Clause */
/* Faithful C++ of bench/my/04_float_arith.my: a serial floating-point recurrence
 * (double; MyLang float_type is IEEE double). Each x depends on the previous x
 * through the recurrence, so the compiler can neither vectorize nor close-form
 * it - no in-loop barrier needed. Printed with str(x, 4) => fixed 4 decimals. */
#include "bench.h"

int main(int argc, char **argv)
{
    long scale = bench_scale(argc, argv);
    long N = 1000000L * scale;
    double x = 1.0;
    for (long i = 0; i < N; i++) {
        x = x * 1.0000001 + 0.5;
        x = x - 0.4999999;
        if (x > 1000000.0)
            x = x / 3.0;
    }
    printf("result: %.4f\n", x);
    return 0;
}
