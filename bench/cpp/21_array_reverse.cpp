/* SPDX-License-Identifier: BSD-2-Clause */
/* Faithful C++ of bench/my/21_array_reverse.my: in-place reversal R times via
 * reverse(); prints a[0] (which ends at 0 or 99999 depending on R's parity). */
#include "bench.h"
#include <algorithm>

int main(int argc, char **argv)
{
    long scale = bench_scale(argc, argv);

    std::vector<long> a(100000);        /* range(100000) */
    for (long i = 0; i < 100000; i++)
        a[i] = i;

    long R = 50 * scale;
    for (long k = 0; k < R; k++) {
        std::reverse(a.begin(), a.end());
        /* MyLang performs every reversal (each O(k)); the barrier stops the
         * compiler from collapsing pairs of reversals (reverse twice == noop). */
        bench_sink_ptr(a.data());
    }

    printf("result: %ld\n", a[0]);
    return 0;
}
