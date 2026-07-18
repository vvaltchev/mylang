/* SPDX-License-Identifier: BSD-2-Clause */
/* Faithful C++ of bench/my/37_range_builtin.my: MyLang range(N) eagerly builds
 * a whole array, then foreach sums it. We build the array (matching the eager
 * allocation, like Python's list(range(N))) and reduce over real memory. */
#include "bench.h"

int main(int argc, char **argv)
{
    long scale = bench_scale(argc, argv);
    long N = 1000000L * scale;

    std::vector<long> r(N);            /* range(N) -> [0..N-1], eagerly built */
    for (long i = 0; i < N; i++)
        r[i] = i;

    /* Materialize the array so the reduction reads memory (can't be
     * closed-formed to N*(N-1)/2); it may still vectorize - the C++ win. */
    bench_sink_ptr(r.data());

    long s = 0;
    for (long e : r)
        s += e;

    printf("result: %ld\n", s);
    return 0;
}
