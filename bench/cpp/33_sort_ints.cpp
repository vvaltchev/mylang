/* SPDX-License-Identifier: BSD-2-Clause */
/* Faithful C++ of bench/my/33_sort_ints.my: fill with an LCG, std::sort,
 * checksum the min+max. Same LCG + data as the .my/.py so the result agrees. */
#include "bench.h"
#include <algorithm>

int main(int argc, char **argv)
{
    long scale = bench_scale(argc, argv);
    const long SZ = 200000;
    long R = 3 * scale;
    long checksum = 0;

    for (long k = 0; k < R; k++) {
        std::vector<long> a(SZ, 0);
        long x = 123456789;
        for (long i = 0; i < SZ; i++) {
            x = (x * 1103515245 + 12345) % 2147483647;
            a[i] = x;
        }
        std::sort(a.begin(), a.end());
        checksum += a[0] + a[SZ - 1];
    }
    printf("result: %ld\n", checksum);
    return 0;
}
