/* SPDX-License-Identifier: BSD-2-Clause */
/* Faithful C++ of bench/my/72_exc_finally.my: try/finally on the NORMAL
 * (non-throwing) path - the finally-region cost. C++ has no `finally`, so the
 * always-run cleanup (fin++) is an RAII scope guard whose destructor runs at
 * the end of each iteration's scope (and would still run on a throw). `s` is a
 * serial modular-sum dependency; `fin` counts iterations. Both feed print. */
#include "bench.h"

struct FinallyGuard {
    long &fin;
    ~FinallyGuard() { ++fin; }
};

int main(int argc, char **argv)
{
    long scale = bench_scale(argc, argv);
    long N = 500000L * scale;
    long s = 0;
    long fin = 0;

    for (long i = 0; i < N; i++) {
        FinallyGuard g{fin};             /* ~FinallyGuard == the `finally` */
        s = (s + i) % 1000000007;
    }

    printf("result: %ld %ld\n", s, fin);
    return 0;
}
