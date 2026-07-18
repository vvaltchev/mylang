/* SPDX-License-Identifier: BSD-2-Clause */
/* Faithful C++ of bench/my/50_autoconst_dce.my: A..D and DEBUG are write-once.
 * The loop guard `(big const arithmetic) > 0 && DEBUG` has DEBUG == 0, so the
 * whole branch is dead. MyLang folds the guard to constant false and eliminates
 * the branch; C++ -O3 does the same dead-code elimination (DEBUG is a
 * compile-time 0), leaving only `s += i`. That residual sum WOULD be
 * close-formed to N*(N-1)/2 (an O(1) collapse) by -O3, which would defeat the
 * linear-scaling test, so bench_sink(s) forces the loop to actually iterate.
 * NOTE: there is no modulo here - `s` grows monotonically (fits int64). */
#include "bench.h"

int main(int argc, char **argv)
{
    long scale = bench_scale(argc, argv);

    const long A = 7, B = 13, C = 5, D = 3, DEBUG = 0;
    long N = 6000000L * scale;
    long s = 0;

    for (long i = 0; i < N; i++) {
        if (((A * B + C) * (D + A) - B * C + (A - D) * (B + C) > 0) && DEBUG) {
            /* dead: never executed, eliminated at compile time */
            s += i * i + 1;
        }
        s += i;
        bench_sink(s);          /* stop -O3 close-forming the sum to O(1) */
    }

    printf("result: %ld\n", s);
    return 0;
}
