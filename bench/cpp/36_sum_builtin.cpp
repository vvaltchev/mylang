/* SPDX-License-Identifier: BSD-2-Clause */
/* Faithful C++ of bench/my/36_sum_builtin.my: sum() over a 1M-element array,
 * repeated R times, with a mod after each sum. */
#include "bench.h"

int main(int argc, char **argv)
{
    long scale = bench_scale(argc, argv);

    const long SZ = 1000000;
    std::vector<long> a(SZ);            /* range(SZ) */
    for (long i = 0; i < SZ; i++)
        a[i] = i;

    long R = 5 * scale;
    long total = 0;

    for (long k = 0; k < R; k++) {
        /* Block hoisting the invariant sum out of the outer loop; MyLang
         * recomputes sum(a) each pass. */
        bench_sink_ptr(a.data());
        long sm = 0;
        for (long e : a)
            sm += e;
        total += sm;
        total = total % 1000000007;
    }

    printf("result: %ld\n", total);
    return 0;
}
