/* SPDX-License-Identifier: BSD-2-Clause */
/* Faithful C++ of bench/my/61_popcount.my: sum of Hamming weights over a range,
 * computed bit-by-bit in a tight inner loop. `>>>` is a logical shift; x is
 * non-negative throughout, so a plain `>>` is identical. The inner while has a
 * data-dependent trip count (not closed-formable) and `total` reaches print. */
#include "bench.h"

int main(int argc, char **argv)
{
    long scale = bench_scale(argc, argv);
    long N = 200000L * scale;
    long total = 0;

    for (long i = 0; i < N; i++) {
        long x = i;                  /* non-negative, so >> == >>> */
        while (x != 0) {
            total += x & 1;
            x >>= 1;
        }
    }

    printf("result: %ld\n", total);
    return 0;
}
