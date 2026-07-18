/* SPDX-License-Identifier: BSD-2-Clause */
/* Faithful C++ of bench/my/38_min_max.my: min()/max() over a 1M-element array
 * (LCG-filled), repeated R times, accumulating min+max each pass. */
#include "bench.h"
#include <algorithm>

int main(int argc, char **argv)
{
    long scale = bench_scale(argc, argv);

    const long SZ = 1000000;
    std::vector<long> a(SZ, 0);
    long x = 42;
    for (long i = 0; i < SZ; i++) {
        x = (x * 1103515245 + 12345) % 2147483647;
        a[i] = x;
    }

    long R = 5 * scale;
    long s = 0;

    for (long k = 0; k < R; k++) {
        /* MyLang's min(a) + max(a) is TWO separate full array scans, not one
         * fused pass. Do the same: a min scan, then an independent max scan.
         * bench_sink_ptr both blocks hoisting the invariant scans out of the
         * outer loop AND sits between the two scans so -O3 can't fuse them. */
        bench_sink_ptr(a.data());
        long mn = *std::min_element(a.begin(), a.end());
        bench_sink_ptr(a.data());
        long mx = *std::max_element(a.begin(), a.end());
        s += mn + mx;
    }

    printf("result: %ld\n", s);
    return 0;
}
