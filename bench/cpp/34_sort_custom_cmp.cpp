/* SPDX-License-Identifier: BSD-2-Clause */
/* Faithful C++ of bench/my/34_sort_custom_cmp.my: fill with an LCG, sort with a
 * custom comparator (p < q, ascending), checksum min+max, R times. The same LCG
 * seed each pass regenerates identical data, so std::sort with the p<q
 * comparator matches MyLang's ascending sort exactly. */
#include "bench.h"
#include <algorithm>

int main(int argc, char **argv)
{
    long scale = bench_scale(argc, argv);
    const long SZ = 50000;
    long R = 2 * scale;
    long checksum = 0;

    for (long k = 0; k < R; k++) {
        std::vector<long> a(SZ, 0);
        long x = 987654321;
        for (long i = 0; i < SZ; i++) {
            x = (x * 1103515245 + 12345) % 2147483647;
            a[i] = x;
        }
        std::sort(a.begin(), a.end(),
                  [](long p, long q) { return p < q; });
        checksum += a[0] + a[SZ - 1];
    }

    printf("result: %ld\n", checksum);
    return 0;
}
