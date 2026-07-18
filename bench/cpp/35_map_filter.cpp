/* SPDX-License-Identifier: BSD-2-Clause */
/* Faithful C++ of bench/my/35_map_filter.my: map(x=>x*2) then
 * filter(x=>x%3==0) over [0..SZ-1], printing len(c). Builds the real
 * intermediate arrays MyLang builds - a sized `b` for map, a grown `c`
 * (push_back) for filter - so the measured work matches. bench_sink keeps the
 * arrays materialized; c.size() reaches print. */
#include "bench.h"

int main(int argc, char **argv)
{
    long scale = bench_scale(argc, argv);
    long SZ = 500000L * scale;

    std::vector<long> a(SZ);
    for (long i = 0; i < SZ; i++)
        a[i] = i;                    /* range(SZ) == [0..SZ-1] */

    std::vector<long> b(SZ);
    for (long i = 0; i < SZ; i++)
        b[i] = a[i] * 2;             /* map(func(x) => x * 2, a) */

    std::vector<long> c;
    for (long i = 0; i < SZ; i++)
        if (b[i] % 3 == 0)           /* filter(func(x) => x % 3 == 0, b) */
            c.push_back(b[i]);

    bench_sink_ptr(b.data());
    bench_sink_ptr(c.data());

    printf("result: %ld\n", (long)c.size());
    return 0;
}
