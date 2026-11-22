/* SPDX-License-Identifier: BSD-2-Clause */
/* Faithful C++ of bench/my/71_exc_no_throw.my: a try/catch that NEVER throws -
 * measures the pure handler-region overhead on the no-exception HOT PATH (with
 * zero-cost EH, ~free). `s` is a serial modular-sum dependency (no barrier
 * needed); `caught` stays 0. Both feed the printed result. */
#include "bench.h"

struct DivByZero {};

int main(int argc, char **argv)
{
    long scale = bench_scale(argc, argv);
    long N = 500000L * scale;
    long s = 0;
    long caught = 0;

    for (long i = 0; i < N; i++) {
        try {
            s = (s + i) % 1000000007;
        } catch (const DivByZero &) {
            caught++;                    /* never taken */
        }
    }

    printf("result: %ld %ld\n", s, caught);
    return 0;
}
