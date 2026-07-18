/* SPDX-License-Identifier: BSD-2-Clause */
/* Faithful C++ of bench/my/05_mixed_arith.my: int->float promotion each
 * iteration (i * 0.5 is long*double, x - i is double-long). A serial recurrence
 * through x, so it can't be vectorized or close-formed - no barrier needed.
 * Printed with str(x, 4) => fixed 4 decimals. */
#include "bench.h"

int main(int argc, char **argv)
{
    long scale = bench_scale(argc, argv);
    long N = 1000000L * scale;
    double x = 0.0;
    for (long i = 0; i < N; i++) {
        x = x + i * 0.5;   /* int * float -> float */
        x = x - i;         /* float - int   -> float */
        if (x > 1000000.0)
            x = 0.0;
    }
    printf("result: %.4f\n", x);
    return 0;
}
