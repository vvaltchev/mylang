/* SPDX-License-Identifier: BSD-2-Clause */
/* Faithful C++ of bench/my/49_autoconst_fold.my: A..D are write-once, so the
 * loop-invariant sub-expression `k` folds to a single literal at compile time
 * (MyLang auto-const + const-fold; C++ -O3 folds the same arithmetic). The
 * per-iteration work that survives is the serial modular accumulator, so the
 * `%` dependency chain keeps the loop from being close-formed - no in-loop
 * barrier needed. */
#include "bench.h"

int main(int argc, char **argv)
{
    long scale = bench_scale(argc, argv);

    const long A = 7, B = 13, C = 5, D = 3;
    long N = 3000000L * scale;
    long s = 0;

    for (long i = 0; i < N; i++) {
        long k = (A * B + C) * (D + A) - B * C + A * B * C * D % 100
                 + (A - D) * (B + C);
        s += k + i;
        s = s % 1000000007;
    }

    printf("result: %ld\n", s);
    return 0;
}
