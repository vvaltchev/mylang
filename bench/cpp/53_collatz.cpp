/* SPDX-License-Identifier: BSD-2-Clause */
/* Faithful C++ of bench/my/53_collatz.my: total stopping time for all starts in
 * 1..N. The inner while loop's trip count is the Collatz stopping time - fully
 * data-dependent, so it can't be close-formed - no barrier needed. Integer
 * division n / 2 is truncating (n is non-negative here), matching MyLang. */
#include "bench.h"

int main(int argc, char **argv)
{
    long scale = bench_scale(argc, argv);
    long N = 30000L * scale;
    long total = 0;
    for (long i = 1; i < N; i++) {
        long n = i;
        long steps = 0;
        while (n != 1) {
            if (n % 2 == 0)
                n = n / 2;
            else
                n = 3 * n + 1;
            steps++;
        }
        total += steps;
    }
    printf("result: %ld\n", total);
    return 0;
}
