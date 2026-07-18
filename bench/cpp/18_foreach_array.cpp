/* SPDX-License-Identifier: BSD-2-Clause */
/* Faithful C++ of bench/my/18_foreach_array.my: foreach over a 1M-element array,
 * repeated R times, accumulating with a mod after each full pass. */
#include "bench.h"

int main(int argc, char **argv)
{
    long scale = bench_scale(argc, argv);

    std::vector<long> a(1000000);       /* range(1000000) */
    for (long i = 0; i < 1000000; i++)
        a[i] = i;

    long R = 3 * scale;
    long s = 0;

    for (long k = 0; k < R; k++) {
        /* MyLang re-scans the whole array every outer pass; block the compiler
         * from hoisting the loop-invariant reduction out of the outer loop. */
        bench_sink_ptr(a.data());
        for (long e : a)
            s += e;
        s = s % 1000000007;
    }

    printf("result: %ld\n", s);
    return 0;
}
