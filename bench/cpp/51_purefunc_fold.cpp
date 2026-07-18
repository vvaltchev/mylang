/* SPDX-License-Identifier: BSD-2-Clause */
/* Faithful C++ of bench/my/51_purefunc_fold.my: sq()/cube() are side-effect
 * free, so MyLang auto-promotes them to pure and folds their constant-argument
 * calls; with A,B,C write-once the whole `k` collapses to one literal. C++ -O3
 * inlines the two functions and folds the same constant arithmetic. The serial
 * modular accumulator (`%` dependency chain) keeps the loop iterating - no
 * in-loop barrier needed. */
#include "bench.h"

static inline long sq(long x)   { return x * x; }
static inline long cube(long x) { return x * x * x; }

int main(int argc, char **argv)
{
    long scale = bench_scale(argc, argv);

    const long A = 7, B = 13, C = 5;
    long N = 3000000L * scale;
    long s = 0;

    for (long i = 0; i < N; i++) {
        long k = sq(A) + cube(B) - sq(C) + sq(A + B) * 2;
        s += k + i;
        s = s % 1000000007;
    }

    printf("result: %ld\n", s);
    return 0;
}
